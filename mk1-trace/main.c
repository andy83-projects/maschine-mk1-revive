#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../mk1-ipc/mk1_ipc.h"

#define TRACE_BOOTSTRAP_PORT NIHA_BOOTSTRAP_PORT
#define TRACE_MAX_NAME_LEN 256
#define TRACE_MAX_PATH_LEN 1024
#define TRACE_DEFAULT_SERIAL "SN-buscwvye"
#define TRACE_DEFAULT_SCP_DEST "m4:~/Documents/GitRepos/maschine-mk1-revive/trace"

#define TRACE_STATE_DEVICE_CONNECT_SENT   1u
#define TRACE_STATE_DEVICE_ACKED          2u
#define TRACE_STATE_SERIAL_CONNECT_SENT   3u
#define TRACE_STATE_INSTANCE_ACKED        4u

static volatile sig_atomic_t g_should_exit = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_should_exit = 1;
    CFRunLoopStop(CFRunLoopGetMain());
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} msg_buf_t;

static void buf_init(msg_buf_t *m)
{
    m->cap = 128;
    m->len = 0;
    m->buf = (uint8_t *)calloc(1, m->cap);
}

static void buf_free(msg_buf_t *m)
{
    if (!m) return;
    free(m->buf);
    m->buf = NULL;
    m->len = 0;
    m->cap = 0;
}

static void buf_ensure(msg_buf_t *m, size_t need)
{
    while (m->len + need > m->cap) {
        m->cap *= 2;
        m->buf = (uint8_t *)realloc(m->buf, m->cap);
    }
}

static void buf_push_u32(msg_buf_t *m, uint32_t value)
{
    buf_ensure(m, sizeof(value));
    memcpy(m->buf + m->len, &value, sizeof(value));
    m->len += sizeof(value);
}

static void buf_push_bytes(msg_buf_t *m, const void *bytes, size_t len)
{
    buf_ensure(m, len);
    memcpy(m->buf + m->len, bytes, len);
    m->len += len;
}

static CFDataRef buf_to_cfdata(msg_buf_t *m)
{
    CFDataRef data = CFDataCreate(NULL, m->buf, (CFIndex)m->len);
    buf_free(m);
    return data;
}

static const char *msg_name(uint32_t type)
{
    switch (type) {
    case NI_MSG_VERSION: return "GetServiceVersion";
    case NI_MSG_PID_CONNECT: return "PID_CONNECT";
    case NI_MSG_SERIAL_CONNECT: return "SERIAL_CONNECT";
    case NI_MSG_ACK_NOTIF_PORT: return "ACK_NOTIF_PORT";
    case NI_MSG_DEVSTATE: return "DEVSTATE";
    case NI_MSG_GETSERIAL: return "GETSERIAL";
    case NI_MSG_SETFOCUS: return "SETFOCUS";
    case NI_EVT_DEVSTATE_BOOL: return "DEVSTATE_BOOL";
    case NI_EVT_DEVICE_ON: return "DEVICE_ON";
    case NI_EVT_DEVICE_OFF: return "DEVICE_OFF";
    case NI_EVT_PAD_DATA: return "PAD_DATA";
    case NI_EVT_BTN_DATA: return "BTN_DATA";
    case NI_EVT_KNOB_ROTATE: return "KNOB_ROTATE";
    case NI_EVT_KNOB_4D: return "KNOB_4D";
    case NI_EVT_TOUCHSTRIP: return "TOUCHSTRIP";
    case NI_CMD_LED: return "LED";
    case NI_CMD_DISPLAY: return "DISPLAY";
    case NI_CMD_START: return "START";
    default: return "UNKNOWN";
    }
}

typedef struct {
    FILE *text_log;
    FILE *bin_log;
    char output_root[TRACE_MAX_PATH_LEN];
    char session_dir[TRACE_MAX_PATH_LEN];
    char archive_path[TRACE_MAX_PATH_LEN];
    char scp_dest[TRACE_MAX_PATH_LEN];

    CFMessagePortRef bootstrap_port;
    CFMessagePortRef dev_request_port;
    CFMessagePortRef inst_request_port;
    CFMessagePortRef dev_notif_port;
    CFMessagePortRef inst_notif_port;
    CFRunLoopSourceRef dev_notif_rls;
    CFRunLoopSourceRef inst_notif_rls;

    char dev_request_name[TRACE_MAX_NAME_LEN];
    char dev_notif_name[TRACE_MAX_NAME_LEN];
    char inst_request_name[TRACE_MAX_NAME_LEN];
    char inst_notif_name[TRACE_MAX_NAME_LEN];
    char serial[TRACE_MAX_NAME_LEN];

    uint32_t state;
    uint32_t tx_seq;
    uint32_t rx_seq;
    bool auto_second_stage;
    bool issue_optional_queries;
    bool saw_any_notification;
} trace_ctx_t;

static void timestamp_iso8601(char *out, size_t out_len)
{
    struct timespec ts;
    struct tm tm_local;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_local);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &tm_local);
    snprintf(out + strlen(out), out_len - strlen(out), ".%03ld", ts.tv_nsec / 1000000L);
}

static void hex_dump(FILE *fp, const uint8_t *bytes, size_t len, const char *prefix)
{
    if (!fp || !bytes) return;
    for (size_t i = 0; i < len; i++) {
        if ((i % 16u) == 0u) {
            fprintf(fp, "%s", prefix ? prefix : "");
        }
        fprintf(fp, "%02x", bytes[i]);
        if ((i % 16u) == 15u || i + 1u == len) {
            fprintf(fp, "\n");
        } else {
            fputc(' ', fp);
        }
    }
}

static void words_dump(FILE *fp, const uint8_t *bytes, size_t len, const char *prefix)
{
    if (!fp || !bytes || len < 4u) return;
    fprintf(fp, "%s", prefix ? prefix : "");
    for (size_t i = 0; i + 3u < len; i += 4u) {
        uint32_t word = 0;
        memcpy(&word, bytes + i, sizeof(word));
        fprintf(fp, "0x%08x", word);
        if (i + 4u < len) {
            fputc(' ', fp);
        }
    }
    fputc('\n', fp);
}

static void ascii_dump(FILE *fp, const uint8_t *bytes, size_t len, const char *prefix)
{
    if (!fp || !bytes) return;
    fprintf(fp, "%s", prefix ? prefix : "");
    for (size_t i = 0; i < len; i++) {
        uint8_t c = bytes[i];
        fputc((c >= 0x20 && c < 0x7f) ? (int)c : '.', fp);
    }
    fputc('\n', fp);
}

static void append_binary_record(FILE *fp,
                                 const char *direction,
                                 const char *port_name,
                                 SInt32 msgid,
                                 const uint8_t *bytes,
                                 size_t len)
{
    uint32_t dir_len = (uint32_t)strlen(direction);
    uint32_t port_len = (uint32_t)strlen(port_name);
    uint32_t payload_len = (uint32_t)len;
    int32_t signed_msgid = (int32_t)msgid;

    if (!fp) return;

    fwrite(&dir_len, sizeof(dir_len), 1, fp);
    fwrite(direction, 1, dir_len, fp);
    fwrite(&port_len, sizeof(port_len), 1, fp);
    fwrite(port_name, 1, port_len, fp);
    fwrite(&signed_msgid, sizeof(signed_msgid), 1, fp);
    fwrite(&payload_len, sizeof(payload_len), 1, fp);
    if (payload_len > 0) {
        fwrite(bytes, 1, payload_len, fp);
    }
    fflush(fp);
}

static void trace_log_message(trace_ctx_t *ctx,
                              const char *direction,
                              const char *port_name,
                              SInt32 msgid,
                              const uint8_t *bytes,
                              size_t len)
{
    char stamp[64] = {0};
    uint32_t type = 0;

    if (!ctx || !ctx->text_log || !bytes || len == 0) return;

    timestamp_iso8601(stamp, sizeof(stamp));
    if (len >= 4) {
        memcpy(&type, bytes, sizeof(type));
    }

    fprintf(ctx->text_log,
            "[%s] %s port=%s msgid=%d len=%zu type=0x%08x (%s)\n",
            stamp,
            direction,
            port_name ? port_name : "-",
            (int)msgid,
            len,
            type,
            msg_name(type));
    words_dump(ctx->text_log, bytes, len, "  words: ");
    hex_dump(ctx->text_log, bytes, len, "  hex:   ");
    ascii_dump(ctx->text_log, bytes, len, "  ascii: ");
    fputc('\n', ctx->text_log);
    fflush(ctx->text_log);

    append_binary_record(ctx->bin_log,
                         direction ? direction : "?",
                         port_name ? port_name : "-",
                         msgid,
                         bytes,
                         len);
}

static bool ensure_dir(const char *path)
{
    char cmd[TRACE_MAX_PATH_LEN + 32];
    if (!path || !path[0]) return false;
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    return system(cmd) == 0;
}

static bool open_logs(trace_ctx_t *ctx)
{
    char path[TRACE_MAX_PATH_LEN];
    time_t now;
    struct tm tm_local;
    char stamp[32];

    if (!ctx) return false;

    now = time(NULL);
    localtime_r(&now, &tm_local);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_local);
    snprintf(ctx->session_dir,
             sizeof(ctx->session_dir),
             "%s/mk1-trace-session-%s",
             ctx->output_root,
             stamp);
    snprintf(ctx->archive_path,
             sizeof(ctx->archive_path),
             "%s/mk1-trace-session-%s.tar.gz",
             ctx->output_root,
             stamp);

    if (!ensure_dir(ctx->session_dir)) {
        fprintf(stderr, "failed to create session dir: %s\n", ctx->session_dir);
        return false;
    }

    snprintf(path, sizeof(path), "%s/session.log", ctx->session_dir);
    ctx->text_log = fopen(path, "w");
    if (!ctx->text_log) return false;

    snprintf(path, sizeof(path), "%s/messages.bin", ctx->session_dir);
    ctx->bin_log = fopen(path, "wb");
    if (!ctx->bin_log) return false;

    fprintf(stderr, "[mk1-trace] session dir: %s\n", ctx->session_dir);
    return true;
}

static void close_logs(trace_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->text_log) fclose(ctx->text_log);
    if (ctx->bin_log) fclose(ctx->bin_log);
    ctx->text_log = NULL;
    ctx->bin_log = NULL;
}

static bool archive_logs(trace_ctx_t *ctx)
{
    char cmd[(TRACE_MAX_PATH_LEN * 3) + 128];
    int status = 0;

    if (!ctx || !ctx->session_dir[0] || !ctx->archive_path[0]) return false;

    snprintf(cmd,
             sizeof(cmd),
             "tar -czf '%s' -C '%s' session.log messages.bin",
             ctx->archive_path,
             ctx->session_dir);
    status = system(cmd);
    if (status != 0) {
        fprintf(stderr,
                "[mk1-trace] failed to create archive '%s' (status=%d)\n",
                ctx->archive_path,
                status);
        return false;
    }

    fprintf(stderr, "[mk1-trace] archived logs: %s\n", ctx->archive_path);
    return true;
}

static void maybe_scp_logs(trace_ctx_t *ctx)
{
    char session_log_path[TRACE_MAX_PATH_LEN];
    char messages_path[TRACE_MAX_PATH_LEN];
    char cmd[(TRACE_MAX_PATH_LEN * 4) + 256];
    int status = 0;

    if (!ctx || !ctx->scp_dest[0]) return;

    snprintf(session_log_path,
             sizeof(session_log_path),
             "%s/session.log",
             ctx->session_dir);
    snprintf(messages_path,
             sizeof(messages_path),
             "%s/messages.bin",
             ctx->session_dir);

    snprintf(cmd,
             sizeof(cmd),
             "scp '%s' '%s' '%s' '%s'",
             ctx->archive_path,
             session_log_path,
             messages_path,
             ctx->scp_dest);
    fprintf(stderr, "[mk1-trace] scp -> %s\n", ctx->scp_dest);
    status = system(cmd);
    if (status != 0) {
        fprintf(stderr,
                "[mk1-trace] scp failed (status=%d)\n",
                status);
    }
}

static bool parse_connect_reply(CFDataRef reply,
                                char *req_name,
                                size_t req_name_len,
                                char *notif_name,
                                size_t notif_name_len)
{
    const uint8_t *p = NULL;
    CFIndex total = 0;
    uint32_t tag = 0;
    uint32_t len_a = 0;
    uint32_t len_b = 0;
    size_t copy_len = 0;
    CFIndex offset = 0;

    if (!reply) return false;
    total = CFDataGetLength(reply);
    p = CFDataGetBytePtr(reply);
    if (!p || total < 12) return false;

    memcpy(&tag, p, sizeof(tag));
    if (tag != NI_TAG_TRUE) return false;

    memcpy(&len_a, p + 4, sizeof(len_a));
    if (len_a == 0 || (CFIndex)(8 + len_a) > total) return false;

    copy_len = len_a;
    if (copy_len > 0 && p[8 + copy_len - 1] == '\0') copy_len--;
    if (copy_len >= req_name_len) copy_len = req_name_len - 1;
    memcpy(req_name, p + 8, copy_len);
    req_name[copy_len] = '\0';

    offset = 8 + (CFIndex)len_a;
    if (offset + 4 > total) return false;

    memcpy(&len_b, p + offset, sizeof(len_b));
    if (len_b == 0 || offset + 4 + (CFIndex)len_b > total) return false;

    copy_len = len_b;
    if (copy_len > 0 && p[offset + 4 + copy_len - 1] == '\0') copy_len--;
    if (copy_len >= notif_name_len) copy_len = notif_name_len - 1;
    memcpy(notif_name, p + offset + 4, copy_len);
    notif_name[copy_len] = '\0';

    return true;
}

static bool extract_device_on_serial(CFDataRef data, char *serial, size_t serial_len)
{
    const uint8_t *bytes = NULL;
    CFIndex len = 0;
    uint32_t type = 0;

    if (!data || !serial || serial_len == 0) return false;
    bytes = CFDataGetBytePtr(data);
    len = CFDataGetLength(data);
    if (!bytes || len < 16) return false;

    memcpy(&type, bytes, sizeof(type));
    if (type != NI_EVT_DEVICE_ON) return false;

    for (CFIndex i = 4; i < len; i++) {
        if (bytes[i] == 'S' && (i + 3) < len && bytes[i + 1] == 'N' && bytes[i + 2] == '-') {
            size_t j = 0;
            while ((i + (CFIndex)j) < len && j + 1 < serial_len) {
                uint8_t c = bytes[i + (CFIndex)j];
                if (c == '\0') break;
                if (!(c == '-' || c == '_' || c == '.' ||
                      (c >= '0' && c <= '9') ||
                      (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z'))) {
                    break;
                }
                serial[j++] = (char)c;
            }
            serial[j] = '\0';
            return j > 0;
        }
    }

    return false;
}

static CFMessagePortRef open_remote_port(const char *name)
{
    CFStringRef cf_name = NULL;
    CFMessagePortRef port = NULL;

    if (!name || !name[0]) return NULL;
    cf_name = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!cf_name) return NULL;
    port = CFMessagePortCreateRemote(NULL, cf_name);
    CFRelease(cf_name);
    return port;
}

static CFMessagePortRef wait_for_remote_port(const char *name, unsigned int retry_delay_seconds)
{
    CFMessagePortRef port = NULL;
    unsigned int attempt = 0;

    if (!name || !name[0]) return NULL;

    while (!g_should_exit) {
        attempt++;
        port = open_remote_port(name);
        if (port) {
            fprintf(stderr,
                    "[mk1-trace] connected to remote port '%s' on attempt %u\n",
                    name,
                    attempt);
            return port;
        }

        fprintf(stderr,
                "[mk1-trace] waiting for remote port '%s' (attempt %u)\n",
                name,
                attempt);
        sleep(retry_delay_seconds);
    }

    return NULL;
}

static bool send_message(trace_ctx_t *ctx,
                         CFMessagePortRef port,
                         const char *port_name,
                         const uint8_t *bytes,
                         size_t len,
                         CFDataRef *reply_out)
{
    CFDataRef payload = NULL;
    CFDataRef reply = NULL;
    SInt32 result;

    if (!ctx || !port || !bytes || len == 0) return false;

    payload = CFDataCreate(NULL, bytes, (CFIndex)len);
    if (!payload) return false;

    trace_log_message(ctx, "TX", port_name, 0, bytes, len);
    result = CFMessagePortSendRequest(port,
                                      0,
                                      payload,
                                      5.0,
                                      5.0,
                                      kCFRunLoopDefaultMode,
                                      &reply);
    CFRelease(payload);

    if (result != kCFMessagePortSuccess) {
        fprintf(stderr, "[mk1-trace] send to %s failed: %d\n", port_name, (int)result);
        return false;
    }

    if (reply) {
        trace_log_message(ctx,
                          "RX",
                          port_name,
                          0,
                          CFDataGetBytePtr(reply),
                          (size_t)CFDataGetLength(reply));
    }

    if (reply_out) {
        *reply_out = reply;
    } else if (reply) {
        CFRelease(reply);
    }
    return true;
}

static bool send_version_probe(trace_ctx_t *ctx)
{
    msg_buf_t m;
    CFDataRef reply = NULL;
    bool ok = false;

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_VERSION);
    CFDataRef payload = buf_to_cfdata(&m);

    if (payload) {
        ok = send_message(ctx,
                          ctx->bootstrap_port,
                          TRACE_BOOTSTRAP_PORT,
                          CFDataGetBytePtr(payload),
                          (size_t)CFDataGetLength(payload),
                          &reply);
        CFRelease(payload);
    }

    if (reply) CFRelease(reply);
    return ok;
}

static bool create_local_notif_port(trace_ctx_t *ctx,
                                    const char *name,
                                    CFMessagePortCallBack callback,
                                    CFMessagePortRef *slot,
                                    CFRunLoopSourceRef *rls_slot)
{
    CFStringRef cf_name = NULL;
    CFMessagePortContext context = {0, ctx, NULL, NULL, NULL};
    Boolean should_free = false;

    if (!ctx || !name || !slot || !rls_slot) return false;

    cf_name = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!cf_name) return false;

    *slot = CFMessagePortCreateLocal(NULL, cf_name, callback, &context, &should_free);
    CFRelease(cf_name);
    if (!*slot) {
        fprintf(stderr, "[mk1-trace] failed to create local notification port '%s'\n", name);
        return false;
    }

    *rls_slot = CFMessagePortCreateRunLoopSource(NULL, *slot, 0);
    if (!*rls_slot) {
        fprintf(stderr, "[mk1-trace] failed to create run-loop source for '%s'\n", name);
        return false;
    }

    CFRunLoopAddSource(CFRunLoopGetMain(), *rls_slot, kCFRunLoopCommonModes);
    fprintf(stderr, "[mk1-trace] listening on local notification port '%s'\n", name);
    return true;
}

static bool send_ack(trace_ctx_t *ctx,
                     CFMessagePortRef request_port,
                     const char *request_name,
                     const char *notif_name)
{
    msg_buf_t m;
    CFDataRef reply = NULL;
    bool ok;
    size_t notif_len = strlen(notif_name) + 1u;

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_ACK_NOTIF_PORT);
    buf_push_u32(&m, NI_TAG_TRUE);
    buf_push_u32(&m, 0u);
    buf_push_u32(&m, (uint32_t)notif_len);
    buf_push_bytes(&m, notif_name, notif_len);

    CFDataRef payload = buf_to_cfdata(&m);
    ok = send_message(ctx,
                      request_port,
                      request_name,
                      CFDataGetBytePtr(payload),
                      (size_t)CFDataGetLength(payload),
                      &reply);
    CFRelease(payload);
    if (reply) CFRelease(reply);
    return ok;
}

static bool send_pid_connect(trace_ctx_t *ctx)
{
    msg_buf_t m;
    CFDataRef reply = NULL;
    bool ok = false;

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_PID_CONNECT);
    buf_push_u32(&m, MK1_DEVICE_ID);
    buf_push_u32(&m, NI_TAG_NIM2);
    buf_push_u32(&m, NI_TAG_PRMY);
    buf_push_u32(&m, 0u);

    CFDataRef payload = buf_to_cfdata(&m);
    if (!payload) return false;

    ok = send_message(ctx,
                      ctx->bootstrap_port,
                      TRACE_BOOTSTRAP_PORT,
                      CFDataGetBytePtr(payload),
                      (size_t)CFDataGetLength(payload),
                      &reply);
    CFRelease(payload);
    if (!ok || !reply) return false;

    ok = parse_connect_reply(reply,
                             ctx->dev_request_name,
                             sizeof(ctx->dev_request_name),
                             ctx->dev_notif_name,
                             sizeof(ctx->dev_notif_name));
    if (ok) {
        fprintf(stderr, "[mk1-trace] device request port: %s\n", ctx->dev_request_name);
        fprintf(stderr, "[mk1-trace] device notif port:   %s\n", ctx->dev_notif_name);
        ctx->dev_request_port = open_remote_port(ctx->dev_request_name);
        if (!ctx->dev_request_port) {
            ok = false;
        }
    }

    if (reply) CFRelease(reply);
    return ok;
}

static bool send_serial_connect(trace_ctx_t *ctx)
{
    msg_buf_t m;
    CFDataRef reply = NULL;
    bool ok = false;
    size_t serial_len;

    if (!ctx->serial[0]) return false;
    serial_len = strlen(ctx->serial) + 1u;

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_SERIAL_CONNECT);
    buf_push_u32(&m, MK1_DEVICE_ID);
    buf_push_u32(&m, NI_TAG_NIM2);
    buf_push_u32(&m, NI_TAG_PRMY);
    buf_push_u32(&m, (uint32_t)serial_len);
    buf_push_bytes(&m, ctx->serial, serial_len);

    CFDataRef payload = buf_to_cfdata(&m);
    if (!payload) return false;

    ok = send_message(ctx,
                      ctx->bootstrap_port,
                      TRACE_BOOTSTRAP_PORT,
                      CFDataGetBytePtr(payload),
                      (size_t)CFDataGetLength(payload),
                      &reply);
    CFRelease(payload);
    if (!ok || !reply) return false;

    ok = parse_connect_reply(reply,
                             ctx->inst_request_name,
                             sizeof(ctx->inst_request_name),
                             ctx->inst_notif_name,
                             sizeof(ctx->inst_notif_name));
    if (ok) {
        fprintf(stderr, "[mk1-trace] instance request port: %s\n", ctx->inst_request_name);
        fprintf(stderr, "[mk1-trace] instance notif port:   %s\n", ctx->inst_notif_name);
        ctx->inst_request_port = open_remote_port(ctx->inst_request_name);
        if (!ctx->inst_request_port) {
            ok = false;
        }
    }

    if (reply) CFRelease(reply);
    return ok;
}

static void maybe_issue_optional_queries(trace_ctx_t *ctx)
{
    msg_buf_t m;
    CFDataRef payload = NULL;
    CFDataRef reply = NULL;

    if (!ctx || !ctx->issue_optional_queries || !ctx->inst_request_port) return;

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_GETSERIAL);
    payload = buf_to_cfdata(&m);
    if (payload) {
        send_message(ctx,
                     ctx->inst_request_port,
                     ctx->inst_request_name,
                     CFDataGetBytePtr(payload),
                     (size_t)CFDataGetLength(payload),
                     &reply);
        CFRelease(payload);
        if (reply) CFRelease(reply);
    }

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_DEVSTATE);
    payload = buf_to_cfdata(&m);
    if (payload) {
        send_message(ctx,
                     ctx->inst_request_port,
                     ctx->inst_request_name,
                     CFDataGetBytePtr(payload),
                     (size_t)CFDataGetLength(payload),
                     &reply);
        CFRelease(payload);
        if (reply) CFRelease(reply);
    }
}

static CFDataRef device_notif_callback(CFMessagePortRef local,
                                       SInt32 msgid,
                                       CFDataRef data,
                                       void *info);
static CFDataRef instance_notif_callback(CFMessagePortRef local,
                                         SInt32 msgid,
                                         CFDataRef data,
                                         void *info);

static bool open_instance_notif(trace_ctx_t *ctx)
{
    return create_local_notif_port(ctx,
                                   ctx->inst_notif_name,
                                   instance_notif_callback,
                                   &ctx->inst_notif_port,
                                   &ctx->inst_notif_rls);
}

static bool open_device_notif(trace_ctx_t *ctx)
{
    return create_local_notif_port(ctx,
                                   ctx->dev_notif_name,
                                   device_notif_callback,
                                   &ctx->dev_notif_port,
                                   &ctx->dev_notif_rls);
}

static void begin_instance_handshake(trace_ctx_t *ctx)
{
    if (!ctx || ctx->state >= TRACE_STATE_SERIAL_CONNECT_SENT || !ctx->serial[0]) return;

    ctx->state = TRACE_STATE_SERIAL_CONNECT_SENT;
    fprintf(stderr, "[mk1-trace] starting instance handshake with serial '%s'\n", ctx->serial);
    if (!send_serial_connect(ctx)) {
        fprintf(stderr, "[mk1-trace] SERIAL_CONNECT failed\n");
        ctx->state = TRACE_STATE_DEVICE_ACKED;
        return;
    }
    if (!open_instance_notif(ctx)) {
        fprintf(stderr, "[mk1-trace] failed to open local instance notification port\n");
        ctx->state = TRACE_STATE_DEVICE_ACKED;
        return;
    }
    if (!send_ack(ctx,
                  ctx->inst_request_port,
                  ctx->inst_request_name,
                  ctx->inst_notif_name)) {
        fprintf(stderr, "[mk1-trace] instance ACK_NOTIF_PORT failed\n");
        ctx->state = TRACE_STATE_DEVICE_ACKED;
        return;
    }
    ctx->state = TRACE_STATE_INSTANCE_ACKED;
    fprintf(stderr, "[mk1-trace] instance handshake complete\n");
    maybe_issue_optional_queries(ctx);
}

static void handle_notification(trace_ctx_t *ctx,
                                const char *port_name,
                                SInt32 msgid,
                                CFDataRef data)
{
    uint32_t type = 0;

    if (!ctx || !data || CFDataGetLength(data) == 0) return;
    ctx->saw_any_notification = true;
    trace_log_message(ctx,
                      "NTF",
                      port_name,
                      msgid,
                      CFDataGetBytePtr(data),
                      (size_t)CFDataGetLength(data));

    if (CFDataGetLength(data) >= 4) {
        memcpy(&type, CFDataGetBytePtr(data), sizeof(type));
    }

    if (type == NI_EVT_DEVICE_ON) {
        if (!ctx->serial[0]) {
            if (extract_device_on_serial(data, ctx->serial, sizeof(ctx->serial))) {
                fprintf(stderr, "[mk1-trace] extracted serial from DEVICE_ON: %s\n", ctx->serial);
            } else {
                fprintf(stderr, "[mk1-trace] DEVICE_ON seen but serial was not parsed\n");
            }
        }

        if (ctx->auto_second_stage) {
            if (ctx->serial[0]) {
                begin_instance_handshake(ctx);
            } else {
                fprintf(stderr, "[mk1-trace] auto second stage is enabled but no serial is available yet\n");
            }
        } else {
            fprintf(stderr, "[mk1-trace] DEVICE_ON seen; second stage is disabled\n");
        }
    }
}

static CFDataRef device_notif_callback(CFMessagePortRef local,
                                       SInt32 msgid,
                                       CFDataRef data,
                                       void *info)
{
    (void)local;
    handle_notification((trace_ctx_t *)info, "device-notification", msgid, data);
    return NULL;
}

static CFDataRef instance_notif_callback(CFMessagePortRef local,
                                         SInt32 msgid,
                                         CFDataRef data,
                                         void *info)
{
    (void)local;
    handle_notification((trace_ctx_t *)info, "instance-notification", msgid, data);
    return NULL;
}

static bool perform_device_handshake(trace_ctx_t *ctx)
{
    if (!send_version_probe(ctx)) {
        fprintf(stderr, "[mk1-trace] version probe failed\n");
        return false;
    }
    if (!send_pid_connect(ctx)) {
        fprintf(stderr, "[mk1-trace] PID_CONNECT failed\n");
        return false;
    }
    if (!open_device_notif(ctx)) {
        fprintf(stderr, "[mk1-trace] failed to open local device notification port\n");
        return false;
    }
    if (!send_ack(ctx,
                  ctx->dev_request_port,
                  ctx->dev_request_name,
                  ctx->dev_notif_name)) {
        fprintf(stderr, "[mk1-trace] device ACK_NOTIF_PORT failed\n");
        return false;
    }
    ctx->state = TRACE_STATE_DEVICE_ACKED;
    fprintf(stderr, "[mk1-trace] device notification ACK accepted for '%s'\n", ctx->dev_notif_name);
    return true;
}

static void cleanup(trace_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->dev_notif_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), ctx->dev_notif_rls, kCFRunLoopCommonModes);
        CFRelease(ctx->dev_notif_rls);
    }
    if (ctx->inst_notif_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), ctx->inst_notif_rls, kCFRunLoopCommonModes);
        CFRelease(ctx->inst_notif_rls);
    }
    if (ctx->dev_notif_port) CFRelease(ctx->dev_notif_port);
    if (ctx->inst_notif_port) CFRelease(ctx->inst_notif_port);
    if (ctx->dev_request_port) CFRelease(ctx->dev_request_port);
    if (ctx->inst_request_port) CFRelease(ctx->inst_request_port);
    if (ctx->bootstrap_port) CFRelease(ctx->bootstrap_port);

    close_logs(ctx);
    if (archive_logs(ctx)) {
        maybe_scp_logs(ctx);
    }
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--no-second-stage] [--serial serial] [--query] [--duration seconds] [--output-dir path] [--scp-dest user@host:/path]\n"
            "\n"
            "Connects to the real NIHardwareAgent on Intel macOS, performs the MK1\n"
            "handshake, logs all request/reply/notification traffic, and optionally\n"
            "performs the second SERIAL_CONNECT stage plus a few passive queries.\n"
            "If the device was already on before capture started, pass --serial to\n"
            "force the second stage without waiting for a fresh DEVICE_ON event.\n"
            "On shutdown it archives session.log + messages.bin into a tar.gz file,\n"
            "and can optionally scp the archive and raw logs to another machine.\n",
            argv0);
}

int main(int argc, char **argv)
{
    trace_ctx_t ctx;
    int duration_seconds = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.auto_second_stage = true;
    strlcpy(ctx.output_root, ".", sizeof(ctx.output_root));
    strlcpy(ctx.serial, TRACE_DEFAULT_SERIAL, sizeof(ctx.serial));
    strlcpy(ctx.scp_dest, TRACE_DEFAULT_SCP_DEST, sizeof(ctx.scp_dest));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-second-stage") == 0) {
            ctx.auto_second_stage = false;
        } else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc) {
            strlcpy(ctx.serial, argv[++i], sizeof(ctx.serial));
        } else if (strcmp(argv[i], "--query") == 0) {
            ctx.issue_optional_queries = true;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            strlcpy(ctx.output_root, argv[++i], sizeof(ctx.output_root));
        } else if (strcmp(argv[i], "--scp-dest") == 0 && i + 1 < argc) {
            strlcpy(ctx.scp_dest, argv[++i], sizeof(ctx.scp_dest));
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!open_logs(&ctx)) {
        fprintf(stderr, "[mk1-trace] failed to open logs\n");
        return 1;
    }

    ctx.bootstrap_port = wait_for_remote_port(TRACE_BOOTSTRAP_PORT, 1);
    if (!ctx.bootstrap_port) {
        fprintf(stderr,
                "[mk1-trace] stopped while waiting for bootstrap port '%s'\n",
                TRACE_BOOTSTRAP_PORT);
        cleanup(&ctx);
        return 1;
    }

    if (!perform_device_handshake(&ctx)) {
        cleanup(&ctx);
        return 1;
    }

    fprintf(stderr, "[mk1-trace] device handshake complete\n");
    if (ctx.serial[0]) {
        fprintf(stderr, "[mk1-trace] using preset serial '%s'\n", ctx.serial);
    }
    if (ctx.auto_second_stage && ctx.serial[0]) {
        begin_instance_handshake(&ctx);
    }
    fprintf(stderr, "[mk1-trace] waiting for notifications; press Ctrl+C to stop\n");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                   dispatch_get_main_queue(),
                   ^{
                       if (!g_should_exit && !ctx.saw_any_notification) {
                           fprintf(stderr,
                                   "[mk1-trace] no notifications arrived within 3s of ACK; "
                                   "NIHA may not be delivering to '%s'\n",
                                   ctx.dev_notif_name[0] ? ctx.dev_notif_name : "(unknown)");
                       }
                   });

    if (duration_seconds > 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)duration_seconds * NSEC_PER_SEC),
                       dispatch_get_main_queue(),
                       ^{
                           if (!g_should_exit) {
                               g_should_exit = 1;
                               CFRunLoopStop(CFRunLoopGetMain());
                           }
                       });
    }

    CFRunLoopRun();
    cleanup(&ctx);
    return 0;
}
