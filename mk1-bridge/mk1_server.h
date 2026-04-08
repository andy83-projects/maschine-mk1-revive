#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>

// ---------------------------------------------------------------------------
// NIHA Impersonation Server
//
// Registers "NIHWMainHandler" bootstrap port and acts as NIHardwareAgent
// for the MK1 device. Maschine software connects to us, we push USB
// events to it as IPC messages, and receive LED/display commands back.
// ---------------------------------------------------------------------------

typedef struct mk1_server mk1_server_t;

// Called on main thread when Maschine software completes handshake
typedef void (*mk1_server_connect_cb_t)(void *context);

// Called when software sends a command (LED, display, etc.)
// raw_msg includes the msg_type header at offset 0.
typedef void (*mk1_server_cmd_cb_t)(uint32_t cmd_type,
                                     const uint8_t *raw_msg,
                                     size_t raw_len,
                                     void *context);

// Start server — registers "NIHWMainHandler" bootstrap port.
// Stock NIHA must NOT be running (it owns the same port name).
mk1_server_t *mk1_server_start(mk1_server_connect_cb_t connect_cb,
                                 mk1_server_cmd_cb_t cmd_cb,
                                 void *context);

// Stop server and release all ports
void mk1_server_stop(mk1_server_t *srv);

// Push device state events to connected Maschine software
bool mk1_server_send_device_on(mk1_server_t *srv, const char *serial);
bool mk1_server_send_device_off(mk1_server_t *srv);

// Push raw event data to software's notification port
bool mk1_server_send_event(mk1_server_t *srv,
                            const uint8_t *data, size_t len);

// Returns true after software has completed PID Connect + ACK
bool mk1_server_is_connected(const mk1_server_t *srv);

// Returns Maschine's instance notification port name after a full instance handshake.
// Used by the reconnect mechanism: a restarted bridge reads this from disk and sends
// DEVICE_OFF to the old port so Maschine re-initiates the NIHWMainHandler handshake.
bool mk1_server_get_maschine_notif_name(const mk1_server_t *srv, char *buf, size_t len);
