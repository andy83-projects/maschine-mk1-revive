#include "mk1_device.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

// TODO: Switch between IOUSBHost (preferred, no deps) and hidapi (fallback)
// For initial bringup we use hidapi since it's cross-platform and well understood.
// IOUSBHost gives us lower-level access if needed for exclusive claim behaviour.
//
// hidapi header will live in vendor/hidapi/ once added as a submodule:
//   git submodule add https://github.com/libusb/hidapi vendor/hidapi
#include "../vendor/hidapi/hidapi.h"

struct mk1_device {
    hid_device *hid;
    pthread_t   read_thread;
    bool        running;

    mk1_pad_callback_t    pad_cb;
    mk1_button_callback_t button_cb;
    void                 *cb_context;
};

// ---------------------------------------------------------------------------
// Internal: HID report parsing
// ---------------------------------------------------------------------------

static void parse_pad_report(mk1_device_t *dev, const uint8_t *buf, size_t len)
{
    // TODO: parse Report ID 0x20
    // Placeholder: log raw bytes until byte map is confirmed
    (void)dev; (void)buf; (void)len;
    fprintf(stderr, "[mk1-usb] pad report: %zu bytes (unparsed)\n", len);
}

static void parse_button_report(mk1_device_t *dev, const uint8_t *buf, size_t len)
{
    // TODO: parse Report ID 0x10
    // Placeholder: log raw bytes until byte map is confirmed
    (void)dev; (void)buf; (void)len;
    fprintf(stderr, "[mk1-usb] button report: %zu bytes (unparsed)\n", len);
}

// ---------------------------------------------------------------------------
// Internal: read thread
// ---------------------------------------------------------------------------

static void *read_thread_fn(void *arg)
{
    mk1_device_t *dev = (mk1_device_t *)arg;
    uint8_t buf[256];

    while (dev->running) {
        int n = hid_read_timeout(dev->hid, buf, sizeof(buf), 100 /*ms*/);
        if (n < 0) {
            fprintf(stderr, "[mk1-usb] hid_read error, stopping\n");
            dev->running = false;
            break;
        }
        if (n == 0) continue; // timeout, loop

        uint8_t report_id = buf[0];
        switch (report_id) {
            case MK1_REPORT_PADS:
                parse_pad_report(dev, buf, (size_t)n);
                break;
            case MK1_REPORT_BUTTONS:
                parse_button_report(dev, buf, (size_t)n);
                break;
            default:
                fprintf(stderr, "[mk1-usb] unknown report ID 0x%02x (%d bytes)\n",
                        report_id, n);
                break;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

mk1_device_t *mk1_device_open(void)
{
    if (hid_init() != 0) {
        fprintf(stderr, "[mk1-usb] hid_init failed\n");
        return NULL;
    }

    hid_device *hid = hid_open(MK1_VENDOR_ID, MK1_PRODUCT_ID, NULL);
    if (!hid) {
        fprintf(stderr, "[mk1-usb] device not found (VID=0x%04x PID=0x%04x)\n",
                MK1_VENDOR_ID, MK1_PRODUCT_ID);
        return NULL;
    }

    mk1_device_t *dev = calloc(1, sizeof(mk1_device_t));
    dev->hid = hid;
    fprintf(stderr, "[mk1-usb] device opened\n");
    return dev;
}

void mk1_device_close(mk1_device_t *dev)
{
    if (!dev) return;
    mk1_device_stop(dev);
    hid_close(dev->hid);
    hid_exit();
    free(dev);
    fprintf(stderr, "[mk1-usb] device closed\n");
}

bool mk1_device_is_open(const mk1_device_t *dev)
{
    return dev && dev->hid;
}

bool mk1_device_start(mk1_device_t *dev,
                      mk1_pad_callback_t    pad_cb,
                      mk1_button_callback_t button_cb,
                      void *context)
{
    if (!dev || dev->running) return false;
    dev->pad_cb     = pad_cb;
    dev->button_cb  = button_cb;
    dev->cb_context = context;
    dev->running    = true;

    if (pthread_create(&dev->read_thread, NULL, read_thread_fn, dev) != 0) {
        fprintf(stderr, "[mk1-usb] failed to create read thread\n");
        dev->running = false;
        return false;
    }
    return true;
}

void mk1_device_stop(mk1_device_t *dev)
{
    if (!dev || !dev->running) return;
    dev->running = false;
    pthread_join(dev->read_thread, NULL);
}

bool mk1_set_led(mk1_device_t *dev, uint8_t led_index, uint8_t brightness)
{
    // TODO: construct correct output report once format is confirmed
    (void)dev; (void)led_index; (void)brightness;
    fprintf(stderr, "[mk1-usb] mk1_set_led: TODO\n");
    return false;
}

bool mk1_set_display(mk1_device_t *dev, uint8_t display_index,
                     const uint8_t *pixels, size_t len)
{
    // TODO: construct correct display output report
    (void)dev; (void)display_index; (void)pixels; (void)len;
    fprintf(stderr, "[mk1-usb] mk1_set_display: TODO\n");
    return false;
}
