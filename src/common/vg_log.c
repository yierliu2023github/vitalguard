#include "vg_log.h"
#include "vg_time.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_tag[24] = "vg";

void vg_log_set_tag(const char *tag)
{
    if (tag) {
        strncpy(g_tag, tag, sizeof(g_tag) - 1);
        g_tag[sizeof(g_tag) - 1] = '\0';
    }
}

void vg_log(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[%llu] %s: ",
            (unsigned long long)vg_now_ms(), g_tag);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
