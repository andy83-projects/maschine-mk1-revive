#pragma once
#include <stdint.h>
#include <stdbool.h>

// MK1 USB identifiers
#define MK1_VENDOR_ID  0x17CC
#define MK1_PRODUCT_ID 0x0808

// HID Report IDs
#define MK1_REPORT_BUTTONS  0x10
#define MK1_REPORT_PADS     0x20

// Pad count
#define MK1_PAD_COUNT 16

// Opaque device handle
typedef struct mk1_device mk1_device_t;

// Pad event: one entry per pad
typedef struct {
    uint8_t  index;     // 0-15
    uint16_t pressure;  // 0-4095 (TODO: confirm range)
} mk1_pad_event_t;

// Button/encoder event
// TODO: expand once byte map is confirmed from capture
typedef struct {
    uint8_t raw[64];    // raw report bytes, parsed later
} mk1_button_event_t;

// Callbacks
typedef void (*mk1_pad_callback_t)(const mk1_pad_event_t *pads,
                                    uint8_t count,
                                    void *context);

typedef void (*mk1_button_callback_t)(const mk1_button_event_t *event,
                                       void *context);

// Lifecycle
mk1_device_t *mk1_device_open(void);
void          mk1_device_close(mk1_device_t *dev);
bool          mk1_device_is_open(const mk1_device_t *dev);

// Start reading (spawns background thread)
bool mk1_device_start(mk1_device_t *dev,
                      mk1_pad_callback_t    pad_cb,
                      mk1_button_callback_t button_cb,
                      void *context);

void mk1_device_stop(mk1_device_t *dev);

// Output
bool mk1_set_led(mk1_device_t *dev, uint8_t led_index, uint8_t brightness);
bool mk1_set_display(mk1_device_t *dev, uint8_t display_index,
                     const uint8_t *pixels, size_t len);
