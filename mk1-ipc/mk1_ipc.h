#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

// ---------------------------------------------------------------------------
// NI IPC Protocol Constants
// ---------------------------------------------------------------------------

// Bootstrap port — NIHardwareAgent listens here
#define NIHA_BOOTSTRAP_PORT   "NIHWMainHandler"

// MK1 device ID (USB PID, also used as protocol device identifier)
#define MK1_DEVICE_ID          0x0808

// Protocol constants (confirmed by terminar/rebellion + biappi/Macchina)
// Modern NIHA (MK2+): msgid=0, message type in payload first 4 bytes
#define NI_MSG_VERSION         0x03536756  // GetServiceVersion
#define NI_MSG_PID_CONNECT     0x03447500  // PID Connect (device-level)
#define NI_MSG_SERIAL_CONNECT  0x03444900  // Serial Connect (instance-level)
#define NI_MSG_ACK_NOTIF_PORT  0x03404300  // Acknowledge notification port
#define NI_MSG_DEVSTATE        0x03447143  // Device state query

// Event message types (received on notification port from NIHA)
#define NI_EVT_DEVICE_ON       0x03444e2b  // Device state: ON (includes serial)
#define NI_EVT_DEVICE_OFF      0x03444e2d  // Device state: OFF
#define NI_EVT_PAD_DATA        0x03504e00  // Pad pressure data
#define NI_EVT_BTN_DATA        0x03734e00  // Button state data
#define NI_EVT_KNOB_ROTATE     0x03654e00  // Knob rotation
#define NI_EVT_KNOB_4D         0x03774e00  // 4D encoder
#define NI_EVT_TOUCHSTRIP      0x03744e00  // Touchstrip data

// Output command types (sent to request port toward NIHA)
#define NI_CMD_LED             0x036c7500  // Set LED state
#define NI_CMD_DISPLAY         0x03647344  // Display draw
#define NI_CMD_START           0x03434300  // Start command

// Tag constants (4-char codes stored as native LE uint32)
#define NI_TAG_NIM2            0x4e694d32  // "NiM2" — Maschine device type
#define NI_TAG_PRMY            0x70726d79  // "prmy" — primary instance
#define NI_TAG_TRUE            0x74727565  // "true" — success flag
#define NI_TAG_STRT            0x73747274  // "strt" — start command

// ---------------------------------------------------------------------------
// Opaque connection handle
// ---------------------------------------------------------------------------

typedef struct mk1_ipc_connection mk1_ipc_connection_t;

// ---------------------------------------------------------------------------
// Callback — called when NIHA sends us a message
// Signature includes msgid so we can distinguish handshake acks from events
// ---------------------------------------------------------------------------

typedef void (*mk1_ipc_message_cb_t)(SInt32 msgid,
                                      CFDataRef data,
                                      void *context);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Connect to NIHardwareAgent bootstrap port — returns NULL if NIHA is not running
mk1_ipc_connection_t *mk1_ipc_connect(mk1_ipc_message_cb_t callback,
                                       void *context);

// Disconnect and free all resources
void mk1_ipc_disconnect(mk1_ipc_connection_t *conn);

// Perform the PID Connect handshake with NIHA (synchronous)
// On success, NIHA returns request + notification port names.
// We create the notification port and acknowledge it.
// Returns true if handshake completed and ports are established.
bool mk1_ipc_handshake(mk1_ipc_connection_t *conn);

// Send raw bytes with a given message ID to NIHA
bool mk1_ipc_send(mk1_ipc_connection_t *conn,
                  SInt32 msgid,
                  const uint8_t *data,
                  size_t len);

// Forward hardware events to NIHA
bool mk1_ipc_send_pad_event(mk1_ipc_connection_t *conn,
                             const uint8_t *data, size_t len);

bool mk1_ipc_send_button_event(mk1_ipc_connection_t *conn,
                                const uint8_t *data, size_t len);

// Returns the device serial number received during handshake
// Only valid after NIHA responds in step 5
// Returns empty string if not yet received
const char *mk1_ipc_get_serial(mk1_ipc_connection_t *conn);
