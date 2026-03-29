#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>

// The Mach port name NIHardwareAgent listens on.
// Confirmed by SamL98/NIProtocol and biappi/Macchina.
#define NIHA_PORT_NAME "NIHWMainHandler"

// Opaque IPC connection handle
typedef struct mk1_ipc_connection mk1_ipc_connection_t;

// Callback: called when NIHardwareAgent sends us a message
// (i.e. forwarding hardware events it received — in our case we
//  are the hardware source, so this path may not be used, but
//  we need it for the handshake ack messages)
typedef void (*mk1_ipc_message_cb_t)(CFDataRef data, void *context);

// Connect to NIHardwareAgent and perform handshake
// Returns NULL on failure
mk1_ipc_connection_t *mk1_ipc_connect(mk1_ipc_message_cb_t callback,
                                       void *context);

// Disconnect cleanly
void mk1_ipc_disconnect(mk1_ipc_connection_t *conn);

// Send a raw message to NIHardwareAgent
bool mk1_ipc_send(mk1_ipc_connection_t *conn,
                  const uint8_t *data, size_t len);

// ---------------------------------------------------------------------------
// Higher-level helpers (stubs — fill in once message format is confirmed)
// ---------------------------------------------------------------------------

// Perform the initial handshake sequence
// TODO: fill in from Macchina / NIProtocol source analysis
bool mk1_ipc_handshake(mk1_ipc_connection_t *conn);

// Forward a pad event to NIHardwareAgent as if it came from hardware
bool mk1_ipc_send_pad_event(mk1_ipc_connection_t *conn,
                             const uint8_t *pad_data, size_t len);

// Forward a button event
bool mk1_ipc_send_button_event(mk1_ipc_connection_t *conn,
                                const uint8_t *button_data, size_t len);
