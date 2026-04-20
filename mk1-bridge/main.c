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
#include <sys/wait.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <dispatch/dispatch.h>

#include "bridge.h"
#include "mk1_server.h"
#include "../mk1-usb/mk1_device.h"
#include "../mk1-ipc/mk1_ipc.h"

extern char **environ;

// ---------------------------------------------------------------------------
// Logging
//
// Category log files written to /tmp/maschine-mk1-revive/.
// Each file is capped at 10 MiB. Once the next write would exceed that size,
// the file is truncated and logging starts over from the beginning.
//   bridge.log  — startup, USB, IPC, errors
//   led.log     — every LED IPC command and EP1 output
//   display.log — every display IPC command
//   buttons.log — pad/button events from USB
//
// Each category also writes to stderr for live terminal monitoring.
// ---------------------------------------------------------------------------

typedef struct {
    FILE *fp;
    char path[PATH_MAX];
} log_file_t;

#define LOG_FILE_MAX_BYTES (10 * 1024 * 1024)

static log_file_t g_log_bridge  = {0};
static log_file_t g_log_led     = {0};
static log_file_t g_log_display = {0};
static log_file_t g_log_buttons = {0};
static log_file_t g_log_encoder = {0};
static log_file_t g_log_timing  = {0};

// When g_quiet is set, only bridge-status messages stay on stderr so the
// terminal remains usable for interactive prompts. Category logs still go to
// their log files as usual.
static bool g_quiet = false;
static bool g_timing_trace = false;
static pid_t g_spawned_maschine_pid = -1;
static bool g_aux_mode_started = false;

static void close_log_file(log_file_t *log)
{
    if (!log) return;
    if (log->fp) {
        fclose(log->fp);
        log->fp = NULL;
    }
}

static void ensure_log_capacity(log_file_t *log, size_t incoming_bytes)
{
    off_t current_size = 0;

    if (!log || !log->path[0]) return;
    if (!log->fp) {
        log->fp = fopen(log->path, "a");
        if (!log->fp) return;
    }

    current_size = ftello(log->fp);
    if (current_size < 0) {
        struct stat st;
        if (stat(log->path, &st) == 0) {
            current_size = st.st_size;
        } else {
            current_size = 0;
        }
    }

    if ((uint64_t)current_size + (uint64_t)incoming_bytes <= (uint64_t)LOG_FILE_MAX_BYTES) {
        return;
    }

    fclose(log->fp);
    log->fp = fopen(log->path, "w");
    if (log->fp) {
        static const char wrap_marker[] = "--- mk1-bridge log wrapped at 10 MiB ---\n";
        fwrite(wrap_marker, 1, sizeof(wrap_marker) - 1, log->fp);
        fflush(log->fp);
    }
}

static void vlog_write(log_file_t *log, bool to_stderr, const char *fmt, va_list args)
{
    va_list measure_args;
    va_list render_args;
    int needed = 0;
    char stack_buf[1024];
    char *line = stack_buf;

    (void)to_stderr;

    va_copy(measure_args, args);
    needed = vsnprintf(NULL, 0, fmt, measure_args);
    va_end(measure_args);
    if (needed < 0) return;

    if ((size_t)needed + 1 > sizeof(stack_buf)) {
        line = malloc((size_t)needed + 1);
        if (!line) return;
    }

    va_copy(render_args, args);
    vsnprintf(line, (size_t)needed + 1, fmt, render_args);
    va_end(render_args);

    if (log && log->path[0]) {
        ensure_log_capacity(log, (size_t)needed + 1);
        if (log->fp) {
            fwrite(line, 1, (size_t)needed, log->fp);
            fputc('\n', log->fp);
            fflush(log->fp);
        }
    }

    if (line != stack_buf) free(line);
}

static void log_write(log_file_t *log, bool to_stderr, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vlog_write(log, to_stderr, fmt, args);
    va_end(args);
}

#define BLOG(fmt, ...)   log_write(&g_log_bridge,  true,     "[bridge]  " fmt, ##__VA_ARGS__)
#define LLOG(fmt, ...)   log_write(&g_log_led,     !g_quiet, "[led]     " fmt, ##__VA_ARGS__)
#define DLOG(fmt, ...)   log_write(&g_log_display, !g_quiet, "[display] " fmt, ##__VA_ARGS__)
#define BTNLOG(fmt, ...) log_write(&g_log_buttons, !g_quiet, "[buttons] " fmt, ##__VA_ARGS__)
#define ENCLOG(fmt, ...) log_write(&g_log_encoder, !g_quiet, "[encoder] " fmt, ##__VA_ARGS__)

static uint64_t timing_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void TLOG(const char *fmt, ...)
{
    va_list args;

    if (!g_timing_trace) return;

    va_start(args, fmt);
    {
        va_list render_args;
        int needed = 0;
        char stack_buf[1024];
        char *line = stack_buf;

        va_copy(render_args, args);
        needed = vsnprintf(NULL, 0, fmt, render_args);
        va_end(render_args);
        if (needed >= 0) {
            if ((size_t)needed + 1 > sizeof(stack_buf)) {
                line = malloc((size_t)needed + 1);
            }
            if (line) {
                va_copy(render_args, args);
                vsnprintf(line, (size_t)needed + 1, fmt, render_args);
                va_end(render_args);
                log_write(&g_log_timing, true, "[timing]  %s", line);
                if (line != stack_buf) free(line);
            }
        }
    }
    va_end(args);
}

static void open_logs(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", "/tmp/maschine-mk1-revive");
    mkdir(dir, 0755);   // no-op if already exists

    snprintf(g_log_bridge.path, sizeof(g_log_bridge.path), "%s/%s", dir, "bridge.log");
    snprintf(g_log_led.path, sizeof(g_log_led.path), "%s/%s", dir, "led.log");
    snprintf(g_log_display.path, sizeof(g_log_display.path), "%s/%s", dir, "display.log");
    snprintf(g_log_buttons.path, sizeof(g_log_buttons.path), "%s/%s", dir, "buttons.log");
    snprintf(g_log_encoder.path, sizeof(g_log_encoder.path), "%s/%s", dir, "encoder.log");
    snprintf(g_log_timing.path, sizeof(g_log_timing.path), "%s/%s", dir, "timing.log");

    mk1_bridge_logf("[bridge] log: %s\n", g_log_bridge.path);
    mk1_bridge_logf("[bridge] log: %s\n", g_log_led.path);
    mk1_bridge_logf("[bridge] log: %s\n", g_log_display.path);
    mk1_bridge_logf("[bridge] log: %s\n", g_log_buttons.path);
    mk1_bridge_logf("[bridge] log: %s\n", g_log_encoder.path);
    mk1_bridge_logf("[bridge] log: %s\n", g_log_timing.path);

    log_write(&g_log_bridge, false, "--- mk1-bridge started ---");
    log_write(&g_log_led, false, "--- mk1-bridge started ---");
    log_write(&g_log_display, false, "--- mk1-bridge started ---");
    log_write(&g_log_buttons, false, "--- mk1-bridge started ---");
    log_write(&g_log_encoder, false, "--- mk1-bridge started ---");
    log_write(&g_log_timing, false, "--- mk1-bridge started ---");

    mk1_device_set_encoder_log_path(g_log_encoder.path);
    mk1_device_set_timing_log_path(g_log_timing.path);
}

static void close_logs(void)
{
    mk1_device_set_timing_log_path(NULL);
    close_log_file(&g_log_bridge);
    close_log_file(&g_log_led);
    close_log_file(&g_log_display);
    close_log_file(&g_log_buttons);
    close_log_file(&g_log_encoder);
    close_log_file(&g_log_timing);
    mk1_device_set_encoder_log_path(NULL);
}

// ---------------------------------------------------------------------------
// Bridge state
// ---------------------------------------------------------------------------

typedef struct {
    mk1_server_t   *srv;
    mk1_device_t   *usb;
    mk1_hotplug_t  *hotplug;
    MIDIClientRef   midi_client;
    MIDIEndpointRef midi_source;
    MIDIEndpointRef midi_destination;
} bridge_t;

static bridge_t g_bridge;
static volatile int g_running = 1;
static uint16_t g_led_pad_pressure[16]   = {0};  // live pad state used for synthetic rubber LEDs
static bool g_led_autowrite_pressed = false;
static uint8_t g_last_led_logical[128] = {0};
static size_t g_last_led_logical_len = 0;
static bool g_guided_mode_only = false;
static int g_open_retry_budget = 0;
static bool g_midi_warned_no_usb = false;

static void sig_handler(int sig) { (void)sig; g_running = 0; CFRunLoopStop(CFRunLoopGetMain()); }
static void maybe_start_guided_mode(bridge_t *br);
static void schedule_device_open_retry(bridge_t *br);
static bool guided_verify_with_maschine(void);
static void emit_led_state(bridge_t *br, const uint8_t *led_logical, size_t full_len);
static void refresh_led_from_cache(bridge_t *br);
static bool midi_bridge_start(bridge_t *br);
static void midi_bridge_stop(bridge_t *br);

static void midi_read_proc(const MIDIPacketList *packet_list,
                           void *read_proc_ref_con,
                           void *src_conn_ref_con)
{
    bridge_t *br = (bridge_t *)read_proc_ref_con;
    const MIDIPacket *packet = NULL;
    (void)src_conn_ref_con;

    if (!br || !packet_list) return;

    if (!br->usb) {
        if (!g_midi_warned_no_usb) {
            BLOG("CoreMIDI received data while MK1 USB device is unavailable");
            g_midi_warned_no_usb = true;
        }
        return;
    }

    packet = &packet_list->packet[0];
    for (UInt32 i = 0; i < packet_list->numPackets; i++) {
        if (packet->length > 0) {
            bool ok = mk1_device_write_midi(br->usb, packet->data, packet->length);
            if (!ok) {
                BLOG("CoreMIDI -> DIN write failed for %u-byte packet", (unsigned)packet->length);
            }
        }
        packet = MIDIPacketNext(packet);
    }
}

static bool midi_bridge_start(bridge_t *br)
{
    OSStatus status = noErr;

    if (!br) return false;
    if (br->midi_client) return true;

    status = MIDIClientCreate(CFSTR("mk1-bridge"), NULL, NULL, &br->midi_client);
    if (status != noErr) {
        BLOG("CoreMIDI client creation failed: %d", (int)status);
        return false;
    }

    status = MIDISourceCreate(br->midi_client, CFSTR("Maschine MK1 DIN In"), &br->midi_source);
    if (status != noErr) {
        BLOG("CoreMIDI source creation failed: %d", (int)status);
        midi_bridge_stop(br);
        return false;
    }

    status = MIDIDestinationCreate(br->midi_client,
                                   CFSTR("Maschine MK1 DIN Out"),
                                   midi_read_proc,
                                   br,
                                   &br->midi_destination);
    if (status != noErr) {
        BLOG("CoreMIDI destination creation failed: %d", (int)status);
        midi_bridge_stop(br);
        return false;
    }

    BLOG("CoreMIDI ports published: source='Maschine MK1 DIN In' destination='Maschine MK1 DIN Out'");
    return true;
}

static void midi_bridge_stop(bridge_t *br)
{
    if (!br) return;
    if (br->midi_destination) {
        MIDIEndpointDispose(br->midi_destination);
        br->midi_destination = 0;
    }
    if (br->midi_source) {
        MIDIEndpointDispose(br->midi_source);
        br->midi_source = 0;
    }
    if (br->midi_client) {
        MIDIClientDispose(br->midi_client);
        br->midi_client = 0;
    }
}

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
static void show_disconnected_led_state(bridge_t *br);  // forward declaration

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
        show_disconnected_led_state(&g_bridge);
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
static int g_display_tick_ms = 16;
static uint32_t g_display_dirty_mask = 0;
static uint64_t g_display_batch_start_ns[DISPLAY_COUNT] = {0};
static uint16_t g_display_row_start[DISPLAY_COUNT] = {0};
static uint16_t g_display_row_end[DISPLAY_COUNT] = {0};
static uint16_t g_display_byte_x_start[DISPLAY_COUNT] = {0};
static uint16_t g_display_byte_x_end[DISPLAY_COUNT] = {0};
static bool g_display_have_region[DISPLAY_COUNT] = {0};
static dispatch_source_t g_display_flush_timer = NULL;
static dispatch_queue_t g_display_flush_queue = NULL;
static bool g_display_flush_active = false;
static pthread_mutex_t g_display_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_display_partial_flush = false;
static size_t g_display_partial_max_payload = 2048;

typedef struct {
    bridge_t *br;
    uint32_t dirty_mask;
    uint64_t batch_start_ns[DISPLAY_COUNT];
    uint8_t frames[DISPLAY_COUNT][DISPLAY_FB_BYTES];
    uint16_t row_start[DISPLAY_COUNT];
    uint16_t row_end[DISPLAY_COUNT];
    uint16_t byte_x_start[DISPLAY_COUNT];
    uint16_t byte_x_end[DISPLAY_COUNT];
    bool have_region[DISPLAY_COUNT];
} display_flush_job_t;

static void send_full_display_frame(bridge_t *br, uint8_t disp_idx, const uint8_t *fb);
static bool send_partial_display_frame(bridge_t *br,
                                       uint8_t disp_idx,
                                       const uint8_t *fb,
                                       uint16_t row_start,
                                       uint16_t row_end,
                                       uint16_t byte_x_start,
                                       uint16_t byte_x_end,
                                       size_t *payload_len_out);
static void flush_display_updates(bridge_t *br);
static void start_display_flush_timer(bridge_t *br);

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
    ['S'] = {{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    ['a'] = {{0x00,0x0E,0x01,0x0F,0x11,0x11,0x0E}},
    ['c'] = {{0x00,0x0E,0x11,0x10,0x10,0x11,0x0E}},
    ['e'] = {{0x00,0x0E,0x11,0x1F,0x10,0x0F,0x00}},
    ['f'] = {{0x06,0x09,0x08,0x1C,0x08,0x08,0x08}},
    ['h'] = {{0x10,0x10,0x16,0x19,0x11,0x11,0x11}},
    ['i'] = {{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}},
    ['n'] = {{0x00,0x1C,0x12,0x11,0x11,0x11,0x00}},
    ['o'] = {{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}},
    ['p'] = {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x00}},
    ['r'] = {{0x00,0x16,0x19,0x10,0x10,0x10,0x00}},
    ['s'] = {{0x00,0x0E,0x10,0x0E,0x01,0x11,0x0E}},
    ['t'] = {{0x08,0x08,0x1C,0x08,0x08,0x09,0x06}},
    ['w'] = {{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}},
};

static inline void bmp_fill(uint8_t bmp[64][255], uint8_t v)
{
    memset(bmp, v, 64 * 255);
}

static inline void bmp_plot(uint8_t bmp[64][255], int x, int y, uint8_t v)
{
    if (x < 0 || x >= 255 || y < 0 || y >= 64) return;
    bmp[y][x] = v;
}

static void bmp_fill_rect(uint8_t bmp[64][255],
                          int x0,
                          int y0,
                          int x1,
                          int y1,
                          uint8_t v)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x1 < 0 || y1 < 0 || x0 >= 255 || y0 >= 64) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 254) x1 = 254;
    if (y1 > 63) y1 = 63;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) bmp[y][x] = v;
    }
}

static void bmp_fill_ellipse(uint8_t bmp[64][255],
                             int cx,
                             int cy,
                             int rx,
                             int ry,
                             uint8_t v)
{
    if (rx <= 0 || ry <= 0) return;
    for (int y = cy - ry; y <= cy + ry; y++) {
        for (int x = cx - rx; x <= cx + rx; x++) {
            long dx = x - cx;
            long dy = y - cy;
            long lhs = dx * dx * ry * ry + dy * dy * rx * rx;
            long rhs = (long)rx * rx * ry * ry;
            if (lhs <= rhs) bmp_plot(bmp, x, y, v);
        }
    }
}

static void bmp_fill_triangle(uint8_t bmp[64][255],
                              int x1,
                              int y1,
                              int x2,
                              int y2,
                              int x3,
                              int y3,
                              uint8_t v)
{
    int min_x = x1, max_x = x1, min_y = y1, max_y = y1;
    if (x2 < min_x) min_x = x2;
    if (x3 < min_x) min_x = x3;
    if (x2 > max_x) max_x = x2;
    if (x3 > max_x) max_x = x3;
    if (y2 < min_y) min_y = y2;
    if (y3 < min_y) min_y = y3;
    if (y2 > max_y) max_y = y2;
    if (y3 > max_y) max_y = y3;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int w2 = (x3 - x2) * (y - y2) - (y3 - y2) * (x - x2);
            int w3 = (x1 - x3) * (y - y3) - (y1 - y3) * (x - x3);
            bool has_neg = (w1 < 0) || (w2 < 0) || (w3 < 0);
            bool has_pos = (w1 > 0) || (w2 > 0) || (w3 > 0);
            if (!(has_neg && has_pos)) bmp_plot(bmp, x, y, v);
        }
    }
}

static void bmp_pack_st7529(uint8_t *fb, uint8_t bmp[64][255])
{
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

static void render_cat_status_fb(uint8_t *fb)
{
    static uint8_t bmp[64][255];
    const uint8_t white = 0x1F;
    const uint8_t black = 0x00;

    bmp_fill(bmp, white);

    // Simple centered cat silhouette with upright tail.
    bmp_fill_ellipse(bmp, 122, 37, 40, 13, black);    // body
    bmp_fill_ellipse(bmp, 85, 25, 12, 10, black);     // head
    bmp_fill_triangle(bmp, 77, 18, 82, 7, 87, 18, black); // left ear
    bmp_fill_triangle(bmp, 84, 18, 89, 6, 95, 19, black); // right ear
    bmp_fill_rect(bmp, 98, 42, 105, 57, black);       // front leg
    bmp_fill_rect(bmp, 114, 42, 121, 57, black);      // front leg
    bmp_fill_rect(bmp, 133, 43, 140, 57, black);      // rear leg
    bmp_fill_rect(bmp, 147, 42, 154, 57, black);      // rear leg
    bmp_fill_ellipse(bmp, 83, 34, 8, 4, white);       // neck cut-in
    bmp_fill_ellipse(bmp, 161, 22, 5, 18, black);     // tail upright
    bmp_fill_ellipse(bmp, 155, 9, 8, 5, black);       // tail curl tip
    bmp_fill_ellipse(bmp, 157, 29, 5, 7, black);      // tail base
    bmp_fill_rect(bmp, 70, 57, 160, 59, black);       // ground shadow line

    bmp_pack_st7529(fb, bmp);
}

// Render up to two lines of text into the 10880-byte ST7529 framebuffer.
static void render_status_fb(uint8_t *fb,
                             const char *line1,
                             const char *line2,
                             bool invert)
{
    // Work in 255×64 logical pixel space (one byte per pixel, 5-bit value 0x00/0x1F).
    static uint8_t bmp[64][255];
    memset(bmp, invert ? 0x1F : 0x00, sizeof(bmp));

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
                    uint8_t v  = on ? (invert ? 0x00 : 0x1F) : (invert ? 0x1F : 0x00);
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

    bmp_pack_st7529(fb, bmp);
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

static void send_full_display_frame(bridge_t *br, uint8_t disp_idx, const uint8_t *fb)
{
    uint64_t start_ns = timing_now_ns();
    uint8_t usb_base;
    static const uint8_t row_cmd[3] = { 0x75, 0x00, 0x3f };   // rows 0–63
    static const uint8_t col_cmd[3] = { 0x15, 0x00, 0x54 };   // cols 0–84
    uint8_t *frame;
    size_t total;
    size_t offset;
    bool ok;

    if (!br || !br->usb || disp_idx >= DISPLAY_COUNT) return;

    usb_base = disp_idx == 0 ? 0x00 : 0x02;
    TLOG("usb-display-begin disp=%u usb_base=0x%02x frame_bytes=%d ts_ns=%llu",
         disp_idx, usb_base, 1 + DISPLAY_FB_BYTES, (unsigned long long)start_ns);
    mk1_set_display(br->usb, usb_base, row_cmd, sizeof(row_cmd));
    mk1_set_display(br->usb, usb_base, col_cmd, sizeof(col_cmd));

    frame = malloc(1 + DISPLAY_FB_BYTES);
    if (!frame) return;
    frame[0] = ST7529_RAMWR;
    memcpy(frame + 1, fb, DISPLAY_FB_BYTES);

    total = 1 + DISPLAY_FB_BYTES;
    offset = 0;
    ok = true;

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
    TLOG("usb-display-end disp=%u usb_base=0x%02x ok=%d dur_us=%llu",
         disp_idx,
         usb_base,
         ok ? 1 : 0,
         (unsigned long long)((timing_now_ns() - start_ns) / 1000ULL));
}

static bool send_partial_display_frame(bridge_t *br,
                                       uint8_t disp_idx,
                                       const uint8_t *fb,
                                       uint16_t row_start,
                                       uint16_t row_end,
                                       uint16_t byte_x_start,
                                       uint16_t byte_x_end,
                                       size_t *payload_len_out)
{
    uint64_t start_ns = timing_now_ns();
    uint8_t usb_base;
    uint8_t row_cmd[3];
    uint8_t col_cmd[3];
    uint8_t *frame = NULL;
    size_t rows;
    size_t bytes_per_row;
    size_t payload_len;
    size_t total;
    size_t offset;
    bool ok = true;

    if (!br || !br->usb || !fb || disp_idx >= DISPLAY_COUNT) return false;
    if (row_start > row_end || row_end >= DISPLAY_ROWS) return false;
    if (byte_x_start > byte_x_end || byte_x_end >= DISPLAY_BYTES_PER_ROW) return false;
    if ((byte_x_start & 1u) != 0 || (byte_x_end & 1u) != 1) return false;

    rows = (size_t)(row_end - row_start + 1);
    bytes_per_row = (size_t)(byte_x_end - byte_x_start + 1);
    if ((bytes_per_row & 1u) != 0) return false;

    usb_base = disp_idx == 0 ? 0x00 : 0x02;
    row_cmd[0] = 0x75;
    row_cmd[1] = (uint8_t)row_start;
    row_cmd[2] = (uint8_t)row_end;
    col_cmd[0] = 0x15;
    col_cmd[1] = (uint8_t)(byte_x_start / 2);
    col_cmd[2] = (uint8_t)(byte_x_end / 2);

    payload_len = rows * bytes_per_row;
    if (payload_len_out) *payload_len_out = payload_len;
    frame = malloc(1 + payload_len);
    if (!frame) return false;
    frame[0] = ST7529_RAMWR;
    for (size_t r = 0; r < rows; r++) {
        memcpy(frame + 1 + r * bytes_per_row,
               fb + ((size_t)row_start + r) * DISPLAY_BYTES_PER_ROW + byte_x_start,
               bytes_per_row);
    }

    TLOG("usb-display-partial-begin disp=%u usb_base=0x%02x rows=%u-%u cols=%u-%u frame_bytes=%zu ts_ns=%llu",
         disp_idx,
         usb_base,
         row_start,
         row_end,
         (unsigned)(byte_x_start / 2),
         (unsigned)(byte_x_end / 2),
         1 + payload_len,
         (unsigned long long)start_ns);

    mk1_set_display(br->usb, usb_base, row_cmd, sizeof(row_cmd));
    mk1_set_display(br->usb, usb_base, col_cmd, sizeof(col_cmd));

    total = 1 + payload_len;
    offset = 0;
    while (offset < total && ok) {
        size_t chunk = (total - offset) > EP8_CHUNK_MAX
                     ? EP8_CHUNK_MAX : (total - offset);
        uint8_t disp = (offset == 0) ? usb_base : (uint8_t)(usb_base | 0x01);
        ok = mk1_set_display(br->usb, disp, frame + offset, chunk);
        offset += chunk;
    }

    free(frame);

    TLOG("usb-display-partial-end disp=%u usb_base=0x%02x ok=%d dur_us=%llu payload=%zu",
         disp_idx,
         usb_base,
         ok ? 1 : 0,
         (unsigned long long)((timing_now_ns() - start_ns) / 1000ULL),
         payload_len);
    return ok;
}

static void flush_display_updates(bridge_t *br)
{
    display_flush_job_t *job;

    pthread_mutex_lock(&g_display_lock);
    if (g_display_flush_active || g_display_dirty_mask == 0) {
        pthread_mutex_unlock(&g_display_lock);
        return;
    }
    g_display_flush_active = true;
    job = calloc(1, sizeof(*job));
    if (!job) {
        g_display_flush_active = false;
        pthread_mutex_unlock(&g_display_lock);
        return;
    }
    job->br = br;
    job->dirty_mask = g_display_dirty_mask;
    memcpy(job->batch_start_ns, g_display_batch_start_ns, sizeof(g_display_batch_start_ns));
    memcpy(job->frames, g_display_fb, sizeof(g_display_fb));
    memcpy(job->row_start, g_display_row_start, sizeof(g_display_row_start));
    memcpy(job->row_end, g_display_row_end, sizeof(g_display_row_end));
    memcpy(job->byte_x_start, g_display_byte_x_start, sizeof(g_display_byte_x_start));
    memcpy(job->byte_x_end, g_display_byte_x_end, sizeof(g_display_byte_x_end));
    memcpy(job->have_region, g_display_have_region, sizeof(g_display_have_region));
    g_display_dirty_mask = 0;
    memset(g_display_batch_start_ns, 0, sizeof(g_display_batch_start_ns));
    memset(g_display_row_start, 0, sizeof(g_display_row_start));
    memset(g_display_row_end, 0, sizeof(g_display_row_end));
    memset(g_display_byte_x_start, 0, sizeof(g_display_byte_x_start));
    memset(g_display_byte_x_end, 0, sizeof(g_display_byte_x_end));
    memset(g_display_have_region, 0, sizeof(g_display_have_region));
    pthread_mutex_unlock(&g_display_lock);

    dispatch_async(g_display_flush_queue, ^{
        for (uint8_t disp_idx = 0; disp_idx < DISPLAY_COUNT; disp_idx++) {
            size_t partial_payload = 0;

            if ((job->dirty_mask & (1u << disp_idx)) == 0) continue;
            if (job->batch_start_ns[disp_idx] != 0) {
                TLOG("display-batch-flush disp=%u batch_us=%llu",
                     disp_idx,
                     (unsigned long long)((timing_now_ns() - job->batch_start_ns[disp_idx]) / 1000ULL));
            }

            if (g_display_partial_flush &&
                job->have_region[disp_idx]) {
                partial_payload =
                    ((size_t)(job->row_end[disp_idx] - job->row_start[disp_idx] + 1)) *
                    ((size_t)(job->byte_x_end[disp_idx] - job->byte_x_start[disp_idx] + 1));
                if (partial_payload <= g_display_partial_max_payload &&
                    send_partial_display_frame(job->br,
                                               disp_idx,
                                               job->frames[disp_idx],
                                               job->row_start[disp_idx],
                                               job->row_end[disp_idx],
                                               job->byte_x_start[disp_idx],
                                               job->byte_x_end[disp_idx],
                                               NULL)) {
                    continue;
                }
                TLOG("usb-display-partial-skip disp=%u payload=%zu limit=%zu",
                     disp_idx, partial_payload, g_display_partial_max_payload);
            }
            send_full_display_frame(job->br, disp_idx, job->frames[disp_idx]);
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            pthread_mutex_lock(&g_display_lock);
            g_display_flush_active = false;
            bool have_more = (g_display_dirty_mask != 0);
            pthread_mutex_unlock(&g_display_lock);
            free(job);
            if (have_more) {
                flush_display_updates(br);
            }
        });
    });
}

static void start_display_flush_timer(bridge_t *br)
{
    if (g_display_flush_timer) return;
    if (!g_display_flush_queue) {
        g_display_flush_queue = dispatch_queue_create("com.dragco.mk1.display-flush",
                                                      DISPATCH_QUEUE_SERIAL);
    }

    g_display_flush_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                                   dispatch_get_main_queue());
    if (!g_display_flush_timer) return;

    dispatch_source_set_timer(g_display_flush_timer,
                              dispatch_time(DISPATCH_TIME_NOW,
                                            (int64_t)g_display_tick_ms * NSEC_PER_MSEC),
                              (uint64_t)g_display_tick_ms * NSEC_PER_MSEC,
                              (uint64_t)1 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_display_flush_timer, ^{
        if (!g_running) return;
        if (g_display_dirty_mask == 0) return;
        flush_display_updates(br);
    });
    dispatch_resume(g_display_flush_timer);
}

// Render and send the disconnected status screens.
static void show_status_screen(bridge_t *br)
{
    if (!br->usb) return;
    uint8_t left_fb[DISPLAY_FB_BYTES];
    uint8_t right_fb[DISPLAY_FB_BYTES];

    render_cat_status_fb(left_fb);
    render_status_fb(right_fb, "Open Maschine", "Software", true);

    send_fb_to_display(br, 0x00, left_fb);
    send_fb_to_display(br, 0x02, right_fb);
    DLOG("status screen shown");
}

static void forward_display(bridge_t *br, const uint8_t *raw_msg, size_t raw_len)
{
    uint64_t start_ns = timing_now_ns();

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
    TLOG("ipc-rx display disp=%u raw_len=%zu y=%u x=%u h=%u w=%u pixel_len=%u ts_ns=%llu",
         disp_idx, raw_len, y, x, h, w, pixel_len, (unsigned long long)start_ns);

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
    pthread_mutex_lock(&g_display_lock);
    for (uint16_t r = 0; r < h; r++) {
        memcpy(g_display_fb[disp_idx] + (y + r) * DISPLAY_BYTES_PER_ROW + byte_x,
               pixels + r * bytes_per_row,
               bytes_per_row);
    }
    if (g_display_batch_start_ns[disp_idx] == 0) {
        g_display_batch_start_ns[disp_idx] = start_ns;
    }
    // Partial mode performs best when we keep the newest region instead of
    // unioning many small updates into a large near-full-frame box.
    g_display_row_start[disp_idx] = y;
    g_display_row_end[disp_idx] = (uint16_t)(y + h - 1);
    g_display_byte_x_start[disp_idx] = (uint16_t)byte_x;
    g_display_byte_x_end[disp_idx] = (uint16_t)(byte_x + bytes_per_row - 1);
    g_display_have_region[disp_idx] = true;
    g_display_dirty_mask |= (1u << disp_idx);
    pthread_mutex_unlock(&g_display_lock);
    TLOG("display-batch-queued disp=%u dirty_mask=0x%x queue_us=%llu",
         disp_idx,
         g_display_dirty_mask,
         (unsigned long long)((timing_now_ns() - start_ns) / 1000ULL));
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
// EP1 DIMM_LEDS format (confirmed by CABL's MaschineMK1 implementation):
//   Packet A: [0x0c, 0x00, device_leds[0..30]]   — 33 bytes total
//   Packet B: [0x0c, 0x1e, device_leds[31..61]]  — 33 bytes total
//
// Build one authoritative 62-slot device LED array, then slice it into the two
// USB writes. This matches CABL's `m_leds[62]` model and avoids treating the
// two packets as unrelated mapping domains.
// ---------------------------------------------------------------------------

static void learn_on_led(const uint8_t *logical, size_t len); // defined after run_led_probe
static void project_capture_on_led(const uint8_t *logical, size_t logical_len,
                                   const uint8_t *phys, size_t phys_len);
static bool env_truthy(const char *name);
static int env_int_or_default(const char *name, int fallback);
static bool parse_size_arg(const char *arg, const char *prefix, size_t *out_value);
static bool parse_int_arg(const char *arg, const char *prefix, int *out_value);
static void print_usage(const char *prog);

typedef enum {
    MK1_LED_EVIDENCE_PROVISIONAL = 0,
    MK1_LED_EVIDENCE_STABLE      = 1,
    MK1_LED_EVIDENCE_CONFIRMED   = 2,
} mk1_led_evidence_t;

typedef struct {
    uint8_t phys_idx;
    const char *label;
    mk1_led_evidence_t evidence;
} mk1_apple_phys_label_t;

typedef struct {
    uint8_t logical_idx;
    uint8_t phys_idx;
    const char *label;
    mk1_led_evidence_t evidence;
} mk1_apple_redirect_evidence_t;

static const mk1_apple_phys_label_t k_apple_phys_labels[] = {
    {  1, "Transport Left", MK1_LED_EVIDENCE_PROVISIONAL },
    {  2, "Restart",        MK1_LED_EVIDENCE_PROVISIONAL },
    {  3, "H",              MK1_LED_EVIDENCE_PROVISIONAL },
    {  4, "G",              MK1_LED_EVIDENCE_PROVISIONAL },
    {  5, "D",              MK1_LED_EVIDENCE_PROVISIONAL },
    {  6, "C",              MK1_LED_EVIDENCE_PROVISIONAL },
    {  7, "F",              MK1_LED_EVIDENCE_PROVISIONAL },
    {  8, "E",              MK1_LED_EVIDENCE_PROVISIONAL },
    {  9, "B",              MK1_LED_EVIDENCE_PROVISIONAL },
    { 10, "A",              MK1_LED_EVIDENCE_PROVISIONAL },
    { 11, "Auto-Write",     MK1_LED_EVIDENCE_PROVISIONAL },
    { 12, "Snap",           MK1_LED_EVIDENCE_PROVISIONAL },
    { 13, "Modules Right",  MK1_LED_EVIDENCE_PROVISIONAL },
    { 14, "Modules Left",   MK1_LED_EVIDENCE_PROVISIONAL },
    { 15, "Sampling",       MK1_LED_EVIDENCE_PROVISIONAL },
    { 16, "Browse",         MK1_LED_EVIDENCE_PROVISIONAL },
    { 17, "Step",           MK1_LED_EVIDENCE_STABLE },
    { 18, "Control",        MK1_LED_EVIDENCE_STABLE },
    { 19, "SA8",            MK1_LED_EVIDENCE_STABLE },
    { 20, "SA7",            MK1_LED_EVIDENCE_STABLE },
    { 21, "SA6",            MK1_LED_EVIDENCE_STABLE },
    { 22, "SA5",            MK1_LED_EVIDENCE_STABLE },
    { 23, "SA4",            MK1_LED_EVIDENCE_STABLE },
    { 24, "SA3",            MK1_LED_EVIDENCE_STABLE },
    { 25, "SA2",            MK1_LED_EVIDENCE_STABLE },
    { 26, "SA1",            MK1_LED_EVIDENCE_STABLE },
    { 27, "Note Repeat",    MK1_LED_EVIDENCE_STABLE },
};

static const mk1_apple_redirect_evidence_t k_apple_redirect_evidence[] = {
    // Record is still retained as a runtime redirect; Mute/Solo direct-slot claims
    // have been rolled back after verify runs failed across lower and upper ranges.
};

static const char *apple_evidence_name(mk1_led_evidence_t evidence)
{
    switch (evidence) {
        case MK1_LED_EVIDENCE_PROVISIONAL: return "provisional";
        case MK1_LED_EVIDENCE_STABLE:      return "stable";
        case MK1_LED_EVIDENCE_CONFIRMED:   return "confirmed";
    }
    return "unknown";
}

static const mk1_apple_phys_label_t *apple_phys_label_entry(uint8_t phys_idx)
{
    for (size_t i = 0; i < sizeof(k_apple_phys_labels) / sizeof(k_apple_phys_labels[0]); i++) {
        if (k_apple_phys_labels[i].phys_idx == phys_idx) return &k_apple_phys_labels[i];
    }
    return NULL;
}

static const mk1_apple_redirect_evidence_t *apple_redirect_evidence_entry(uint8_t logical_idx)
{
    for (size_t i = 0; i < sizeof(k_apple_redirect_evidence) / sizeof(k_apple_redirect_evidence[0]); i++) {
        if (k_apple_redirect_evidence[i].logical_idx == logical_idx) {
            return &k_apple_redirect_evidence[i];
        }
    }
    return NULL;
}

static bool apple_upper_has_non_passthrough_override(const void *entries_void, int count, uint8_t logical_idx)
{
    const struct {
        uint8_t log_idx;
        uint8_t override;
        const char *name;
    } *entries = entries_void;

    for (int i = 0; i < count; i++) {
        if (entries[i].log_idx != logical_idx) continue;
        if (entries[i].override != 0xFF) return true;
    }
    return false;
}

static bool suppress_project_dim_cluster_slot(uint8_t logical_idx)
{
    // Project captures on Apple Silicon show this low legacy cluster asserting dim
    // LEDs that do not light on Intel for the same projects. Keep Groups G/H out
    // of this mask until they are isolated separately.
    switch (logical_idx) {
        case 4:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
        case 13:
        case 14:
        case 15:
            return true;
        default:
            return false;
    }
}

static void emit_led_state(bridge_t *br, const uint8_t *led_logical, size_t full_len)
{
    // Base linear remap for selector-6 LEDs.
    //
    // This is a legacy logical->phys ordering used as the starting point before any
    // Apple Silicon overrides are applied. The recent guided probe work gives a much
    // better picture of what the physical slots are, but it does NOT by itself prove
    // which NI logical byte drives each slot. Keep that distinction explicit:
    //
    //   direct probe    => "phys[N] visibly lights <label>"
    //   bridge remap    => "logical[N] currently forwards to phys[M]"
    //
    // Pad LED experiments stay out of the default path.
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
    // The old comment block here leaned too heavily on project-capture inference.
    // Current evidence should be read like this instead:
    //
    //   1. `PROJCAP phys` is bridge output, not hardware truth.
    //   2. Guided direct probe gives reliable physical slot labels.
    //   3. Only a small subset of logical->phys redirects are actually confirmed.
    //
    // Evidence is tracked separately from runtime remap behavior:
    //   k_apple_phys_labels          = current physical-slot labels from guided probe
    //   k_apple_redirect_evidence    = logical->phys redirects strong enough to encode
    //
    // Current interpretation:
    //   phys[ 1..16] = provisional project-context labels
    //   phys[17..27] = stable direct-probe labels
    //   direct-slot redirects for Mute/Solo/Record are NOT currently trusted
    //   the currently known-bad fallback slots stay suppressed on Apple Silicon
    //   until isolated press-to-visible-slot evidence exists
    //
    // For phys[28..32], guided probe currently shows no useful visible effect beyond
    // phys[28] acting like a backlight/control-adjacent slot.
    //
    // NOTE: the table below is intentionally conservative. Suppression here means
    // "known bad or untrusted on Apple Silicon", not "known absent in hardware".
    static const struct {
        uint8_t log_idx;
        uint8_t override;   // 0xFF=passthrough, 0xFE=suppress, 0..31=redirect
        const char *name;
    } k_apple_upper[] = {
        // --- Scene-row buttons: suppress from Packet B; handled in device_leds[16..29] ---
        { 16, 0xFE, "Mute (scene-row)"          },  // assumed
        { 17, 0xFE, "Solo (scene-row)"           },  // confirmed
        { 18, 0xFE, "Select (scene-row)"         },  // confirmed
        { 19, 0xFE, "Duplicate (scene-row)"      },  // confirmed
        { 20, 0xFE, "Navigate (scene-row)"       },  // confirmed
        { 21, 0xFE, "PadMode (scene-row)"        },  // confirmed
        { 22, 0xFE, "Pattern (scene-row)"        },  // confirmed
        { 23, 0xFE, "Scene (scene-row)"          },  // confirmed
        { 24, 0xFE, "Shift (scene-row)"          },  // assumed sequential
        { 25, 0xFE, "Erase (scene-row)"          },  // confirmed
        { 26, 0xFE, "Grid (scene-row)"           },  // confirmed
        { 28, 0xFE, "Record (scene-row)"         },  // confirmed
        { 29, 0xFE, "Play (scene-row)"           },  // confirmed
        { 30, 0xFE, "TransportLeft (scene-row)"  },  // confirmed

        // --- TransportRight: suppress Packet B legacy path; handled directly at slot 27 ---
        // logical[27] = TransportRight confirmed from learn 2026-04-13.
        // The base linear Packet B path lands on the wrong second-bank slot.
        { 27, 0xFE, "TRight (suppress until slot confirmed)" },

        // Lower-range slots observed active in project-context runs:
        {  1, 0xFF, "Group G"  }, {  2, 0xFF, "Group H"  }, {  3, 0xFE, "log3"  },
        {  4, 0xFF, "log4"    }, {  5, 0xFF, "log5"    }, {  6, 0xFF, "Group F"  },
        {  7, 0xFF, "Group C"  }, {  8, 0xFF, "log8"   }, {  9, 0xFF, "log9"  },
        { 10, 0xFF, "log10" }, { 11, 0xFF, "log11" }, { 12, 0xFF, "log12" },
        { 13, 0xFF, "log13" }, { 14, 0xFF, "log14" }, { 15, 0xFF, "log15" },

        // Restart is driven by logical[31]. Pressing Restart also makes Maschine
        // assert Play, so logical[29] changes are expected collateral and should
        // not be treated as Restart evidence. Keep this redirect after the
        // low-range suppressions so they do not clear phys[2] after we write it.
        { 31, 2,    "Restart" },
    };
    static const int k_apple_upper_n =
        (int)(sizeof(k_apple_upper) / sizeof(k_apple_upper[0]));

    if (!br->usb) {
        LLOG("no USB device");
        return;
    }

    for (size_t i = 32; i < full_len && i < 57; i++) {
        if (led_logical[i] == 0) continue;
        if (i >= 37 && i <= 52) continue; // rubber pad range (logical[37..52] = pads 1..16)
        LLOG("unresolved logical[%zu]=0x%02x", i, led_logical[i]);
    }

    uint8_t second_block[32] = {0}; // index 1..31 maps to device slots 31..61

    // Route IPC logical[0..31] into the CABL-aligned second block.
    // k_hw_by_logical[N] is the legacy second-block-relative index.
    for (size_t log_idx = 0; log_idx < 32 && log_idx < full_len; log_idx++) {
        uint8_t phy_idx = k_hw_by_logical[log_idx];
        if (phy_idx == 0) continue;
        if (phy_idx < 32) {
            second_block[phy_idx] = led_logical[log_idx];
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

        if (getenv("MK1_SUPPRESS_APPLE_PROJECT_DIM_CLUSTER") &&
            suppress_project_dim_cluster_slot(li)) {
            if (old_phys < 32) second_block[old_phys] = 0;
            LLOG("upper-slot suppressed by MK1_SUPPRESS_APPLE_PROJECT_DIM_CLUSTER: %s logical[%u]=0x%02x cleared phys[%u]",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)old_phys);
            continue;
        }

        if (ov == 0xFF) {
            if (apple_upper_has_non_passthrough_override(k_apple_upper, k_apple_upper_n, li)) {
                continue;
            }
            // passthrough — linear mapping already applied; log for correlation
            const mk1_apple_phys_label_t *phys = apple_phys_label_entry(old_phys);
            LLOG("upper-slot active: %s logical[%u]=0x%02x -> phys[%u]%s%s%s%s%s (linear)",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)old_phys,
                 phys ? " " : "",
                 phys ? phys->label : "",
                 phys ? " [" : "",
                 phys ? apple_evidence_name(phys->evidence) : "",
                 phys ? "]" : "");
        } else if (ov == 0xFE) {
            // suppress — clear the linearly-written byte, do not forward
            if (old_phys < 32) second_block[old_phys] = 0;
            LLOG("upper-slot suppressed: %s logical[%u]=0x%02x cleared phys[%u]",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)old_phys);
        } else {
            // redirect — clear linear byte, write value to override phys
            const mk1_apple_redirect_evidence_t *claim = apple_redirect_evidence_entry(li);
            const mk1_apple_phys_label_t *phys = apple_phys_label_entry(ov);
            if (old_phys < 32) second_block[old_phys] = 0;
            if (ov < 32)       second_block[ov] = led_logical[li];
            LLOG("upper-slot redirect: %s logical[%u]=0x%02x -> phys[%u]%s%s%s%s%s (was phys[%u], %s%s%s)",
                 k_apple_upper[k].name, (unsigned)li, led_logical[li], (unsigned)ov,
                 phys ? " " : "",
                 phys ? phys->label : "",
                 phys ? " [" : "",
                 phys ? apple_evidence_name(phys->evidence) : "",
                 phys ? "]" : "",
                 (unsigned)old_phys,
                 claim ? claim->label : "runtime-only",
                 claim ? ", " : "",
                 claim ? apple_evidence_name(claim->evidence) : "");
        }
    }

    // Selected extended logical indices are now strong enough to route directly.
    // These are outside the legacy 0..31 selector-6 range, so they are handled
    // separately after the base remap/override pass.
    static const struct {
        uint8_t log_idx;
        uint8_t phys_idx;
        const char *name;
    } k_extended_upper[] = {
        { 56, 27, "Note Repeat" }, // confirmed visually in normal bridge workflow
    };
    for (size_t k = 0; k < sizeof(k_extended_upper) / sizeof(k_extended_upper[0]); k++) {
        uint8_t li = k_extended_upper[k].log_idx;
        uint8_t phys = k_extended_upper[k].phys_idx;
        if ((size_t)li >= full_len || led_logical[li] == 0) continue;
        if (phys < 32) second_block[phys] = led_logical[li];
        LLOG("extended-slot redirect: %s logical[%u]=0x%02x -> phys[%u]",
             k_extended_upper[k].name, (unsigned)li, led_logical[li], (unsigned)phys);
    }

    // TransportRight is still unresolved. Allow a runtime Packet B candidate phys
    // slot to be tested without another source edit.
    {
        int tright_phys = env_int_or_default("MK1_TRANSPORT_RIGHT_B_PHYS", -1);
        if (tright_phys >= 0 && tright_phys < 32 &&
            full_len > 27 && led_logical[27] != 0) {
            second_block[tright_phys] = led_logical[27];
            LLOG("upper-slot override: TransportRight logical[27]=0x%02x -> phys[%d]",
                 led_logical[27], tright_phys);
        }
    }

    if (env_truthy("MK1_PROJECT_CAPTURE")) {
        for (size_t i = 1; i <= 16; i++) second_block[i] = 0;
        second_block[31] = 0;
    }

    uint8_t device_leds[62] = {0};

    // Packet A pad rubbers: IPC logical[0..15] = CABL m_leds[0..15] (Pad4..Pad13, hardware order).
    // These also correlate with group membership — when a group is selected, its representative
    // pad rubber shows 0x32 at the same logical index as the pad's CABL rubber slot.
    for (size_t i = 0; i < 16 && i < full_len; i++) {
        device_leds[i] = led_logical[i];
    }

    // Scene-row: logical[] index → device_leds[] slot (absolute device order).
    static const struct { uint8_t log_idx; uint8_t dev_slot; } k_scene_row[] = {
        { 16, 16 },  // Mute          (assumed by position)
        { 17, 17 },  // Solo          (confirmed)
        { 18, 18 },  // Select        (confirmed)
        { 19, 19 },  // Duplicate     (confirmed)
        { 20, 20 },  // Navigate      (confirmed)
        { 21, 21 },  // Pad Mode      (confirmed)
        { 22, 22 },  // Pattern       (confirmed)
        { 23, 23 },  // Scene         (confirmed)
        { 24, 24 },  // Shift         (assumed sequential)
        { 25, 25 },  // Erase         (confirmed)
        { 26, 26 },  // Grid          (confirmed)
        { 27, 27 },  // TransportRight
        { 28, 28 },  // Record        (confirmed)
        { 29, 29 },  // Play          (confirmed)
    };
    static const int k_scene_row_n = (int)(sizeof(k_scene_row)/sizeof(k_scene_row[0]));

    // Scene-row buttons in the first 31-slot device window.
    for (int k = 0; k < k_scene_row_n; k++) {
        uint8_t li = k_scene_row[k].log_idx;
        uint8_t slot = k_scene_row[k].dev_slot;
        if ((size_t)li < full_len) {
            device_leds[slot] = led_logical[li];
        }
    }

    // Copy the legacy second packet-relative data into the absolute device LED array.
    for (int rel = 1; rel <= 31; rel++) {
        device_leds[30 + rel] = second_block[rel];
    }

    // CABL places TransportLeft/Loop (Restart) at the start of the second window.
    if (full_len > 30) device_leds[31] = led_logical[30];  // TransportLeft

    // Apple Silicon second-block: IPC logical[N] -> device_leds[N+1] for N=32..56.
    //
    // The IPC payload is the CABL m_leds[] array with Unused1 (m_leds[30]) skipped,
    // shifting all subsequent indices by -1:
    //   logical[32] = m_leds[33] = GroupH  -> device_leds[33] = phys[3]
    //   logical[33] = m_leds[34] = GroupG  -> device_leds[34] = phys[4]
    //   logical[34] = m_leds[35] = GroupD  -> device_leds[35] = phys[5]
    //   logical[35] = m_leds[36] = GroupC  -> device_leds[36] = phys[6]
    //   logical[36] = m_leds[37] = GroupF  -> device_leds[37] = phys[7]
    //   logical[37] = m_leds[38] = GroupE  -> device_leds[38] = phys[8]
    //   logical[38] = m_leds[39] = GroupB  -> device_leds[39] = phys[9]
    //   logical[39] = m_leds[40] = GroupA  -> device_leds[40] = phys[10]
    //   logical[40] = m_leds[41] = AutoWrite -> device_leds[41] = phys[11]
    //   logical[41] = m_leds[42] = Snap    -> device_leds[42] = phys[12]
    //   ...
    //   logical[56] = m_leds[57] = NoteRepeat -> device_leds[57] = phys[27]
    //
    // This also overwrites any stale pad-rubber values that k_hw_by_logical
    // placed into group positions (logical[0..15] = CABL pad rubbers land on
    // phys[3..10] via the legacy table; this pass corrects them).
    for (uint8_t li = 32; li <= 56 && (size_t)li < full_len; li++) {
        uint8_t dev_slot = (uint8_t)(li + 1);
        if (dev_slot < 62 && led_logical[li] != 0) device_leds[dev_slot] = led_logical[li];
    }

    if (g_led_autowrite_pressed) {
        device_leds[41] = 0x32;
    }

    // Keep the display backlight asserted during normal LED traffic, matching CABL's
    // persistent m_leds[DisplayBacklight] behavior after init.
    device_leds[58] = 0x5c;

    // TransportRight is still configurable at runtime for slot testing.
    {
        int tright_slot = env_int_or_default("MK1_TRANSPORT_RIGHT_A_SLOT", -1);
        if (tright_slot >= 0 && tright_slot <= 30 &&
            full_len > 27 && led_logical[27] != 0) {
            device_leds[tright_slot] = led_logical[27];
            LLOG("scene-row override: TransportRight logical[27]=0x%02x -> device slot %d",
                 led_logical[27], tright_slot);
        }
    }

    uint8_t capture_phys[33] = {0};
    capture_phys[0] = 0x1e;
    memcpy(capture_phys + 1, second_block + 1, 31);
    project_capture_on_led(led_logical, full_len, capture_phys, 33);

    uint8_t packet_a[33] = {0};
    packet_a[0] = 0x0c;
    packet_a[1] = 0x00;
    memcpy(packet_a + 2, device_leds, 31);

    {
        char hex[33 * 3 + 1];
        for (size_t i = 0; i < 33; i++) snprintf(hex + i*3, 4, "%02x ", packet_a[i]);
        hex[33 * 3] = '\0';
        LLOG("EP1 A offset=0x00 (33): %s", hex);
    }

    if (!mk1_device_write_endpoint(br->usb, 0x01, packet_a, 33)) {
        LLOG("EP1 A write FAILED");
    }

    uint8_t packet_b[33] = {0};
    packet_b[0] = 0x0c;
    packet_b[1] = 0x1e;
    memcpy(packet_b + 2, device_leds + 31, 31);

    {
        char hex[33 * 3 + 1];
        for (size_t i = 0; i < 33; i++) snprintf(hex + i*3, 4, "%02x ", packet_b[i]);
        hex[33 * 3] = '\0';
        LLOG("EP1 B offset=0x1e (33): %s", hex);
    }

    if (!mk1_device_write_endpoint(br->usb, 0x01, packet_b, 33)) {
        LLOG("EP1 B write FAILED");
    }
}

static void forward_led(bridge_t *br, const uint8_t *raw_msg, size_t raw_len)
{
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

    const size_t cache_len = full_len < sizeof(g_last_led_logical) ? full_len : sizeof(g_last_led_logical);
    memcpy(g_last_led_logical, led_logical, cache_len);
    if (cache_len < sizeof(g_last_led_logical)) {
        memset(g_last_led_logical + cache_len, 0, sizeof(g_last_led_logical) - cache_len);
    }
    g_last_led_logical_len = cache_len;

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

    emit_led_state(br, led_logical, full_len);
}

static void refresh_led_from_cache(bridge_t *br)
{
    if (!br || !br->usb) return;
    if (g_last_led_logical_len < 32) return;
    LLOG("refreshing LEDs from cached logical state (len=%zu)", g_last_led_logical_len);
    emit_led_state(br, g_last_led_logical, g_last_led_logical_len);
}

static void show_disconnected_led_state(bridge_t *br)
{
    if (!br || !br->usb) return;

    enum { CONTROL_LOGICAL_INDEX = 47 };
    uint8_t led_logical[128] = {0};
    size_t logical_len = g_last_led_logical_len;
    if (logical_len <= CONTROL_LOGICAL_INDEX) logical_len = CONTROL_LOGICAL_INDEX + 1;
    if (logical_len > sizeof(led_logical)) logical_len = sizeof(led_logical);

    uint8_t control_level = 0x32;
    if (g_last_led_logical_len > CONTROL_LOGICAL_INDEX &&
        g_last_led_logical[CONTROL_LOGICAL_INDEX] != 0) {
        control_level = g_last_led_logical[CONTROL_LOGICAL_INDEX];
    }
    led_logical[CONTROL_LOGICAL_INDEX] = control_level;

    memcpy(g_last_led_logical, led_logical, logical_len);
    if (logical_len < sizeof(g_last_led_logical)) {
        memset(g_last_led_logical + logical_len, 0, sizeof(g_last_led_logical) - logical_len);
    }
    g_last_led_logical_len = logical_len;

    LLOG("showing disconnected LED state: Control only");
    emit_led_state(br, led_logical, logical_len);
}

// ---------------------------------------------------------------------------
// Server command callback
// ---------------------------------------------------------------------------

static void on_cmd(uint32_t cmd_type, const uint8_t *raw_msg, size_t raw_len, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    TLOG("ipc-rx cmd=0x%08x raw_len=%zu ts_ns=%llu",
         cmd_type, raw_len, (unsigned long long)timing_now_ns());
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

    maybe_start_guided_mode(br);
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

// Runtime-configurable pad thresholds (set via environment variables or MK1 Revive Settings).
// Defaults: pressure=200, hit_on=300, hit_off=150, debounce=10ms.
// Override at launch via MK1_PAD_PRESSURE, MK1_PAD_HIT_ON, MK1_PAD_HIT_OFF, MK1_PAD_DEBOUNCE_MS.
static int      g_pad_pressure_threshold = 200;   // MK1_PAD_PRESSURE
static int      g_pad_hit_on_threshold   = 300;   // MK1_PAD_HIT_ON
static int      g_pad_hit_off_threshold  = 150;   // MK1_PAD_HIT_OFF
static uint64_t g_pad_debounce_ns        = 10000000ULL; // MK1_PAD_DEBOUNCE_MS (ms → ns)

static uint16_t g_prev_pressure[16]      = {0};  // for hit_on/hit_off transitions
static uint16_t g_sent_pressure[16]      = {0};  // last pressure value forwarded via IPC
static uint64_t g_hit_off_time_ns[16]    = {0};  // timestamp of last hit_off per pad

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

    uint64_t start_ns = timing_now_ns();
    bool ok = mk1_server_send_event(srv, buf, sizeof(buf));
    TLOG("ipc-tx pad idx=%u ev=%u ok=%d dur_us=%llu value=%.4f hw_ts_ns=%llu",
         pad_index,
         event_type,
         ok ? 1 : 0,
         (unsigned long long)((timing_now_ns() - start_ns) / 1000ULL),
         (double)value,
         (unsigned long long)ts_ns);
}

static void on_pad(const mk1_pad_event_t *pads, uint8_t count, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    if (!br->srv || !mk1_server_is_input_ready(br->srv)) return;

    struct timespec ts;
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
    uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    for (uint8_t i = 0; i < count && i < 16; i++) {
        uint32_t idx      = pads[i].index;    // EP4 pair == IPC pad_index (confirmed)
        uint16_t pressure = pads[i].pressure;
        uint16_t prev     = g_prev_pressure[idx];
        // was_active is true only if a hit_on was actually sent (g_sent_pressure > 0).
        // Using g_prev_pressure >= hit_off_threshold caused orphaned hit_off events:
        // pressure drifting into [150,300) set was_active without a hit_on, then
        // any subsequent drop below 150 emitted hit_off with no corresponding hit_on.
        bool     was_active = g_sent_pressure[idx] > 0;
        bool     is_active =
            pressure >= (uint16_t)(was_active ? g_pad_hit_off_threshold : g_pad_hit_on_threshold);

        if (pressure == prev) continue;

        float value = is_active ? (pressure / PAD_PRESSURE_MAX) : 0.0f;

        if (!was_active && is_active) {
            // Debounce: suppress hit_on if we just released this pad within the window.
            if (ts_ns - g_hit_off_time_ns[idx] < g_pad_debounce_ns) {
                g_prev_pressure[idx] = pressure;
                g_led_pad_pressure[idx] = pressure;
                continue;
            }
            // pad struck — send hit_on then initial pressure_update
            // Dump all 16 pressures to diagnose crosstalk / rogue hits.
            {
                char snap[256];
                int  snap_off = 0;
                for (uint8_t k = 0; k < 16 && k < count; k++) {
                    snap_off += snprintf(snap + snap_off, sizeof(snap) - (size_t)snap_off,
                                        " p%u=%u", k, pads[k].pressure);
                }
                BTNLOG("pad idx=%u hit_on pressure=%u (%.3f) snapshot:%s",
                       idx, pressure, (double)value, snap);
            }
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_ON,         value);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_PRESSURE_UPDATE, value);
            g_sent_pressure[idx] = pressure;
        } else if (was_active && !is_active) {
            // pad released
            BTNLOG("pad idx=%u hit_off", idx);
            send_pad_record(br->srv, ts_ns, idx, PAD_EVT_HIT_OFF, 0.0f);
            g_sent_pressure[idx] = 0;
            g_hit_off_time_ns[idx] = ts_ns;
        } else if (is_active) {
            // pad held — only forward if pressure changed enough to be meaningful
            uint16_t sent = g_sent_pressure[idx];
            uint16_t delta = pressure > sent ? pressure - sent : sent - pressure;
            if (delta >= (uint16_t)g_pad_pressure_threshold) {
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
    if (!br->srv || !mk1_server_is_input_ready(br->srv)) return;

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

    if (msg_type == NI_EVT_BTN_DATA && ev->len >= 24) {
        uint32_t control_index = 0;
        uint32_t pressed = 0;
        memcpy(&control_index, ev->raw + 16, 4);
        memcpy(&pressed,       ev->raw + 20, 4);
        BTNLOG("button control_index=0x%x pressed=%u", control_index, pressed);
        if (control_index == 0x1c) {
            bool new_pressed = (pressed != 0);
            if (g_led_autowrite_pressed != new_pressed) {
                g_led_autowrite_pressed = new_pressed;
                refresh_led_from_cache(br);
            }
        }
    }

    uint64_t start_ns = timing_now_ns();
    bool ok = mk1_server_send_event(br->srv, ev->raw, ev->len);
    TLOG("ipc-tx button type=0x%08x len=%zu ok=%d dur_us=%llu",
         msg_type,
         ev->len,
         ok ? 1 : 0,
         (unsigned long long)((timing_now_ns() - start_ns) / 1000ULL));
}

static void on_midi(const mk1_midi_event_t *ev, void *ctx)
{
    bridge_t *br = (bridge_t *)ctx;
    Byte packet_buffer[1200];
    MIDIPacketList *packet_list = (MIDIPacketList *)packet_buffer;
    MIDIPacket *packet = NULL;
    OSStatus status = noErr;

    if (!br || !ev || ev->len == 0 || !br->midi_source) return;

    packet = MIDIPacketListInit(packet_list);
    packet = MIDIPacketListAdd(packet_list,
                               sizeof(packet_buffer),
                               packet,
                               0,
                               (UInt16)ev->len,
                               ev->raw);
    if (!packet) {
        BLOG("DIN MIDI -> CoreMIDI packet too large: %zu bytes", ev->len);
        return;
    }

    status = MIDIReceived(br->midi_source, packet_list);
    if (status != noErr) {
        BLOG("DIN MIDI -> CoreMIDI delivery failed: %d", (int)status);
    }
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

static bool parse_size_arg(const char *arg, const char *prefix, size_t *out_value)
{
    const char *value = NULL;
    char *end = NULL;
    unsigned long long parsed = 0;

    if (!arg || !prefix || !out_value) return false;
    if (strncmp(arg, prefix, strlen(prefix)) != 0) return false;
    value = arg + strlen(prefix);
    if (!value[0]) return false;

    parsed = strtoull(value, &end, 10);
    if (!end || *end != '\0') return false;
    *out_value = (size_t)parsed;
    return true;
}

static bool parse_int_arg(const char *arg, const char *prefix, int *out_value)
{
    size_t parsed = 0;

    if (!out_value) return false;
    if (!parse_size_arg(arg, prefix, &parsed)) return false;
    *out_value = (int)parsed;
    return true;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [--partial_display_max=N] [--display_tick_ms=N] [--no-partial-display]\n",
           prog ? prog : "mk1-bridge");
}

static const char *maschine_exec_path(void)
{
    const char *path = getenv("MK1_MASCHINE_EXEC");
    if (path && path[0]) return path;
    return "/Applications/Native Instruments/Maschine 2/Maschine 2.app/Contents/MacOS/Maschine 2";
}

static void init_led_packet(uint8_t packet[34])
{
    memset(packet, 0, 34);
    packet[0] = 0x0c;
    packet[1] = 0x1e;   // offset byte — start writing at device LED position 30
    packet[29] = 0x5c;  // device pos 57 (30+27) = LCD backlight; keep lit during probe
}

static bool parse_next_slot(const char **cursor, int *out_slot)
{
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (*p == '\0') {
        *cursor = p;
        return false;
    }

    char *end = NULL;
    long parsed = strtol(p, &end, 10);
    if (end == p) return false;
    *cursor = end;
    *out_slot = (int)parsed;
    return true;
}

static int parse_slot_list(const char *text, int *slots, int max_slots)
{
    if (!text || !text[0] || max_slots <= 0) return 0;
    int count = 0;
    const char *cursor = text;
    int slot = -1;
    while (count < max_slots && parse_next_slot(&cursor, &slot)) {
        if (slot >= 0 && slot <= 32) slots[count++] = slot;
    }
    return count;
}

static bool maschine_process_running(void)
{
    FILE *pg = popen("pgrep -x 'Maschine 2' 2>/dev/null", "r");
    if (!pg) return false;
    char pid_line[32] = {0};
    bool running = (fgets(pid_line, sizeof(pid_line), pg) != NULL && pid_line[0] != '\0');
    pclose(pg);
    return running;
}

static void maybe_launch_maschine(void)
{
    if (!env_truthy("MK1_AUTO_OPEN_MASCHINE")) return;
    if (maschine_process_running()) {
        BLOG("Maschine already running — auto-open skipped");
        return;
    }

    const char *path = maschine_exec_path();
    if (access(path, X_OK) != 0) {
        BLOG("Maschine auto-open failed: executable not found or not executable: %s", path);
        return;
    }

    char *const argv[] = { (char *)path, NULL };
    pid_t pid = -1;
    int err = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (err != 0) {
        BLOG("Maschine auto-open failed: posix_spawn err=%d", err);
        return;
    }

    g_spawned_maschine_pid = pid;
    BLOG("Maschine auto-open launched pid=%d", (int)pid);
}

static void maybe_close_maschine(void)
{
    if (!env_truthy("MK1_AUTO_CLOSE_MASCHINE")) return;
    if (g_spawned_maschine_pid <= 0) {
        BLOG("Maschine auto-close skipped: no bridge-launched pid");
        return;
    }

    if (kill(g_spawned_maschine_pid, SIGTERM) != 0) {
        BLOG("Maschine auto-close SIGTERM failed for pid=%d", (int)g_spawned_maschine_pid);
        g_spawned_maschine_pid = -1;
        return;
    }

    BLOG("Maschine auto-close sent SIGTERM to pid=%d", (int)g_spawned_maschine_pid);
    for (int i = 0; i < 20; i++) {
        int status = 0;
        pid_t result = waitpid(g_spawned_maschine_pid, &status, WNOHANG);
        if (result == g_spawned_maschine_pid) {
            g_spawned_maschine_pid = -1;
            return;
        }
        usleep(100 * 1000);
    }

    if (kill(g_spawned_maschine_pid, SIGKILL) == 0) {
        BLOG("Maschine auto-close escalated to SIGKILL for pid=%d", (int)g_spawned_maschine_pid);
    }
    g_spawned_maschine_pid = -1;
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
// Optional: MK1_LED_LEARN_FILTER='TransportRight,Note Repeat,Restart'
// Optional: MK1_LED_LEARN_REPEATS=2
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

static bool learn_filter_token_matches(const char *token, size_t token_len,
                                       const char *name, int seq_index)
{
    while (token_len > 0 && (*token == ' ' || *token == '	')) {
        token++;
        token_len--;
    }
    while (token_len > 0 && (token[token_len - 1] == ' ' || token[token_len - 1] == '	')) {
        token_len--;
    }
    if (token_len == 0) return false;

    char *end = NULL;
    long ordinal = strtol(token, &end, 10);
    if (end && end == token + token_len && ordinal == (long)(seq_index + 1)) {
        return true;
    }

    size_t name_len = strlen(name);
    if (name_len != token_len) return false;
    for (size_t i = 0; i < token_len; i++) {
        unsigned char a = (unsigned char)token[i];
        unsigned char b = (unsigned char)name[i];
        if (tolower(a) != tolower(b)) return false;
    }
    return true;
}

static bool learn_sequence_selected(const char *filter, const learn_btn_t *btn, int seq_index)
{
    if (!filter || !filter[0]) return true;

    const char *tok = filter;
    while (*tok) {
        const char *comma = strchr(tok, ',');
        size_t len = comma ? (size_t)(comma - tok) : strlen(tok);
        if (learn_filter_token_matches(tok, len, btn->name, seq_index)) return true;
        if (!comma) break;
        tok = comma + 1;
    }
    return false;
}

// Buttons in probe order. phys = expected physical slot from CODEX probe map.
// Group B is before Group A: pressing B lets us see A's slot turn off and B's turn on.
// Then pressing A gives us A's slot turning on and B's turning off.
static const learn_btn_t k_learn_seq[] = {
    // offset=0x1E region (groups, transport, display buttons)
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
    // offset=0x00 region (PADS row + transport area buttons)
    { "Mute",          -1 },
    { "Solo",          -1 },
    { "Select",        -1 },
    { "Duplicate",     -1 },
    { "Navigate",      -1 },
    { "Pad Mode",      -1 },
    { "Pattern",       -1 },
    { "Scene",         -1 },
    { "Shift",         -1 },
    { "Erase",         -1 },
    { "Grid",          -1 },
    { "TransportLeft", -1 },
    { "TransportRight",-1 },
    { "Record",        -1 },
    { "Play",          -1 },
};
#define LEARN_COUNT (int)(sizeof(k_learn_seq)/sizeof(k_learn_seq[0]))

static pthread_mutex_t g_learn_mu       = PTHREAD_MUTEX_INITIALIZER;
static bool            g_learn_armed    = false;
static bool            g_learn_got_upd  = false;
static uint8_t         g_learn_prev[57] = {0};
static uint8_t         g_learn_curr[57] = {0};

// Called from forward_led on every IPC LED message (holds no lock — fast path).
static void learn_on_led(const uint8_t *logical, size_t len)
{
    pthread_mutex_lock(&g_learn_mu);
    if (!g_learn_armed) { pthread_mutex_unlock(&g_learn_mu); return; }

    size_t n = len < 57 ? len : 57;
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        if (logical[i] != g_learn_prev[i]) { changed = true; break; }
    }
    if (changed) {
        memcpy(g_learn_curr, logical, n);
        if (n < 57) memset(g_learn_curr + n, 0, 57 - n);
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

    const char *filter = getenv("MK1_LED_LEARN_FILTER");
    const int repeats = env_int_or_default("MK1_LED_LEARN_REPEATS", 1);

    fprintf(tty,
        "\n=== MK1 LED LEARN MODE ===\n"
        "For each button: press it (and release), then hit ENTER.\n"
        "Each result shows what changed vs the previous button's state.\n"
        "Results go to led.log (grep LEARN).\n");
    if (filter && filter[0]) {
        fprintf(tty,
                "Filter: %s\n"
                "Only matching button names or 1-based step numbers will run.\n",
                filter);
    }
    if (repeats > 1) {
        fprintf(tty,
                "Repeats: %d\n"
                "Each selected button will be prompted this many times.\n",
                repeats);
    }
    fputc('\n', tty);
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
    uint8_t baseline[57];
    pthread_mutex_lock(&g_learn_mu);
    g_learn_armed = false;
    memcpy(baseline, g_learn_curr, 57);
    pthread_mutex_unlock(&g_learn_mu);

    // Button sequence — single capture per button, diff vs rolling baseline.
    int selected = 0;
    for (int i = 0; i < LEARN_COUNT; i++) {
        if (learn_sequence_selected(filter, &k_learn_seq[i], i)) selected++;
    }
    if (selected == 0) {
        fprintf(tty, "[learn] filter matched no buttons: %s\n", filter ? filter : "");
        LLOG("LEARN filter matched no buttons: %s", filter ? filter : "");
        if (tty != stderr) fclose(tty);
        return NULL;
    }

    int total_prompts = selected * (repeats > 0 ? repeats : 1);
    int ordinal = 0;
    for (int i = 0; i < LEARN_COUNT; i++) {
        const learn_btn_t *b = &k_learn_seq[i];
        if (!learn_sequence_selected(filter, b, i)) continue;
        const char *phys_name = (b->phys >= 0 && b->phys <= 32) ? k_phys_label[b->phys] : "?";

        for (int rep = 0; rep < repeats; rep++) {
            ordinal++;
            fprintf(tty, "[%2d/%2d] Press [%-14s]", ordinal, total_prompts, b->name);
            if (repeats > 1) {
                fprintf(tty, " (%d/%d)", rep + 1, repeats);
            }
            fprintf(tty, "  then ENTER: ");
            fflush(tty);

            // Arm capture before reading Enter so we don't miss updates during the press.
            pthread_mutex_lock(&g_learn_mu);
            g_learn_got_upd = false;
            g_learn_armed   = true;
            pthread_mutex_unlock(&g_learn_mu);

            if (!fgets(buf, sizeof(buf), tty)) goto learn_done;

            // Wait up to 3s for an update that differs from current baseline.
            bool ok = learn_wait_update(3000);

            uint8_t snap[57];
            pthread_mutex_lock(&g_learn_mu);
            g_learn_armed = false;
            memcpy(snap, g_learn_curr, 57);
            pthread_mutex_unlock(&g_learn_mu);

            if (!ok) {
                fprintf(tty, "  [no LED update received]\n");
                LLOG("LEARN [%d/%d] %s: TIMEOUT", ordinal, total_prompts, b->name);
                // Don't update baseline — keep previous state.
                continue;
            }

            // Diff snap vs rolling baseline — full 57-byte window.
            bool any = false;
            for (int s = 0; s < 57; s++) {
                if (snap[s] != baseline[s]) {
                    const char *region = (s < 32) ? "B-pkt" : (s >= 37 && s <= 52) ? "pad" : "?";
                    fprintf(tty, "  logical[%2d]: 0x%02x -> 0x%02x  (%s, expect->phys[%d]=%s)\n",
                            s, baseline[s], snap[s], region, b->phys, phys_name);
                    LLOG("LEARN [%d/%d] %s: logical[%d] 0x%02x->0x%02x  %s expect->phys[%d]=%s",
                         ordinal, total_prompts, b->name, s, baseline[s], snap[s], region, b->phys, phys_name);
                    any = true;
                }
            }
            if (!any) {
                fprintf(tty, "  [no change vs previous state]\n");
                LLOG("LEARN [%d/%d] %s: no change", ordinal, total_prompts, b->name);
            }

            // This prompt's state becomes the baseline for the next prompt.
            memcpy(baseline, snap, 57);
        }
    }

learn_done:
    fprintf(tty, "\n=== LED LEARN COMPLETE — grep led.log for LEARN ===\n");
    if (tty != stderr) fclose(tty);
    return NULL;
}

static void run_led_probe(mk1_device_t *dev)
{
    const int from = env_int_or_default("MK1_LED_PROBE_FROM", 0);
    const int to = env_int_or_default("MK1_LED_PROBE_TO", 30);
    const useconds_t dwell_us = (useconds_t)(env_int_or_default("MK1_LED_PROBE_MS", 700) * 1000);
    const uint8_t level = (uint8_t)env_int_or_default("MK1_LED_PROBE_LEVEL", 0x32);
    const bool interactive = env_truthy("MK1_LED_PROBE_INTERACTIVE");

    // MK1_LED_PROBE_OFFSET controls the second byte of the EP1 DIMM_LEDS packet.
    // Default -1 means "use existing format": {0x0C, 0x1E, data[0..32]}.
    // Set to 0 to probe the cabl-style offset=0 region: {0x0C, 0x00, data[0..30]}.
    // Any value 0..255 is sent literally as byte[1], and slots index into data[].
    const int probe_offset_raw = env_int_or_default("MK1_LED_PROBE_OFFSET", -1);
    const bool use_offset_mode = (probe_offset_raw >= 0 && probe_offset_raw <= 255);
    const uint8_t probe_offset = use_offset_mode ? (uint8_t)probe_offset_raw : 0x1e;

    BLOG("LED probe mode: slots %d..%d level=0x%02x dwell=%u ms offset=%s",
         from, to, level, (unsigned)(dwell_us / 1000),
         use_offset_mode ? "explicit" : "0x1e (default)");

    for (int idx = from; idx <= to; idx++) {
        uint8_t packet[34];
        size_t packet_len;

        if (use_offset_mode) {
            // cabl-style: {0x0C, offset, leds[0..30]} — 33 bytes, one slot lit
            if (idx < 0 || idx > 30) continue;
            memset(packet, 0, sizeof(packet));
            packet[0] = 0x0c;
            packet[1] = probe_offset;
            packet[2 + idx] = level;
            packet_len = 33;
        } else {
            // Existing format: {0x0C, 0x1E, phys[0..32]} — 34 bytes
            if (idx < 0 || idx > 32) continue;
            init_led_packet(packet);
            packet[1 + idx] = level;
            packet_len = 34;
        }

        {
            char hex[34 * 3 + 1];
            for (size_t i = 0; i < packet_len; i++) snprintf(hex + i * 3, 4, "%02x ", packet[i]);
            hex[packet_len * 3] = '\0';
            LLOG("LED probe slot %d (offset=0x%02x): %s", idx, (unsigned)probe_offset, hex);
        }

        mk1_device_write_endpoint(dev, 0x01, packet, packet_len);
        if (interactive) {
            char note[256];
            printf("\n[probe] slot %d (offset=0x%02x) active. Type what lit up, then press Enter\n"
                   "[probe] example: Scene | Mute | none\n> ",
                   idx, (unsigned)probe_offset);
            fflush(stdout);
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

    // Clear — use whatever format we were probing with
    {
        uint8_t off_packet[34];
        if (use_offset_mode) {
            memset(off_packet, 0, 33);
            off_packet[0] = 0x0c;
            off_packet[1] = probe_offset;
            mk1_device_write_endpoint(dev, 0x01, off_packet, 33);
        } else {
            init_led_packet(off_packet);
            mk1_device_write_endpoint(dev, 0x01, off_packet, 34);
        }
    }
}

static void run_led_verify(mk1_device_t *dev)
{
    const char *slots_env = getenv("MK1_LED_VERIFY_SLOTS");
    const char *label = getenv("MK1_LED_VERIFY_LABEL");
    const uint8_t level = (uint8_t)env_int_or_default("MK1_LED_VERIFY_LEVEL", 0x32);
    const int repeats = env_int_or_default("MK1_LED_VERIFY_REPEATS", 3);
    const int dwell_ms = env_int_or_default("MK1_LED_VERIFY_DWELL_MS", 900);
    const int gap_ms = env_int_or_default("MK1_LED_VERIFY_GAP_MS", 500);
    const int startup_delay_ms = env_int_or_default("MK1_LED_VERIFY_STARTUP_DELAY_MS", 5000);
    const bool randomize = env_truthy("MK1_LED_VERIFY_RANDOMIZE");
    const bool yes_no = env_truthy("MK1_LED_VERIFY_YN");
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) tty = stderr;

    if (!slots_env || !slots_env[0]) {
        BLOG("LED verify requested without MK1_LED_VERIFY_SLOTS");
        if (tty != stderr) fclose(tty);
        return;
    }

    int slots[64];
    int nslots = parse_slot_list(slots_env, slots, (int)(sizeof(slots) / sizeof(slots[0])));
    if (nslots <= 0) {
        BLOG("LED verify requested with no valid slots: %s", slots_env);
        if (tty != stderr) fclose(tty);
        return;
    }

    fprintf(tty,
            "\n=== MK1 LED VERIFY ===\n"
            "This mode tests explicit candidate phys slots with isolated EP1 packets.\n"
            "Known constants are preserved on every write: phys[0]=0x1e, phys[28]=0x5c.\n"
            "Target: %s\n"
            "Slots: %s\n"
            "Level: 0x%02x\n"
            "Startup delay: %d ms\n\n",
            (label && label[0]) ? label : "(unlabeled)",
            slots_env, level, startup_delay_ms);
    fflush(tty);

    if (startup_delay_ms > 0) {
        fprintf(tty,
                "[verify] waiting %d ms for Maschine/UI to settle before trials...\n",
                startup_delay_ms);
        fflush(tty);
        usleep((useconds_t)(startup_delay_ms * 1000));
    }

    int trials[64 * 16];
    int trial_count = 0;
    int max_trials = (int)(sizeof(trials) / sizeof(trials[0]));
    for (int r = 0; r < repeats; r++) {
        for (int i = 0; i < nslots && trial_count < max_trials; i++) {
            trials[trial_count++] = slots[i];
        }
    }
    if (randomize) {
        for (int i = trial_count - 1; i > 0; i--) {
            int j = (int)arc4random_uniform((uint32_t)(i + 1));
            int tmp = trials[i];
            trials[i] = trials[j];
            trials[j] = tmp;
        }
    }

    int score_correct[33] = {0};
    int score_wrong[33] = {0};
    int score_none[33] = {0};
    int score_ambig[33] = {0};

    for (int ordinal = 0; ordinal < trial_count; ordinal++) {
        int slot = trials[ordinal];
        uint8_t packet[34];
        init_led_packet(packet);
        packet[1 + slot] = level;

        {
            char hex[34 * 3 + 1];
            for (size_t i = 0; i < sizeof(packet); i++) snprintf(hex + i * 3, 4, "%02x ", packet[i]);
            hex[sizeof(packet) * 3] = '\0';
            LLOG("LED verify %s slot %d packet: %s",
                 (label && label[0]) ? label : "(unlabeled)", slot, hex);
        }

        mk1_device_write_endpoint(dev, 0x01, packet, sizeof(packet));

        fprintf(tty, "[verify %d/%d] %s candidate phys[%d] active.\n",
                ordinal + 1, trial_count,
                (label && label[0]) ? label : "LED",
                slot);
        if (yes_no) {
            fprintf(tty, "Is this %s? y=yes n=no\n> ",
                    (label && label[0]) ? label : "the target LED");
        } else {
            fprintf(tty, "Score: 1=correct 2=wrong-led 3=nothing 4=ambiguous\n> ");
        }
        fflush(tty);

        char note[256];
        if (!fgets(note, sizeof(note), tty)) {
            LLOG("LED verify %s slot %d observed: <stdin closed>",
                 (label && label[0]) ? label : "(unlabeled)", slot);
            clearerr(tty);
            break;
        }
        char score = note[0];
        if (yes_no) {
            if (score == 'y' || score == 'Y') score = '1';
            else if (score == 'n' || score == 'N') score = '2';
            else score = '4';
        }
        switch (score) {
            case '1': score_correct[slot]++; break;
            case '2': score_wrong[slot]++; break;
            case '3': score_none[slot]++; break;
            default:  score_ambig[slot]++; score = '4'; break;
        }
        LLOG("LED verify %s slot %d score=%c",
             (label && label[0]) ? label : "(unlabeled)", slot, score);

        usleep((useconds_t)(dwell_ms * 1000));
        uint8_t off_packet[34];
        init_led_packet(off_packet);
        mk1_device_write_endpoint(dev, 0x01, off_packet, sizeof(off_packet));
        usleep((useconds_t)(gap_ms * 1000));
    }

    {
        uint8_t packet[34];
        init_led_packet(packet);
        mk1_device_write_endpoint(dev, 0x01, packet, sizeof(packet));
    }

    fprintf(tty, "\nSummary for %s:\n", (label && label[0]) ? label : "(unlabeled)");
    for (int i = 0; i < nslots; i++) {
        int slot = slots[i];
        fprintf(tty,
                "  phys[%d]: correct=%d wrong=%d none=%d ambiguous=%d\n",
                slot, score_correct[slot], score_wrong[slot], score_none[slot], score_ambig[slot]);
        LLOG("LED verify summary %s phys[%d]: correct=%d wrong=%d none=%d ambiguous=%d",
             (label && label[0]) ? label : "(unlabeled)",
             slot,
             score_correct[slot], score_wrong[slot], score_none[slot], score_ambig[slot]);
    }

    fprintf(tty, "\n=== LED VERIFY COMPLETE ===\n");
    if (tty != stderr) fclose(tty);
}

static void *guided_mode_thread_fn(void *arg)
{
    bridge_t *br = (bridge_t *)arg;
    if (!br || !br->usb) return NULL;

    if (env_truthy("MK1_LED_VERIFY")) {
        BLOG("guided LED verify starting after Maschine connection");
        run_led_verify(br->usb);
    } else if (env_truthy("MK1_LED_PROBE")) {
        const int delay_ms = env_int_or_default("MK1_LED_PROBE_STARTUP_DELAY_MS", 5000);
        if (delay_ms > 0) {
            BLOG("guided LED probe waiting %d ms after Maschine connection", delay_ms);
            usleep((useconds_t)(delay_ms * 1000));
        }
        if (!g_running || !br->usb) return NULL;
        BLOG("guided LED probe starting");
        run_led_probe(br->usb);
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        g_running = 0;
        CFRunLoopStop(CFRunLoopGetMain());
    });
    return NULL;
}

static bool guided_verify_with_maschine(void)
{
    return env_truthy("MK1_LED_VERIFY") && env_truthy("MK1_LED_VERIFY_WITH_MASCHINE");
}

static void maybe_start_guided_mode(bridge_t *br)
{
    if (g_aux_mode_started) return;
    if (!env_truthy("MK1_LED_PROBE") && !env_truthy("MK1_LED_VERIFY")) return;
    if (!br || !br->usb) return;
    if (!g_guided_mode_only && (!br->srv || !mk1_server_is_connected(br->srv))) return;

    g_aux_mode_started = true;
    pthread_t tid;
    pthread_create(&tid, NULL, guided_mode_thread_fn, br);
    pthread_detach(tid);
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

    g_midi_warned_no_usb = false;

    if (!mk1_device_start(br->usb, on_pad, on_button, on_midi, br)) {
        BLOG("WARNING: could not start USB read thread after arrival");
    }

    // If Maschine is already connected, announce the device now.
    if (br->srv && mk1_server_is_connected(br->srv)) {
        char serial[32] = "MK1000000000000";
        mk1_device_get_serial(br->usb, serial, sizeof(serial));
        if (!mk1_server_send_device_on(br->srv, serial)) {
            BLOG("WARNING: DEVICE_ON send failed after arrival");
        }
        maybe_start_guided_mode(br);
    } else {
        // Maschine not yet connected — show prompt on both displays.
        show_status_screen(br);
        maybe_start_guided_mode(br);
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
    if (device_open_and_init(br)) {
        g_open_retry_budget = 0;
        return;
    }

    // On Apple Silicon the matching notification can beat the device-userclient
    // readiness. Keep the default bridge behavior conservative, but allow a few
    // extra retries in standalone guided mode so raw slot probes do not require
    // further usb-open surgery.
    g_open_retry_budget = g_guided_mode_only ? 6 : 1;
    schedule_device_open_retry(br);
}

static void schedule_device_open_retry(bridge_t *br)
{
    const int retry_ms = 500;

    if (!br || br->usb) {
        g_open_retry_budget = 0;
        return;
    }
    if (g_open_retry_budget <= 0) {
        BLOG("retry failed — device not usable");
        return;
    }

    BLOG("open failed — retrying in %dms (%d remaining)", retry_ms, g_open_retry_budget);
    g_open_retry_budget--;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, retry_ms * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (!g_running || br->usb) return;
        BLOG("MK1 retry open");
        if (device_open_and_init(br)) {
            g_open_retry_budget = 0;
            return;
        }
        schedule_device_open_retry(br);
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
    g_midi_warned_no_usb = false;

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
    const char *timing_env = getenv("MK1_TIMING_TRACE");

    g_timing_trace = timing_env && timing_env[0] && strcmp(timing_env, "0") != 0;
    g_pad_hit_on_threshold   = env_int_or_default("MK1_PAD_HIT_ON",        300);
    g_pad_hit_off_threshold  = env_int_or_default("MK1_PAD_HIT_OFF",       150);
    g_pad_pressure_threshold = env_int_or_default("MK1_PAD_PRESSURE",      200);
    g_pad_debounce_ns = (uint64_t)env_int_or_default("MK1_PAD_DEBOUNCE_MS", 10) * 1000000ULL;
    g_display_tick_ms = env_int_or_default("MK1_DISPLAY_TICK_MS", 16);
    if (g_display_tick_ms <= 0) g_display_tick_ms = 16;
    g_display_partial_flush = true;
    if (env_truthy("MK1_DISPLAY_PARTIAL")) {
        g_display_partial_flush = true;
    }
    g_display_partial_max_payload =
        (size_t)env_int_or_default("MK1_DISPLAY_PARTIAL_MAX", 2048);
    if (g_display_partial_max_payload == 0) g_display_partial_max_payload = 2048;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-partial-display") == 0) {
            g_display_partial_flush = false;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (parse_size_arg(argv[i], "--partial_display_max=", &g_display_partial_max_payload)) {
            if (g_display_partial_max_payload == 0) g_display_partial_max_payload = 2048;
            continue;
        }
        if (parse_int_arg(argv[i], "--display_tick_ms=", &g_display_tick_ms)) {
            if (g_display_tick_ms <= 0) g_display_tick_ms = 16;
            continue;
        }

        BLOG("unknown argument: %s", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    open_logs();

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    BLOG("mk1-bridge starting");
    BLOG("display flush scheduler tick=%d ms", g_display_tick_ms);
    BLOG("display partial flush=%s", g_display_partial_flush ? "enabled" : "disabled");
    BLOG("display partial max payload=%zu", g_display_partial_max_payload);
    midi_bridge_start(&g_bridge);

    if (env_truthy("MK1_LED_PROBE") || (env_truthy("MK1_LED_VERIFY") && !guided_verify_with_maschine())) {
        g_guided_mode_only = true;
        g_quiet = true;
        BLOG("guided LED test mode active — standalone USB mode");
    } else {
        g_bridge.srv = mk1_server_start(on_connect, on_cmd, &g_bridge);
        if (!g_bridge.srv) {
            BLOG("Failed to start IPC server");
            close_logs();
            return 1;
        }
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
    if (g_bridge.srv) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{ try_maschine_reconnect(); });
    }

    // Hot-plug watcher — fires immediately if device is already connected,
    // and again on any future plug/unplug while the run loop is running.
    g_bridge.hotplug = mk1_hotplug_start(on_device_arrived, on_device_removed, &g_bridge);
    if (!g_bridge.hotplug) {
        BLOG("WARNING: hot-plug watcher failed to start");
    }
    start_display_flush_timer(&g_bridge);

    if (!g_guided_mode_only) {
        maybe_launch_maschine();
    }

    BLOG("waiting for %s...", g_guided_mode_only ? "MK1 hardware" : "MK1 and Maschine software");
    CFRunLoopRun();

    if (g_bridge.usb && g_bridge.srv && mk1_server_is_connected(g_bridge.srv)) {
        mk1_server_send_device_off(g_bridge.srv);
    }
    if (g_display_flush_timer) {
        dispatch_source_cancel(g_display_flush_timer);
        g_display_flush_timer = NULL;
    }
    mk1_hotplug_stop(g_bridge.hotplug);
    if (g_bridge.srv) {
        mk1_server_stop(g_bridge.srv);
    }
    if (g_bridge.usb) {
        // Clear all LEDs (including backlight at phys[0]) before closing.
        uint8_t leds_off[33] = { 0x0c }; // cmd=0x0c, all phys positions = 0
        mk1_device_write_endpoint(g_bridge.usb, 0x01, leds_off, sizeof(leds_off));
        mk1_device_stop(g_bridge.usb);
        mk1_device_close(g_bridge.usb);
    }
    midi_bridge_stop(&g_bridge);

    maybe_close_maschine();

    BLOG("shutdown complete");
    close_logs();
    return 0;
}
