#include <stdio.h>
#include <signal.h>
#include <CoreFoundation/CoreFoundation.h>

#include "../mk1-usb/mk1_device.h"
#include "../mk1-ipc/mk1_ipc.h"

// ---------------------------------------------------------------------------
// mk1-bridge: main daemon
//
// Responsibilities:
//   1. Claim MK1 USB device before NIHardwareAgent can
//   2. Connect to NIHardwareAgent via CFMessagePort
//   3. Perform IPC handshake
//   4. Forward USB HID events → IPC messages to NIHA
//   5. Forward IPC output commands (LEDs, display) → USB HID writes
// ---------------------------------------------------------------------------

static mk1_device_t         *g_device = NULL;
static mk1_ipc_connection_t *g_ipc    = NULL;

// ---------------------------------------------------------------------------
// USB callbacks — called from mk1-usb read thread
// ---------------------------------------------------------------------------

static void on_pad_event(const mk1_pad_event_t *pads, uint8_t count, void *ctx)
{
    (void)ctx;
    // TODO: translate pad events to IPC message format and forward to NIHA
    fprintf(stderr, "[bridge] pad event: %d pads (forwarding TODO)\n", count);
    (void)pads;
}

static void on_button_event(const mk1_button_event_t *event, void *ctx)
{
    (void)ctx;
    // TODO: translate button event to IPC message format and forward to NIHA
    fprintf(stderr, "[bridge] button event (forwarding TODO)\n");
    (void)event;
}

// ---------------------------------------------------------------------------
// IPC callback — messages arriving from NIHardwareAgent
// (output commands: set LEDs, write display, etc.)
// ---------------------------------------------------------------------------

static void on_ipc_message(CFDataRef data, void *ctx)
{
    (void)ctx;
    // TODO: parse IPC message and dispatch to mk1_set_led / mk1_set_display
    fprintf(stderr, "[bridge] IPC message from NIHA: %ld bytes (dispatch TODO)\n",
            (long)CFDataGetLength(data));
}

// ---------------------------------------------------------------------------
// Signal handling — clean shutdown
// ---------------------------------------------------------------------------

static void on_signal(int sig)
{
    fprintf(stderr, "[bridge] caught signal %d, shutting down\n", sig);
    CFRunLoopStop(CFRunLoopGetMain());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    fprintf(stderr, "[bridge] mk1-bridge starting\n");

    // Step 1: Claim USB device
    // We must do this before NIHardwareAgent starts, or it will grab it first.
    // The launchd plist should ensure we launch early enough.
    g_device = mk1_device_open();
    if (!g_device) {
        fprintf(stderr, "[bridge] failed to open MK1 device — is it plugged in?\n");
        return 1;
    }

    // Step 2: Connect to NIHardwareAgent IPC
    // NIHA must already be running. launchd ordering handles this.
    g_ipc = mk1_ipc_connect(on_ipc_message, NULL);
    if (!g_ipc) {
        fprintf(stderr, "[bridge] failed to connect to NIHardwareAgent\n");
        mk1_device_close(g_device);
        return 1;
    }

    // Step 3: Perform IPC handshake
    if (!mk1_ipc_handshake(g_ipc)) {
        fprintf(stderr, "[bridge] IPC handshake failed (TODO: not yet implemented)\n");
        // Don't exit — keep running so we can log what NIHA sends us
    }

    // Step 4: Start USB read loop
    if (!mk1_device_start(g_device, on_pad_event, on_button_event, NULL)) {
        fprintf(stderr, "[bridge] failed to start USB read thread\n");
        mk1_ipc_disconnect(g_ipc);
        mk1_device_close(g_device);
        return 1;
    }

    // Clean shutdown on Ctrl-C / SIGTERM
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "[bridge] running — waiting for events\n");

    // CFRunLoop drives the IPC receive callbacks
    CFRunLoopRun();

    // Cleanup
    fprintf(stderr, "[bridge] shutting down\n");
    mk1_device_stop(g_device);
    mk1_ipc_disconnect(g_ipc);
    mk1_device_close(g_device);

    return 0;
}
