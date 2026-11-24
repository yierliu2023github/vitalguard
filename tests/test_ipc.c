/*
 * test_ipc.c — ping-pong test of the POSIX IPC backend.
 *
 * A child process listens and echoes; the parent sends 200 framed
 * messages and verifies every reply, then reports the average
 * round-trip time (informational — the alarm path's host transport
 * budget is documented in docs/design.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "vg.h"
#include "vg_time.h"
#include "ipc.h"

#define MSG_PING 99
#define MSG_PONG 100
#define ROUNDS   200

typedef struct {
    uint32_t seq;
    char     pad[60];
} payload_t;

int main(void)
{
    char dir[128];
    pid_t pid;

    snprintf(dir, sizeof(dir), "/tmp/vg_ipc_test_%d", (int)getpid());
    setenv("VG_RUNTIME_DIR", dir, 1);

    pid = fork();
    if (pid == 0) {
        /* child: echo server */
        vg_ipc_listener_t *lst = vg_ipc_listen("testep");
        vg_ipc_conn_t *conn;
        int i;
        if (!lst)
            _exit(2);
        conn = vg_ipc_accept(lst, 10000);
        if (!conn)
            _exit(3);
        for (i = 0; i < ROUNDS; i++) {
            payload_t p;
            uint32_t type;
            if (vg_ipc_recv(conn, &type, NULL, &p, sizeof(p), 10000) != 1)
                _exit(4);
            if (type != MSG_PING)
                _exit(5);
            if (vg_ipc_send(conn, MSG_PONG, &p, sizeof(p), 0) != 0)
                _exit(6);
        }
        _exit(0);
    }

    /* parent: ping client */
    {
        vg_ipc_conn_t *conn = vg_ipc_connect("testep", 10000);
        uint64_t t0, total;
        int i, status;

        if (!conn) {
            printf("test_ipc: FAIL — cannot connect\n");
            kill(pid, SIGKILL);
            return 1;
        }
        t0 = vg_now_ms();
        for (i = 0; i < ROUNDS; i++) {
            payload_t p, q;
            uint32_t type;
            memset(&p, 0, sizeof(p));
            p.seq = (uint32_t)i;
            memset(p.pad, 'A' + (i % 26), sizeof(p.pad));
            if (vg_ipc_send(conn, MSG_PING, &p, sizeof(p), 0) != 0) {
                printf("test_ipc: FAIL — send %d\n", i);
                return 1;
            }
            if (vg_ipc_recv(conn, &type, NULL, &q, sizeof(q), 5000) != 1 ||
                type != MSG_PONG || q.seq != (uint32_t)i ||
                memcmp(p.pad, q.pad, sizeof(p.pad)) != 0) {
                printf("test_ipc: FAIL — reply %d mismatch\n", i);
                return 1;
            }
        }
        total = vg_now_ms() - t0;
        vg_ipc_close(conn);
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("test_ipc: FAIL — server exit status %d\n", status);
            return 1;
        }
        printf("test_ipc: %d ping-pongs in %llu ms "
               "(avg %.3f ms round trip)\n",
               ROUNDS, (unsigned long long)total,
               (double)total / ROUNDS);
        printf("test_ipc: ALL PASSED\n");
        return 0;
    }
}
