/*
 * vg_path.h — locate the directory this process's executable lives in.
 *
 * vgsupervisor spawns its sibling binaries (alarmgr / vitaldsp /
 * vitalgen) from its own directory, and `faultctl hog start` finds
 * vghog the same way, so both need a portable "where am I" that does
 * not depend on the caller's working directory.
 *
 * Resolution order in vg_self_dir():
 *   1. $VG_BINDIR — explicit override, wins on every platform.  The
 *      QEMU image autostart sets it; tests may too.
 *   2. Linux: readlink("/proc/self/exe").
 *   3. argv[0]: if it contains a '/', its (realpath-resolved)
 *      directory; otherwise a $PATH search for an executable of
 *      that name, using the winning entry's directory.
 *
 * Why no QNX-native mechanism in step 2: the SDP 8.0 headers were
 * checked on disk — QNX procfs has no `exe` path symlink (its
 * `exefile` entry is the executable *image*, not a path, so
 * readlink() cannot work), <sys/process.h> offers only
 * getprogname()/_cmdname() (the invocation name, no directory), and
 * the only path-bearing procfs interface is the debugger route
 * (DCMD_PROC_MAPDEBUG -> procfs_debuginfo.path), which is a debug
 * interface, not a runtime locator.  Step 3 covers QNX: the target
 * binaries are always launched either by path (/system/bin/...) or
 * by name through $PATH, which includes their directory.
 */
#ifndef VG_PATH_H
#define VG_PATH_H

#include <stddef.h>

/* Write the NUL-terminated directory containing this process's
 * executable into buf (no trailing slash, except for "/").
 * argv0 is main()'s argv[0].  Returns 0 on success, -1 on failure. */
int vg_self_dir(char *buf, size_t size, const char *argv0);

#endif /* VG_PATH_H */
