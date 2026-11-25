#include "vg_path.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Copy the directory part of `path` into buf.  A path with no '/'
 * yields "."; a path directly under root yields "/". */
static int dir_of(const char *path, char *buf, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t n;

    if (!slash) {
        if (size < 2)
            return -1;
        buf[0] = '.';
        buf[1] = '\0';
        return 0;
    }
    n = (size_t)(slash - path);
    if (n == 0)
        n = 1;                      /* "/prog" -> "/" */
    if (n + 1 > size)
        return -1;
    memcpy(buf, path, n);
    buf[n] = '\0';
    return 0;
}

static int copy_dir(const char *dir, char *buf, size_t size)
{
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/')
        n--;                        /* strip trailing slashes */
    if (n + 1 > size)
        return -1;
    memcpy(buf, dir, n);
    buf[n] = '\0';
    return 0;
}

int vg_self_dir(char *buf, size_t size, const char *argv0)
{
    const char *env = getenv("VG_BINDIR");
    char tmp[PATH_MAX];

    if (env && env[0])
        return copy_dir(env, buf, size);

#if defined(__linux__)
    {
        ssize_t n = readlink("/proc/self/exe", tmp, sizeof(tmp) - 1);
        if (n > 0) {
            tmp[n] = '\0';
            return dir_of(tmp, buf, size);
        }
    }
#endif

    if (argv0 && argv0[0]) {
        if (strchr(argv0, '/')) {
            if (realpath(argv0, tmp) != NULL)
                return dir_of(tmp, buf, size);
            return dir_of(argv0, buf, size);
        }
        /* Bare name: search $PATH for an executable match. */
        {
            const char *path = getenv("PATH");
            if (path) {
                const char *p = path;
                for (;;) {
                    const char *colon = strchr(p, ':');
                    size_t dlen = colon ? (size_t)(colon - p)
                                        : strlen(p);
                    if (dlen > 0 && dlen + 1 + strlen(argv0) <
                        sizeof(tmp)) {
                        memcpy(tmp, p, dlen);
                        tmp[dlen] = '\0';
                        strncat(tmp, "/", sizeof(tmp) - strlen(tmp) - 1);
                        strncat(tmp, argv0,
                                sizeof(tmp) - strlen(tmp) - 1);
                        if (access(tmp, X_OK) == 0)
                            return dir_of(tmp, buf, size);
                    }
                    if (!colon)
                        break;
                    p = colon + 1;
                }
            }
        }
    }
    return -1;
}
