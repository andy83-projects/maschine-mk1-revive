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
#include <time.h>
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
    snprintf(dir, sizeof(dir), "%s/Documents/GitRepos/maschine-mk1-revive/build/Debug/bridge-logs", home);
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
// IPC DISPLAY message layout (validated from bridge logs / sniffer):
//   [0..3]   cmd_type     (NI_CMD_DISPLAY)
//   [4..7]   flags/index  (low byte = 0 left, 1 right; upper bits observed 0x10000000)
//   [8..11]  frame/page   (observed 0 or 1)
//   [12..15] format/flags (observed 0x00ff0040 or 0x00ff003c)
//   [16..19] pixelDataLen (4 bytes, LE)
//   [20+]    pixelData    (8bpp grayscale)
//
// USB EP8 framebuffer format (from usb.pcapng):
//   First byte of payload is 0x5c (ST7529 RAMWR command).
//   Remaining bytes are pixel data as-is (already 8bpp grayscale).
//   Chunked at 508 bytes via mk1_set_display():
//     chunk 0: display_idx=base,      508 bytes = [0x5c, <507 px>]
//     chunk N: display_idx=base|0x01, 508 bytes = [<508 px>]
//     last:    display_idx=base|0x01, <remainder bytes>
// ---------------------------------------------------------------------------

#define IPC_DISPLAY_HDR_LEN   20
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

    uint8_t ipc_idx = raw_msg[4];
    uint32_t pixel_len;
    memcpy(&pixel_len, raw_msg + 16, 4);

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

    // Remap logical → physical hardware indices, passing values through raw.
    // No normalization or boost: the shim (reference) does the same, and
    // Maschine sends 0x13/0x32/0x3f which the firmware accepts directly.
    // Boosting dim pads to 0x32 previously made all active pads identical
    // to a just-pressed pad, eliminating all visual feedback.
    uint8_t remapped[32] = {0};
    for (size_t i = 0; i < 32 && i < led_len; i++) {
        remapped[hw_by_logical[i]] = led_logical[i];
    }

    // Build and send DIMM_LEDS (0x0c) packet on EP1
    uint8_t packet[33];
    packet[0] = 0x0c;
    memcpy(packet + 1, remapped, sizeof(remapped));

    {
        char hex[33 * 3 + 1];
        for (size_t i = 0; i < sizeof(packet); i++) snprintf(hex + i*3, 4, "%02x ", packet[i]);
        hex[sizeof(packet) * 3] = '\0';
        LLOG("EP1 (%zu): %s", sizeof(packet), hex);
    }

    if (!mk1_device_write_endpoint(br->usb, 0x01, packet, sizeof(packet))) {
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

// NI_EVT_PAD_DATA wire format (from CLAUDE.md):
//   uint32_t msg_type
//   uint32_t timestamp_hi  (upper 32 bits of 64-bit monotonic ns clock)
//   uint32_t timestamp_lo  (lower 32 bits)
//   uint32_t count         (number of PadEventRecord records)
//   PadEventRecord × count:
//     uint32_t pad_index   (0-indexed)
//     uint32_t event_type  (1=hit_on, 3=hit_off, 4=pressure_update)
//     float    value       (normalized 0.0–1.0)
#define PAD_EVT_HIT_ON           1
#define PAD_EVT_PRESSURE_UPDATE  4
#define PAD_EVT_HIT_OFF          3
#define PAD_PRESSURE_MAX         4095.0f

// EP4 64-byte pad report: pairs 0-15 (bytes 0-31) are pressure channels for 16 pads.
// Confirmed from live hardware testing: EP4 pairs are in sequential pad order,
// pair 0 = pad 1 (IPC idx 0), pair 15 = pad 16 (IPC idx 15).
// No remap needed — pair index == IPC pad_index.

static uint16_t g_prev_pressure[16] = {0};

static void send_pad_record(mk1_server_t *srv,
                             uint64_t ts_ns,
                             uint32_t pad_index,
                             uint32_t event_type,
                             float value)
{
    // header(16) + 1 record(12) = 28 bytes
    uint8_t buf[28];
    uint32_t msg_type = NI_EVT_PAD_DATA;
    uint32_t ts_hi    = (uint32_t)(ts_ns >> 32);
    uint32_t ts_lo    = (uint32_t)(ts_ns & 0xffffffffu);
    uint32_t cnt      = 1;

    memcpy(buf +  0, &msg_type,   4);
    memcpy(buf +  4, &ts_hi,      4);
    memcpy(buf +  8, &ts_lo,      4);
    memcpy(buf + 12, &cnt,        4);
    memcpy(buf + 16, &pad_index,  4);
    memcpy(buf + 20, &event_type, 4);
    memcpy(buf + 24, &value,      4);

    mk1_server_send_event(srv, buf, sizeof(buf));
}

static void on_pad(const mk1_pad_event_t *pads, uint8_t count, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->srv || !mk1_server_is_connected(br->srv)) return;

    struct timespec ts;
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
    uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    for (uint8_t i = 0; i < count && i < 16; i++) {
        uint32_t idx      = pads[i].index;    // 0-15; pair index == IPC pad_index
        uint16_t pressure = pads[i].pressure;
        uint16_t prev     = g_prev_pressure[idx];

        if (pressure == prev) continue;

        float value = (pressure > 0) ? (pressure / PAD_PRESSURE_MAX) : 0.0f;

        if (prev == 0 && pressure > 0) {
            // pad struck — send hit_on then pressure_update
            BTNLOG("pad idx=%u hit_on pressure=%u (%.3f)", idx, pressure, (double)value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_ON,          value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE,  value);
        } else if (prev > 0 && pressure == 0) {
            // pad released
            BTNLOG("pad idx=%u hit_off", idx);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_OFF, 0.0f);
        } else {
            // pressure changed while held
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE, value);
        }

        if (idx < 16) g_prev_pressure[idx] = pressure;
    }
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
