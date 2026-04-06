// mk1_ipc_sniffer.c
//
// Passive CFMessagePort sniffer using DYLD interpose (no flat namespace needed).
// Inject into Maschine 2 (or NIHA) to log all IPC traffic without interference.
//
// Usage:
//   DYLD_INSERT_LIBRARIES=/path/to/libmk1-ipc-sniffer.dylib \
//   /path/to/Maschine\ 2.app/Contents/MacOS/Maschine\ 2
//
// Output: /tmp/mk1-ipc-sniffer.log (override with MK1_IPC_SNIFFER_PATH)

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// DYLD interpose macro — the proper way to hook on macOS.
// Unlike DYLD_FORCE_FLAT_NAMESPACE, this only redirects calls from OTHER
// images into our wrapper.  Calls to the original from WITHIN this dylib
// go straight to the real implementation.
// ---------------------------------------------------------------------------

#define DYLD_INTERPOSE(_wrapper, _original) \
    __attribute__((used)) \
    static struct { const void *replacement; const void *original; } \
    _interpose_##_wrapper \
    __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_wrapper, \
        (const void *)(unsigned long)&_original \
    }

#define SNIFFER_ENV_PATH     "MK1_IPC_SNIFFER_PATH"
#define SNIFFER_DEFAULT_PATH "/tmp/mk1-ipc-sniffer.log"
#define SNIFFER_MAX_SLOTS    32
#define SNIFFER_PREVIEW_BYTES 256

// --- Known NI message type names (first 4 bytes of payload) ---

static const char *ni_msg_name(uint32_t type)
{
    switch (type) {
    case 0x03536756: return "GetServiceVersion";
    case 0x03447500: return "PID_CONNECT";
    case 0x03444900: return "SERIAL_CONNECT";
    case 0x03404300: return "ACK_NOTIF_PORT";
    case 0x03447143: return "DEVSTATE";
    case 0x03436753: return "GETSERIAL";
    case 0x03434e00: return "SETFOCUS";
    case 0x03444e00: return "DEVSTATE_BOOL";
    case 0x03444e2b: return "DEVICE_ON";
    case 0x03444e2d: return "DEVICE_OFF";
    case 0x03504e00: return "PAD_DATA";
    case 0x03734e00: return "BTN_DATA";
    case 0x03654e00: return "KNOB_ROTATE";
    case 0x03774e00: return "KNOB_4D";
    case 0x03744e00: return "TOUCHSTRIP";
    case 0x036c7500: return "LED";
    case 0x03647344: return "DISPLAY";
    case 0x03434300: return "START";
    default:         return NULL;
    }
}

// --- Logging infrastructure ---

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_fp = NULL;

static FILE *sniffer_fp(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_fp) {
        const char *path = getenv(SNIFFER_ENV_PATH);
        if (!path || !path[0]) path = SNIFFER_DEFAULT_PATH;
        g_fp = fopen(path, "a");
        if (!g_fp) g_fp = stderr;
    }
    pthread_mutex_unlock(&g_lock);
    return g_fp;
}

static void sniffer_timestamp(char *out, size_t len)
{
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(out, len, "%H:%M:%S", &tm);
    snprintf(out + strlen(out), len - strlen(out), ".%03ld", ts.tv_nsec / 1000000L);
}

static void sniffer_log(const char *direction,
                        const char *port_name,
                        SInt32 msgid,
                        const uint8_t *bytes,
                        size_t len)
{
    char stamp[32];
    FILE *fp = sniffer_fp();
    uint32_t type = 0;
    const char *type_name = NULL;

    if (!fp) return;

    sniffer_timestamp(stamp, sizeof(stamp));

    if (bytes && len >= 4) {
        memcpy(&type, bytes, sizeof(type));
        type_name = ni_msg_name(type);
    }

    pthread_mutex_lock(&g_lock);

    fprintf(fp, "[%s] %s port=\"%s\" msgid=%d len=%zu",
            stamp, direction,
            port_name ? port_name : "?",
            (int)msgid, len);
    if (type_name) {
        fprintf(fp, " type=%s(0x%08x)", type_name, type);
    } else if (len >= 4) {
        fprintf(fp, " type=0x%08x", type);
    }
    fputc('\n', fp);

    // Hex dump
    if (bytes && len > 0) {
        size_t dump_len = len < SNIFFER_PREVIEW_BYTES ? len : SNIFFER_PREVIEW_BYTES;
        fprintf(fp, "  hex: ");
        for (size_t i = 0; i < dump_len; i++) {
            fprintf(fp, "%02x", bytes[i]);
            if (i + 1 < dump_len) fputc(' ', fp);
        }
        if (dump_len < len) {
            fprintf(fp, " ... +%zu bytes", len - dump_len);
        }
        fputc('\n', fp);

        // Words view (LE uint32s)
        if (len >= 4) {
            fprintf(fp, "  u32: ");
            for (size_t i = 0; i + 3 < dump_len; i += 4) {
                uint32_t w;
                memcpy(&w, bytes + i, 4);
                fprintf(fp, "0x%08x ", w);
            }
            fputc('\n', fp);
        }

        // ASCII view
        fprintf(fp, "  asc: ");
        for (size_t i = 0; i < dump_len; i++) {
            uint8_t c = bytes[i];
            fputc((c >= 0x20 && c < 0x7f) ? (int)c : '.', fp);
        }
        fputc('\n', fp);
    }

    fflush(fp);
    pthread_mutex_unlock(&g_lock);
}

// --- Port name helper ---

static bool get_port_name(CFMessagePortRef port, char *out, size_t out_len)
{
    CFStringRef name;

    if (!port || !out || out_len == 0) return false;
    name = CFMessagePortGetName(port);
    if (!name) {
        snprintf(out, out_len, "port@%p", (void *)port);
        return false;
    }
    CFStringGetCString(name, out, (CFIndex)out_len, kCFStringEncodingUTF8);
    return true;
}

// --- Callback wrapper for incoming notifications ---
//
// CFMessagePortCreateLocal takes a callback function pointer.
// We wrap it so we can log the notification before passing to the real one.

typedef struct {
    CFMessagePortCallBack real_callback;
    void *real_info;
    char port_name[256];
} callback_slot_t;

static callback_slot_t g_slots[SNIFFER_MAX_SLOTS];
static int g_slot_count = 0;

static CFDataRef sniffer_callback_wrapper(CFMessagePortRef local,
                                          SInt32 msgid,
                                          CFDataRef data,
                                          void *info)
{
    callback_slot_t *slot = (callback_slot_t *)info;
    const uint8_t *bytes = NULL;
    size_t len = 0;

    if (data) {
        bytes = CFDataGetBytePtr(data);
        len = (size_t)CFDataGetLength(data);
    }

    sniffer_log("RX-NOTIF", slot->port_name, msgid, bytes, len);

    // Call the real callback
    if (slot->real_callback) {
        return slot->real_callback(local, msgid, data, slot->real_info);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Interpose: CFMessagePortCreateLocal
//
// We wrap the callback to log notifications.  With DYLD_INTERPOSE, calling
// CFMessagePortCreateLocal() from inside this function goes to the REAL
// implementation (dyld only redirects calls from other images).
// ---------------------------------------------------------------------------

CFMessagePortRef wrap_CFMessagePortCreateLocal(CFAllocatorRef allocator,
                                               CFStringRef name,
                                               CFMessagePortCallBack callout,
                                               CFMessagePortContext *context,
                                               Boolean *shouldFreeInfo)
{
    char name_buf[256] = {0};
    callback_slot_t *slot = NULL;
    CFMessagePortContext wrapped_ctx;
    CFMessagePortRef result;

    if (name) {
        CFStringGetCString(name, name_buf, sizeof(name_buf), kCFStringEncodingUTF8);
    }

    // Only wrap NI-related ports
    if (name_buf[0] && (strstr(name_buf, "NIHW") || strstr(name_buf, "NIH"))) {
        pthread_mutex_lock(&g_lock);
        if (g_slot_count < SNIFFER_MAX_SLOTS) {
            slot = &g_slots[g_slot_count++];
            slot->real_callback = callout;
            slot->real_info = context ? context->info : NULL;
            strlcpy(slot->port_name, name_buf, sizeof(slot->port_name));
        }
        pthread_mutex_unlock(&g_lock);
    }

    if (slot) {
        wrapped_ctx.version = 0;
        wrapped_ctx.info = slot;
        wrapped_ctx.retain = NULL;
        wrapped_ctx.release = NULL;
        wrapped_ctx.copyDescription = NULL;

        // This calls the REAL CFMessagePortCreateLocal (dyld interpose magic)
        result = CFMessagePortCreateLocal(allocator, name, sniffer_callback_wrapper,
                                          &wrapped_ctx, shouldFreeInfo);

        FILE *fp = sniffer_fp();
        if (fp) {
            char stamp[32];
            sniffer_timestamp(stamp, sizeof(stamp));
            pthread_mutex_lock(&g_lock);
            fprintf(fp, "[%s] CREATE-LOCAL port=\"%s\" -> wrapped callback\n", stamp, name_buf);
            fflush(fp);
            pthread_mutex_unlock(&g_lock);
        }
    } else {
        result = CFMessagePortCreateLocal(allocator, name, callout, context, shouldFreeInfo);

        if (name_buf[0]) {
            FILE *fp = sniffer_fp();
            if (fp) {
                char stamp[32];
                sniffer_timestamp(stamp, sizeof(stamp));
                pthread_mutex_lock(&g_lock);
                fprintf(fp, "[%s] CREATE-LOCAL port=\"%s\" (not NI, passthrough)\n", stamp, name_buf);
                fflush(fp);
                pthread_mutex_unlock(&g_lock);
            }
        }
    }

    return result;
}

DYLD_INTERPOSE(wrap_CFMessagePortCreateLocal, CFMessagePortCreateLocal);

// ---------------------------------------------------------------------------
// Interpose: CFMessagePortSendRequest
//
// Log every outgoing message and its reply.
// ---------------------------------------------------------------------------

SInt32 wrap_CFMessagePortSendRequest(CFMessagePortRef remote,
                                     SInt32 msgid,
                                     CFDataRef data,
                                     CFTimeInterval sendTimeout,
                                     CFTimeInterval rcvTimeout,
                                     CFStringRef replyMode,
                                     CFDataRef *returnData)
{
    char port_name[256] = {0};
    SInt32 result;

    get_port_name(remote, port_name, sizeof(port_name));

    // Log outgoing
    if (data) {
        sniffer_log("TX", port_name, msgid,
                    CFDataGetBytePtr(data),
                    (size_t)CFDataGetLength(data));
    } else {
        sniffer_log("TX", port_name, msgid, NULL, 0);
    }

    // Call real implementation
    result = CFMessagePortSendRequest(remote, msgid, data,
                                      sendTimeout, rcvTimeout,
                                      replyMode, returnData);

    // Log reply
    if (result == kCFMessagePortSuccess && returnData && *returnData) {
        sniffer_log("RX-REPLY", port_name, msgid,
                    CFDataGetBytePtr(*returnData),
                    (size_t)CFDataGetLength(*returnData));
    } else if (result != kCFMessagePortSuccess) {
        FILE *fp = sniffer_fp();
        if (fp) {
            pthread_mutex_lock(&g_lock);
            fprintf(fp, "  -> SEND FAILED: result=%d\n", (int)result);
            fflush(fp);
            pthread_mutex_unlock(&g_lock);
        }
    }

    return result;
}

DYLD_INTERPOSE(wrap_CFMessagePortSendRequest, CFMessagePortSendRequest);

// ---------------------------------------------------------------------------
// Interpose: CFMessagePortCreateRemote
//
// Just log which remote ports are being opened.
// ---------------------------------------------------------------------------

CFMessagePortRef wrap_CFMessagePortCreateRemote(CFAllocatorRef allocator,
                                                CFStringRef name)
{
    char name_buf[256] = {0};
    CFMessagePortRef result;

    if (name) {
        CFStringGetCString(name, name_buf, sizeof(name_buf), kCFStringEncodingUTF8);
    }

    result = CFMessagePortCreateRemote(allocator, name);

    FILE *fp = sniffer_fp();
    if (fp) {
        char stamp[32];
        sniffer_timestamp(stamp, sizeof(stamp));
        pthread_mutex_lock(&g_lock);
        fprintf(fp, "[%s] CREATE-REMOTE port=\"%s\" -> %s\n",
                stamp, name_buf, result ? "OK" : "FAILED");
        fflush(fp);
        pthread_mutex_unlock(&g_lock);
    }

    return result;
}

DYLD_INTERPOSE(wrap_CFMessagePortCreateRemote, CFMessagePortCreateRemote);

// --- Init / Shutdown ---

__attribute__((constructor))
static void sniffer_init(void)
{
    FILE *fp = sniffer_fp();
    char stamp[32];

    sniffer_timestamp(stamp, sizeof(stamp));

    pthread_mutex_lock(&g_lock);
    fprintf(fp, "\n========================================\n");
    fprintf(fp, "[%s] mk1-ipc-sniffer loaded, pid=%d\n", stamp, getpid());
    fprintf(fp, "========================================\n\n");
    fflush(fp);
    pthread_mutex_unlock(&g_lock);
}

__attribute__((destructor))
static void sniffer_fini(void)
{
    char stamp[32];
    FILE *fp = sniffer_fp();

    sniffer_timestamp(stamp, sizeof(stamp));

    pthread_mutex_lock(&g_lock);
    fprintf(fp, "\n[%s] mk1-ipc-sniffer unloaded\n", stamp);
    fflush(fp);
    if (g_fp && g_fp != stderr) {
        fclose(g_fp);
        g_fp = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
