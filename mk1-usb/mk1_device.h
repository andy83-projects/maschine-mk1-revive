#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// MK1 USB identifiers
#define MK1_VENDOR_ID  0x17CC
#define MK1_PRODUCT_ID 0x0808

// HID Report IDs
#define MK1_REPORT_BUTTONS  0x10
#define MK1_REPORT_PADS     0x20

// Pad count
#define MK1_PAD_COUNT 16

// Display geometry (confirmed from usb.pcapng analysis)
// Two displays: left index 0x00, right index 0x02
// EP8 wire format: [display_idx, len_hi, len_lo, payload...]
// Framebuffer write: first byte of payload is 0x5c (ST7529 RAMWR command)
#define MK1_DISPLAY_WIDTH    170   // pixels (85 col-addresses × 2px each)
#define MK1_DISPLAY_HEIGHT    64   // rows
#define MK1_DISPLAY_FB_BYTES 10880 // 170 × 64 × 1 byte/pixel (8bpp grayscale)

// Opaque device handle
typedef struct mk1_device mk1_device_t;
typedef struct mk1_user_client mk1_user_client_t;

// Pad event: one entry per pad
typedef struct {
    uint8_t  index;     // 0-15
    uint16_t pressure;  // 0-4095 (TODO: confirm range)
} mk1_pad_event_t;

// Button/encoder event
// TODO: expand once byte map is confirmed from capture
typedef struct {
    size_t  len;
    uint8_t raw[64];    // raw report bytes, parsed later
} mk1_button_event_t;

typedef struct {
    size_t  len;
    uint8_t raw[1024];
} mk1_midi_event_t;

// Callbacks
typedef void (*mk1_pad_callback_t)(const mk1_pad_event_t *pads,
                                    uint8_t count,
                                    void *context);

typedef void (*mk1_button_callback_t)(const mk1_button_event_t *event,
                                       void *context);

typedef void (*mk1_midi_callback_t)(const mk1_midi_event_t *event,
                                     void *context);

// Lifecycle
mk1_device_t *mk1_device_open(void);
void          mk1_device_close(mk1_device_t *dev);
bool          mk1_device_is_open(const mk1_device_t *dev);
bool          mk1_device_get_serial(const mk1_device_t *dev, char *serial, size_t len);

// Hot-plug: watch for MK1 arrival and removal via IOKit notifications.
// Integrates with CFRunLoop — call mk1_hotplug_start() before CFRunLoopRun().
// arrived_cb fires when the device appears (including if already plugged in at start).
// removed_cb fires when the device is unplugged.
// Drain the initial iterator in arrived_cb before doing anything — notifications
// use an edge-triggered model and the first callback arms future ones.
typedef void (*mk1_hotplug_arrived_cb_t)(void *ctx);
typedef void (*mk1_hotplug_removed_cb_t)(void *ctx);

typedef struct mk1_hotplug mk1_hotplug_t;

mk1_hotplug_t *mk1_hotplug_start(mk1_hotplug_arrived_cb_t arrived,
                                   mk1_hotplug_removed_cb_t removed,
                                   void *ctx);
void mk1_hotplug_stop(mk1_hotplug_t *hp);

// Start reading (spawns background thread)
bool mk1_device_start(mk1_device_t *dev,
                      mk1_pad_callback_t    pad_cb,
                      mk1_button_callback_t button_cb,
                      mk1_midi_callback_t   midi_cb,
                      void *context);

void mk1_device_stop(mk1_device_t *dev);

// Set log path for encoder/knob diagnostics and timing traces.
// Pass NULL to disable. Called from main.c after configuring log files.
void mk1_device_set_encoder_log_path(const char *path);
void mk1_device_set_timing_log_path(const char *path);

// Output
bool mk1_device_write_endpoint(mk1_device_t *dev,
                               uint8_t endpoint_number,
                               const uint8_t *data,
                               size_t len);

bool mk1_device_write_midi(mk1_device_t *dev,
                           const uint8_t *data,
                           size_t len);

// Send caiaq protocol init sequence (GET_DEVICE_INFO, AUTO_MSG, DIMM_LEDS, EP8 config).
// Must be called after mk1_device_open() and before mk1_device_start().
// Without this, the device stays in post-enumeration idle.
bool mk1_device_init_hardware(mk1_device_t *dev);

// Debug: replay full captured startup init from ep01.txt/ep08.txt files
bool mk1_device_replay_startup_init(mk1_device_t *dev);
bool mk1_set_led(mk1_device_t *dev, uint8_t led_index, uint8_t brightness);
bool mk1_set_display(mk1_device_t *dev, uint8_t display_index,
                     const uint8_t *pixels, size_t len);

// Minimal user-space replacement for the NIUSBUserClient selector surface.
typedef enum {
    MK1_UC_SELECTOR_I2C_READ = 0,
    MK1_UC_SELECTOR_I2C_WRITE = 1,
    MK1_UC_SELECTOR_WRITE_IO = 2,
    MK1_UC_SELECTOR_GET_DEVICE_INFO = 3,
    MK1_UC_SELECTOR_MIDI_WRITE = 4,
    MK1_UC_SELECTOR_SET_AUTO_MSG = 5,
    MK1_UC_SELECTOR_SET_LEDS = 6,
    MK1_UC_SELECTOR_DISPLAY_COMMAND = 7,
    MK1_UC_SELECTOR_GET_HARDWARE_BUFFER_SIZE = 8,
    MK1_UC_SELECTOR_SET_HARDWARE_BUFFER_SIZE = 9,
    MK1_UC_SELECTOR_SET_THRESHOLDS = 10,
    MK1_UC_SELECTOR_MIDI_WRITE_FAKE = 12,
    MK1_UC_SELECTOR_DISPLAY_COMMAND_LONG = 17,
    MK1_UC_SELECTOR_GET_DEVICE_SPEC = 18,
    MK1_UC_SELECTOR_READ_USER_DATA = 19,
    MK1_UC_SELECTOR_WRITE_USER_DATA = 20,
    MK1_UC_SELECTOR_DIGITAL_INPUT_ARM = 21,
} mk1_user_client_selector_t;

typedef enum {
    MK1_UC_ASYNC_MIDI_READ = 0,
    MK1_UC_ASYNC_ANALOG_INPUT_READ = 1,
    MK1_UC_ASYNC_DIGITAL_INPUT_READ = 2,
    MK1_UC_ASYNC_ERP_INPUT_READ = 3,
    MK1_UC_ASYNC_SERIAL_DATA_READ = 4,
    MK1_UC_ASYNC_SAMPLE_BUFFER_READ = 5,
    MK1_UC_ASYNC_MIDI_READ_TIMESTAMPED = 6,
    MK1_UC_ASYNC_ANALOG_INPUT_READ_TIMESTAMPED = 7,
    MK1_UC_ASYNC_DIGITAL_INPUT_READ_TIMESTAMPED = 8,
    MK1_UC_ASYNC_ERP_INPUT_READ_TIMESTAMPED = 9,
    MK1_UC_ASYNC_SAMPLE_BUFFER_READ_TIMESTAMPED = 10,
    MK1_UC_ASYNC_ENCODER_INPUT_READ = 11,
} mk1_user_client_async_selector_t;

mk1_user_client_t *mk1_user_client_open(void);
void mk1_user_client_close(mk1_user_client_t *client);
bool mk1_user_client_is_open(const mk1_user_client_t *client);

int32_t mk1_user_client_call_method(mk1_user_client_t *client,
                                    uint32_t selector,
                                    const uint64_t *input_scalars,
                                    uint32_t input_scalar_count,
                                    const void *input_struct,
                                    size_t input_struct_count,
                                    uint64_t *output_scalars,
                                    uint32_t *output_scalar_count,
                                    void *output_struct,
                                    size_t *output_struct_count);

int32_t mk1_user_client_call_async_method(mk1_user_client_t *client,
                                          uint32_t selector,
                                          const uint64_t *input_scalars,
                                          uint32_t input_scalar_count,
                                          const void *input_struct,
                                          size_t input_struct_count,
                                          bool timestamped);

bool mk1_user_client_map_memory(mk1_user_client_t *client,
                                uint32_t memory_type,
                                void **address,
                                size_t *size);
