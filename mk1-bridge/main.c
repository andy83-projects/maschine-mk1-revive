// mk1-bridge
//
// Main daemon: ties USB (mk1-usb) + IPC (mk1-ipc) together.
//
// Architecture:
//   Main thread  — CFRunLoop, processes IPC events from NIHA
//   USB thread   — reads HID reports, calls back into main
//
// Data flow:
//   USB → IPC:  pad/button/knob reports forwarded to NIHA request port
//   IPC → USB:  LED/display commands from NIHA forwarded to hardware

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

#include "../mk1-ipc/mk1_ipc.h"
#include "../mk1-usb/mk1_device.h"

// ---------------------------------------------------------------------------
// Bridge state
// ---------------------------------------------------------------------------

typedef struct {
    mk1_ipc_connection_t *ipc;
    mk1_device_t         *usb;
    bool                  running;
} bridge_t;

static bridge_t g_bridge = {0};

// ---------------------------------------------------------------------------
// IPC → USB: handle events from NIHA on notification port
//
// NIHA pushes output commands here (LED state, display pixels) that
// originated from the Maschine software. We forward them to hardware.
// Also receives device state changes and focus events.
// ---------------------------------------------------------------------------

static void on_niha_event(SInt32 msgid, CFDataRef data, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    (void)msgid;  // always 0 in modern protocol

    if (!data) return;
    CFIndex len = CFDataGetLength(data);
    if (len < 4) return;

    const uint8_t *bytes = CFDataGetBytePtr(data);
    uint32_t msg_type;
    memcpy(&msg_type, bytes, 4);

    switch (msg_type) {

    case NI_EVT_DEVICE_ON: {
        fprintf(stderr, "[bridge] ← device ON\n");
        // Payload after header may contain serial number
        // Format: [msg_type, unk, port_uid, serial_len, serial_bytes]
        if (len >= 20) {
            uint32_t serial_len;
            memcpy(&serial_len, bytes + 12, 4);
            if (serial_len > 0 && serial_len < 32 && (CFIndex)(16 + serial_len) <= len) {
                char serial[32] = {0};
                memcpy(serial, bytes + 16, serial_len);
                fprintf(stderr, "[bridge]   serial: \"%s\"\n", serial);
            }
        }
        break;
    }

    case NI_EVT_DEVICE_OFF:
        fprintf(stderr, "[bridge] ← device OFF\n");
        break;

    case NI_CMD_LED:
        // LED update from Maschine software via NIHA
        // Forward to USB hardware
        if (br->usb && len > 4) {
            fprintf(stderr, "[bridge] ← LED update (%ld bytes)\n", (long)len);
            // TODO: parse LED payload and call mk1_set_led
            // For now, log raw data for format analysis
            fprintf(stderr, "[bridge]   hex:");
            for (CFIndex i = 0; i < len && i < 64; i++) {
                if (i % 16 == 0) fprintf(stderr, "\n[bridge]    ");
                fprintf(stderr, " %02x", bytes[i]);
            }
            fprintf(stderr, "\n");
        }
        break;

    case NI_CMD_DISPLAY:
        // Display update from Maschine software via NIHA
        if (br->usb && len > 4) {
            fprintf(stderr, "[bridge] ← display update (%ld bytes)\n", (long)len);
            // TODO: parse display payload and call mk1_set_display
        }
        break;

    default:
        fprintf(stderr, "[bridge] ← event type=0x%08x len=%ld\n",
                msg_type, (long)len);
        // Hex dump unknown events for protocol analysis
        if (len <= 128) {
            fprintf(stderr, "[bridge]   hex:");
            for (CFIndex i = 0; i < len; i++) {
                if (i % 16 == 0) fprintf(stderr, "\n[bridge]    ");
                fprintf(stderr, " %02x", bytes[i]);
            }
            fprintf(stderr, "\n");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// USB → IPC: forward hardware input events to NIHA
// These callbacks are called from the USB read thread.
// ---------------------------------------------------------------------------

static void on_pad_event(const mk1_pad_event_t *pads, uint8_t count, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->ipc) return;

    // TODO: construct proper NI IPC envelope once format confirmed
    // Per rebellion: USB data and IPC data may share same format
    mk1_ipc_send_pad_event(br->ipc, (const uint8_t *)pads,
                            count * sizeof(mk1_pad_event_t));
}

static void on_button_event(const mk1_button_event_t *event, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->ipc) return;

    mk1_ipc_send_button_event(br->ipc, event->raw, sizeof(event->raw));
}

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static void on_signal(int sig)
{
    fprintf(stderr, "\n[bridge] caught signal %d, shutting down\n", sig);
    g_bridge.running = false;
    CFRunLoopStop(CFRunLoopGetMain());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    fprintf(stderr, "[bridge] mk1-bridge starting\n");

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // --- Step 1: Connect to NIHA ---
    fprintf(stderr, "[bridge] connecting to NIHardwareAgent...\n");
    g_bridge.ipc = mk1_ipc_connect(on_niha_event, &g_bridge);
    if (!g_bridge.ipc) {
        fprintf(stderr, "[bridge] NIHA not running — exiting\n");
        return 1;
    }

    // --- Step 2: Handshake ---
    fprintf(stderr, "[bridge] performing handshake...\n");
    if (!mk1_ipc_handshake(g_bridge.ipc)) {
        fprintf(stderr, "[bridge] handshake failed — exiting\n");
        mk1_ipc_disconnect(g_bridge.ipc);
        return 1;
    }
    fprintf(stderr, "[bridge] IPC handshake complete\n");

    // --- Step 3: Open USB device (optional) ---
    fprintf(stderr, "[bridge] looking for MK1 USB device...\n");
    g_bridge.usb = mk1_device_open();
    if (g_bridge.usb) {
        fprintf(stderr, "[bridge] MK1 found — starting USB read loop\n");
        mk1_device_start(g_bridge.usb, on_pad_event, on_button_event,
                          &g_bridge);
    } else {
        fprintf(stderr, "[bridge] MK1 not found — running IPC-only mode\n");
        fprintf(stderr, "[bridge] (plug in MK1 and restart to enable USB)\n");
    }

    // --- Step 4: Run event loop ---
    g_bridge.running = true;
    fprintf(stderr, "[bridge] running (Ctrl-C to stop)\n");
    CFRunLoopRun();

    // --- Cleanup ---
    fprintf(stderr, "[bridge] shutting down\n");
    if (g_bridge.usb) {
        mk1_device_stop(g_bridge.usb);
        mk1_device_close(g_bridge.usb);
    }
    mk1_ipc_disconnect(g_bridge.ipc);
    fprintf(stderr, "[bridge] done\n");
    return 0;
}
