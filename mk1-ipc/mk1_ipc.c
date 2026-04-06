// mk1_ipc.c
//
// NIHardwareAgent IPC bridge implementation.
//
// Protocol based primarily on biappi/Macchina's MK1-era reverse engineering.
//
// The implementation here sticks to the simpler controller-level handshake:
//   - msgid is always 0
//   - message type is the first uint32 in the payload
//   - uint32 fields are written in native byte order
//
// Step 1: PID Connect → bootstrap port ("NIHWMainHandler")
//   Send:  [0x03447500, device_id, "NiM2", "prmy", 0x00000000]
//   Reply: ["true", req_port_name_len, req_port_name, notif_port_name_len, notif_port_name]
//
// Step 2: Create local CFMessagePort with notif_port_name from reply
//
// Step 3: Connect to remote req_port_name from reply
//
// Step 4: ACK notification port → request port
//   Send:  [0x03404300, "true", 0, notif_name_len, notif_name_bytes]
//
// After handshake: NIHA pushes hardware events to notification port.
// ---------------------------------------------------------------------------

#include "mk1_ipc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Connection struct (opaque to callers)
// ---------------------------------------------------------------------------

struct mk1_ipc_connection {
    CFMessagePortRef    bootstrap_port;   // "NIHWMainHandler" — initial contact
    CFMessagePortRef    request_port;     // Per-session request port from NIHA
    CFMessagePortRef    notif_port;       // Our local notification port
    CFRunLoopSourceRef  notif_rls;        // Run loop source for notif port

    mk1_ipc_message_cb_t callback;
    void               *cb_context;

    char                req_port_name[128];    // e.g. "NIHWMaschineController0001Request"
    char                notif_port_name[128];  // e.g. "NIHWMaschineController0001Notification"
    char                serial[64];            // Device serial from NIHA
    bool                handshake_done;
};

// ---------------------------------------------------------------------------
// Internal: logging helpers
// ---------------------------------------------------------------------------

static void log_data(const char *label, const uint8_t *bytes, size_t len)
{
    fprintf(stderr, "[mk1-ipc]   %s (%zu bytes):", label, len);
    for (size_t i = 0; i < len && i < 128; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n[mk1-ipc]    ");
        fprintf(stderr, " %02x", bytes[i]);
    }
    if (len > 128) fprintf(stderr, "\n[mk1-ipc]    ... (%zu more)", len - 128);
    fprintf(stderr, "\n");

    // Native (little-endian) uint32 words
    if (len >= 4) {
        fprintf(stderr, "[mk1-ipc]   words:");
        for (size_t i = 0; i + 3 < len && i < 64; i += 4) {
            uint32_t w;
            memcpy(&w, bytes + i, 4);
            fprintf(stderr, " 0x%08x", w);
        }
        fprintf(stderr, "\n");
    }

    // ASCII
    fprintf(stderr, "[mk1-ipc]   ascii: ");
    for (size_t i = 0; i < len && i < 128; i++) {
        fprintf(stderr, "%c", (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? bytes[i] : '.');
    }
    fprintf(stderr, "\n");
}

static void log_cfdata(const char *label, CFDataRef data)
{
    if (!data) { fprintf(stderr, "[mk1-ipc]   %s: (null)\n", label); return; }
    CFIndex len = CFDataGetLength(data);
    if (len == 0) { fprintf(stderr, "[mk1-ipc]   %s: (empty)\n", label); return; }
    log_data(label, CFDataGetBytePtr(data), (size_t)len);
}

// ---------------------------------------------------------------------------
// Internal: message builder
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} msg_buf_t;

static void buf_init(msg_buf_t *m)
{
    m->cap = 64;
    m->len = 0;
    m->buf = calloc(1, m->cap);
}

static void buf_ensure(msg_buf_t *m, size_t need)
{
    while (m->len + need > m->cap) {
        m->cap *= 2;
        m->buf = realloc(m->buf, m->cap);
    }
}

static void buf_push_u32(msg_buf_t *m, uint32_t val)
{
    buf_ensure(m, 4);
    // NI IPC protocol uses native (little-endian) byte order
    // Confirmed by all three RE projects: Macchina, NIProtocol, Rebellion
    memcpy(m->buf + m->len, &val, 4);
    m->len += 4;
}

static void buf_push_bytes(msg_buf_t *m, const void *data, size_t len)
{
    buf_ensure(m, len);
    memcpy(m->buf + m->len, data, len);
    m->len += len;
}

static CFDataRef buf_to_cfdata(msg_buf_t *m)
{
    CFDataRef d = CFDataCreate(NULL, m->buf, (CFIndex)m->len);
    free(m->buf);
    m->buf = NULL;
    return d;
}

// ---------------------------------------------------------------------------
// Internal: send to a port with synchronous reply
// ---------------------------------------------------------------------------

static CFDataRef send_and_recv(CFMessagePortRef port,
                               CFDataRef payload,
                               const char *desc)
{
    CFDataRef reply = NULL;

    fprintf(stderr, "[mk1-ipc] → %s\n", desc);
    if (payload) log_cfdata("send", payload);

    SInt32 result = CFMessagePortSendRequest(port,
                                              0,       // msgid=0, type in payload
                                              payload,
                                              5.0,     // send timeout
                                              5.0,     // recv timeout
                                              kCFRunLoopDefaultMode,
                                              &reply);
    if (result != kCFMessagePortSuccess) {
        fprintf(stderr, "[mk1-ipc] ✗ %s FAILED (CFMessagePort error %d)\n",
                desc, (int)result);
        return NULL;
    }

    fprintf(stderr, "[mk1-ipc] ✓ %s OK\n", desc);
    if (reply && CFDataGetLength(reply) > 0) {
        log_cfdata("recv", reply);
    } else {
        fprintf(stderr, "[mk1-ipc]   (no reply data)\n");
    }
    return reply;
}

// ---------------------------------------------------------------------------
// Internal: parse PID Connect reply
//
// Expected format:
//   bytes 0-3:  "true" flag (0x74727565) or "fail"
//   bytes 4-7:  req_port_name_len (little-endian int32!)
//   bytes 8+:   req_port_name (ASCII, len bytes)
//   next 4:     notif_port_name_len (little-endian int32)
//   next N:     notif_port_name (ASCII, len bytes)
// ---------------------------------------------------------------------------

static bool parse_pid_connect_reply(CFDataRef reply,
                                    char *req_name, size_t req_name_sz,
                                    char *notif_name, size_t notif_name_sz)
{
    if (!reply) return false;
    CFIndex total = CFDataGetLength(reply);
    const uint8_t *p = CFDataGetBytePtr(reply);

    if (total < 12) {
        fprintf(stderr, "[mk1-ipc] reply too short (%ld bytes)\n", (long)total);
        return false;
    }

    // Check for "true" tag (0x74727565) at offset 0
    // Stored as native LE: bytes are 65 75 72 74 ("eurt")
    uint32_t tag;
    memcpy(&tag, p, 4);
    if (tag != NI_TAG_TRUE) {
        fprintf(stderr, "[mk1-ipc] reply does not start with 'true' — got: "
                "0x%08x (%02x %02x %02x %02x)\n",
                tag, p[0], p[1], p[2], p[3]);
        return false;
    }

    // req_port_name_len at offset 4 (native little-endian)
    int32_t req_len;
    memcpy(&req_len, p + 4, 4);

    if (req_len <= 0 || (CFIndex)(8 + req_len) > total) {
        fprintf(stderr, "[mk1-ipc] bad req_port_name_len: %d\n", req_len);
        return false;
    }

    // Copy request port name (strip trailing null if present)
    size_t copy_len = (size_t)req_len;
    if (p[8 + copy_len - 1] == '\0') copy_len--;
    if (copy_len >= req_name_sz) copy_len = req_name_sz - 1;
    memcpy(req_name, p + 8, copy_len);
    req_name[copy_len] = '\0';

    // notif_port_name_len
    CFIndex offset = 8 + req_len;
    if (offset + 4 > total) {
        fprintf(stderr, "[mk1-ipc] reply truncated before notif_port_name_len\n");
        return false;
    }

    int32_t notif_len;
    memcpy(&notif_len, p + offset, 4);

    if (notif_len <= 0 || offset + 4 + notif_len > total) {
        fprintf(stderr, "[mk1-ipc] bad notif_port_name_len: %d\n", notif_len);
        return false;
    }

    copy_len = (size_t)notif_len;
    if (p[offset + 4 + copy_len - 1] == '\0') copy_len--;
    if (copy_len >= notif_name_sz) copy_len = notif_name_sz - 1;
    memcpy(notif_name, p + offset + 4, copy_len);
    notif_name[copy_len] = '\0';

    fprintf(stderr, "[mk1-ipc] parsed port names:\n");
    fprintf(stderr, "[mk1-ipc]   request:      \"%s\"\n", req_name);
    fprintf(stderr, "[mk1-ipc]   notification: \"%s\"\n", notif_name);
    return true;
}

// ---------------------------------------------------------------------------
// Internal: notification port callback
// ---------------------------------------------------------------------------

static CFDataRef notif_port_callback(CFMessagePortRef local,
                                      SInt32 msgid,
                                      CFDataRef data,
                                      void *info)
{
    mk1_ipc_connection_t *conn = (mk1_ipc_connection_t *)info;

    size_t len = data ? (size_t)CFDataGetLength(data) : 0;
    fprintf(stderr, "[mk1-ipc] ← notification msgid=%d len=%zu\n",
            (int)msgid, len);

    if (data && len > 0) {
        log_cfdata("event", data);
    }

    // Forward to user callback
    if (conn->callback) {
        conn->callback(msgid, data, conn->cb_context);
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// Public API — connect (just opens bootstrap port)
// ---------------------------------------------------------------------------

mk1_ipc_connection_t *mk1_ipc_connect(mk1_ipc_message_cb_t callback,
                                       void *context)
{
    CFStringRef name = CFStringCreateWithCString(NULL, NIHA_BOOTSTRAP_PORT,
                                                  kCFStringEncodingUTF8);
    CFMessagePortRef bootstrap = CFMessagePortCreateRemote(NULL, name);
    CFRelease(name);

    if (!bootstrap) {
        fprintf(stderr, "[mk1-ipc] bootstrap port '%s' not found.\n"
                        "[mk1-ipc] Is NIHardwareAgent running?\n",
                NIHA_BOOTSTRAP_PORT);
        return NULL;
    }

    mk1_ipc_connection_t *conn = calloc(1, sizeof(mk1_ipc_connection_t));
    conn->bootstrap_port = bootstrap;
    conn->callback       = callback;
    conn->cb_context     = context;

    fprintf(stderr, "[mk1-ipc] connected to bootstrap port '%s'\n",
            NIHA_BOOTSTRAP_PORT);
    return conn;
}

// ---------------------------------------------------------------------------
// Public API — disconnect
// ---------------------------------------------------------------------------

void mk1_ipc_disconnect(mk1_ipc_connection_t *conn)
{
    if (!conn) return;
    if (conn->notif_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), conn->notif_rls,
                              kCFRunLoopCommonModes);
        CFRelease(conn->notif_rls);
    }
    if (conn->notif_port)     CFRelease(conn->notif_port);
    if (conn->request_port)   CFRelease(conn->request_port);
    if (conn->bootstrap_port) CFRelease(conn->bootstrap_port);
    free(conn);
    fprintf(stderr, "[mk1-ipc] disconnected\n");
}

// ---------------------------------------------------------------------------
// Public API — handshake
//
// MK1-style protocol:
//   1. PID Connect to bootstrap → get port names back synchronously
//   2. Create local notification port
//   3. Connect to request port
//   4. ACK notification port name to request port
// ---------------------------------------------------------------------------

bool mk1_ipc_handshake(mk1_ipc_connection_t *conn)
{
    if (!conn) return false;

    fprintf(stderr, "[mk1-ipc] === beginning handshake ===\n");
    fprintf(stderr, "[mk1-ipc] device_id=0x%04x (%s)\n",
            MK1_DEVICE_ID, "MASCHINE_MK1");

    msg_buf_t m;
    CFDataRef payload;
    CFDataRef reply;

    // ----- Probe: GetServiceVersion -----
    buf_init(&m);
    buf_push_u32(&m, NI_MSG_VERSION);
    payload = buf_to_cfdata(&m);
    reply = send_and_recv(conn->bootstrap_port, payload,
                           "GetServiceVersion (probe)");
    CFRelease(payload);
    if (reply) CFRelease(reply);

    // ----- Step 1: PID Connect -----
    buf_init(&m);
    buf_push_u32(&m, NI_MSG_PID_CONNECT);
    buf_push_u32(&m, MK1_DEVICE_ID);
    buf_push_u32(&m, NI_TAG_NIM2);
    buf_push_u32(&m, NI_TAG_PRMY);
    buf_push_u32(&m, 0);
    payload = buf_to_cfdata(&m);

    reply = send_and_recv(conn->bootstrap_port, payload,
                           "PID Connect (0x0808 MK1)");
    CFRelease(payload);

    if (!reply || CFDataGetLength(reply) == 0) {
        fprintf(stderr, "[mk1-ipc] no reply from PID Connect.\n"
                        "[mk1-ipc] stock NIHA may have dropped MK1 support.\n"
                        "[mk1-ipc] try with patched NIHA:\n"
                        "[mk1-ipc]   kill NIHA: launchctl bootout gui/$(id -u) /Library/LaunchAgents/com.native-instruments.NIHardwareAgent.plist\n"
                        "[mk1-ipc]   start patched: ~/Desktop/NIHardwareAgent-patched.app/Contents/MacOS/NIHardwareAgent &\n");
        if (reply) CFRelease(reply);
        return false;
    }

    // Parse reply: "true" | req_name_len | req_name | notif_name_len | notif_name
    bool parsed = parse_pid_connect_reply(reply,
                                           conn->req_port_name,
                                           sizeof(conn->req_port_name),
                                           conn->notif_port_name,
                                           sizeof(conn->notif_port_name));
    CFRelease(reply);

    if (!parsed) {
        fprintf(stderr, "[mk1-ipc] failed to parse PID Connect reply\n");
        return false;
    }

    // ----- Step 2: Create local notification port -----
    fprintf(stderr, "[mk1-ipc] creating notification port: '%s'\n",
            conn->notif_port_name);

    CFStringRef notif_name_cf = CFStringCreateWithCString(NULL,
                                    conn->notif_port_name,
                                    kCFStringEncodingUTF8);
    CFMessagePortContext ctx = { 0, conn, NULL, NULL, NULL };
    Boolean should_free = false;
    conn->notif_port = CFMessagePortCreateLocal(NULL, notif_name_cf,
                                                 notif_port_callback,
                                                 &ctx, &should_free);
    CFRelease(notif_name_cf);

    if (!conn->notif_port) {
        fprintf(stderr, "[mk1-ipc] failed to create notification port '%s'\n",
                conn->notif_port_name);
        return false;
    }

    // Schedule on run loop
    conn->notif_rls = CFMessagePortCreateRunLoopSource(NULL,
                                                        conn->notif_port, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), conn->notif_rls,
                       kCFRunLoopCommonModes);
    fprintf(stderr, "[mk1-ipc] notification port registered on run loop\n");

    // ----- Step 3: Connect to request port -----
    fprintf(stderr, "[mk1-ipc] connecting to request port: '%s'\n",
            conn->req_port_name);

    CFStringRef req_name_cf = CFStringCreateWithCString(NULL,
                                  conn->req_port_name,
                                  kCFStringEncodingUTF8);
    conn->request_port = CFMessagePortCreateRemote(NULL, req_name_cf);
    CFRelease(req_name_cf);

    if (!conn->request_port) {
        fprintf(stderr, "[mk1-ipc] request port '%s' not found!\n",
                conn->req_port_name);
        return false;
    }
    fprintf(stderr, "[mk1-ipc] request port connected\n");

    // ----- Step 4: ACK notification port -----
    // Send to REQUEST port: [msg_type, 0, 0, name_len, name_bytes]
    // Sniffer confirms: second word is 0, NOT "true" as previously assumed
    size_t name_len = strlen(conn->notif_port_name) + 1; // include null terminator

    buf_init(&m);
    buf_push_u32(&m, NI_MSG_ACK_NOTIF_PORT);
    buf_push_u32(&m, 0);
    buf_push_u32(&m, 0);
    buf_push_u32(&m, (uint32_t)name_len);
    buf_push_bytes(&m, conn->notif_port_name, name_len);
    payload = buf_to_cfdata(&m);

    reply = send_and_recv(conn->request_port, payload,
                           "ACK notification port");
    CFRelease(payload);
    if (!reply || CFDataGetLength(reply) < 4) {
        fprintf(stderr, "[mk1-ipc] ACK notification port failed\n");
        if (reply) CFRelease(reply);
        return false;
    }
    log_cfdata("ack-reply", reply);
    CFRelease(reply);

    // ----- Done -----
    conn->handshake_done = true;
    fprintf(stderr, "[mk1-ipc] === handshake complete ===\n");
    fprintf(stderr, "[mk1-ipc] listening for events on '%s'\n",
            conn->notif_port_name);
    return true;
}

// ---------------------------------------------------------------------------
// Public API — send raw to request port
// ---------------------------------------------------------------------------

bool mk1_ipc_send(mk1_ipc_connection_t *conn,
                  SInt32 msgid,
                  const uint8_t *data, size_t len)
{
    if (!conn || !conn->request_port || !data) return false;
    CFDataRef payload = CFDataCreate(NULL, data, (CFIndex)len);
    SInt32 result = CFMessagePortSendRequest(conn->request_port,
                                              0,       // msgid=0
                                              payload,
                                              1.0, 0.0,
                                              NULL, NULL);
    CFRelease(payload);
    return result == kCFMessagePortSuccess;
}

// ---------------------------------------------------------------------------
// Public API — synchronous query to request port
// ---------------------------------------------------------------------------

CFDataRef mk1_ipc_query(mk1_ipc_connection_t *conn,
                         const uint8_t *data, size_t len)
{
    if (!conn || !conn->request_port || !data) return NULL;
    CFDataRef payload = CFDataCreate(NULL, data, (CFIndex)len);
    CFDataRef reply = NULL;

    SInt32 result = CFMessagePortSendRequest(conn->request_port,
                                              0,
                                              payload,
                                              5.0, 5.0,
                                              kCFRunLoopDefaultMode,
                                              &reply);
    CFRelease(payload);
    if (result != kCFMessagePortSuccess) return NULL;
    return reply;
}

// ---------------------------------------------------------------------------
// Public API — forward hardware events
// ---------------------------------------------------------------------------

bool mk1_ipc_send_pad_event(mk1_ipc_connection_t *conn,
                             const uint8_t *data, size_t len)
{
    // TODO: wrap in correct IPC envelope once message format confirmed
    fprintf(stderr, "[mk1-ipc] send_pad_event: %zu bytes (TODO: envelope)\n", len);
    return mk1_ipc_send(conn, 0, data, len);
}

bool mk1_ipc_send_button_event(mk1_ipc_connection_t *conn,
                                const uint8_t *data, size_t len)
{
    // TODO: wrap in correct IPC envelope
    fprintf(stderr, "[mk1-ipc] send_button_event: %zu bytes (TODO: envelope)\n", len);
    return mk1_ipc_send(conn, 0, data, len);
}

// ---------------------------------------------------------------------------
// Public API — get serial
// ---------------------------------------------------------------------------

const char *mk1_ipc_get_serial(mk1_ipc_connection_t *conn)
{
    if (!conn) return "";
    return conn->serial;
}
