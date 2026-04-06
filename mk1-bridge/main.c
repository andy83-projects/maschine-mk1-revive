// mk1-bridge/main.c
//
// Bridges Maschine software (IPC) to MK1 hardware (USB).
// Impersonates NIHardwareAgent via mk1_server, forwards display/LED
// commands to the USB device, and pushes input events back.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <CoreFoundation/CoreFoundation.h>

#include "mk1_server.h"
#include "../mk1-usb/mk1_device.h"
#include "../mk1-ipc/mk1_ipc.h"

// ---------------------------------------------------------------------------
// Logging
//
// Four log files written to ~/Library/Logs/mk1-bridge/:
//   bridge.log  — startup, USB, IPC, errors
//   led.log     — every LED IPC command and EP1 output
//   display.log — every display IPC command
//   buttons.log — pad/button events from USB
//
// Each category also writes to stderr for live terminal monitoring.
// ---------------------------------------------------------------------------

static FILE *g_log_bridge  = NULL;
static FILE *g_log_led     = NULL;
static FILE *g_log_display = NULL;
static FILE *g_log_buttons = NULL;

#define LOG(fp, fmt, ...) do { \
    fprintf(stderr,  fmt "\n", ##__VA_ARGS__); \
    if (fp) { fprintf(fp, fmt "\n", ##__VA_ARGS__); fflush(fp); } \
} while (0)

#define BLOG(fmt, ...)  LOG(g_log_bridge,  "[bridge]  " fmt, ##__VA_ARGS__)
#define LLOG(fmt, ...)  LOG(g_log_led,     "[led]     " fmt, ##__VA_ARGS__)
#define DLOG(fmt, ...)  LOG(g_log_display, "[display] " fmt, ##__VA_ARGS__)
#define BTNLOG(fmt, ...) LOG(g_log_buttons,"[buttons] " fmt, ##__VA_ARGS__)

static void open_logs(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/Library/Logs/mk1-bridge", home);
    mkdir(dir, 0755);   // no-op if already exists

    char path[600];

#define OPEN_LOG(var, name) \
    snprintf(path, sizeof(path), "%s/" name, dir); \
    var = fopen(path, "a"); \
    if (var) { fprintf(var, "\n--- mk1-bridge started ---\n"); fflush(var); } \
    fprintf(stderr, "[bridge] log: %s\n", path);

    OPEN_LOG(g_log_bridge,  "bridge.log")
    OPEN_LOG(g_log_led,     "led.log")
    OPEN_LOG(g_log_display, "display.log")
    OPEN_LOG(g_log_buttons, "buttons.log")

#undef OPEN_LOG
}

static void close_logs(void)
{
    if (g_log_bridge)  { fclose(g_log_bridge);  g_log_bridge  = NULL; }
    if (g_log_led)     { fclose(g_log_led);     g_log_led     = NULL; }
    if (g_log_display) { fclose(g_log_display); g_log_display = NULL; }
    if (g_log_buttons) { fclose(g_log_buttons); g_log_buttons = NULL; }
}

// ---------------------------------------------------------------------------
// Bridge state
// ---------------------------------------------------------------------------

typedef struct {
    mk1_server_t  *srv;
    mk1_device_t  *usb;
} bridge_t;

static bridge_t g_bridge;
static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; CFRunLoopStop(CFRunLoopGetMain()); }

// ---------------------------------------------------------------------------
// Display: IPC -> USB
//
// IPC DISPLAY message layout (assumption — IPC_DISPLAY_HDR_LEN unverified):
//   [0..3]   cmd_type     (4 bytes)
//   [4..7]   displayIndex (4 bytes, LE): 0=left, 1=right
//   [8..11]  unknown
//   [12..15] pixelDataLen (4 bytes, LE)
//   [16+]    pixelData    (8bpp grayscale, 170x64 = 10880 bytes)
//
// USB EP8 framebuffer format (from usb.pcapng):
//   First byte of payload is 0x5c (ST7529 RAMWR command).
//   Remaining bytes are pixel data as-is (already 8bpp grayscale).
//   Chunked at 508 bytes via mk1_set_display():
//     chunk 0: display_idx=base,      508 bytes = [0x5c, <507 px>]
//     chunk N: display_idx=base|0x01, 508 bytes = [<508 px>]
//     last:    display_idx=base|0x01, <remainder bytes>
// ---------------------------------------------------------------------------

#define IPC_DISPLAY_HDR_LEN   16
#define EP8_CHUNK_MAX         508
#define ST7529_RAMWR          0x5c

static void forward_display(bridge_t *br, const uint8_t *raw_msg, size_t raw_len)
{
    if (!br->usb) {
        DLOG("no USB device");
        return;
    }

    // Hex dump first 32 bytes so we can verify the header layout
    char hex[32 * 3 + 1];
    size_t dump_n = raw_len < 32 ? raw_len : 32;
    for (size_t i = 0; i < dump_n; i++) snprintf(hex + i*3, 4, "%02x ", raw_msg[i]);
    hex[dump_n * 3] = '\0';
    DLOG("raw (%zu): %s", raw_len, hex);

    if (raw_len < IPC_DISPLAY_HDR_LEN + 1) {
        DLOG("too short (%zu)", raw_len);
        return;
    }

    uint8_t ipc_idx = raw_msg[0];
    uint32_t pixel_len;
    memcpy(&pixel_len, raw_msg + 12, 4);

    if (pixel_len == 0 || (size_t)(IPC_DISPLAY_HDR_LEN + pixel_len) > raw_len) {
        DLOG("bad pixel_len=%u raw_len=%zu", pixel_len, raw_len);
        return;
    }

    const uint8_t *pixels = raw_msg + IPC_DISPLAY_HDR_LEN;
    uint8_t usb_base = (uint8_t)(ipc_idx * 2);   // IPC 0=left(0x00), 1=right(0x02)

    DLOG("ipc_idx=%u -> usb_base=0x%02x pixel_len=%u", ipc_idx, usb_base, pixel_len);

    // Build framebuffer: [0x5c, <pixel_data>]
    size_t fb_len = 1 + pixel_len;
    uint8_t *fb = malloc(fb_len);
    if (!fb) return;
    fb[0] = ST7529_RAMWR;
    memcpy(fb + 1, pixels, pixel_len);

    size_t remaining = fb_len;
    size_t offset = 0;
    bool ok = true;

    while (remaining > 0 && ok) {
        size_t chunk = remaining > EP8_CHUNK_MAX ? EP8_CHUNK_MAX : remaining;
        uint8_t disp = (offset == 0) ? usb_base : (uint8_t)(usb_base | 0x01);
        ok = mk1_set_display(br->usb, disp, fb + offset, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    free(fb);

    if (!ok) {
        DLOG("EP8 write failed");
    } else {
        DLOG("sent %zu bytes to display %u OK", fb_len, usb_base);
    }
}

// ---------------------------------------------------------------------------
// LED: IPC -> USB
//
// NI_CMD_LED wire format (confirmed from bridge logs 2026-04-05):
//   bytes[0..3]  = msg_type (NI_CMD_LED = 0x036c7500)
//   bytes[4..7]  = led_len  (LE uint32, observed = 57)
//   bytes[8..39] = logical brightness array indices 0–31  ← remap these
//   bytes[40..]  = extra (other NI device slots, ignored; cap at 32)
//
// EP1 DIMM_LEDS format (confirmed from usb.pcapng):
//   [0x0c, phys[0], phys[1], ..., phys[31]]  — 33 bytes, NO start_index
//
// Remap: hw_by_logical[logical_i] = physical_i
// (ported from mk1_shim_remap_led_payload in mk1-shim/mk1_shim.c)
// ---------------------------------------------------------------------------

static void forward_led(bridge_t *br, const uint8_t *raw_msg, size_t raw_len)
{
    static const uint8_t hw_by_logical[32] = {
         0,
         4,  3,  2,  1,
         8,  7,  6,  5,
        12, 11, 10,  9,
        16, 15, 14, 13,
        17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30,
        31
    };

    if (!br->usb) {
        LLOG("no USB device");
        return;
    }
    if (raw_len < 8 + 1) {
        LLOG("message too short (%zu)", raw_len);
        return;
    }

    uint32_t led_len_hdr;
    memcpy(&led_len_hdr, raw_msg + 4, 4);

    const uint8_t *led_logical = raw_msg + 8;
    size_t led_len = led_len_hdr;
    if (led_len > raw_len - 8) led_len = raw_len - 8;
    if (led_len > 32) led_len = 32;   // hardware expects 32 LED bytes max

    // Log the logical input (all led_len bytes)
    {
        char hex[32 * 3 + 1];
        for (size_t i = 0; i < led_len; i++) snprintf(hex + i*3, 4, "%02x ", led_logical[i]);
        hex[led_len * 3] = '\0';
        LLOG("logical (%zu): %s", led_len, hex);
    }

    // Remap logical → physical hardware indices
    uint8_t remapped[32];
    memcpy(remapped, led_logical, led_len);
    for (size_t i = 0; i < 32 && i < led_len; i++) {
        size_t phys = hw_by_logical[i];
        if (phys < led_len) {
            remapped[phys] = led_logical[i];
        }
    }

    // Build and send DIMM_LEDS (0x0c) packet on EP1
    uint8_t packet[33];
    packet[0] = 0x0c;
    memcpy(packet + 1, remapped, led_len);

    {
        char hex[33 * 3 + 1];
        for (size_t i = 0; i < 1 + led_len; i++) snprintf(hex + i*3, 4, "%02x ", packet[i]);
        hex[(1 + led_len) * 3] = '\0';
        LLOG("EP1 (%zu): %s", 1 + led_len, hex);
    }

    if (!mk1_device_write_endpoint(br->usb, 0x01, packet, 1 + led_len)) {
        LLOG("EP1 write FAILED");
    }
}

// ---------------------------------------------------------------------------
// Server command callback
// ---------------------------------------------------------------------------

static void on_cmd(uint32_t cmd_type, const uint8_t *raw_msg, size_t raw_len, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    switch (cmd_type) {
        case NI_CMD_DISPLAY:
            forward_display(br, raw_msg, raw_len);
            break;
        case NI_CMD_LED:
            forward_led(br, raw_msg, raw_len);
            break;
        default:
            BLOG("unhandled CMD 0x%08x (%zu bytes)", cmd_type, raw_len);
            break;
    }
}

// ---------------------------------------------------------------------------
// Server connect callback
// ---------------------------------------------------------------------------

static void on_connect(void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    BLOG("Maschine software connected");

    if (!br->usb) {
        BLOG("WARNING: no USB device — IPC-only mode");
        return;
    }

    char serial[32] = "MK1000000000000";
    mk1_device_get_serial(br->usb, serial, sizeof(serial));

    if (!mk1_server_send_device_on(br->srv, serial)) {
        BLOG("WARNING: DEVICE_ON send failed");
    }
}

// ---------------------------------------------------------------------------
// USB input callbacks
// ---------------------------------------------------------------------------

static void on_pad(const mk1_pad_event_t *pads, uint8_t count, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->srv || !mk1_server_is_connected(br->srv)) return;

    for (uint8_t i = 0; i < count && i < 16; i++) {
        BTNLOG("pad idx=%u pressure=%u", pads[i].index, pads[i].pressure);
    }

    uint8_t buf[4 + 1 + 16 * 3];
    uint32_t type = NI_EVT_PAD_DATA;
    memcpy(buf, &type, 4);
    buf[4] = count;
    for (uint8_t i = 0; i < count && i < 16; i++) {
        buf[5 + i * 3] = pads[i].index;
        buf[6 + i * 3] = (uint8_t)(pads[i].pressure >> 8);
        buf[7 + i * 3] = (uint8_t)(pads[i].pressure & 0xff);
    }
    mk1_server_send_event(br->srv, buf, 5 + (size_t)count * 3);
}

static void on_button(const mk1_button_event_t *ev, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->srv || !mk1_server_is_connected(br->srv)) return;

    BTNLOG("button event len=%zu", ev->len);

    uint8_t buf[4 + ev->len];
    uint32_t type = NI_EVT_BTN_DATA;
    memcpy(buf, &type, 4);
    memcpy(buf + 4, ev->raw, ev->len);
    mk1_server_send_event(br->srv, buf, 4 + ev->len);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    open_logs();

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    BLOG("mk1-bridge starting");

    g_bridge.usb = mk1_device_open();
    if (g_bridge.usb) {
        BLOG("USB device opened");
        if (!mk1_device_init_hardware(g_bridge.usb)) {
            BLOG("WARNING: hardware init failed");
        }
        if (!mk1_device_start(g_bridge.usb, on_pad, on_button, &g_bridge)) {
            BLOG("WARNING: could not start USB read thread");
        }
    } else {
        BLOG("No USB device — IPC-only mode");
    }

    g_bridge.srv = mk1_server_start(on_connect, on_cmd, &g_bridge);
    if (!g_bridge.srv) {
        BLOG("Failed to start IPC server");
        if (g_bridge.usb) mk1_device_close(g_bridge.usb);
        close_logs();
        return 1;
    }

    BLOG("waiting for Maschine software...");
    CFRunLoopRun();

    if (g_bridge.usb && mk1_server_is_connected(g_bridge.srv)) {
        mk1_server_send_device_off(g_bridge.srv);
    }
    mk1_server_stop(g_bridge.srv);
    if (g_bridge.usb) {
        mk1_device_stop(g_bridge.usb);
        mk1_device_close(g_bridge.usb);
    }

    BLOG("shutdown complete");
    close_logs();
    return 0;
}
