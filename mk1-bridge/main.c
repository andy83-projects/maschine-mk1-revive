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
#include <dispatch/dispatch.h>

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
static FILE *g_log_encoder = NULL;

#define LOG(fp, fmt, ...) do { \
    fprintf(stderr,  fmt "\n", ##__VA_ARGS__); \
    if (fp) { fprintf(fp, fmt "\n", ##__VA_ARGS__); fflush(fp); } \
} while (0)

#define BLOG(fmt, ...)   LOG(g_log_bridge,  "[bridge]  " fmt, ##__VA_ARGS__)
#define LLOG(fmt, ...)   LOG(g_log_led,     "[led]     " fmt, ##__VA_ARGS__)
#define DLOG(fmt, ...)   LOG(g_log_display, "[display] " fmt, ##__VA_ARGS__)
#define BTNLOG(fmt, ...) LOG(g_log_buttons, "[buttons] " fmt, ##__VA_ARGS__)
#define ENCLOG(fmt, ...) LOG(g_log_encoder, "[encoder] " fmt, ##__VA_ARGS__)

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
    OPEN_LOG(g_log_encoder, "encoder.log")

#undef OPEN_LOG

    mk1_device_set_encoder_log(g_log_encoder);
}

static void close_logs(void)
{
    if (g_log_bridge)  { fclose(g_log_bridge);  g_log_bridge  = NULL; }
    if (g_log_led)     { fclose(g_log_led);     g_log_led     = NULL; }
    if (g_log_display) { fclose(g_log_display); g_log_display = NULL; }
    if (g_log_buttons) { fclose(g_log_buttons); g_log_buttons = NULL; }
    if (g_log_encoder) { fclose(g_log_encoder); g_log_encoder = NULL; }
    mk1_device_set_encoder_log(NULL);
}

// ---------------------------------------------------------------------------
// Bridge state
// ---------------------------------------------------------------------------

typedef struct {
    mk1_server_t   *srv;
    mk1_device_t   *usb;
    mk1_hotplug_t  *hotplug;
} bridge_t;

static bridge_t g_bridge;
static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; CFRunLoopStop(CFRunLoopGetMain()); }

// ---------------------------------------------------------------------------
// Session state — for bridge reconnect while Maschine is running
//
// When a full instance handshake completes, we write Maschine's notification
// port name to a file.  On the next bridge startup, if Maschine is still
// running, we connect to that port and send DEVICE_OFF.  This kicks Maschine
// out of its stale "connected" state so it retries the NIHWMainHandler
// handshake — which now hits our freshly-registered bridge.
//
// Maschine's reconnect window is timing-sensitive so we retry DEVICE_OFF up
// to 5 times at 1.5-second intervals.
// ---------------------------------------------------------------------------

#define SESSION_FILE           "/tmp/mk1bridge.session"
#define RECONNECT_POLL_MS      1500   // how often to check if Maschine reconnected
#define RECONNECT_TIMEOUT_SECS 30     // give up after this many seconds

static int g_reconnect_poll_count = 0;

static void save_session_state(const char *notif_name)
{
    FILE *f = fopen(SESSION_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", notif_name);
    fclose(f);
    BLOG("session saved: notif port '%s'", notif_name);
}

static bool load_session_state(char *buf, size_t len)
{
    FILE *f = fopen(SESSION_FILE, "r");
    if (!f) return false;
    bool ok = (fgets(buf, (int)len, f) != NULL);
    fclose(f);
    if (ok) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
    }
    return ok && buf[0] != '\0';
}

// Poll for reconnect. Runs on the main queue every RECONNECT_POLL_MS after
// a single DEVICE_OFF was sent at startup. We do NOT re-send DEVICE_OFF on
// each poll — multiple sends can interfere with Maschine's reconnect sequence.
static void reconnect_poll(void)
{
    g_reconnect_poll_count++;

    if (mk1_server_is_connected(g_bridge.srv)) {
        BLOG("reconnect: Maschine connected (poll %d)", g_reconnect_poll_count);
        return;
    }

    int max_polls = (RECONNECT_TIMEOUT_SECS * 1000) / RECONNECT_POLL_MS;
    if (g_reconnect_poll_count >= max_polls) {
        BLOG("reconnect: timed out after %ds — restart Maschine 2 manually",
             RECONNECT_TIMEOUT_SECS);
        return;
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 (int64_t)(RECONNECT_POLL_MS * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(), ^{ reconnect_poll(); });
}

static void try_maschine_reconnect(void)
{
    g_reconnect_poll_count = 0;

    char notif_name[256] = {0};
    if (!load_session_state(notif_name, sizeof(notif_name))) return;

    // Check if Maschine is running ("Maschine 2" on current NI installs).
    FILE *pg = popen("pgrep -x 'Maschine 2' 2>/dev/null", "r");
    if (!pg) return;
    char pid_line[32] = {0};
    bool running = (fgets(pid_line, sizeof(pid_line), pg) != NULL && pid_line[0] != '\0');
    pclose(pg);

    if (!running) {
        BLOG("reconnect: Maschine not running — stale session file removed");
        unlink(SESSION_FILE);
        return;
    }

    BLOG("reconnect: Maschine running with stale session — sending DEVICE_OFF once to '%s'",
         notif_name);
    unlink(SESSION_FILE);

    CFStringRef port_cf = CFStringCreateWithCString(NULL, notif_name, kCFStringEncodingUTF8);
    CFMessagePortRef port = CFMessagePortCreateRemote(NULL, port_cf);
    CFRelease(port_cf);

    if (!port) {
        BLOG("reconnect: notif port not reachable — Maschine may have already cleaned up");
        return;
    }

    // DEVICE_OFF = [NI_EVT_DEVICE_OFF(4 LE), MK1_DEVICE_ID(4 LE)]
    uint8_t msg[8];
    uint32_t type = NI_EVT_DEVICE_OFF;
    uint32_t dev  = MK1_DEVICE_ID;
    memcpy(msg,     &type, 4);
    memcpy(msg + 4, &dev,  4);

    CFDataRef payload = CFDataCreate(NULL, msg, sizeof(msg));
    SInt32 result = CFMessagePortSendRequest(port, 0, payload, 1.0, 0.0, NULL, NULL);
    CFRelease(payload);
    CFRelease(port);

    if (result == kCFMessagePortSuccess) {
        BLOG("reconnect: DEVICE_OFF sent — polling for reconnect (timeout %ds)",
             RECONNECT_TIMEOUT_SECS);
        // Poll until Maschine reconnects; do NOT re-send DEVICE_OFF
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                     (int64_t)(RECONNECT_POLL_MS * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{ reconnect_poll(); });
    } else {
        BLOG("reconnect: DEVICE_OFF send failed (err=%d)", (int)result);
    }
}

// ---------------------------------------------------------------------------
// Maschine process exit watcher
//
// Polls every 2 s with pgrep. When Maschine exits, shows the status screen.
// Starts after the first successful Maschine connection so we don't run
// pgrep unnecessarily before Maschine has ever been opened.
// ---------------------------------------------------------------------------

static bool g_maschine_process_running = false;
static bool g_maschine_exit_watcher_started = false;
static void show_status_screen(bridge_t *br);  // forward declaration

static void poll_maschine_process(void)
{
    if (!g_running) return;

    FILE *pg = popen("pgrep -x 'Maschine 2' 2>/dev/null", "r");
    bool running = false;
    if (pg) {
        char buf[32] = {0};
        running = (fgets(buf, sizeof(buf), pg) != NULL && buf[0] != '\0');
        pclose(pg);
    }

    if (g_maschine_process_running && !running && g_bridge.usb) {
        BLOG("Maschine exited — showing status screen");
        show_status_screen(&g_bridge);
    }
    g_maschine_process_running = running;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{ poll_maschine_process(); });
}

// ---------------------------------------------------------------------------
// Display: IPC -> USB
//
// IPC DISPLAY message layout (confirmed from Macchina RE + bridge logs):
//   [0..3]   cmd_type   (NI_CMD_DISPLAY = 0x03647344)
//   [4..7]   dispn      (byte[4]: 0=left, 1=right; byte[7]: 0x10 flag)
//   [8..9]   y          (LE uint16, row origin of update region)
//   [10..11] x          (LE uint16, always 0 observed)
//   [12..13] h          (LE uint16, height of update region in rows)
//   [14..15] w          (LE uint16, logical pixel width; always 255 observed)
//   [16..19] pixel_len  (LE uint32, byte count of encoded pixel data)
//   [20+]    pixel_data (ST7529-encoded: 3 logical pixels → 2 bytes, 5 bits/pixel)
//
// Pixel encoding (from Macchina NIImageConversions NI24BPPToST7529Data):
//   Every group of 3 pixels packs into 2 bytes:
//     byte[0] = px0<<3 | px1>>2
//     byte[1] = px1<<6 | px2
//   where each pxN is a 5-bit grayscale value (0x00=black, 0x1F=white).
//   e.g. 3 white pixels → 0xFF 0xDF; full-white row (255px) = 170 bytes.
//   Full frame: 255 × 64 × (2/3) = 10,880 bytes → 170 bytes/row on hardware.
//
// Partial update strategy:
//   Maschine sends partial updates with y≠0 or h<64. RAMWR always resets the
//   write pointer to the start of the ST7529 address window (row 0), so writing
//   partial data directly shifts content up by y rows. Fix: composite each
//   update into a per-display framebuffer at the correct y offset, then always
//   send the full 10,880-byte frame so the write pointer is always at row 0.
//
// USB EP8 framebuffer format (from usb.pcapng):
//   Payload: [0x5c (ST7529 RAMWR), <10880 pixel bytes>], chunked at 508 bytes:
//     chunk 0: display_idx=base,      508 bytes = [0x5c, <507 px>]
//     chunk N: display_idx=base|0x01, 508 bytes = [<508 px>]
//     last:    display_idx=base|0x01, <remainder>
// ---------------------------------------------------------------------------

#define IPC_DISPLAY_HDR_LEN      20
#define EP8_CHUNK_MAX            508
#define ST7529_RAMWR             0x5c
#define DISPLAY_ROWS             64
#define DISPLAY_BYTES_PER_ROW    170    // 255 logical px × (2/3) bytes/px
#define DISPLAY_FB_BYTES         10880  // DISPLAY_ROWS × DISPLAY_BYTES_PER_ROW
#define DISPLAY_COUNT            2

// Per-display framebuffers — composited here, full frame always sent to hardware.
static uint8_t g_display_fb[DISPLAY_COUNT][DISPLAY_FB_BYTES];

// ---------------------------------------------------------------------------
// Status screen — "Open Maschine" shown at startup and when Maschine exits
//
// Font: 5×7 pixels per glyph, scaled 3× for legibility on the 170×64 display.
// Framebuffer: 170 bytes/row in ST7529 format (3 logical px packed into 2 bytes).
// Logical pixel space: 255 wide × 64 tall (maps to 170 physical pixels wide).
// ---------------------------------------------------------------------------

#define STATUS_SCALE   3                              // render each font pixel at 3×3
#define GLYPH_W        5
#define GLYPH_H        7
#define GLYPH_GAP      2                              // pixels between characters
#define CHAR_STEP      (GLYPH_W * STATUS_SCALE + GLYPH_GAP)  // 17 logical px/char

typedef struct { uint8_t r[7]; } mk1_glyph_t;

// 5×7 bitmap font — only characters needed for status messages.
// Each byte = one row, bit[4]=leftmost pixel, bit[0]=rightmost pixel.
static const mk1_glyph_t g_font[128] = {
    [' '] = {{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    ['M'] = {{0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
    ['O'] = {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    ['a'] = {{0x00,0x0E,0x01,0x0F,0x11,0x11,0x0E}},
    ['c'] = {{0x00,0x0E,0x11,0x10,0x10,0x11,0x0E}},
    ['e'] = {{0x00,0x0E,0x11,0x1F,0x10,0x0F,0x00}},
    ['h'] = {{0x10,0x10,0x16,0x19,0x11,0x11,0x11}},
    ['i'] = {{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}},
    ['n'] = {{0x00,0x1C,0x12,0x11,0x11,0x11,0x00}},
    ['p'] = {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x00}},
    ['s'] = {{0x00,0x0E,0x10,0x0E,0x01,0x11,0x0E}},
};

// Render up to two lines of text into the 10880-byte ST7529 framebuffer.
static void render_status_fb(uint8_t *fb, const char *line1, const char *line2)
{
    // Work in 255×64 logical pixel space (one byte per pixel, 5-bit value 0x00/0x1F).
    static uint8_t bmp[64][255];
    memset(bmp, 0, sizeof(bmp));

    const char *lines[2] = { line1, line2 };
    int line_h  = GLYPH_H * STATUS_SCALE;   // 21
    int n_lines = (line2 && *line2) ? 2 : 1;
    int gap_y   = 3;
    int total_h = n_lines * line_h + (n_lines > 1 ? gap_y : 0);
    int start_y = (64 - total_h) / 2;

    for (int li = 0; li < n_lines; li++) {
        if (!lines[li] || !*lines[li]) continue;
        int slen   = (int)strlen(lines[li]);
        int text_w = slen * CHAR_STEP - GLYPH_GAP;
        int cx     = (255 - text_w) / 2;
        int cy     = start_y + li * (line_h + gap_y);

        for (int ci = 0; ci < slen; ci++) {
            unsigned char ch = (unsigned char)lines[li][ci];
            const mk1_glyph_t *g = (ch < 128) ? &g_font[ch] : &g_font[' '];
            int gx = cx + ci * CHAR_STEP;

            for (int row = 0; row < GLYPH_H; row++) {
                for (int col = 0; col < GLYPH_W; col++) {
                    uint8_t on = (g->r[row] >> (4 - col)) & 1;
                    uint8_t v  = on ? 0x1F : 0x00;
                    for (int sy = 0; sy < STATUS_SCALE; sy++) {
                        int py = cy + row * STATUS_SCALE + sy;
                        if (py < 0 || py >= 64) continue;
                        for (int sx = 0; sx < STATUS_SCALE; sx++) {
                            int px = gx + col * STATUS_SCALE + sx;
                            if (px < 0 || px >= 255) continue;
                            bmp[py][px] = v;
                        }
                    }
                }
            }
        }
    }

    // Pack 255-wide 5-bit bitmap → 170-byte ST7529 rows (3 px → 2 bytes).
    for (int y = 0; y < 64; y++) {
        uint8_t *row = fb + y * DISPLAY_BYTES_PER_ROW;
        for (int gr = 0; gr < 85; gr++) {
            uint8_t a = bmp[y][gr * 3];
            uint8_t b = bmp[y][gr * 3 + 1];
            uint8_t c = bmp[y][gr * 3 + 2];
            row[gr * 2]     = (a << 3) | (b >> 2);
            row[gr * 2 + 1] = (uint8_t)((b << 6) | c);
        }
    }
}

// Send a pre-rendered framebuffer to one display (EP8).
static void send_fb_to_display(bridge_t *br, uint8_t usb_base, const uint8_t *fb)
{
    static const uint8_t row_cmd[3] = { 0x75, 0x00, 0x3f };
    static const uint8_t col_cmd[3] = { 0x15, 0x00, 0x54 };
    mk1_set_display(br->usb, usb_base, row_cmd, sizeof(row_cmd));
    mk1_set_display(br->usb, usb_base, col_cmd, sizeof(col_cmd));

    uint8_t *frame = malloc(1 + DISPLAY_FB_BYTES);
    if (!frame) return;
    frame[0] = ST7529_RAMWR;
    memcpy(frame + 1, fb, DISPLAY_FB_BYTES);

    size_t total = 1 + DISPLAY_FB_BYTES, offset = 0;
    while (offset < total) {
        size_t chunk = (total - offset) > EP8_CHUNK_MAX
                     ? EP8_CHUNK_MAX : (total - offset);
        uint8_t disp = (offset == 0) ? usb_base : (uint8_t)(usb_base | 0x01);
        mk1_set_display(br->usb, disp, frame + offset, chunk);
        offset += chunk;
    }
    free(frame);
}

// Render and send "Open Maschine" to both displays.
static void show_status_screen(bridge_t *br)
{
    if (!br->usb) return;
    uint8_t fb[DISPLAY_FB_BYTES];
    render_status_fb(fb, "Open", "Maschine");
    send_fb_to_display(br, 0x00, fb);
    send_fb_to_display(br, 0x02, fb);
    DLOG("status screen shown");
}

static void forward_display(bridge_t *br, const uint8_t *raw_msg, size_t raw_len)
{
    if (!br->usb) {
        DLOG("no USB device");
        return;
    }

    if (raw_len < IPC_DISPLAY_HDR_LEN + 1) {
        DLOG("too short (%zu)", raw_len);
        return;
    }

    uint8_t  disp_idx = raw_msg[4] & 0x01;   // 0=left, 1=right
    uint16_t y, x, h, w;
    uint32_t pixel_len;
    memcpy(&y,         raw_msg + 8,  2);
    memcpy(&x,         raw_msg + 10, 2);
    memcpy(&h,         raw_msg + 12, 2);
    memcpy(&w,         raw_msg + 14, 2);
    memcpy(&pixel_len, raw_msg + 16, 4);

    // x/w are in logical pixels; NI encoding packs 3px → 2 bytes.
    // x must be divisible by 3. byte_x = (x/3)*2.
    size_t byte_x = ((size_t)x / 3) * 2;

    if (h == 0 || (uint32_t)y + h > DISPLAY_ROWS) {
        DLOG("disp=%u bad y=%u h=%u", disp_idx, y, h);
        return;
    }
    if (pixel_len == 0 || (size_t)(IPC_DISPLAY_HDR_LEN + pixel_len) > raw_len) {
        DLOG("disp=%u bad pixel_len=%u raw_len=%zu", disp_idx, pixel_len, raw_len);
        return;
    }

    size_t bytes_per_row = pixel_len / h;
    if (bytes_per_row == 0 || byte_x + bytes_per_row > DISPLAY_BYTES_PER_ROW) {
        DLOG("disp=%u unexpected bytes_per_row=%zu byte_x=%zu (pixel_len=%u h=%u w=%u)",
             disp_idx, bytes_per_row, byte_x, pixel_len, h, w);
        return;
    }

    DLOG("disp=%u y=%u x=%u h=%u w=%u bytes_per_row=%zu pixel_len=%u",
         disp_idx, y, x, h, w, bytes_per_row, pixel_len);

    const uint8_t *pixels = raw_msg + IPC_DISPLAY_HDR_LEN;

    // Skip all-zero (pure black) updates — Maschine sends these as view-transition clears
    // before new content arrives. Holding the previous frame prevents a brief dark flash.
    bool all_black = true;
    for (size_t i = 0; i < (size_t)pixel_len && all_black; i++) {
        if (pixels[i] != 0x00) all_black = false;
    }
    if (all_black) {
        DLOG("disp=%u skipping all-black update (pixel_len=%u)", disp_idx, pixel_len);
        return;
    }

    // Composite update region into the local framebuffer at the correct (y, x) position.
    for (uint16_t r = 0; r < h; r++) {
        memcpy(g_display_fb[disp_idx] + (y + r) * DISPLAY_BYTES_PER_ROW + byte_x,
               pixels + r * bytes_per_row,
               bytes_per_row);
    }

    // Reset address window before RAMWR. No 0x30 (enter extension) needed —
    // the display stays in extension mode between frames (init ends in extension mode,
    // no 0x31 is ever sent). pcap confirms: steady-state frames send 0x75+0x15+RAMWR only.
    uint8_t usb_base = disp_idx == 0 ? 0x00 : 0x02;
    static const uint8_t row_cmd[3] = { 0x75, 0x00, 0x3f };   // rows 0–63
    static const uint8_t col_cmd[3] = { 0x15, 0x00, 0x54 };   // cols 0–84
    mk1_set_display(br->usb, usb_base, row_cmd, sizeof(row_cmd));
    mk1_set_display(br->usb, usb_base, col_cmd, sizeof(col_cmd));

    uint8_t *frame = malloc(1 + DISPLAY_FB_BYTES);
    if (!frame) return;
    frame[0] = ST7529_RAMWR;
    memcpy(frame + 1, g_display_fb[disp_idx], DISPLAY_FB_BYTES);

    size_t total = 1 + DISPLAY_FB_BYTES;
    size_t offset = 0;
    bool ok = true;

    while (offset < total && ok) {
        size_t chunk = (total - offset) > EP8_CHUNK_MAX
                     ? EP8_CHUNK_MAX : (total - offset);
        uint8_t disp = (offset == 0) ? usb_base : (uint8_t)(usb_base | 0x01);
        ok = mk1_set_display(br->usb, disp, frame + offset, chunk);
        offset += chunk;
    }

    free(frame);

    if (!ok) {
        DLOG("EP8 write failed");
    } else {
        DLOG("sent full frame (%d bytes) to display %u OK", 1 + DISPLAY_FB_BYTES, usb_base);
    }
}

// ---------------------------------------------------------------------------
// LED: IPC -> USB
//
// NI_CMD_LED wire format (confirmed from bridge logs 2026-04-06):
//   bytes[0..3]  = msg_type (NI_CMD_LED = 0x036c7500)
//   bytes[4..7]  = led_len  (LE uint32, observed = 57)
//   bytes[8..39] = logical[0..31]  — button/group/transport LEDs → remap via hw_by_logical
//   bytes[44..59]= logical[36..51] — pad rubber LEDs for pads 1–16 (logical[N+36] = pad N+1)
//                  (bytes[40..43] = logical[32..35], not used)
//
// EP1 DIMM_LEDS format (confirmed from usb.pcapng):
//   [0x0c, phys[0], ..., phys[32]]  — 34 bytes total (extended for pad rubber LEDs)
//   phys[0..16]  = button/group/transport LEDs (remapped from logical[0..31])
//   phys[17..32] = pad rubber LEDs (logical[37+K] → phys[17+K], K=0..15)
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

    // Log the FULL raw IPC message to see all bytes (led_len may be > 32)
    if (raw_len > 8) {
        size_t full_len = raw_len - 8;
        char hex[128 * 3 + 1];
        size_t log_len = full_len < 128 ? full_len : 128;
        for (size_t i = 0; i < log_len; i++) snprintf(hex + i*3, 4, "%02x ", raw_msg[8 + i]);
        hex[log_len * 3] = '\0';
        LLOG("full IPC payload (hdr_len=%u raw=%zu): %s", led_len_hdr, full_len, hex);
    }

    const uint8_t *led_logical = raw_msg + 8;
    size_t full_len = raw_len - 8;

    // Remap logical[0..31] → physical button/group/transport positions.
    // No normalization or boost: the shim (reference) does the same, and
    // Maschine sends 0x13/0x32/0x3f which the firmware accepts directly.
    // phys[33] covers pad 16 rubber LED at phys[32].
    uint8_t remapped[33] = {0};
    size_t btn_len = full_len < 32 ? full_len : 32;
    for (size_t i = 0; i < 32 && i < btn_len; i++) {
        remapped[hw_by_logical[i]] = led_logical[i];
    }
    // Original NIHA always sends 0x1e at phys[0] in full-state packets (pcap confirmed).
    remapped[0] = 0x1e;

    // Inject pad rubber LED data: logical[37+K] → phys[17+K] for K=0..15.
    // Confirmed from led.log: logical[N+36] flashes to velocity on pad N press.
    for (size_t k = 0; k < 16; k++) {
        size_t src = 37 + k;
        size_t dst = 17 + k;   // phys[17..32]
        if (src < full_len) {
            remapped[dst] = led_logical[src];
        }
    }

    // Build and send extended DIMM_LEDS (0x0c) packet on EP1:
    // 34 bytes = command byte + phys[0..32] (33 positions, covers pad 16 rubber at phys[32])
    uint8_t packet[34];
    packet[0] = 0x0c;
    memcpy(packet + 1, remapped, sizeof(remapped));

    {
        char hex[34 * 3 + 1];
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

    // Persist Maschine's notification port name so a future bridge restart can
    // send DEVICE_OFF through it to trigger Maschine to reconnect.
    // This fires for both device and instance phases; the name is only available
    // after the instance phase, so the first call is a no-op.
    char notif_name[256] = {0};
    if (mk1_server_get_maschine_notif_name(br->srv, notif_name, sizeof(notif_name))) {
        save_session_state(notif_name);
    }

    // Start polling for Maschine process exit the first time it connects.
    if (!g_maschine_exit_watcher_started) {
        g_maschine_exit_watcher_started = true;
        g_maschine_process_running = true;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{ poll_maschine_process(); });
    }

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

// Minimum pressure change (out of 4095) required to forward a pressure_update event.
// Filters ADC jitter at 700Hz without suppressing real aftertouch changes.
// ~5% of full range — coarse enough to avoid flooding, fine enough to feel responsive.
#define PAD_PRESSURE_THRESHOLD  200

static uint16_t g_prev_pressure[16]      = {0};  // for hit_on/hit_off transitions
static uint16_t g_sent_pressure[16]      = {0};  // last pressure value forwarded via IPC

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
            // pad struck — send hit_on then initial pressure_update
            BTNLOG("pad idx=%u hit_on pressure=%u (%.3f)", idx, pressure, (double)value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_ON,         value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE, value);
            g_sent_pressure[idx] = pressure;
        } else if (prev > 0 && pressure == 0) {
            // pad released
            BTNLOG("pad idx=%u hit_off", idx);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_OFF, 0.0f);
            g_sent_pressure[idx] = 0;
        } else {
            // pad held — only forward if pressure changed enough to be meaningful
            uint16_t sent = g_sent_pressure[idx];
            uint16_t delta = pressure > sent ? pressure - sent : sent - pressure;
            if (delta >= PAD_PRESSURE_THRESHOLD) {
                send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE, value);
                g_sent_pressure[idx] = pressure;
            }
        }

        if (idx < 16) g_prev_pressure[idx] = pressure;
    }
}

static void on_button(const mk1_button_event_t *ev, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->srv || !mk1_server_is_connected(br->srv)) return;

    uint32_t msg_type = 0;
    if (ev->len >= 4) memcpy(&msg_type, ev->raw, 4);

    if (msg_type == NI_EVT_KNOB_ROTATE && ev->len >= 24) {
        uint32_t encoder_index = 0;
        float    delta         = 0.0f;
        memcpy(&encoder_index, ev->raw + 16, 4);
        memcpy(&delta,         ev->raw + 20, 4);
        ENCLOG("idx=%u delta=%.4f", encoder_index, delta);
    } else {
        BTNLOG("button event len=%zu type=0x%08x", ev->len, msg_type);
    }

    mk1_server_send_event(br->srv, ev->raw, ev->len);
}

// ---------------------------------------------------------------------------
// Hot-plug callbacks
// ---------------------------------------------------------------------------

// Open, init, and start USB. Returns true on success.
static bool device_open_and_init(bridge_t *br)
{
    br->usb = mk1_device_open();
    if (!br->usb) return false;

    if (!mk1_device_init_hardware(br->usb)) {
        BLOG("WARNING: hardware init failed after arrival");
    }

    if (!mk1_device_start(br->usb, on_pad, on_button, br)) {
        BLOG("WARNING: could not start USB read thread after arrival");
    }

    // If Maschine is already connected, announce the device now.
    if (br->srv && mk1_server_is_connected(br->srv)) {
        char serial[32] = "MK1000000000000";
        mk1_device_get_serial(br->usb, serial, sizeof(serial));
        if (!mk1_server_send_device_on(br->srv, serial)) {
            BLOG("WARNING: DEVICE_ON send failed after arrival");
        }
    } else {
        // Maschine not yet connected — show prompt on both displays.
        show_status_screen(br);
    }
    return true;
}

static void on_device_arrived(void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;

    if (br->usb) {
        BLOG("device arrived but already open — ignoring");
        return;
    }

    BLOG("MK1 arrived — opening");
    if (device_open_and_init(br)) return;

    // kIOFirstMatchNotification can fire before the USB stack has finished
    // publishing interface services. Retry once after a short delay.
    BLOG("open failed — retrying in 500ms");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (br->usb) return; // arrived again while we were waiting
        BLOG("MK1 retry open");
        if (!device_open_and_init(br)) {
            BLOG("retry failed — device not usable");
        }
    });
}

static void on_device_removed(void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;

    if (!br->usb) {
        BLOG("device removed but not open — ignoring");
        return;
    }

    BLOG("MK1 removed — closing");

    // Notify Maschine before tearing down USB so the IPC message goes out.
    if (br->srv && mk1_server_is_connected(br->srv)) {
        mk1_server_send_device_off(br->srv);
    }

    mk1_device_stop(br->usb);
    mk1_device_close(br->usb);
    br->usb = NULL;

    // Reset pad state so stale pressure readings don't linger on reconnect.
    memset(g_prev_pressure, 0, sizeof(g_prev_pressure));
    memset(g_sent_pressure, 0, sizeof(g_sent_pressure));
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

    g_bridge.srv = mk1_server_start(on_connect, on_cmd, &g_bridge);
    if (!g_bridge.srv) {
        BLOG("Failed to start IPC server");
        close_logs();
        return 1;
    }

    // If Maschine is already running with a stale bridge session, send DEVICE_OFF
    // via the saved notification port so it re-initiates the NIHWMainHandler handshake.
    // Run after a short delay to ensure our NIHWMainHandler port is fully registered
    // in bootstrap before Maschine's reconnect attempt fires.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ try_maschine_reconnect(); });

    // Hot-plug watcher — fires immediately if device is already connected,
    // and again on any future plug/unplug while the run loop is running.
    g_bridge.hotplug = mk1_hotplug_start(on_device_arrived, on_device_removed, &g_bridge);
    if (!g_bridge.hotplug) {
        BLOG("WARNING: hot-plug watcher failed to start");
    }

    BLOG("waiting for MK1 and Maschine software...");
    CFRunLoopRun();

    if (g_bridge.usb && mk1_server_is_connected(g_bridge.srv)) {
        mk1_server_send_device_off(g_bridge.srv);
    }
    mk1_hotplug_stop(g_bridge.hotplug);
    mk1_server_stop(g_bridge.srv);
    if (g_bridge.usb) {
        mk1_device_stop(g_bridge.usb);
        mk1_device_close(g_bridge.usb);
    }

    BLOG("shutdown complete");
    close_logs();
    return 0;
}
