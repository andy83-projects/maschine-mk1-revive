#ifndef BRIDGE_H
#define BRIDGE_H

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static inline const char *mk1_bridge_log_path(void)
{
    return "/tmp/maschine-mk1-revive/bridge.log";
}

static inline void mk1_bridge_write_bytes(const char *path, const char *bytes, size_t len)
{
    struct stat st;
    FILE *fp = NULL;
    static const size_t kMaxBytes = 10 * 1024 * 1024;

    if (!path || !path[0] || !bytes || len == 0) return;

    if (stat(path, &st) == 0 && (uint64_t)st.st_size + (uint64_t)len > (uint64_t)kMaxBytes) {
        fp = fopen(path, "w");
        if (fp) {
            static const char wrap_marker[] = "--- bridge.log wrapped at 10 MiB ---\n";
            fwrite(wrap_marker, 1, sizeof(wrap_marker) - 1, fp);
        }
    } else {
        fp = fopen(path, "a");
    }

    if (!fp) return;
    fwrite(bytes, 1, len, fp);
    fflush(fp);
    fclose(fp);
}

static inline int mk1_bridge_vfprintf(FILE *stream, const char *fmt, va_list args)
{
    va_list measure_args;
    va_list render_args;
    int needed = 0;
    char stack_buf[1024];
    char *line = stack_buf;

    if (stream != stderr) {
        return vfprintf(stream, fmt, args);
    }

    va_copy(measure_args, args);
    needed = vsnprintf(NULL, 0, fmt, measure_args);
    va_end(measure_args);
    if (needed < 0) return needed;

    if ((size_t)needed + 1 > sizeof(stack_buf)) {
        line = malloc((size_t)needed + 1);
        if (!line) return -1;
    }

    va_copy(render_args, args);
    vsnprintf(line, (size_t)needed + 1, fmt, render_args);
    va_end(render_args);

    mk1_bridge_write_bytes(mk1_bridge_log_path(), line, (size_t)needed);

    if (line != stack_buf) free(line);
    return needed;
}

static inline int mk1_bridge_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list args;
    int result = 0;

    va_start(args, fmt);
    result = mk1_bridge_vfprintf(stream, fmt, args);
    va_end(args);
    return result;
}

static inline void mk1_bridge_logf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    mk1_bridge_vfprintf(stderr, fmt, args);
    va_end(args);
}

#endif // BRIDGE_H
