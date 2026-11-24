/*
 * ipc_posix.c — host (Linux) backend for the VitalGuard IPC abstraction.
 *
 * Unix domain stream sockets, one per endpoint name, in the runtime
 * directory.  Wire format per message:
 *
 *     uint32 type | uint32 length | uint64 timestamp_ms | payload[length]
 *
 * (native byte order — both ends are always the same machine, on host
 * and on the QNX target alike).
 */
#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "vg_time.h"

struct vg_ipc_conn {
    int  fd;
    char name[64];
};

struct vg_ipc_listener {
    int  fd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

/* ------------------------------------------------------------------ */

const char *vg_ipc_runtime_dir(void)
{
    const char *dir = getenv("VG_RUNTIME_DIR");
    return (dir && dir[0]) ? dir : "/tmp/vitalguard";
}

static int ensure_runtime_dir(void)
{
    return mkdir(vg_ipc_runtime_dir(), 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int endpoint_path(const char *name, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s/%s.sock",
                     vg_ipc_runtime_dir(), name);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

/* ------------------------------------------------------------------ */

vg_ipc_listener_t *vg_ipc_listen(const char *name)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un addr;
    vg_ipc_listener_t *lst;

    if (ensure_runtime_dir() != 0)
        return NULL;
    if (endpoint_path(name, path, sizeof(path)) != 0)
        return NULL;

    lst = calloc(1, sizeof(*lst));
    if (!lst)
        return NULL;

    lst->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lst->fd < 0) {
        free(lst);
        return NULL;
    }

    unlink(path);   /* replace a stale socket from a previous run */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(lst->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lst->fd, 8) != 0) {
        close(lst->fd);
        free(lst);
        return NULL;
    }
    snprintf(lst->path, sizeof(lst->path), "%s", path);
    return lst;
}

vg_ipc_conn_t *vg_ipc_accept(vg_ipc_listener_t *lst, int timeout_ms)
{
    struct pollfd pfd;
    vg_ipc_conn_t *conn;
    int fd;

    pfd.fd     = lst->fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return NULL;

    fd = accept(lst->fd, NULL, NULL);
    if (fd < 0)
        return NULL;

    conn = calloc(1, sizeof(*conn));
    if (!conn) {
        close(fd);
        return NULL;
    }
    conn->fd = fd;
    return conn;
}

vg_ipc_conn_t *vg_ipc_connect(const char *name, int timeout_ms)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    uint64_t deadline;

    if (endpoint_path(name, path, sizeof(path)) != 0)
        return NULL;

    deadline = vg_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        if (fd < 0)
            return NULL;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            vg_ipc_conn_t *conn = calloc(1, sizeof(*conn));
            if (!conn) {
                close(fd);
                return NULL;
            }
            conn->fd = fd;
            strncpy(conn->name, name, sizeof(conn->name) - 1);
            return conn;
        }
        close(fd);
        if (vg_now_ms() >= deadline)
            return NULL;
        vg_sleep_ms(25);
    }
}

/* ------------------------------------------------------------------ */

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        /* MSG_NOSIGNAL: a dead peer must surface as an error return,
         * never as a SIGPIPE that kills the sender (vitalgen must
         * survive vitaldsp being restarted under it). */
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read exactly len bytes; 1 = ok, 0 = timeout before first byte,
 * -1 = error/EOF (a partial frame is always an error). */
static int read_all(int fd, void *buf, size_t len, int timeout_ms)
{
    char *p = (char *)buf;
    int waited = 0;
    while (len > 0) {
        struct pollfd pfd;
        ssize_t n;
        pfd.fd     = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, waited ? 1000 : timeout_ms) <= 0)
            return waited ? -1 : 0;
        waited = 1;
        n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;              /* orderly shutdown mid-frame */
        p   += n;
        len -= (size_t)n;
    }
    return 1;
}

int vg_ipc_send(vg_ipc_conn_t *conn, uint32_t type,
                const void *payload, uint32_t length, uint64_t timestamp_ms)
{
    struct {
        uint32_t type;
        uint32_t length;
        uint64_t ts;
    } hdr;

    if (!conn || length > VG_IPC_MAX_PAYLOAD)
        return -1;
    hdr.type   = type;
    hdr.length = length;
    hdr.ts     = timestamp_ms;
    if (write_all(conn->fd, &hdr, sizeof(hdr)) != 0)
        return -1;
    if (length > 0 && write_all(conn->fd, payload, length) != 0)
        return -1;
    return 0;
}

int vg_ipc_recv(vg_ipc_conn_t *conn, uint32_t *type_out, uint64_t *ts_out,
                void *buf, uint32_t buf_size, int timeout_ms)
{
    struct {
        uint32_t type;
        uint32_t length;
        uint64_t ts;
    } hdr;
    int rc = read_all(conn->fd, &hdr, sizeof(hdr), timeout_ms);
    if (rc <= 0)
        return rc;
    if (hdr.length > buf_size || hdr.length > VG_IPC_MAX_PAYLOAD)
        return -1;
    if (hdr.length > 0) {
        rc = read_all(conn->fd, buf, hdr.length, 1000);
        if (rc != 1)
            return -1;
    }
    if (type_out)
        *type_out = hdr.type;
    if (ts_out)
        *ts_out = hdr.ts;
    return 1;
}

/* ------------------------------------------------------------------ */

void vg_ipc_close(vg_ipc_conn_t *conn)
{
    if (!conn)
        return;
    close(conn->fd);
    free(conn);
}

void vg_ipc_listener_close(vg_ipc_listener_t *lst)
{
    if (!lst)
        return;
    close(lst->fd);
    unlink(lst->path);
    free(lst);
}

int vg_ipc_conn_fd(const vg_ipc_conn_t *conn)
{
    return conn ? conn->fd : -1;
}

int vg_ipc_listener_fd(const vg_ipc_listener_t *lst)
{
    return lst ? lst->fd : -1;
}
