// mk1_server.c
//
// NIHA impersonation server. Registers "NIHWMainHandler" and handles
// the MK1-style PID Connect / ACK / event handshake that Maschine
// software expects from NIHardwareAgent.
//
// Protocol flow (confirmed by sniffer capture 2026-04-03):
//   1. Software -> bootstrap: GetServiceVersion
//   2. Software -> bootstrap: PID Connect (device_id=0x0808)
//      <- reply: [true, req_name_len, req_name\0, notif_name_len, notif_name\0]
//   3. Software creates LOCAL notif port, connects to REMOTE request port
//   4. Software -> request port: ACK [type, 0, 0, notif_name_len, notif_name\0]
//      <- reply: [true] (4 bytes only)
//   5. We connect to software's notif port, push DEVICE_ON
//   6. Software -> bootstrap: SERIAL_CONNECT [type, 0x0808, NiM2, prmy, serial_len, serial\0]
//      <- reply: [true, inst_req_name_len, ..., inst_notif_name_len, ...]
//   7. Software -> inst request port: ACK (same format as step 4)
//      <- reply: [true], then we push DEVSTATE_BOOL
//   8. Software -> inst request: GETSERIAL <- reply: [serial_len, padded_serial\0]
//   9. Software -> inst request: START [type, strt]
//  10. Software -> inst request: instance name [type, 0, 0, name_len, name\0]
//      -> we push SETFOCUS after receiving instance name

#include "mk1_server.h"
#include "../mk1-ipc/mk1_ipc.h"   // protocol constants
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Message builder (native LE, same as mk1_ipc.c)
// ---------------------------------------------------------------------------

typedef struct { uint8_t *buf; size_t len, cap; } msg_buf_t;

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
    memcpy(m->buf + m->len, &val, 4);  // native LE
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
// Helper: create space-padded null-terminated serial (17 bytes)
// Matches NIHA format: 16 printable chars (space-padded) + null
// ---------------------------------------------------------------------------

static void pad_serial(char *out, const char *serial)
{
    memset(out, ' ', NI_SERIAL_PADDED_LEN);
    out[NI_SERIAL_PADDED_LEN - 1] = '\0';
    size_t slen = serial ? strlen(serial) : 0;
    if (slen > NI_SERIAL_PADDED_LEN - 1) slen = NI_SERIAL_PADDED_LEN - 1;
    if (slen > 0) memcpy(out, serial, slen);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static const char *message_name(uint32_t msg_type)
{
    switch (msg_type) {
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
    case NI_EVT_KNOB_ROTATE: return "KNOB_ANALOG";
    case NI_EVT_KNOB_4D: return "KNOB_4D";
    case NI_EVT_TOUCHSTRIP: return "TOUCHSTRIP";
    case NI_CMD_LED: return "LED";
    case NI_CMD_DISPLAY: return "DISPLAY";
    case NI_CMD_START: return "START";
    case NI_MSG_INSTANCE_NAME: return "INSTANCE_NAME";
    case NI_MSG_UNKNOWN_IST: return "UNKNOWN_IST";
    case NI_MSG_DISCONNECT: return "DISCONNECT";
    default: return "UNKNOWN";
    }
}

static bool server_verbose_io_enabled(void)
{
    const char *value = getenv("MK1_VERBOSE_IO");
    return value && value[0] && strcmp(value, "0") != 0;
}

static bool server_should_log_request_summary(uint32_t msg_type)
{
    if (server_verbose_io_enabled()) return true;
    switch (msg_type) {
    case NI_MSG_ACK_NOTIF_PORT:
    case NI_MSG_DEVSTATE:
    case NI_MSG_GETSERIAL:
    case NI_CMD_START:
    case NI_CMD_LED:
    case NI_CMD_DISPLAY:
    case NI_MSG_INSTANCE_NAME:
    case NI_MSG_UNKNOWN_IST:
        return false;
    default:
        return true;
    }
}

static void log_state_transition(const char *label, const char *detail)
{
    fprintf(stderr, "[server] state: %s", label ? label : "unknown");
    if (detail && detail[0]) {
        fprintf(stderr, " | %s", detail);
    }
    fprintf(stderr, "\n");
}

static void log_hex(const char *label, const uint8_t *bytes, size_t len)
{
    if (!server_verbose_io_enabled()) return;

    fprintf(stderr, "[server]   %s (%zu bytes):", label, len);
    for (size_t i = 0; i < len && i < 128; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n[server]    ");
        fprintf(stderr, " %02x", bytes[i]);
    }
    if (len > 128) fprintf(stderr, "\n[server]    ... (%zu more)", len - 128);
    fprintf(stderr, "\n");

    // ASCII
    fprintf(stderr, "[server]   ascii: ");
    for (size_t i = 0; i < len && i < 128; i++)
        fprintf(stderr, "%c", (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? bytes[i] : '.');
    fprintf(stderr, "\n");
}

static CFDataRef finish_reply(const char *label, msg_buf_t *m)
{
    CFDataRef reply = buf_to_cfdata(m);
    log_hex(label, CFDataGetBytePtr(reply), (size_t)CFDataGetLength(reply));
    return reply;
}

// ---------------------------------------------------------------------------
// Server state
// ---------------------------------------------------------------------------

struct mk1_server {
    // Bootstrap port — "NIHWMainHandler" (LOCAL, we own)
    CFMessagePortRef    bootstrap_port;
    CFRunLoopSourceRef  bootstrap_rls;

    CFMessagePortRef    dev_request_port;
    CFRunLoopSourceRef  dev_request_rls;
    CFMessagePortRef    dev_notif_remote;
    char                dev_req_name[128];
    char                dev_notif_name[128];
    bool                dev_connected;

    CFMessagePortRef    inst_request_port;
    CFRunLoopSourceRef  inst_request_rls;
    CFMessagePortRef    inst_notif_remote;
    char                inst_req_name[128];
    char                inst_notif_name[128];
    bool                inst_connected;

    char                serial[64];

    // Saved for reconnect: Maschine's instance-level notification port name.
    // Persisted to disk so a restarted bridge can send DEVICE_OFF and trigger
    // Maschine to re-initiate the NIHWMainHandler handshake.
    char                maschine_inst_notif_name[256];

    // Callbacks
    mk1_server_connect_cb_t connect_cb;
    mk1_server_cmd_cb_t     cmd_cb;
    void                   *cb_context;

    // State
    int                 port_seq;
    uint32_t            bootstrap_seq;
    uint32_t            request_seq;
};

static void log_connection_summary(const mk1_server_t *srv)
{
    fprintf(stderr,
            "[server] state: connection-summary | dev_connected=%s inst_connected=%s dev_req='%s' inst_req='%s'\n",
            (srv && srv->dev_connected) ? "true" : "false",
            (srv && srv->inst_connected) ? "true" : "false",
            (srv && srv->dev_req_name[0]) ? srv->dev_req_name : "-",
            (srv && srv->inst_req_name[0]) ? srv->inst_req_name : "-");
}

static bool refresh_remote_port(CFMessagePortRef *slot, const char *name,
                                const char *label)
{
    if (!slot || !name || !name[0]) {
        return false;
    }

    if (*slot) {
        CFRelease(*slot);
        *slot = NULL;
    }

    CFStringRef port_name = CFStringCreateWithCString(NULL, name,
                                                      kCFStringEncodingUTF8);
    if (!port_name) {
        fprintf(stderr, "[server] failed to create %s port name '%s'\n",
                label, name);
        return false;
    }

    CFMessagePortRef remote = CFMessagePortCreateRemote(NULL, port_name);
    CFRelease(port_name);
    if (!remote) {
        fprintf(stderr, "[server] failed to reconnect %s port '%s'\n",
                label, name);
        return false;
    }

    *slot = remote;
    fprintf(stderr, "[server] reconnected %s port '%s'\n", label, name);
    return true;
}

// Forward declarations for callbacks
static CFDataRef bootstrap_callback(CFMessagePortRef local, SInt32 msgid,
                                     CFDataRef data, void *info);
static CFDataRef request_callback(CFMessagePortRef local, SInt32 msgid,
                                   CFDataRef data, void *info);
static bool push_numeric_event(mk1_server_t *srv, uint32_t msg_type, uint32_t value);

// ---------------------------------------------------------------------------
// Bootstrap port callback
//
// Handles: GetServiceVersion, PID Connect, Serial Connect
// ---------------------------------------------------------------------------

static CFDataRef bootstrap_callback(CFMessagePortRef local, SInt32 msgid,
                                     CFDataRef data, void *info)
{
    mk1_server_t *srv = (mk1_server_t *)info;
    (void)local;
    (void)msgid;

    if (!data || CFDataGetLength(data) < 4) {
        fprintf(stderr, "[server] bootstrap ← empty/short message (msgid=%d)\n",
                (int)msgid);
        return NULL;
    }

    const uint8_t *bytes = CFDataGetBytePtr(data);
    CFIndex len = CFDataGetLength(data);
    uint32_t msg_type;
    memcpy(&msg_type, bytes, 4);
    srv->bootstrap_seq++;

    if (server_verbose_io_enabled()) {
        fprintf(stderr, "[server] bootstrap#%u ← %s type=0x%08x len=%ld msgid=%d\n",
                srv->bootstrap_seq, message_name(msg_type), msg_type, (long)len, (int)msgid);
    }
    log_hex("recv", bytes, (size_t)len);

    switch (msg_type) {

    // --- GetServiceVersion ---
    case NI_MSG_VERSION: {
        if (server_verbose_io_enabled()) {
            log_state_transition("reply", "GetServiceVersion");
        }
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, 0x00020802);  // version (matches real NIHA)
        buf_push_u32(&m, 0x00000003);  // build
        return finish_reply("reply", &m);
    }

    // --- PID Connect (device-level) ---
    case NI_MSG_PID_CONNECT: {
        if (len < 8) return NULL;

        uint32_t device_id;
        memcpy(&device_id, bytes + 4, 4);
        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] PID Connect: device_id=0x%04x\n", device_id);
        }

        // Only handle MK1 — ignore other devices
        if (device_id != MK1_DEVICE_ID) {
            if (server_verbose_io_enabled()) {
                fprintf(stderr,
                        "[server] bootstrap#%u not MK1 (0x%04x != 0x%04x), returning empty\n",
                        srv->bootstrap_seq,
                        device_id, MK1_DEVICE_ID);
            }
            return NULL;
        }

        // Clean up previous session
        if (srv->dev_request_rls) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), srv->dev_request_rls,
                                  kCFRunLoopCommonModes);
            CFRelease(srv->dev_request_rls);
            srv->dev_request_rls = NULL;
        }
        if (srv->dev_request_port) {
            CFMessagePortInvalidate(srv->dev_request_port);
            CFRelease(srv->dev_request_port);
            srv->dev_request_port = NULL;
        }
        if (srv->dev_notif_remote) {
            CFRelease(srv->dev_notif_remote);
            srv->dev_notif_remote = NULL;
        }
        srv->dev_connected = false;

        // Generate request and notification port names for this session.
        srv->port_seq++;
        snprintf(srv->dev_req_name, sizeof(srv->dev_req_name),
                 "NIHWS%04x%04dRequest", MK1_DEVICE_ID, srv->port_seq);
        snprintf(srv->dev_notif_name, sizeof(srv->dev_notif_name),
                 "NIHWS%04x%04dNotification", MK1_DEVICE_ID, srv->port_seq);

        // Create LOCAL request port
        CFStringRef req_cf = CFStringCreateWithCString(NULL, srv->dev_req_name,
                                                        kCFStringEncodingUTF8);
        CFMessagePortContext ctx = { 0, srv, NULL, NULL, NULL };
        Boolean should_free = false;
        srv->dev_request_port = CFMessagePortCreateLocal(NULL, req_cf,
                                                         request_callback,
                                                         &ctx, &should_free);
        CFRelease(req_cf);

        if (!srv->dev_request_port) {
            fprintf(stderr, "[server] FAILED to create dev request port '%s'\n",
                    srv->dev_req_name);
            return NULL;
        }

        srv->dev_request_rls = CFMessagePortCreateRunLoopSource(NULL,
                                                                srv->dev_request_port, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), srv->dev_request_rls,
                           kCFRunLoopCommonModes);

        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] dev request port: '%s'\n", srv->dev_req_name);
            fprintf(stderr, "[server] dev notif name:   '%s'\n", srv->dev_notif_name);
        }
        log_state_transition("device-session-created", srv->dev_req_name);

        // Build reply: [true, req_len, req_name, notif_len, notif_name]
        size_t req_name_len = strlen(srv->dev_req_name) + 1;
        size_t notif_name_len = strlen(srv->dev_notif_name) + 1;

        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_TAG_TRUE);
        buf_push_u32(&m, (uint32_t)req_name_len);
        buf_push_bytes(&m, srv->dev_req_name, req_name_len);
        buf_push_u32(&m, (uint32_t)notif_name_len);
        buf_push_bytes(&m, srv->dev_notif_name, notif_name_len);
        CFDataRef reply = finish_reply("reply", &m);
        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] → PID Connect reply (%ld bytes)\n",
                    (long)CFDataGetLength(reply));
        }
        return reply;
    }

    case NI_MSG_SERIAL_CONNECT: {
        if (server_verbose_io_enabled()) {
            log_state_transition("recv", "Serial Connect");
        }
        log_hex("serial-connect", bytes, (size_t)len);

        if (srv->inst_request_rls) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), srv->inst_request_rls,
                                  kCFRunLoopCommonModes);
            CFRelease(srv->inst_request_rls);
            srv->inst_request_rls = NULL;
        }
        if (srv->inst_request_port) {
            CFMessagePortInvalidate(srv->inst_request_port);
            CFRelease(srv->inst_request_port);
            srv->inst_request_port = NULL;
        }
        if (srv->inst_notif_remote) {
            CFRelease(srv->inst_notif_remote);
            srv->inst_notif_remote = NULL;
        }
        srv->inst_connected = false;

        srv->port_seq++;
        snprintf(srv->inst_req_name, sizeof(srv->inst_req_name),
                 "NIHWS%04x%04dRequest", MK1_DEVICE_ID, srv->port_seq);
        snprintf(srv->inst_notif_name, sizeof(srv->inst_notif_name),
                 "NIHWS%04x%04dNotification", MK1_DEVICE_ID, srv->port_seq);

        CFStringRef req_cf = CFStringCreateWithCString(NULL, srv->inst_req_name,
                                                       kCFStringEncodingUTF8);
        CFMessagePortContext ctx = { 0, srv, NULL, NULL, NULL };
        Boolean should_free = false;
        srv->inst_request_port = CFMessagePortCreateLocal(NULL, req_cf,
                                                          request_callback,
                                                          &ctx, &should_free);
        CFRelease(req_cf);

        if (!srv->inst_request_port) {
            fprintf(stderr, "[server] FAILED to create inst request port '%s'\n",
                    srv->inst_req_name);
            return NULL;
        }

        srv->inst_request_rls = CFMessagePortCreateRunLoopSource(NULL,
                                                                 srv->inst_request_port, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), srv->inst_request_rls,
                           kCFRunLoopCommonModes);

        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] inst request port: '%s'\n", srv->inst_req_name);
            fprintf(stderr, "[server] inst notif name:   '%s'\n", srv->inst_notif_name);
        }
        log_state_transition("instance-session-created", srv->inst_req_name);

        size_t req_name_len = strlen(srv->inst_req_name) + 1;
        size_t notif_name_len = strlen(srv->inst_notif_name) + 1;

        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_TAG_TRUE);
        buf_push_u32(&m, (uint32_t)req_name_len);
        buf_push_bytes(&m, srv->inst_req_name, req_name_len);
        buf_push_u32(&m, (uint32_t)notif_name_len);
        buf_push_bytes(&m, srv->inst_notif_name, notif_name_len);
        return finish_reply("reply", &m);
    }

    default:
        fprintf(stderr, "[server] bootstrap: unknown type 0x%08x\n", msg_type);
        log_hex("unknown", bytes, (size_t)len);
        return NULL;
    }
}

// ---------------------------------------------------------------------------
// Request port callback
//
// Handles: ACK notification port, Start command, LED/display commands
// ---------------------------------------------------------------------------

static CFDataRef request_callback(CFMessagePortRef local, SInt32 msgid,
                                   CFDataRef data, void *info)
{
    mk1_server_t *srv = (mk1_server_t *)info;
    (void)local;
    const char *channel = "unknown";

    if (!data || CFDataGetLength(data) < 4) {
        fprintf(stderr, "[server] request ← empty/short message\n");
        return NULL;
    }

    const uint8_t *bytes = CFDataGetBytePtr(data);
    CFIndex len = CFDataGetLength(data);
    uint32_t msg_type;
    memcpy(&msg_type, bytes, 4);

    if (srv->inst_request_port && local == srv->inst_request_port) {
        channel = "instance";
    } else if (srv->dev_request_port && local == srv->dev_request_port) {
        channel = "device";
    }

    srv->request_seq++;

    if (server_should_log_request_summary(msg_type)) {
        fprintf(stderr, "[server] request#%u[%s] ← %s type=0x%08x len=%ld msgid=%d\n",
                srv->request_seq,
                channel,
                message_name(msg_type),
                msg_type, (long)len, (int)msgid);
    }
    log_hex("recv", bytes, (size_t)len);

    switch (msg_type) {

    // --- ACK notification port ---
    case NI_MSG_ACK_NOTIF_PORT: {
        // Format: [msg_type, tag/uid, flags, name_len, name_bytes]
        if (len < 16) {
            fprintf(stderr, "[server] ACK too short (%ld bytes)\n", (long)len);
            break;
        }

        // Extract client's notification port name at offset 12+
        uint32_t name_len;
        memcpy(&name_len, bytes + 12, 4);

        if (name_len == 0 || name_len > 256 || (CFIndex)(16 + name_len) > len) {
            fprintf(stderr, "[server] ACK: bad name_len=%u\n", name_len);
            break;
        }

        char client_notif[256] = {0};
        size_t copy = (name_len < 255) ? name_len : 255;
        memcpy(client_notif, bytes + 16, copy);
        while (copy > 0 && client_notif[copy - 1] == '\0') copy--;
        client_notif[copy] = '\0';

        bool is_instance = (srv->inst_request_port != NULL &&
                            local == srv->inst_request_port);
        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] ACK (%s): client notif port = '%s'\n",
                    is_instance ? "instance" : "device", client_notif);
        }

        // Connect to client's notification port (REMOTE)
        CFStringRef notif_cf = CFStringCreateWithCString(NULL, client_notif,
                                                          kCFStringEncodingUTF8);
        CFMessagePortRef remote = CFMessagePortCreateRemote(NULL, notif_cf);
        CFRelease(notif_cf);

        if (!remote) {
            fprintf(stderr, "[server] FAILED to connect to client notif port '%s'\n",
                    client_notif);
            break;
        }

        if (is_instance) {
            if (srv->inst_notif_remote) CFRelease(srv->inst_notif_remote);
            srv->inst_notif_remote = remote;
            srv->inst_connected = true;
            strlcpy(srv->maschine_inst_notif_name, client_notif,
                    sizeof(srv->maschine_inst_notif_name));
            if (server_verbose_io_enabled()) {
                fprintf(stderr, "[server] === instance handshake complete ===\n");
            }
            push_numeric_event(srv, NI_EVT_DEVSTATE_BOOL, NI_TAG_TRUE);
            // SETFOCUS is sent later, after instance name command (step 10)
        } else {
            if (srv->dev_notif_remote) CFRelease(srv->dev_notif_remote);
            srv->dev_notif_remote = remote;
            srv->dev_connected = true;
            if (server_verbose_io_enabled()) {
                fprintf(stderr, "[server] === device handshake complete ===\n");
            }
        }
        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] event push target (%s): '%s'\n",
                    is_instance ? "instance" : "device", client_notif);
        }
        log_connection_summary(srv);

        if (srv->connect_cb) {
            srv->connect_cb(srv->cb_context);
        }

        // Reply with just "true" (4 bytes) — confirmed by sniffer
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_TAG_TRUE);
        return finish_reply("reply", &m);
    }

    // --- Start command ---
    case NI_CMD_START: {
        if (server_verbose_io_enabled()) {
            log_state_transition("recv", "Start command");
        }
        if (srv->cmd_cb) {
            srv->cmd_cb(msg_type, bytes, (size_t)len, srv->cb_context);
        }
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_CMD_START);
        buf_push_u32(&m, NI_TAG_TRUE);
        return finish_reply("reply", &m);
    }

    case NI_MSG_DEVSTATE: {
        if (server_verbose_io_enabled()) {
            log_state_transition("recv", "Device state query");
        }
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_MSG_DEVSTATE);
        buf_push_u32(&m, NI_TAG_TRUE);
        buf_push_u32(&m, MK1_DEVICE_ID);
        buf_push_u32(&m, 1);
        return finish_reply("reply", &m);
    }

    case NI_MSG_GETSERIAL: {
        // Sniffer-confirmed reply: [serialLen, paddedSerial\0] (21 bytes)
        // No type prefix, no "true" flag
        if (server_verbose_io_enabled()) {
            log_state_transition("recv", "GetSerial query");
        }
        char padded[NI_SERIAL_PADDED_LEN];
        pad_serial(padded, srv->serial[0] ? srv->serial : "MK1-BRIDGE");
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_SERIAL_PADDED_LEN);
        buf_push_bytes(&m, padded, NI_SERIAL_PADDED_LEN);
        return finish_reply("reply", &m);
    }

    // --- Instance name (sent by Maschine after START) ---
    case NI_MSG_INSTANCE_NAME: {
        // Format: [type, 0, 0, name_len, name\0] (28 bytes observed)
        char inst_name[256] = {0};
        if (len >= 16) {
            uint32_t name_len;
            memcpy(&name_len, bytes + 12, 4);
            if (name_len > 0 && name_len < sizeof(inst_name) &&
                (CFIndex)(16 + name_len) <= len) {
                memcpy(inst_name, bytes + 16, name_len);
                inst_name[name_len] = '\0';
            }
        }
        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] instance name: '%s'\n", inst_name);
        }

        if (srv->cmd_cb) {
            srv->cmd_cb(msg_type, bytes, (size_t)len, srv->cb_context);
        }

        // Reply with [type, true]
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, NI_MSG_INSTANCE_NAME);
        buf_push_u32(&m, NI_TAG_TRUE);
        CFDataRef reply = finish_reply("reply", &m);

        // Push SETFOCUS after instance name (sniffer-confirmed ordering)
        push_numeric_event(srv, NI_MSG_SETFOCUS, NI_TAG_TRUE);

        return reply;
    }

    // --- All other commands (LED, display, etc.) ---
    default: {
        if (server_verbose_io_enabled()) {
            fprintf(stderr, "[server] ← %s command 0x%08x (%ld bytes)\n",
                    message_name(msg_type), msg_type, (long)len);
        }

        // Forward full raw message to bridge callback
        if (srv->cmd_cb) {
            srv->cmd_cb(msg_type, bytes, (size_t)len, srv->cb_context);
        }

        // ACK all commands with "true"
        msg_buf_t m;
        buf_init(&m);
        buf_push_u32(&m, msg_type);
        buf_push_u32(&m, NI_TAG_TRUE);
        return finish_reply("reply", &m);
    }
    }

    // Fallthrough for error cases in ACK
    return NULL;
}

// ---------------------------------------------------------------------------
// Internal: push event to connected software
// ---------------------------------------------------------------------------

static bool push_to_port(mk1_server_t *srv, CFMessagePortRef *target_slot,
                         const char *target_name, const char *target_label,
                         CFDataRef payload)
{
    uint32_t msgid = 0;
    const uint8_t *bytes = NULL;
    CFIndex len = 0;
    CFMessagePortRef target = NULL;
    SInt32 result = kCFMessagePortIsInvalid;

    if (!srv || !target_slot) return false;
    if (payload) {
        len = CFDataGetLength(payload);
        bytes = CFDataGetBytePtr(payload);
    }
    if (bytes && len >= 4) {
        memcpy(&msgid, bytes, sizeof(msgid));
        if ((msgid & 0xff000000u) != 0x03000000u && len >= 16) {
            uint32_t nested_msgid = 0;
            memcpy(&nested_msgid, bytes + 12, sizeof(nested_msgid));
            if ((nested_msgid & 0xff000000u) == 0x03000000u) {
                msgid = nested_msgid;
            }
        }
    }

    target = *target_slot;
    if ((!target || !CFMessagePortIsValid(target)) &&
        !refresh_remote_port(target_slot, target_name, target_label)) {
        fprintf(stderr, "[server] %s target is unavailable\n", target_label);
        return false;
    }

    target = *target_slot;
    result = CFMessagePortSendRequest(target,
                                      (SInt32)msgid,
                                      payload,
                                      0.025,    // send timeout — fail fast; drop if queue full
                                      0.0,      // no recv (fire-and-forget)
                                      NULL,
                                      NULL);
    if (result == kCFMessagePortIsInvalid &&
        refresh_remote_port(target_slot, target_name, target_label)) {
        target = *target_slot;
        result = CFMessagePortSendRequest(target,
                                          (SInt32)msgid,
                                          payload,
                                          0.025,
                                          0.0,
                                          NULL,
                                          NULL);
    }

    if (result != kCFMessagePortSuccess) {
        fprintf(stderr,
                "[server] push_event failed (%s msgid=0x%08x error %d)\n",
                target_label,
                msgid,
                (int)result);
        return false;
    }
    if (server_verbose_io_enabled()) {
        fprintf(stderr,
                "[server] push_event ok (%s msgid=0x%08x %ld bytes)\n",
                target_label,
                msgid,
                (long)CFDataGetLength(payload));
    }
    return true;
}

static bool push_event(mk1_server_t *srv, CFDataRef payload)
{
    if (srv->inst_notif_name[0]) {
        if (push_to_port(srv, &srv->inst_notif_remote, srv->inst_notif_name,
                         "instance", payload)) {
            return true;
        }
    }
    if (srv->dev_notif_name[0]) {
        return push_to_port(srv, &srv->dev_notif_remote, srv->dev_notif_name,
                            "device", payload);
    }
    return false;
}

static bool push_numeric_event(mk1_server_t *srv, uint32_t msg_type, uint32_t value)
{
    msg_buf_t m;
    buf_init(&m);
    buf_push_u32(&m, msg_type);
    buf_push_u32(&m, value);

    CFDataRef payload = buf_to_cfdata(&m);
    if (server_verbose_io_enabled()) {
        fprintf(stderr, "[server] → numeric event %s 0x%08x value=0x%08x\n",
                message_name(msg_type), msg_type, value);
    }
    log_hex("send", CFDataGetBytePtr(payload), (size_t)CFDataGetLength(payload));

    bool ok = push_event(srv, payload);
    CFRelease(payload);
    return ok;
}

// ---------------------------------------------------------------------------
// Public: start server
// ---------------------------------------------------------------------------

mk1_server_t *mk1_server_start(mk1_server_connect_cb_t connect_cb,
                                 mk1_server_cmd_cb_t cmd_cb,
                                 void *context)
{
    // Evict any running NIHardwareAgent — it owns the bootstrap port we need.
    // We are the replacement; it must not be running alongside us.
    if (system("pgrep -q NIHardwareAgent") == 0) {
        fprintf(stderr, "[server] NIHardwareAgent is running — killing it\n");
        system("killall NIHardwareAgent 2>/dev/null");
        usleep(300000);   // 300 ms: let the port release from the bootstrap namespace
    }

    mk1_server_t *srv = calloc(1, sizeof(mk1_server_t));
    srv->connect_cb  = connect_cb;
    srv->cmd_cb      = cmd_cb;
    srv->cb_context  = context;

    // Register "NIHWMainHandler" bootstrap port (LOCAL)
    CFStringRef name = CFStringCreateWithCString(NULL, NIHA_BOOTSTRAP_PORT,
                                                  kCFStringEncodingUTF8);
    CFMessagePortContext ctx = { 0, srv, NULL, NULL, NULL };
    Boolean should_free = false;

    srv->bootstrap_port = CFMessagePortCreateLocal(NULL, name,
                                                    bootstrap_callback,
                                                    &ctx, &should_free);
    CFRelease(name);

    if (!srv->bootstrap_port) {
        fprintf(stderr,
            "[server] FAILED to register '%s'\n"
            "[server] Is NIHardwareAgent still running? Kill it first:\n"
            "[server]   sudo kill $(pgrep NIHardwareAgent)\n",
            NIHA_BOOTSTRAP_PORT);
        free(srv);
        return NULL;
    }

    // Schedule on main run loop
    srv->bootstrap_rls = CFMessagePortCreateRunLoopSource(NULL,
                                                           srv->bootstrap_port, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), srv->bootstrap_rls,
                       kCFRunLoopCommonModes);

    fprintf(stderr, "[server] registered bootstrap port '%s'\n", NIHA_BOOTSTRAP_PORT);
    log_state_transition("bootstrap-owned", NIHA_BOOTSTRAP_PORT);
    fprintf(stderr, "[server] waiting for Maschine software...\n");
    return srv;
}

// ---------------------------------------------------------------------------
// Public: stop server
// ---------------------------------------------------------------------------

void mk1_server_stop(mk1_server_t *srv)
{
    if (!srv) return;

    if (srv->inst_request_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), srv->inst_request_rls,
                              kCFRunLoopCommonModes);
        CFRelease(srv->inst_request_rls);
    }
    if (srv->inst_notif_remote) CFRelease(srv->inst_notif_remote);
    if (srv->inst_request_port) {
        CFMessagePortInvalidate(srv->inst_request_port);
        CFRelease(srv->inst_request_port);
    }
    if (srv->dev_request_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), srv->dev_request_rls,
                              kCFRunLoopCommonModes);
        CFRelease(srv->dev_request_rls);
    }
    if (srv->dev_notif_remote) CFRelease(srv->dev_notif_remote);
    if (srv->dev_request_port) {
        CFMessagePortInvalidate(srv->dev_request_port);
        CFRelease(srv->dev_request_port);
    }

    // Bootstrap cleanup
    if (srv->bootstrap_rls) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), srv->bootstrap_rls,
                              kCFRunLoopCommonModes);
        CFRelease(srv->bootstrap_rls);
    }
    if (srv->bootstrap_port) {
        CFMessagePortInvalidate(srv->bootstrap_port);
        CFRelease(srv->bootstrap_port);
    }
    free(srv);
    fprintf(stderr, "[server] stopped\n");
}

// ---------------------------------------------------------------------------
// Public: push DEVICE_ON event
// ---------------------------------------------------------------------------

bool mk1_server_send_device_on(mk1_server_t *srv, const char *serial)
{
    if (!srv || !srv->dev_connected) return false;

    if (serial && serial[0]) {
        strlcpy(srv->serial, serial, sizeof(srv->serial));
    }

    // Sniffer-confirmed DEVICE_ON format (33 bytes):
    //   [type(4), field1(4), deviceID(4), serialLen(4), paddedSerial(17)]
    char padded[NI_SERIAL_PADDED_LEN];
    pad_serial(padded, srv->serial[0] ? srv->serial : "MK1-BRIDGE");

    msg_buf_t m;
    buf_init(&m);
    buf_push_u32(&m, NI_EVT_DEVICE_ON);
    buf_push_u32(&m, NI_DEVICE_ON_FIELD1);   // 0x03774720 (observed constant)
    buf_push_u32(&m, MK1_DEVICE_ID);         // 0x00000808
    buf_push_u32(&m, NI_SERIAL_PADDED_LEN);  // 17
    buf_push_bytes(&m, padded, NI_SERIAL_PADDED_LEN);

    CFDataRef payload = buf_to_cfdata(&m);
    if (server_verbose_io_enabled()) {
        fprintf(stderr, "[server] -> DEVICE_ON serial='%s' (%ld bytes)\n",
                srv->serial, (long)CFDataGetLength(payload));
    }
    log_hex("send", CFDataGetBytePtr(payload), (size_t)CFDataGetLength(payload));

    bool ok = push_event(srv, payload);
    CFRelease(payload);
    return ok;
}

// ---------------------------------------------------------------------------
// Public: push DEVICE_OFF event
// ---------------------------------------------------------------------------

bool mk1_server_send_device_off(mk1_server_t *srv)
{
    if (!srv || !srv->dev_connected) return false;

    msg_buf_t m;
    buf_init(&m);
    buf_push_u32(&m, NI_EVT_DEVICE_OFF);
    buf_push_u32(&m, MK1_DEVICE_ID);

    CFDataRef payload = buf_to_cfdata(&m);
    fprintf(stderr, "[server] → DEVICE_OFF\n");

    bool ok = push_event(srv, payload);
    CFRelease(payload);
    return ok;
}

// ---------------------------------------------------------------------------
// Public: push raw event
// ---------------------------------------------------------------------------

bool mk1_server_send_event(mk1_server_t *srv,
                            const uint8_t *data, size_t len)
{
    if (!srv || !data || len == 0) return false;
    CFDataRef payload = CFDataCreate(NULL, data, (CFIndex)len);
    bool ok = push_event(srv, payload);
    CFRelease(payload);
    return ok;
}

// ---------------------------------------------------------------------------
// Public: connection check
// ---------------------------------------------------------------------------

bool mk1_server_is_connected(const mk1_server_t *srv)
{
    return srv && (srv->dev_connected || srv->inst_connected);
}

bool mk1_server_is_input_ready(const mk1_server_t *srv)
{
    return srv && srv->inst_connected;
}

// ---------------------------------------------------------------------------
// Public: get Maschine's instance notification port name (for reconnect)
// ---------------------------------------------------------------------------

bool mk1_server_get_maschine_notif_name(const mk1_server_t *srv, char *buf, size_t len)
{
    if (!srv || !buf || len == 0 || !srv->maschine_inst_notif_name[0]) return false;
    strlcpy(buf, srv->maschine_inst_notif_name, len);
    return true;
}
