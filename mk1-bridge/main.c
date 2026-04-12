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
#include <pthread.h>
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

// When g_quiet is set, only bridge-status messages stay on stderr so the
// terminal remains usable for interactive prompts. Category logs still go to
// their log files as usual.
static bool g_quiet = false;

#define LOG(fp, fmt, ...) do { \
    if (!g_quiet) fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
    if (fp) { fprintf(fp, fmt "\n", ##__VA_ARGS__); fflush(fp); } \
} while (0)

#define BLOG(fmt, ...) do { \
    fprintf(stderr, "[bridge]  " fmt "\n", ##__VA_ARGS__); \
    if (g_log_bridge) { fprintf(g_log_bridge, "[bridge]  " fmt "\n", ##__VA_ARGS__); fflush(g_log_bridge); } \
} while (0)
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
static uint16_t g_led_pad_pressure[16]   = {0};  // live pad state used for synthetic rubber LEDs

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
//   bytes[8..39] = logical[0..31]  — canonical 32-slot selector-6 payload
//   bytes[44..59]= logical[36..51] — candidate extended pad rubber block for pads 1–16
//                  (bytes[40..43] = logical[32..35], not used)
//
// EP1 DIMM_LEDS format:
//   canonical: [0x0c, phys[0], ..., phys[31]]              — 33 bytes total
//   extended : [0x0c, phys[0], ..., phys[32]]              — 34 bytes total
//
// The base 32-slot remap matches mk1_shim_remap_led_payload / mk1_remap_led_payload.
// For playback testing we can optionally append the newer direct pad block from
// logical[37..52] onto phys[17..32] in the same packet, instead of mixing it with
// separate live-pad synthesis writes.
// ---------------------------------------------------------------------------

static void learn_on_led(const uint8_t *logical, size_t len); // defined after run_led_probe
static void project_capture_on_led(const uint8_t *logical, size_t logical_len,
                                   const uint8_t *phys, size_t phys_len);
static bool env_truthy(const char *name);

static void forward_led(bridge_t *br, const uint8_t *raw_msg, size_t raw_len)
{
    // Restore the full 32-slot selector-6 remap for button/group/transport LEDs.
    // This matches the prior checked-in bridge behavior and the shim's authoritative
    // logical-to-physical ordering. Pad LED experiments stay out of the default path.
    // Intel-era table: logical[17..31]→phys[17..31] identity, logical[16]→phys[13].
    static const uint8_t k_hw_by_logical[32] = {
         0,
         4,  3,  2,  1,
         8,  7,  6,  5,
        12, 11, 10,  9,
        16, 15, 14, 13,
        17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30,
        31
    };

    // Apple Silicon upper-slot override table.
    //
    // The linear k_hw_by_logical mapping is wrong for several upper slots on AS.
    // This table runs AFTER the linear remap and corrects specific slots.
    //
    //   0xFF = passthrough  — linear mapping stands; logs "upper-slot active" when non-zero
    //   0xFE = suppress     — clear linear phys write, do not forward
    //   0..31 = redirect    — clear linear phys, write to this phys instead
    //
    // Confirmed physical range (Apple Silicon, latest project capture 2026-04-11f):
    //   phys[17]=Mute,    phys[18]=Select,   phys[20]=Navigate, phys[21]=Pad Mode,
    //   phys[23]=Solo,
    //   phys[24]=Play,    phys[25]=Erase,    phys[26]=Shift,
    //   phys[28]=LCD-BL,  phys[29]=Record,   phys[30]=Scene,
    //   phys[19]/[22]/[27]/[31] = still unresolved in current evidence
    // Focused capture after the phys[0] fix resolved Mute/Solo as 17/23 respectively.
    //
    // NOTE: overrides for [24]→25, [28]→29, [30]→26 clobber the linear writes
    // for logical[25], [29], [26] respectively. If those slots also carry button
    // LEDs they may need their own entries below — add 0xFF passthrough to observe.
    static const struct {
        uint8_t log_idx;
        uint8_t override;   // 0xFF=passthrough, 0xFE=suppress, 0..31=redirect
        const char *name;
    } k_apple_upper[] = {
        // --- Confirmed redirects ---
        { 17,   23, "Solo"      },  // CONFIRMED logical[17] -> phys[23]
        { 16,   17, "Mute"      },  // CONFIRMED logical[16] -> phys[17]
        { 28,   29, "Record"    },  // CONFIRMED phys[29]=Record

        // --- Identified from log 2026-04-10: ---
        // logical[30] = LEFT nav carrier (NOT Shift — prior "Shift confirmed" was pressing Left!)
        // logical[27] = RIGHT nav carrier
        // logical[25] = fires with Left press; likely Erase carrier or secondary Left state
        // Older linear assumption for the Apple Silicon Pad Section is stale.
        // Latest PROJCAP evidence points to:
        //   logical[16] -> phys[17] = Mute
        //   logical[17] -> phys[23] = Solo
        //   logical[18] -> phys[18] = Select
        //   logical[20] -> phys[20] = Navigate
        //   logical[21] -> phys[21] = Pad Mode
        //
        // NOTE 2026-04-11: Intel-era lower-range phys mapping is WRONG on Apple Silicon.
        // phys[4]=pad1 rubber, phys[5]=pad8 rubber, phys[16]=pad13 rubber, phys[31]=dead.
        // Upper range fully mapped — nav arrows NOT in phys[17..31].
        // Current project-loaded evidence only supports:
        //   Mute(17), Select(18), Navigate(20), PadMode(21), Solo(23), Scene(30),
        //   Play(24), Erase(25), Shift(26), LCDBl(28), Record(29).
        // Duplicate / Pattern / Grid and several adjacent slots still need
        // isolated confirmation before they should be encoded as fixed mappings.
        //
        // CONFIRMED 2026-04-11: logical[24]=LEFT nav carrier, logical[27]=RIGHT nav carrier.
        // (No lower-range slots fire on Left/Right press — confirmed via log sweep.)
        // logical[30] fires alongside Left but is a Scene state update, not the nav carrier.
        // phys[32]=Play LED confirmed (writing to byte 33 of packet lights Play).
        // phys[0..32] fully explored — nav arrows NOT found in no-project state.
        // Hypothesis: nav arrows are dark without a project loaded; retest with project.
        { 24, 0xFF, "log24" }, { 25, 0xFF, "log25" }, { 26, 0xFF, "log26" },
        { 27, 0xFF, "log27" }, { 29, 0xFF, "log29" }, { 30, 0xFF, "log30" },
        // Lower-range slots — fire in with-project state for Left/Right nav:
        {  1, 0xFF, "log1"  }, {  2, 0xFF, "log2"  }, {  3, 0xFF, "log3"  },
        {  4, 0xFF, "log4"  }, {  5, 0xFF, "log5"  }, {  6, 0xFF, "log6"  },
        {  7, 0xFF, "log7"  }, {  8, 0xFF, "log8"  }, {  9, 0xFF, "log9"  },
        { 10, 0xFF, "log10" }, { 11, 0xFF, "log11" }, { 12, 0xFF, "log12" },
        { 13, 0xFF, "log13" }, { 14, 0xFF, "log14" }, { 15, 0xFF, "log15" },
        { 16, 0xFF, "log16" },
    };
    static const int k_apple_upper_n =
        (int)(sizeof(k_apple_upper) / sizeof(k_apple_upper[0]));

    if (!br->usb) {
        LLOG("no USB device");
        return;
    }
    if (raw_len < 8 + 32) {
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
    static bool s_have_prev = false;
    static uint8_t s_prev_logical[57] = {0};

    // Notify learn mode (no-op if not armed).
    learn_on_led(led_logical, full_len);

    if (led_len_hdr < 32 || full_len < 32) {
        LLOG("short LED payload (hdr_len=%u raw=%zu)", led_len_hdr, full_len);
        return;
    }

    for (size_t i = 32; i < full_len && i < 57; i++) {
        if (led_logical[i] == 0) continue;
        if (i >= 37 && i <= 52) continue; // rubber pad range (logical[37..52] = pads 1..16)
        LLOG("unresolved logical[%zu]=0x%02x", i, led_logical[i]);
    }

    if (full_len >= 32) {
        char delta[512];
        size_t pos = 0;
        bool any = false;
        const size_t cmp_len = full_len < sizeof(s_prev_logical) ? full_len : sizeof(s_prev_logical);

        if (s_have_prev) {
            for (size_t i = 0; i < cmp_len; i++) {
                if (led_logical[i] == s_prev_logical[i]) continue;
                int wrote = snprintf(delta + pos, sizeof(delta) - pos,
                                     "%slogical[%zu]:%02x->%02x",
                                     any ? " " : "",
                                     i, s_prev_logical[i], led_logical[i]);
                if (wrote < 0 || (size_t)wrote >= sizeof(delta) - pos) {
                    pos = sizeof(delta) - 1;
                    break;
                }
                pos += (size_t)wrote;
                any = true;
            }
            if (any) {
                LLOG("logical delta: %s", delta);
            }
        } else {
            LLOG("logical delta: <initial snapshot>");
            s_have_prev = true;
        }

        memcpy(s_prev_logical, led_logical, cmp_len);
        if (cmp_len < sizeof(s_prev_logical)) {
            memset(s_prev_logical + cmp_len, 0, sizeof(s_prev_logical) - cmp_len);
        }
        s_have_prev = true;
    }

    uint8_t remapped[34] = {0};  // [0..32]; phys[32] used for nav arrow test
    remapped[0]  = 0x1e; // control register — constant in all real NIHA full-state packets

    // Apply the full selector-6 remap for logical[0..31].
    // logical[0] is not a button LED carrier on EP1; phys[0] is the DIMM_LEDS
    // control register and must remain fixed at 0x1e.
    for (size_t log_idx = 0; log_idx < 32 && log_idx < full_len; log_idx++) {
        uint8_t phy_idx = k_hw_by_logical[log_idx];
        if (phy_idx == 0) continue;
        if (phy_idx < 33) {
            remapped[phy_idx] = led_logical[log_idx];
        }
    }

    // Apply Apple Silicon upper-slot overrides after the linear remap, so each
    // entry can clear the linearly-written physical byte before redirecting.
    for (int k = 0; k < k_apple_upper_n; k++) {
        uint8_t li = k_apple_upper[k].log_idx;
        uint8_t ov = k_apple_upper[k].override;
        // Skip if slot is out of range or currently zero (no LED change).
        if ((size_t)li >= full_len || led_logical[li] == 0) continue;

        uint8_t old_phys = k_hw_by_logical[li];  // li < 32 guaranteed by table

        if (ov == 0xFF) {
            // passthrough — linear mapping already applied; log for correlation
            LLOG("upper-slot active: %s logical[%u]=0x%02x -> phys[%u] (linear)",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)old_phys);
        } else if (ov == 0xFE) {
            // suppress — clear the linearly-written byte, do not forward
            if (old_phys < 34) remapped[old_phys] = 0;
            LLOG("upper-slot suppressed: %s logical[%u]=0x%02x cleared phys[%u]",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)old_phys);
        } else {
            // redirect — clear linear byte, write value to override phys
            if (old_phys < 34) remapped[old_phys] = 0;
            if (ov < 34)       remapped[ov] = led_logical[li];
            LLOG("upper-slot redirect: %s logical[%u]=0x%02x -> phys[%u] (was phys[%u])",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)ov, (unsigned)old_phys);
        }
    }

    // Rubber pad LEDs: mechanism not yet identified.
    // We intentionally do not synthesize or remap pad LEDs here in the default path.
    // phys[17..27] = Step/Control/SA1-SA8/NoteRepeat buttons (confirmed by probe sweep + user).
    // phys[28] = LCD backlight. phys[29..31] = no visible effect.
    // logical[37..52] contains pad brightness data from NIHA but correct phys positions unknown.

    if (env_truthy("MK1_PROJECT_CAPTURE")) {
        // During in-project mapping passes, suppress the unresolved lower range so
        // stale Intel-era remaps do not light misleading Group/transport LEDs.
        for (size_t phys = 1; phys <= 16; phys++) remapped[phys] = 0;
        // phys[32] is currently a visible contaminant during capture; keep project
        // mapping mode non-visual and rely on logical/phys diff logs instead.
        remapped[32] = 0;
    }

    project_capture_on_led(led_logical, full_len, remapped, 33);

    uint8_t packet[34] = {0};
    packet[0] = 0x0c;
    memcpy(packet + 1, remapped, 33);  // 33 bytes: phys[0..32]; phys[32] used for nav arrow test
    size_t packet_len = 34;

    {
        char hex[34 * 3 + 1];
        for (size_t i = 0; i < packet_len; i++) snprintf(hex + i*3, 4, "%02x ", packet[i]);
        hex[packet_len * 3] = '\0';
        LLOG("EP1 (%zu): %s", packet_len, hex);
    }

    if (!mk1_device_write_endpoint(br->usb, 0x01, packet, packet_len)) {
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
            if (g_quiet &&
                (cmd_type == NI_CMD_START ||
                 cmd_type == NI_MSG_INSTANCE_NAME ||
                 cmd_type == NI_MSG_UNKNOWN_IST)) {
                break;
            }
            BLOG("unhandled CMD 0x%08x (%zu bytes)", cmd_type, raw_len);
            if (raw_msg && raw_len > 0) {
                char hex[128 * 3 + 1];
                size_t log_len = raw_len < 128 ? raw_len : 128;
                for (size_t i = 0; i < log_len; i++) {
                    snprintf(hex + i * 3, 4, "%02x ", raw_msg[i]);
                }
                hex[log_len * 3] = '\0';
                BLOG("unhandled CMD 0x%08x payload: %s", cmd_type, hex);
            }
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
#define PAD_HIT_ON_THRESHOLD    256
#define PAD_HIT_OFF_THRESHOLD    96

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
        bool     was_active = prev >= PAD_HIT_OFF_THRESHOLD;
        bool     is_active =
            pressure >= (was_active ? PAD_HIT_OFF_THRESHOLD : PAD_HIT_ON_THRESHOLD);

        if (pressure == prev) continue;

        float value = is_active ? (pressure / PAD_PRESSURE_MAX) : 0.0f;

        if (!was_active && is_active) {
            // pad struck — send hit_on then initial pressure_update
            BTNLOG("pad idx=%u hit_on pressure=%u (%.3f)", idx, pressure, (double)value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_ON,         value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE, value);
            g_sent_pressure[idx] = pressure;
        } else if (was_active && !is_active) {
            // pad released
            BTNLOG("pad idx=%u hit_off", idx);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_OFF, 0.0f);
            g_sent_pressure[idx] = 0;
        } else if (is_active) {
            // pad held — only forward if pressure changed enough to be meaningful
            uint16_t sent = g_sent_pressure[idx];
            uint16_t delta = pressure > sent ? pressure - sent : sent - pressure;
            if (delta >= PAD_PRESSURE_THRESHOLD) {
                send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE, value);
                g_sent_pressure[idx] = pressure;
            }
        }

        if (idx < 16) {
            g_prev_pressure[idx] = pressure;
            g_led_pad_pressure[idx] = pressure;
        }
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

static bool env_truthy(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static int env_int_or_default(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !value[0]) return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0') return fallback;
    return (int)parsed;
}

#define LED_CAPTURE_LOGICAL_MAX 57
#define LED_CAPTURE_PHYS_MAX    33

typedef struct {
    pthread_mutex_t mu;
    uint64_t        seq;
    size_t          logical_len;
    size_t          phys_len;
    uint8_t         logical[LED_CAPTURE_LOGICAL_MAX];
    uint8_t         phys[LED_CAPTURE_PHYS_MAX];
} led_capture_state_t;

static led_capture_state_t g_project_capture = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
};

static void project_capture_on_led(const uint8_t *logical, size_t logical_len,
                                   const uint8_t *phys, size_t phys_len)
{
    const size_t log_n = logical_len < LED_CAPTURE_LOGICAL_MAX ? logical_len : LED_CAPTURE_LOGICAL_MAX;
    const size_t phy_n = phys_len    < LED_CAPTURE_PHYS_MAX    ? phys_len    : LED_CAPTURE_PHYS_MAX;

    pthread_mutex_lock(&g_project_capture.mu);

    bool changed =
        g_project_capture.logical_len != log_n ||
        g_project_capture.phys_len    != phy_n ||
        memcmp(g_project_capture.logical, logical, log_n) != 0 ||
        memcmp(g_project_capture.phys, phys, phy_n) != 0;

    if (changed) {
        memcpy(g_project_capture.logical, logical, log_n);
        memcpy(g_project_capture.phys,    phys,    phy_n);
        if (log_n < LED_CAPTURE_LOGICAL_MAX) {
            memset(g_project_capture.logical + log_n, 0, LED_CAPTURE_LOGICAL_MAX - log_n);
        }
        if (phy_n < LED_CAPTURE_PHYS_MAX) {
            memset(g_project_capture.phys + phy_n, 0, LED_CAPTURE_PHYS_MAX - phy_n);
        }
        g_project_capture.logical_len = log_n;
        g_project_capture.phys_len    = phy_n;
        g_project_capture.seq++;
    }

    pthread_mutex_unlock(&g_project_capture.mu);
}

static bool project_capture_snapshot(uint64_t *seq_out,
                                     uint8_t *logical_out, size_t *logical_len_out,
                                     uint8_t *phys_out, size_t *phys_len_out)
{
    pthread_mutex_lock(&g_project_capture.mu);

    bool have = g_project_capture.seq != 0;
    if (have) {
        if (seq_out) *seq_out = g_project_capture.seq;
        if (logical_out && logical_len_out) {
            memcpy(logical_out, g_project_capture.logical, g_project_capture.logical_len);
            *logical_len_out = g_project_capture.logical_len;
        }
        if (phys_out && phys_len_out) {
            memcpy(phys_out, g_project_capture.phys, g_project_capture.phys_len);
            *phys_len_out = g_project_capture.phys_len;
        }
    }

    pthread_mutex_unlock(&g_project_capture.mu);
    return have;
}

static bool project_capture_wait_for_seq_gt(uint64_t start_seq, int timeout_ms,
                                            uint64_t *seq_out,
                                            uint8_t *logical_out, size_t *logical_len_out,
                                            uint8_t *phys_out, size_t *phys_len_out)
{
    for (int t = 0; t < timeout_ms / 10; t++) {
        pthread_mutex_lock(&g_project_capture.mu);
        bool ready = g_project_capture.seq > start_seq;
        if (ready) {
            if (seq_out) *seq_out = g_project_capture.seq;
            if (logical_out && logical_len_out) {
                memcpy(logical_out, g_project_capture.logical, g_project_capture.logical_len);
                *logical_len_out = g_project_capture.logical_len;
            }
            if (phys_out && phys_len_out) {
                memcpy(phys_out, g_project_capture.phys, g_project_capture.phys_len);
                *phys_len_out = g_project_capture.phys_len;
            }
            pthread_mutex_unlock(&g_project_capture.mu);
            return true;
        }
        pthread_mutex_unlock(&g_project_capture.mu);
        usleep(10 * 1000);
    }
    return false;
}

static bool arrays_equal_padded(const uint8_t *a, size_t a_len,
                                const uint8_t *b, size_t b_len,
                                size_t max_len)
{
    for (size_t i = 0; i < max_len; i++) {
        uint8_t av = i < a_len ? a[i] : 0;
        uint8_t bv = i < b_len ? b[i] : 0;
        if (av != bv) return false;
    }
    return true;
}

static void print_led_diff(FILE *tty, const char *kind,
                           const uint8_t *before, size_t before_len,
                           const uint8_t *after,  size_t after_len,
                           size_t max_len, const char *label,
                           int step_idx, int diff_limit)
{
    bool any = false;
    int shown = 0;
    char line[1024];
    size_t pos = 0;

    for (size_t i = 0; i < max_len; i++) {
        uint8_t old_v = i < before_len ? before[i] : 0;
        uint8_t new_v = i < after_len  ? after[i]  : 0;
        if (old_v == new_v) continue;
        any = true;
        if (shown >= diff_limit) continue;

        int wrote = snprintf(line + pos, sizeof(line) - pos,
                             "%s%s[%zu]:%02x->%02x",
                             shown ? " " : "", kind, i, old_v, new_v);
        if (wrote < 0 || (size_t)wrote >= sizeof(line) - pos) {
            pos = sizeof(line) - 1;
            break;
        }
        pos += (size_t)wrote;
        shown++;
    }

    if (!any) {
        fprintf(tty, "  [%s] no change vs baseline\n", kind);
        LLOG("PROJCAP [%d] %s %s: no change", step_idx, label, kind);
        return;
    }

    if (shown == 0) {
        fprintf(tty, "  [%s] changed, but exceeded diff limit %d\n", kind, diff_limit);
        LLOG("PROJCAP [%d] %s %s: changed but exceeded diff limit %d",
             step_idx, label, kind, diff_limit);
        return;
    }

    fprintf(tty, "  %s\n", line);
    if (shown < diff_limit) {
        LLOG("PROJCAP [%d] %s %s: %s", step_idx, label, kind, line);
    } else {
        LLOG("PROJCAP [%d] %s %s: %s ...", step_idx, label, kind, line);
    }
}

static void *project_capture_thread_fn(void *arg)
{
    (void)arg;

    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) tty = stderr;

    const int timeout_ms = env_int_or_default("MK1_PROJECT_CAPTURE_TIMEOUT_MS", 6000);
    const int diff_limit = env_int_or_default("MK1_PROJECT_CAPTURE_DIFF_LIMIT", 16);
    char label[128];

    fprintf(tty,
            "\n=== MK1 PROJECT LED CAPTURE ===\n"
            "Open a project first. This mode records logical-slot and remapped phys-byte\n"
            "diffs for each action you perform in Maschine.\n"
            "Workflow: type a short label, press ENTER, perform the action within %d ms.\n"
            "Type 'q' to quit. Results are logged with PROJCAP in led.log.\n\n",
            timeout_ms);
    fflush(tty);

    uint8_t baseline_logical[LED_CAPTURE_LOGICAL_MAX] = {0};
    uint8_t baseline_phys[LED_CAPTURE_PHYS_MAX] = {0};
    size_t baseline_logical_len = 0;
    size_t baseline_phys_len = 0;
    uint64_t baseline_seq = 0;

    fprintf(tty, "[baseline] waiting for an LED snapshot from Maschine...\n");
    fflush(tty);

    while (!project_capture_wait_for_seq_gt(0, 10000,
                                            &baseline_seq,
                                            baseline_logical, &baseline_logical_len,
                                            baseline_phys, &baseline_phys_len)) {
        fprintf(tty,
                "[baseline] no LED snapshot yet. Keep Maschine open with a project loaded; retrying...\n");
        fflush(tty);
        LLOG("PROJCAP baseline: timeout waiting for initial LED snapshot");
    }

    fprintf(tty, "[baseline] captured seq=%llu\n", (unsigned long long)baseline_seq);
    fflush(tty);
    LLOG("PROJCAP baseline: seq=%llu logical_len=%zu phys_len=%zu",
         (unsigned long long)baseline_seq, baseline_logical_len, baseline_phys_len);

    for (int step = 1; ; step++) {
        fprintf(tty, "[step %d] action label (or q): ", step);
        fflush(tty);
        if (!fgets(label, sizeof(label), tty)) break;

        size_t len = strlen(label);
        while (len > 0 && (label[len - 1] == '\n' || label[len - 1] == '\r')) {
            label[--len] = '\0';
        }
        if (strcmp(label, "q") == 0 || strcmp(label, "quit") == 0) break;
        if (label[0] == '\0') snprintf(label, sizeof(label), "step-%d", step);

        uint64_t start_seq = 0;
        if (!project_capture_snapshot(&start_seq, NULL, NULL, NULL, NULL)) start_seq = 0;

        fprintf(tty, "  armed at seq=%llu; perform '%s' now...\n",
                (unsigned long long)start_seq, label);
        fflush(tty);

        uint8_t snap_logical[LED_CAPTURE_LOGICAL_MAX] = {0};
        uint8_t snap_phys[LED_CAPTURE_PHYS_MAX] = {0};
        size_t snap_logical_len = 0;
        size_t snap_phys_len = 0;
        uint64_t snap_seq = 0;

        if (!project_capture_wait_for_seq_gt(start_seq, timeout_ms,
                                             &snap_seq,
                                             snap_logical, &snap_logical_len,
                                             snap_phys, &snap_phys_len)) {
            fprintf(tty, "  [timeout] no LED update observed for '%s'\n", label);
            LLOG("PROJCAP [%d] %s: TIMEOUT after %d ms", step, label, timeout_ms);
            continue;
        }

        fprintf(tty, "  captured seq=%llu\n", (unsigned long long)snap_seq);
        fflush(tty);

        print_led_diff(tty, "logical",
                       baseline_logical, baseline_logical_len,
                       snap_logical, snap_logical_len,
                       LED_CAPTURE_LOGICAL_MAX, label, step, diff_limit);
        print_led_diff(tty, "phys",
                       baseline_phys, baseline_phys_len,
                       snap_phys, snap_phys_len,
                       LED_CAPTURE_PHYS_MAX, label, step, diff_limit);

        if (!arrays_equal_padded(baseline_logical, baseline_logical_len,
                                 snap_logical, snap_logical_len,
                                 LED_CAPTURE_LOGICAL_MAX) ||
            !arrays_equal_padded(baseline_phys, baseline_phys_len,
                                 snap_phys, snap_phys_len,
                                 LED_CAPTURE_PHYS_MAX)) {
            memcpy(baseline_logical, snap_logical, snap_logical_len);
            memcpy(baseline_phys, snap_phys, snap_phys_len);
            if (snap_logical_len < LED_CAPTURE_LOGICAL_MAX) {
                memset(baseline_logical + snap_logical_len, 0,
                       LED_CAPTURE_LOGICAL_MAX - snap_logical_len);
            }
            if (snap_phys_len < LED_CAPTURE_PHYS_MAX) {
                memset(baseline_phys + snap_phys_len, 0,
                       LED_CAPTURE_PHYS_MAX - snap_phys_len);
            }
            baseline_logical_len = snap_logical_len;
            baseline_phys_len = snap_phys_len;
            baseline_seq = snap_seq;
        }
    }

    fprintf(tty, "\n=== PROJECT CAPTURE COMPLETE — grep led.log for PROJCAP ===\n");
    if (tty != stderr) fclose(tty);
    return NULL;
}

// ---------------------------------------------------------------------------
// LED learn mode — derives hw_by_logical from live Maschine IPC button presses
// ---------------------------------------------------------------------------
// Usage: MK1_LED_LEARN=1 ./mk1-bridge
// Run normally with Maschine connected. Follow prompts: press each named
// button on the hardware, then hit ENTER. The tool diffs logical[0..31]
// on every IPC LED update and logs which slot changed per button press.
// Results are written to led.log with "LEARN" prefix for easy grepping.
// ---------------------------------------------------------------------------

static const char *k_phys_label[] = {
    "none/ctrl",      // 0
    "Transport Left", // 1
    "Restart",        // 2
    "Group H",        // 3
    "Group G",        // 4
    "Group D",        // 5
    "Group C",        // 6
    "Group F",        // 7
    "Group E",        // 8
    "Group B",        // 9
    "Group A",        // 10
    "Auto Write",     // 11
    "Snap",           // 12
    "Modules Right",  // 13
    "Modules Left",   // 14
    "Sampling",       // 15
    "Browse",         // 16
    "Step",           // 17
    "Control",        // 18
    "SA8",            // 19
    "SA7",            // 20
    "SA6",            // 21
    "SA5",            // 22
    "SA4",            // 23
    "SA3",            // 24
    "SA2",            // 25
    "SA1",            // 26
    "Note Repeat",    // 27
    "LCD BL on",      // 28
    "LCD BL off",     // 29
    "none",           // 30
    "none",           // 31
    "none",           // 32
};

typedef struct { const char *name; int phys; } learn_btn_t;

// Buttons in probe order. phys = expected physical slot from CODEX probe map.
// Group B is before Group A: pressing B lets us see A's slot turn off and B's turn on.
// Then pressing A gives us A's slot turning on and B's turning off.
static const learn_btn_t k_learn_seq[] = {
    { "Group B",        9 },
    { "Group A",       10 },
    { "Group C",        6 },
    { "Group D",        5 },
    { "Group E",        8 },
    { "Group F",        7 },
    { "Group G",        4 },
    { "Group H",        3 },
    { "Auto Write",    11 },
    { "Snap",          12 },
    { "Modules Left",  14 },
    { "Modules Right", 13 },
    { "Sampling",      15 },
    { "Browse",        16 },
    { "Left (nav)",     1 },
    { "Restart",        2 },
    { "Control",       18 },
    { "SA8",           19 },
    { "SA7",           20 },
    { "SA6",           21 },
    { "SA5",           22 },
    { "SA4",           23 },
    { "SA3",           24 },
    { "SA2",           25 },
    { "SA1",           26 },
    { "Note Repeat",   27 },
};
#define LEARN_COUNT (int)(sizeof(k_learn_seq)/sizeof(k_learn_seq[0]))

static pthread_mutex_t g_learn_mu       = PTHREAD_MUTEX_INITIALIZER;
static bool            g_learn_armed    = false;
static bool            g_learn_got_upd  = false;
static uint8_t         g_learn_prev[32] = {0};
static uint8_t         g_learn_curr[32] = {0};

// Called from forward_led on every IPC LED message (holds no lock — fast path).
static void learn_on_led(const uint8_t *logical, size_t len)
{
    pthread_mutex_lock(&g_learn_mu);
    if (!g_learn_armed) { pthread_mutex_unlock(&g_learn_mu); return; }

    size_t n = len < 32 ? len : 32;
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        if (logical[i] != g_learn_prev[i]) { changed = true; break; }
    }
    if (changed) {
        memcpy(g_learn_curr, logical, n);
        if (n < 32) memset(g_learn_curr + n, 0, 32 - n);
        g_learn_got_upd = true;
        g_learn_armed   = false;
    }
    pthread_mutex_unlock(&g_learn_mu);
}

// Arm capture and wait up to timeout_ms for an LED update.
// Returns true if an update was captured.
static bool learn_wait_update(int timeout_ms)
{
    for (int t = 0; t < timeout_ms / 10; t++) {
        pthread_mutex_lock(&g_learn_mu);
        bool got = g_learn_got_upd;
        pthread_mutex_unlock(&g_learn_mu);
        if (got) return true;
        usleep(10 * 1000);
    }
    pthread_mutex_lock(&g_learn_mu);
    g_learn_armed = false;
    pthread_mutex_unlock(&g_learn_mu);
    return false;
}

static void *learn_thread_fn(void *arg)
{
    (void)arg;
    char buf[32];

    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) tty = stderr;

    fprintf(tty, "\n[learn] starting — connect Maschine and open a project. Prompts in 10s...\n");
    fflush(tty);

    sleep(10);

    fprintf(tty,
        "\n=== MK1 LED LEARN MODE ===\n"
        "For each button: press it (and release), then hit ENTER.\n"
        "Each result shows what changed vs the previous button's state.\n"
        "Results go to led.log (grep LEARN).\n\n");
    fflush(tty);

    // Capture real initial LED state as baseline before any button presses.
    // Arm immediately — Maschine will send a full-state update on connect/project load.
    pthread_mutex_lock(&g_learn_mu);
    memset(g_learn_curr, 0, 32);
    g_learn_got_upd = false;
    g_learn_armed   = true;
    pthread_mutex_unlock(&g_learn_mu);

    fprintf(tty, "[baseline] waiting for Maschine LED state (up to 5s)...\n");
    fflush(tty);
    learn_wait_update(5000);

    // g_learn_curr now has the real initial state (or zeros if nothing arrived).
    // Use it as the rolling baseline.
    uint8_t baseline[32];
    pthread_mutex_lock(&g_learn_mu);
    g_learn_armed = false;
    memcpy(baseline, g_learn_curr, 32);
    pthread_mutex_unlock(&g_learn_mu);

    // Button sequence — single capture per button, diff vs rolling baseline.
    for (int i = 0; i < LEARN_COUNT; i++) {
        const learn_btn_t *b = &k_learn_seq[i];
        const char *phys_name = (b->phys >= 0 && b->phys <= 32) ? k_phys_label[b->phys] : "?";

        fprintf(tty, "[%2d/%2d] Press [%-14s]  then ENTER: ", i + 1, LEARN_COUNT, b->name);
        fflush(tty);

        // Arm capture before reading Enter so we don't miss updates during the press.
        pthread_mutex_lock(&g_learn_mu);
        g_learn_got_upd = false;
        g_learn_armed   = true;
        pthread_mutex_unlock(&g_learn_mu);

        if (!fgets(buf, sizeof(buf), tty)) break;

        // Wait up to 3s for an update that differs from current baseline.
        bool ok = learn_wait_update(3000);

        uint8_t snap[32];
        pthread_mutex_lock(&g_learn_mu);
        g_learn_armed = false;
        memcpy(snap, g_learn_curr, 32);
        pthread_mutex_unlock(&g_learn_mu);

        if (!ok) {
            fprintf(tty, "  [no LED update received]\n");
            LLOG("LEARN [%d/%d] %s: TIMEOUT", i+1, LEARN_COUNT, b->name);
            // Don't update baseline — keep previous state.
            continue;
        }

        // Diff snap vs rolling baseline.
        bool any = false;
        for (int s = 0; s < 32; s++) {
            if (snap[s] != baseline[s]) {
                fprintf(tty, "  logical[%2d]: 0x%02x -> 0x%02x  (expect->phys[%d]=%s)\n",
                        s, baseline[s], snap[s], b->phys, phys_name);
                LLOG("LEARN [%d/%d] %s: logical[%d] 0x%02x->0x%02x  expect->phys[%d]=%s",
                     i+1, LEARN_COUNT, b->name, s, baseline[s], snap[s], b->phys, phys_name);
                any = true;
            }
        }
        if (!any) {
            fprintf(tty, "  [no change vs previous state]\n");
            LLOG("LEARN [%d/%d] %s: no change", i+1, LEARN_COUNT, b->name);
        }

        // This button's state becomes the baseline for the next button.
        memcpy(baseline, snap, 32);
    }

    fprintf(tty, "\n=== LED LEARN COMPLETE — grep led.log for LEARN ===\n");
    if (tty != stderr) fclose(tty);
    return NULL;
}

static void run_led_probe(mk1_device_t *dev)
{
    const int from = env_int_or_default("MK1_LED_PROBE_FROM", 0);
    const int to = env_int_or_default("MK1_LED_PROBE_TO", 32);
    const useconds_t dwell_us = (useconds_t)(env_int_or_default("MK1_LED_PROBE_MS", 700) * 1000);
    const uint8_t level = (uint8_t)env_int_or_default("MK1_LED_PROBE_LEVEL", 0x32);
    const bool interactive = env_truthy("MK1_LED_PROBE_INTERACTIVE");

    BLOG("LED probe mode: slots %d..%d level=0x%02x dwell=%u ms",
         from, to, level, (unsigned)(dwell_us / 1000));

    for (int idx = from; idx <= to; idx++) {
        uint8_t packet[34] = {0};

        if (idx < 0 || idx > 32) continue;

        packet[0] = 0x0c;
        packet[1] = 0x1e; // keep the controller-side LED engine/backlight control slot consistent
        packet[1 + idx] = level;

        {
            char hex[34 * 3 + 1];
            for (size_t i = 0; i < sizeof(packet); i++) snprintf(hex + i * 3, 4, "%02x ", packet[i]);
            hex[sizeof(packet) * 3] = '\0';
            LLOG("LED probe slot %d: %s", idx, hex);
        }

        mk1_device_write_endpoint(dev, 0x01, packet, sizeof(packet));
        if (interactive) {
            char note[256];
            fprintf(stderr,
                    "\n[probe] slot %d active. Type what lit up, then press Enter\n"
                    "[probe] example: Group A bright | none | LCD backlight on\n> ",
                    idx);
            fflush(stderr);
            if (fgets(note, sizeof(note), stdin)) {
                size_t len = strlen(note);
                while (len > 0 && (note[len - 1] == '\n' || note[len - 1] == '\r')) {
                    note[--len] = '\0';
                }
                LLOG("LED probe slot %d observed: %s", idx, note[0] ? note : "(blank)");
            } else {
                LLOG("LED probe slot %d observed: <stdin closed>", idx);
                clearerr(stdin);
            }
        } else {
            usleep(dwell_us);
        }
    }

    {
        uint8_t packet[34] = {0x0c, 0x1e};
        mk1_device_write_endpoint(dev, 0x01, packet, sizeof(packet));
    }
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

    // Clear both displays to black. The ST7529 controller GRAM retains whatever
    // was last written (including garbage from a previous session or hot-plug).
    // Maschine only repaints on a state change, so without this the display shows
    // stale pixels until the user does something in the app.
    {
        static const uint8_t k_black[MK1_DISPLAY_FB_BYTES];
        mk1_set_display(br->usb, 0x00, k_black, sizeof(k_black));
        mk1_set_display(br->usb, 0x02, k_black, sizeof(k_black));
    }

    // Turn on display backlight. The init sequence clears DIMM_LEDS to all zeros
    // (phys[0] = 0 = backlight off). Sending phys[0] = 0x1e here restores the
    // backlight before showing the status screen.
    {
        uint8_t bl[33] = { 0x0c, 0x1e }; // cmd=0x0c, phys[0]=0x1e ctrl-reg
        bl[29] = 0x5c; // phys[28] = LCD backlight — constant in all real NIHA packets
        mk1_device_write_endpoint(br->usb, 0x01, bl, sizeof(bl));
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
    memset(g_led_pad_pressure, 0, sizeof(g_led_pad_pressure));
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

    if (env_truthy("MK1_LED_PROBE")) {
        BLOG("standalone LED probe requested");
        g_bridge.usb = mk1_device_open();
        if (!g_bridge.usb) {
            BLOG("LED probe failed: could not open MK1");
            close_logs();
            return 1;
        }
        if (!mk1_device_init_hardware(g_bridge.usb)) {
            BLOG("LED probe warning: hardware init failed");
        }
        if (!mk1_device_start(g_bridge.usb, on_pad, on_button, &g_bridge)) {
            BLOG("LED probe warning: could not start USB read thread");
        }
        usleep(250 * 1000);
        run_led_probe(g_bridge.usb);
        mk1_device_stop(g_bridge.usb);
        mk1_device_close(g_bridge.usb);
        g_bridge.usb = NULL;
        BLOG("LED probe complete");
        close_logs();
        return 0;
    }

    g_bridge.srv = mk1_server_start(on_connect, on_cmd, &g_bridge);
    if (!g_bridge.srv) {
        BLOG("Failed to start IPC server");
        close_logs();
        return 1;
    }

    if (env_truthy("MK1_LED_LEARN")) {
        g_quiet = true;
        BLOG("LED learn mode active — suppressing verbose stderr");
        pthread_t learn_tid;
        pthread_create(&learn_tid, NULL, learn_thread_fn, NULL);
        pthread_detach(learn_tid);
    }

    if (env_truthy("MK1_PROJECT_CAPTURE")) {
        g_quiet = true;
        BLOG("project LED capture mode active");
        pthread_t capture_tid;
        pthread_create(&capture_tid, NULL, project_capture_thread_fn, NULL);
        pthread_detach(capture_tid);
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
        // Clear all LEDs (including backlight at phys[0]) before closing.
        uint8_t leds_off[33] = { 0x0c }; // cmd=0x0c, all phys positions = 0
        mk1_device_write_endpoint(g_bridge.usb, 0x01, leds_off, sizeof(leds_off));
        mk1_device_stop(g_bridge.usb);
        mk1_device_close(g_bridge.usb);
    }

    BLOG("shutdown complete");
    close_logs();
    return 0;
}
