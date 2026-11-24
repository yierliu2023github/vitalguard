/*
 * vg_log.h — tiny structured logger shared by all VitalGuard processes.
 *
 * Host build: lines go to stderr (children under the supervisor are
 * redirected to per-process log files in the runtime directory).
 * QNX port: vg_log() is the single seam where output is re-targeted to
 * slogger2 (slog2_register/slog2c) so the alarm audit trail lands in the
 * target's structured log.  Keep all printing behind this interface.
 */
#ifndef VG_LOG_H
#define VG_LOG_H

/* Set the process tag used as line prefix, e.g. "vitaldsp". */
void vg_log_set_tag(const char *tag);

/* Printf-style log line: "[<wall-ms>] <tag>: <message>\n" on stderr. */
void vg_log(const char *fmt, ...);

#endif /* VG_LOG_H */
