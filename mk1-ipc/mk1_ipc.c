#include "mk1_ipc.h"
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// CFMessagePort-based IPC to NIHardwareAgent
//
// References:
//   biappi/Macchina  — MK1, original RE, CFMessagePort confirmed
//   SamL98/NIProtocol — MK2, handshake + message format
//   terminar/rebellion — MK3, most complete implementation
//
// TODO: Study Macchina source code in detail and port handshake here.
//       The protocol may have changed between MK1-era and current NIHA.
//       Capture on Intel Mac will confirm.
// ---------------------------------------------------------------------------

struct mk1_ipc_connection {
    CFMessagePortRef    remote_port;    // NIHA's port (we send to this)
    CFMessagePortRef    local_port;     // Our port (NIHA sends acks here)
    CFRunLoopSourceRef  run_loop_src;

    mk1_ipc_message_cb_t callback;
    void                *cb_context;
};

// ---------------------------------------------------------------------------
// Internal: local port callback (receives messages from NIHA)
// ---------------------------------------------------------------------------

static CFDataRef local_port_callback(CFMessagePortRef local,
                                      SInt32 msgid,
                                      CFDataRef data,
                                      void *info)
{
    mk1_ipc_connection_t *conn = (mk1_ipc_connection_t *)info;
    fprintf(stderr, "[mk1-ipc] received msgid=%d len=%ld\n",
            (int)msgid,
            data ? (long)CFDataGetLength(data) : 0L);

    if (conn->callback && data) {
        conn->callback(data, conn->cb_context);
    }
    return NULL; // no reply
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

mk1_ipc_connection_t *mk1_ipc_connect(mk1_ipc_message_cb_t callback,
                                       void *context)
{
    // Check NIHardwareAgent port exists
    CFStringRef port_name = CFStringCreateWithCString(NULL, NIHA_PORT_NAME,
                                                       kCFStringEncodingUTF8);
    CFMessagePortRef remote = CFMessagePortCreateRemote(NULL, port_name);
    CFRelease(port_name);

    if (!remote) {
        fprintf(stderr, "[mk1-ipc] NIHardwareAgent port '%s' not found. "
                        "Is NIHardwareAgent running?\n", NIHA_PORT_NAME);
        return NULL;
    }

    mk1_ipc_connection_t *conn = calloc(1, sizeof(mk1_ipc_connection_t));
    conn->remote_port = remote;
    conn->callback    = callback;
    conn->cb_context  = context;

    // Create our local port so NIHA can reply / push events to us
    // Port name: "SIHWMainHandler" per SamL98/NIProtocol
    // TODO: confirm this is still correct for MK1-era NIHA
    CFStringRef local_name = CFStringCreateWithCString(NULL, "SIHWMainHandler",
                                                        kCFStringEncodingUTF8);
    CFMessagePortContext ctx = { 0, conn, NULL, NULL, NULL };
    Boolean should_free = false;
    conn->local_port = CFMessagePortCreateLocal(NULL, local_name,
                                                 local_port_callback,
                                                 &ctx, &should_free);
    CFRelease(local_name);

    if (!conn->local_port) {
        fprintf(stderr, "[mk1-ipc] failed to create local port\n");
        CFRelease(conn->remote_port);
        free(conn);
        return NULL;
    }

    conn->run_loop_src = CFMessagePortCreateRunLoopSource(NULL,
                                                           conn->local_port, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), conn->run_loop_src,
                       kCFRunLoopCommonModes);

    fprintf(stderr, "[mk1-ipc] connected to NIHardwareAgent\n");
    return conn;
}

void mk1_ipc_disconnect(mk1_ipc_connection_t *conn)
{
    if (!conn) return;

    if (conn->run_loop_src) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), conn->run_loop_src,
                              kCFRunLoopCommonModes);
        CFRelease(conn->run_loop_src);
    }
    if (conn->local_port)  CFRelease(conn->local_port);
    if (conn->remote_port) CFRelease(conn->remote_port);
    free(conn);
    fprintf(stderr, "[mk1-ipc] disconnected\n");
}

bool mk1_ipc_send(mk1_ipc_connection_t *conn,
                  const uint8_t *data, size_t len)
{
    if (!conn || !data) return false;

    CFDataRef payload = CFDataCreate(NULL, data, (CFIndex)len);
    SInt32 result = CFMessagePortSendRequest(conn->remote_port,
                                              0,        // msgid — TODO: correct value
                                              payload,
                                              1.0,      // send timeout
                                              0.0,      // recv timeout (no reply expected)
                                              NULL, NULL);
    CFRelease(payload);

    if (result != kCFMessagePortSuccess) {
        fprintf(stderr, "[mk1-ipc] send failed: %d\n", (int)result);
        return false;
    }
    return true;
}

bool mk1_ipc_handshake(mk1_ipc_connection_t *conn)
{
    // TODO: implement handshake sequence
    // Study biappi/Macchina and SamL98/NIProtocol for message sequence.
    // Likely involves:
    //   1. Send "hello" / register message with our local port name
    //   2. Wait for NIHA ack
    //   3. Send device descriptor (MK1 VID/PID?)
    //   4. Wait for device focus / ready signal
    fprintf(stderr, "[mk1-ipc] handshake: TODO\n");
    (void)conn;
    return false;
}

bool mk1_ipc_send_pad_event(mk1_ipc_connection_t *conn,
                             const uint8_t *pad_data, size_t len)
{
    // TODO: wrap pad_data in correct IPC message envelope
    fprintf(stderr, "[mk1-ipc] send_pad_event: TODO\n");
    return mk1_ipc_send(conn, pad_data, len);
}

bool mk1_ipc_send_button_event(mk1_ipc_connection_t *conn,
                                const uint8_t *button_data, size_t len)
{
    // TODO: wrap button_data in correct IPC message envelope
    fprintf(stderr, "[mk1-ipc] send_button_event: TODO\n");
    return mk1_ipc_send(conn, button_data, len);
}
