#include "mk1_device.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../mk1-ipc/mk1_ipc.h"

#define MK1_INTERFACE_NUMBER    0
#define MK1_ALTERNATE_SETTING   1
#define MK1_READ_BUFFER_SIZE    512
#define MK1_MAX_INPUT_PIPES     4
#define MK1_MAX_OUTPUT_PIPES    4
#define MK1_UC_DEVICE_INFO_SIZE 0x6e
#define MK1_UC_DEVICE_SPEC_SIZE 0x0e
#define MK1_UC_USER_DATA_SIZE   0x21
#define MK1_UC_CLIENT_MEM_SIZE  0x10000

#ifndef kIOReturnSuccess
#define kIOReturnSuccess         0
#define kIOReturnError           0xe00002bc
#define kIOReturnNoDevice        0xe00002c0
#define kIOReturnBadArgument     0xe00002c2
#define kIOReturnUnsupported     0xe00002c7
#define kIOReturnExclusiveAccess 0xe00002d5
#define kIOReturnNotOpen         0xe00002d9
#define kIOReturnNoMemory        0xe00002bd
#endif

typedef struct {
    struct mk1_device *device;
    UInt8              pipe_ref;
    UInt8              endpoint_number;
    bool               started;
    pthread_t          thread;
    bool               have_last_report;
    size_t             last_report_len;
    uint8_t            last_report[MK1_READ_BUFFER_SIZE];
    bool               have_last_scan_report;
    size_t             last_scan_report_len;
    uint8_t            last_scan_report[64];
} mk1_pipe_reader_t;

struct mk1_device {
    io_service_t             service;
    io_service_t             interface_service;
    IOUSBDeviceInterface   **device_interface;
    IOUSBInterfaceInterface **interface;
    UInt8                    input_pipes[MK1_MAX_INPUT_PIPES];
    UInt8                    output_pipes[MK1_MAX_OUTPUT_PIPES];
    UInt8                    input_pipe_count;
    UInt8                    output_pipe_count;
    bool                     device_open;
    bool                     interface_open;
    bool                     running;
    char                     serial[64];
    mk1_pipe_reader_t        readers[MK1_MAX_INPUT_PIPES];

    mk1_pad_callback_t       pad_cb;
    mk1_button_callback_t    button_cb;
    void                    *cb_context;
    bool                     trace_reports;
    bool                     trace_scan_reports;
    bool                     trace_all_pipes;
    bool                     scan_baseline_set;
    size_t                   scan_baseline_len;
    uint8_t                  scan_baseline[64];
    bool                     ep1_short_report_valid;
    uint8_t                  ep1_short_report[8];
    bool                     ep1_33_autowrite_pressed;
    bool                     ep1_33_sampling_pressed;
    bool                     ep1_len33_prev_valid;
    uint8_t                  ep1_len33_prev[33];

    // Pad calibration: first EP4 report is captured as baseline.
    // Pressure is reported as delta above baseline, clamped to 0-4095.
    bool                     pad_baseline_set;
    uint16_t                 pad_baseline[MK1_PAD_COUNT];

    pthread_mutex_t          reply_lock;
    bool                     have_device_spec_reply;
    bool                     have_user_data_reply;
    bool                     saw_device_info_reply;
    uint8_t                  device_spec_reply[MK1_UC_DEVICE_SPEC_SIZE];
    uint8_t                  user_data_reply[MK1_UC_USER_DATA_SIZE];
};

typedef struct {
    bool     registered;
    bool     timestamped;
    uint32_t input_scalar_count;
    size_t   input_struct_size;
} mk1_uc_async_registration_t;

struct mk1_user_client {
    mk1_device_t                  *device;
    char                           serial[64];
    uint8_t                        user_data[MK1_UC_USER_DATA_SIZE];
    bool                           user_data_valid;
    void                          *client_memory[2];
    size_t                         client_memory_size[2];
    mk1_uc_async_registration_t    midi_read;
    mk1_uc_async_registration_t    analog_read;
    mk1_uc_async_registration_t    digital_read;
    mk1_uc_async_registration_t    erp_read;
    mk1_uc_async_registration_t    sample_buffer_read;
    mk1_uc_async_registration_t    encoder_read;
};

static int32_t mk1_uc_send_ep1_command(mk1_user_client_t *client,
                                       uint8_t command,
                                       const void *payload,
                                       size_t payload_len,
                                       const char *label);
static void mk1_uc_fill_device_info(mk1_user_client_t *client, void *buffer, size_t len);
static void mk1_uc_fill_device_spec(mk1_user_client_t *client, void *buffer, size_t len);
static mk1_uc_async_registration_t *mk1_uc_async_slot(mk1_user_client_t *client,
                                                       uint32_t selector,
                                                       bool *timestamped);
static bool mk1_device_drain_ep1_replies(mk1_device_t *dev,
                                         unsigned int timeout_ms,
                                         unsigned int max_packets);
static size_t mk1_remap_led_payload(const void *input_payload,
                                    size_t input_len,
                                    uint8_t *output_payload,
                                    size_t output_capacity);
static uint8_t mk1_normalize_led_brightness(uint8_t value);
static void mk1_device_dispatch_input_report(mk1_pipe_reader_t *reader,
                                             const uint8_t *data,
                                             size_t len);

static void trim_trailing_ascii_whitespace(char *text)
{
    size_t len = 0;

    if (!text) return;
    len = strlen(text);
    while (len > 0) {
        unsigned char ch = (unsigned char)text[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        text[--len] = '\0';
    }
}

static bool copy_cfstring(CFTypeRef value, char *dst, size_t dst_size)
{
    if (!value || !dst || dst_size == 0 || CFGetTypeID(value) != CFStringGetTypeID()) {
        return false;
    }
    if (!CFStringGetCString((CFStringRef)value, dst, dst_size, kCFStringEncodingUTF8)) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

static bool copy_cfnumber_u32(CFTypeRef value, uint32_t *dst)
{
    int tmp = 0;

    if (!value || !dst || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return false;
    }
    if (!CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &tmp)) {
        return false;
    }
    *dst = (uint32_t)tmp;
    return true;
}

static CFTypeRef copy_registry_property(io_service_t service, CFStringRef key)
{
    return IORegistryEntrySearchCFProperty(service,
                                           kIOServicePlane,
                                           key,
                                           kCFAllocatorDefault,
                                           kIORegistryIterateParents | kIORegistryIterateRecursively);
}

static bool get_registry_u32(io_service_t service, CFStringRef key, uint32_t *value)
{
    CFTypeRef prop = copy_registry_property(service, key);
    bool ok = copy_cfnumber_u32(prop, value);
    if (prop) CFRelease(prop);
    return ok;
}

static bool get_registry_string(io_service_t service, CFStringRef key, char *dst, size_t dst_size)
{
    CFTypeRef prop = copy_registry_property(service, key);
    bool ok = copy_cfstring(prop, dst, dst_size);
    if (prop) CFRelease(prop);
    if (ok) trim_trailing_ascii_whitespace(dst);
    return ok;
}

static bool get_device_identity(io_service_t service,
                                uint32_t *vendor_id,
                                uint32_t *product_id,
                                char *serial,
                                size_t serial_size)
{
    bool have_vendor = false;
    bool have_product = false;

    if (vendor_id) *vendor_id = 0;
    if (product_id) *product_id = 0;
    if (serial && serial_size > 0) serial[0] = '\0';

    if (vendor_id) {
        have_vendor = get_registry_u32(service, CFSTR("idVendor"), vendor_id);
    } else {
        uint32_t ignored = 0;
        have_vendor = get_registry_u32(service, CFSTR("idVendor"), &ignored);
    }

    if (product_id) {
        have_product = get_registry_u32(service, CFSTR("idProduct"), product_id);
    } else {
        uint32_t ignored = 0;
        have_product = get_registry_u32(service, CFSTR("idProduct"), &ignored);
    }

    if (serial && serial_size > 0) {
        if (!get_registry_string(service, CFSTR("USB Serial Number"), serial, serial_size)) {
            get_registry_string(service, CFSTR("Serial Number"), serial, serial_size);
        }
    }

    return have_vendor && have_product;
}

static void copy_service_class_name(io_service_t service, char *dst, size_t dst_size)
{
    io_name_t class_name = {0};

    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (IOObjectGetClass(service, class_name) == kIOReturnSuccess) {
        strlcpy(dst, class_name, dst_size);
    }
}

static void copy_service_name(io_service_t service, char *dst, size_t dst_size)
{
    io_name_t name = {0};

    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (IORegistryEntryGetName(service, name) == kIOReturnSuccess) {
        strlcpy(dst, name, dst_size);
    }
}

static void log_service_path(io_service_t service, const char *plane, const char *label)
{
    io_string_t path = {0};

    if (IORegistryEntryGetPath(service, plane, path) == kIOReturnSuccess) {
        fprintf(stderr, "[mk1-usb] %s path[%s]=%s\n", label, plane, path);
    }
}

static void log_usb_candidate(io_service_t service, const char *class_name)
{
    char manufacturer[128] = {0};
    char product[128] = {0};
    char serial[128] = {0};
    uint32_t vendor = 0;
    uint32_t product_id = 0;

    get_registry_string(service, CFSTR("USB Vendor Name"), manufacturer, sizeof(manufacturer));
    get_registry_string(service, CFSTR("USB Product Name"), product, sizeof(product));
    get_device_identity(service, &vendor, &product_id, serial, sizeof(serial));

    fprintf(stderr,
            "[mk1-usb] USB candidate class=%s vendor=0x%04x product=0x%04x manufacturer='%s' product='%s' serial='%s'\n",
            class_name,
            vendor,
            product_id,
            manufacturer[0] ? manufacturer : "?",
            product[0] ? product : "?",
            serial[0] ? serial : "?");
}

static const char *transfer_type_name(UInt8 transfer_type)
{
    switch (transfer_type) {
    case 0: return "control";
    case 1: return "isochronous";
    case 2: return "bulk";
    case 3: return "interrupt";
    default: return "unknown";
    }
}

static const char *direction_name(UInt8 direction)
{
    return direction ? "in" : "out";
}

static int preferred_input_pipe_rank(UInt8 pipe_ref, UInt8 endpoint_number)
{
    if (endpoint_number == 4) return 0;
    if (endpoint_number == 1) return 1;
    if (pipe_ref == 3) return 2;
    if (pipe_ref == 2) return 3;
    return 10 + pipe_ref;
}

static void log_iokit_error(const char *label, IOReturn kr)
{
    fprintf(stderr, "[mk1-usb] %s failed: 0x%08x\n", label, kr);
}

static void log_short_bytes(const char *label, const uint8_t *data, size_t len, size_t max_len)
{
    size_t limit = len < max_len ? len : max_len;

    fprintf(stderr, "[mk1-usb] %s:", label);
    for (size_t i = 0; i < limit; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n[mk1-usb]  ");
        fprintf(stderr, " %02x", data[i]);
    }
    if (len > limit) {
        fprintf(stderr, "\n[mk1-usb]  ... (%zu more bytes)", len - limit);
    }
    fprintf(stderr, "\n");
}

static bool build_repo_relative_path(const char *relative_path, char *dst, size_t dst_size)
{
    const char *source_path = __FILE__;
    const char *suffix = strstr(source_path, "/mk1-usb/mk1_device.c");
    size_t root_len = 0;

    if (!dst || dst_size == 0 || !relative_path || !suffix) {
        return false;
    }

    root_len = (size_t)(suffix - source_path);
    if (root_len + 1 + strlen(relative_path) + 1 > dst_size) {
        return false;
    }

    memcpy(dst, source_path, root_len);
    dst[root_len] = '\0';
    strcat(dst, "/");
    strcat(dst, relative_path);
    return true;
}

static size_t append_hexdump_line_bytes(const char *line, uint8_t *buffer, size_t count, size_t capacity)
{
    const char *ascii_sep = strstr(line, "   ");
    const char *cursor = line;

    if (!ascii_sep) return count;
    if ((size_t)(ascii_sep - line) < 6) return count;
    cursor = line + 6;

    while (cursor < ascii_sep) {
        unsigned int value = 0;

        while (cursor < ascii_sep && *cursor == ' ') cursor++;
        if (cursor + 2 > ascii_sep) break;
        if (sscanf(cursor, "%2x", &value) != 1) break;
        if (count < capacity) {
            buffer[count++] = (uint8_t)value;
        }
        cursor += 2;
        while (cursor < ascii_sep && *cursor != ' ') cursor++;
    }

    return count;
}

static bool replay_capture_packet(mk1_device_t *dev,
                                  uint8_t endpoint_number,
                                  const uint8_t *packet,
                                  size_t packet_len,
                                  size_t *selected_index,
                                  size_t skip_packets,
                                  size_t max_packets,
                                  size_t *replayed_packets)
{
    if (packet_len < 41) return true;
    if (packet[0] != 0x01 || packet[1] != 0x01 || packet[2] != 0x28 || packet[3] != 0x01) {
        return true;
    }

    (*selected_index)++;
    if (*selected_index <= skip_packets) {
        return true;
    }
    if (max_packets != 0 && *replayed_packets >= max_packets) {
        return false;
    }

    if (!mk1_device_write_endpoint(dev, endpoint_number, packet + 40, packet_len - 40)) {
        return false;
    }
    (*replayed_packets)++;
    usleep(2000);
    return true;
}

static bool replay_capture_file(mk1_device_t *dev,
                                uint8_t endpoint_number,
                                const char *relative_path,
                                size_t skip_packets,
                                size_t max_packets)
{
    char path[PATH_MAX];
    FILE *file = NULL;
    char line[512];
    uint8_t packet[1024];
    size_t packet_len = 0;
    size_t selected_index = 0;
    size_t replayed_packets = 0;

    if (!build_repo_relative_path(relative_path, path, sizeof(path))) {
        fprintf(stderr, "[mk1-usb] failed to build path for %s\n", relative_path);
        return false;
    }

    file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "[mk1-usb] failed to open capture file %s\n", path);
        return false;
    }

    fprintf(stderr,
            "[mk1-usb] replaying capture file %s for endpoint 0x%02x (skip=%zu max=%zu)\n",
            path,
            endpoint_number,
            skip_packets,
            max_packets);

    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "0000", 4) == 0 && packet_len > 0) {
            if (!replay_capture_packet(dev,
                                       endpoint_number,
                                       packet,
                                       packet_len,
                                       &selected_index,
                                       skip_packets,
                                       max_packets,
                                       &replayed_packets)) {
                break;
            }
            packet_len = 0;
        }

        if (line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (strncmp(line, "0000", 4) == 0 || (line[0] >= '0' && line[0] <= '9')) {
            packet_len = append_hexdump_line_bytes(line, packet, packet_len, sizeof(packet));
        }
    }

    if (packet_len > 0) {
        replay_capture_packet(dev,
                              endpoint_number,
                              packet,
                              packet_len,
                              &selected_index,
                              skip_packets,
                              max_packets,
                              &replayed_packets);
    }

    fclose(file);
    fprintf(stderr,
            "[mk1-usb] replayed %zu packets from %s\n",
            replayed_packets,
            relative_path);
    return replayed_packets > 0;
}

static void replay_visual_cleanup(mk1_device_t *dev)
{
    static const uint8_t cleanup_packets[][5] = {
        { 0x00, 0x00, 0x02, 0x20, 0x00 },
        { 0x02, 0x00, 0x02, 0x20, 0x00 },
        { 0x00, 0x00, 0x02, 0xbb, 0x00 },
        { 0x02, 0x00, 0x02, 0xbb, 0x00 },
    };

    fprintf(stderr, "[mk1-usb] applying post-init visual cleanup\n");
    for (size_t i = 0; i < sizeof(cleanup_packets) / sizeof(cleanup_packets[0]); i++) {
        mk1_device_write_endpoint(dev, 0x08, cleanup_packets[i], sizeof(cleanup_packets[i]));
        usleep(2000);
    }
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool mk1_is_scanned_button_report(const uint8_t *data, size_t len)
{
    if (!data || len != 64) {
        return false;
    }

    for (size_t i = 0; i < 16; i++) {
        uint16_t current = read_le16(data + (i * 2));
        uint16_t expected = (uint16_t)((read_le16(data) + (uint16_t)(i * 0x1000)) & 0xf000);
        uint16_t duplicate = read_le16(data + 32 + (i * 2));

        if (current != expected || duplicate != current) {
            return false;
        }
    }

    return true;
}

static uint8_t mk1_scan_phase(const uint8_t *data, size_t len)
{
    if (!data || len < 2) {
        return 0;
    }
    return (uint8_t)((read_le16(data) >> 12) & 0x0f);
}

static void mk1_normalize_scan_report(const uint8_t *input, size_t len, uint8_t *output)
{
    uint8_t phase = mk1_scan_phase(input, len);
    size_t word_count = len / 2;

    if (!input || !output || len != 64 || word_count < 16) {
        return;
    }

    // The observed non-pad button report is a 16-word ring duplicated twice.
    // Rotate it back to phase 0 so button-induced content deltas are comparable.
    for (size_t i = 0; i < 16; i++) {
        size_t source_index = (i + 16 - phase) & 0x0f;
        output[(i * 2) + 0] = input[(source_index * 2) + 0];
        output[(i * 2) + 1] = input[(source_index * 2) + 1];
        output[32 + (i * 2) + 0] = output[(i * 2) + 0];
        output[32 + (i * 2) + 1] = output[(i * 2) + 1];
    }
}

static void log_hex_bytes(const uint8_t *data, size_t len, size_t max_len)
{
    size_t limit = len < max_len ? len : max_len;

    for (size_t i = 0; i < limit; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n[mk1-usb]  ");
        fprintf(stderr, " %02x", data[i]);
    }
    if (len > limit) {
        fprintf(stderr, "\n[mk1-usb]  ... (%zu more bytes)", len - limit);
    }
}

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);

    if (!value || !value[0]) return false;
    if (strcmp(value, "0") == 0) return false;
    if (strcasecmp(value, "false") == 0) return false;
    if (strcasecmp(value, "no") == 0) return false;
    if (strcasecmp(value, "off") == 0) return false;
    return true;
}

static void log_report_baseline(UInt8 pipe_ref, const uint8_t *data, size_t len)
{
    if (len == 64) {
        uint16_t ch0 = read_le16(data);
        uint16_t ch1 = read_le16(data + 2);

        fprintf(stderr,
                "[mk1-usb] input pipe=%u baseline len=64 ch0=0x%04x ch1=0x%04x tail=%02x%02x%02x%02x...\n",
                pipe_ref,
                ch0,
                ch1,
                data[4],
                data[5],
                data[6],
                data[7]);
        return;
    }

    fprintf(stderr, "[mk1-usb] raw report baseline pipe=%u (%zu bytes):", pipe_ref, len);
    log_hex_bytes(data, len, 96);
    fprintf(stderr, "\n");
}

static void log_report_change(UInt8 pipe_ref,
                              const uint8_t *previous,
                              size_t previous_len,
                              const uint8_t *current,
                              size_t current_len)
{
    size_t shared_len = previous_len < current_len ? previous_len : current_len;
    size_t change_count = 0;
    size_t first_change = shared_len;
    size_t last_change = 0;

    for (size_t i = 0; i < shared_len; i++) {
        if (previous[i] != current[i]) {
            if (first_change == shared_len) first_change = i;
            last_change = i;
            change_count++;
        }
    }

    if (previous_len != current_len) {
        if (first_change == shared_len) first_change = shared_len;
        last_change = current_len > previous_len ? current_len - 1 : previous_len - 1;
        change_count += (current_len > previous_len) ? (current_len - shared_len) : (previous_len - shared_len);
    }

    if (change_count == 0) {
        return;
    }

    fprintf(stderr,
            "[mk1-usb] input pipe=%u changed bytes=%zu first=%zu last=%zu len=%zu",
            pipe_ref,
            change_count,
            first_change,
            last_change,
            current_len);
    if (current_len == 64) {
        fprintf(stderr,
                " ch0=0x%04x ch1=0x%04x tail=%02x%02x%02x%02x",
                read_le16(current),
                read_le16(current + 2),
                current[4],
                current[5],
                current[6],
                current[7]);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "[mk1-usb] current pipe=%u:", pipe_ref);
    log_hex_bytes(current, current_len, 96);
    fprintf(stderr, "\n");
}

static void log_raw_report(mk1_pipe_reader_t *reader, const uint8_t *data, size_t len)
{
    mk1_device_t *dev = reader ? reader->device : NULL;

    if (!dev || (!dev->trace_reports && !dev->trace_scan_reports)) {
        return;
    }

    if (dev->trace_reports) {
        if (!reader->have_last_report) {
            log_report_baseline(reader->pipe_ref, data, len);
        } else {
            log_report_change(reader->pipe_ref,
                              reader->last_report,
                              reader->last_report_len,
                              data,
                              len);
        }
    }

    if (len > sizeof(reader->last_report)) {
        len = sizeof(reader->last_report);
    }
    memcpy(reader->last_report, data, len);
    reader->last_report_len = len;
    reader->have_last_report = true;
}

static void log_scan_report(mk1_pipe_reader_t *reader, const uint8_t *data, size_t len)
{
    mk1_device_t *dev = reader ? reader->device : NULL;
    bool changed = !reader || !reader->have_last_scan_report || reader->last_scan_report_len != len;
    uint8_t normalized[64] = {0};
    uint8_t diff_preview[16] = {0};
    size_t diff_count = 0;
    size_t preview_count = 0;

    if (!dev || !dev->trace_scan_reports) {
        return;
    }

    if (!changed && data) {
        changed = memcmp(reader->last_scan_report, data, len) != 0;
    }
    if (!changed) {
        return;
    }

    if (len == 64) {
        mk1_normalize_scan_report(data, len, normalized);
        if (!dev->scan_baseline_set) {
            memcpy(dev->scan_baseline, normalized, len);
            dev->scan_baseline_len = len;
            dev->scan_baseline_set = true;
        } else if (dev->scan_baseline_len == len) {
            for (size_t i = 0; i < len; i++) {
                if (normalized[i] != dev->scan_baseline[i]) {
                    diff_count++;
                    if (preview_count < sizeof(diff_preview)) {
                        diff_preview[preview_count++] = (uint8_t)i;
                    }
                }
            }
        }
    }

    fprintf(stderr,
            "[mk1-usb] scan frame pipe=%u endpoint=%u len=%zu phase=%u base=0x%04x normalized_diff=%zu\n",
            reader ? reader->pipe_ref : 0,
            reader ? reader->endpoint_number : 0,
            len,
            mk1_scan_phase(data, len),
            len >= 2 ? read_le16(data) : 0,
            diff_count);
    log_short_bytes("scan payload", data, len, 96);
    if (preview_count > 0) {
        fprintf(stderr, "[mk1-usb] scan normalized diff bytes:");
        for (size_t i = 0; i < preview_count; i++) {
            fprintf(stderr, " %u", diff_preview[i]);
        }
        fprintf(stderr, "\n");
    }

    if (reader && len <= sizeof(reader->last_scan_report)) {
        memcpy(reader->last_scan_report, data, len);
        reader->last_scan_report_len = len;
        reader->have_last_scan_report = true;
    }
}

static void log_ep1_packet_diff(mk1_pipe_reader_t *reader,
                                const uint8_t *previous,
                                size_t previous_len,
                                const uint8_t *current,
                                size_t current_len)
{
    size_t shared_len = previous_len < current_len ? previous_len : current_len;
    size_t diff_count = 0;
    size_t preview_count = 0;
    uint8_t diff_preview[16] = {0};

    for (size_t i = 0; i < shared_len; i++) {
        if (previous[i] != current[i]) {
            diff_count++;
            if (preview_count < sizeof(diff_preview)) {
                diff_preview[preview_count++] = (uint8_t)i;
            }
        }
    }

    if (previous_len != current_len) {
        diff_count += previous_len > current_len ? (previous_len - shared_len) : (current_len - shared_len);
    }

    if (diff_count == 0) {
        return;
    }

    fprintf(stderr,
            "[mk1-usb] EP1-in changed pipe=%u len=%zu diff=%zu",
            reader ? reader->pipe_ref : 0,
            current_len,
            diff_count);
    if (current_len >= 8) {
        fprintf(stderr,
                " b0=%02x b4=%02x b5=%02x b6=%02x b7=%02x",
                current[0],
                current[4],
                current[5],
                current[6],
                current[7]);
    }
    fprintf(stderr, "\n");

    if (preview_count > 0) {
        fprintf(stderr, "[mk1-usb] EP1-in diff bytes:");
        for (size_t i = 0; i < preview_count; i++) {
            size_t index = diff_preview[i];
            fprintf(stderr, " %zu(%02x->%02x)", index, previous[index], current[index]);
        }
        fprintf(stderr, "\n");
    }

    log_short_bytes("EP1-in payload", current, current_len, 64);
}

static void log_all_pipe_report(mk1_pipe_reader_t *reader, const uint8_t *data, size_t len)
{
    mk1_device_t *dev = reader ? reader->device : NULL;
    bool changed = !reader || !reader->have_last_report || reader->last_report_len != len;

    if (!dev || !dev->trace_all_pipes) {
        return;
    }

    // The useful unexplained traffic is currently EP1-in short replies.
    // Ignore the rest here so button runs stay readable.
    if (!reader || reader->endpoint_number != 1 || (len != 8 && len != 33)) {
        return;
    }

    if (!changed && data) {
        changed = memcmp(reader->last_report, data, len) != 0;
    }

    if (!reader->have_last_report || reader->last_report_len != len) {
        fprintf(stderr,
                "[mk1-usb] EP1-in baseline pipe=%u len=%zu\n",
                reader->pipe_ref,
                len);
        log_short_bytes("EP1-in payload", data, len, 64);
    } else if (changed) {
        log_ep1_packet_diff(reader,
                            reader->last_report,
                            reader->last_report_len,
                            data,
                            len);
    }

    if (len > sizeof(reader->last_report)) {
        len = sizeof(reader->last_report);
    }
    memcpy(reader->last_report, data, len);
    reader->last_report_len = len;
    reader->have_last_report = true;
}

static void store_u32_le(uint8_t *dst, uint32_t value)
{
    if (!dst) return;
    memcpy(dst, &value, sizeof(value));
}

static uint64_t mk1_current_timestamp_ns(void)
{
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_UPTIME_RAW, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void mk1_emit_button_event(mk1_device_t *dev,
                                  const char *label,
                                  uint32_t control_index,
                                  bool pressed)
{
    if (!dev || !dev->button_cb) {
        return;
    }

    mk1_button_event_t event = {0};
    event.len = 6 * sizeof(uint32_t);
    uint8_t *cursor = event.raw;

    store_u32_le(cursor + 0 * 4, NI_EVT_BTN_DATA);
    uint64_t timestamp = mk1_current_timestamp_ns();
    store_u32_le(cursor + 1 * 4, (uint32_t)(timestamp >> 32));
    store_u32_le(cursor + 2 * 4, (uint32_t)(timestamp & 0xffffffffu));
    store_u32_le(cursor + 3 * 4, 0x00000001);
    store_u32_le(cursor + 4 * 4, control_index);
    store_u32_le(cursor + 5 * 4, pressed ? 1 : 0);

    if (dev->trace_all_pipes) {
        fprintf(stderr,
                "[mk1-usb] btn evt %s idx=0x%02x %s\n",
                label ? label : "unknown",
                control_index,
                pressed ? "press" : "release");
    }

    dev->button_cb(&event, dev->cb_context);
}

static int8_t mk1_wrapped_u8_delta(uint8_t previous, uint8_t current)
{
    int delta = (int)current - (int)previous;
    if (delta > 127) delta -= 256;
    if (delta < -127) delta += 256;
    return (int8_t)delta;
}

static void mk1_emit_knob_event(mk1_device_t *dev,
                                const char *label,
                                uint32_t encoder_index,
                                float delta)
{
    if (!dev || !dev->button_cb || delta == 0.0f) {
        return;
    }

    mk1_button_event_t event = {0};
    event.len = 6 * sizeof(uint32_t);
    uint8_t *cursor = event.raw;

    store_u32_le(cursor + 0 * 4, NI_EVT_KNOB_ROTATE);
    uint64_t timestamp = mk1_current_timestamp_ns();
    store_u32_le(cursor + 1 * 4, (uint32_t)(timestamp >> 32));
    store_u32_le(cursor + 2 * 4, (uint32_t)(timestamp & 0xffffffffu));
    store_u32_le(cursor + 3 * 4, 0x00000001);
    store_u32_le(cursor + 4 * 4, encoder_index);
    memcpy(cursor + 5 * 4, &delta, sizeof(delta));

    if (dev->trace_all_pipes) {
        fprintf(stderr,
                "[mk1-usb] knob evt %s idx=%u delta=%.4f\n",
                label ? label : "unknown",
                encoder_index,
                (double)delta);
    }

    dev->button_cb(&event, dev->cb_context);
}

static void mk1_process_ep1_short_buttons(mk1_device_t *dev, const uint8_t *new_report)
{
    // EP1 short reports carry several independent button banks across different bytes.
    // The mappings below mix two sources:
    // 1. raw Apple Silicon EP1 bit positions
    // 2. observed Maschine behavior for the currently-working BTN_DATA path
    //
    // Several Intel control_index assumptions were off by one bank during earlier
    // correlation work. The Group row is corrected here to match the actions seen
    // in Maschine itself.
    static const struct {
        uint8_t byte_index;
        uint8_t bit;
        uint32_t control_index;
        const char *name;
    } button_map[] = {
        // Pad section (byte 1, confirmed from Apple Silicon EP1 traces + Intel IPC capture 2026-04-07)
        { 1, 0x80, 0x07, "Scene" },
        { 1, 0x40, 0x06, "Pattern" },
        { 1, 0x20, 0x05, "Pad Mode" },
        { 1, 0x10, 0x04, "Navigate" },
        { 1, 0x08, 0x03, "Duplicate" },
        { 1, 0x04, 0x02, "Select" },
        { 1, 0x02, 0x01, "Solo" },
        { 1, 0x01, 0x00, "Mute" },
        { 4, 0x01, 0x18, "Control" },
        { 4, 0x02, 0x19, "Browse" },
        { 4, 0x04, 0x1a, "Left" },
        { 4, 0x08, 0x1b, "Snap" },
        { 4, 0x20, 0x1d, "Right" },
        { 4, 0x80, 0x1f, "Step" },
        { 3, 0x80, 0x17, "Group A" },
        { 3, 0x40, 0x16, "Group B" },
        { 3, 0x20, 0x15, "Group C" },
        { 3, 0x10, 0x14, "Group D" },
        { 3, 0x01, 0x10, "Group E" },
        { 3, 0x02, 0x11, "Group F" },
        { 3, 0x04, 0x12, "Group G" },
        { 3, 0x08, 0x13, "Group H" },
        { 2, 0x80, 0x0f, "Restart" },
        { 2, 0x40, 0x0e, "Transport Left" },
        { 2, 0x20, 0x0d, "Transport Right" },
        { 2, 0x10, 0x0c, "Grid" },
        // Screen buttons (byte 5, confirmed from Apple Silicon EP1 traces + Intel IPC capture 2026-04-07)
        { 5, 0x80, 0x27, "Screen 1" },
        { 5, 0x40, 0x26, "Screen 2" },
        { 5, 0x20, 0x25, "Screen 3" },
        { 5, 0x10, 0x24, "Screen 4" },
        { 5, 0x08, 0x23, "Screen 5" },
        { 5, 0x04, 0x22, "Screen 6" },
        { 5, 0x02, 0x21, "Screen 7" },
        { 5, 0x01, 0x20, "Screen 8" },
        { 6, 0x01, 0x28, "Note Repeat" },
        { 6, 0x02, 0x29, "Play" },
        { 2, 0x02, 0x09, "Record" },
        // TODO: Revisit Erase. Raw EP1 mask and Intel control_index are confirmed,
        // but Maschine behavior still appears uncertain in live testing.
        { 2, 0x04, 0x0a, "Erase" },
        { 2, 0x08, 0x0b, "Shift" },
    };

    if (!dev || !new_report) {
        return;
    }

    if (!dev->ep1_short_report_valid) {
        dev->ep1_short_report_valid = true;
        memcpy(dev->ep1_short_report, new_report, sizeof(dev->ep1_short_report));
        return;
    }

    for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
        uint8_t byte_index = button_map[i].byte_index;
        if (byte_index >= sizeof(dev->ep1_short_report)) {
            continue;
        }

        uint8_t previous = dev->ep1_short_report[byte_index];
        uint8_t current = new_report[byte_index];
        uint8_t changed = previous ^ current;
        if ((changed & button_map[i].bit) == 0) continue;

        bool pressed = (current & button_map[i].bit) != 0;
        mk1_emit_button_event(dev, button_map[i].name, button_map[i].control_index, pressed);
    }

    memcpy(dev->ep1_short_report, new_report, sizeof(dev->ep1_short_report));
}

static void mk1_process_ep1_len33_buttons(mk1_device_t *dev, const uint8_t *data)
{
    if (!dev || !data) {
        return;
    }

    static const struct {
        uint8_t byte_a;
        uint8_t byte_b;
        uint32_t encoder_index;
        const char *name;
    } knob_map[] = {
        // Byte positions come from Apple Silicon USB traces; encoder indexes are
        // confirmed by the dedicated Intel Volume/Tempo/Swing capture.
        { 17, 18, 0, "Volume" },
        { 11, 12, 1, "Tempo" },
        {  5,  6, 2, "Swing" },
    };

    for (size_t i = 0; i < sizeof(knob_map) / sizeof(knob_map[0]); i++) {
        uint8_t prev_a = dev->ep1_len33_prev[knob_map[i].byte_a];
        uint8_t curr_a = data[knob_map[i].byte_a];
        uint8_t prev_b = dev->ep1_len33_prev[knob_map[i].byte_b];
        uint8_t curr_b = data[knob_map[i].byte_b];

        if (prev_a == 0 && prev_b == 0 && !dev->ep1_len33_prev_valid) {
            continue;
        }

        int8_t delta_primary = mk1_wrapped_u8_delta(prev_a, curr_a);
        int8_t delta_secondary = mk1_wrapped_u8_delta(prev_b, curr_b);
        int8_t chosen_delta = delta_primary;
        if (chosen_delta == 0) {
            chosen_delta = delta_secondary;
        }

        if (chosen_delta != 0) {
            float normalized = (float)chosen_delta / 255.0f;
            mk1_emit_knob_event(dev, knob_map[i].name, knob_map[i].encoder_index, normalized);
        }
    }

    uint8_t br = data[4];
    uint8_t high_nibble = br & 0xf0;
    // Observed traces: Auto Write pulses show a 0x7x high nibble, Sampling pulses appear as 0x6x,
    // and the shared release state resides in the 0x5x range.  We emit NI BTN_DATA events
    // whenever we cross those boundaries.

    if (high_nibble == 0x70) {
        if (!dev->ep1_33_autowrite_pressed) {
            dev->ep1_33_autowrite_pressed = true;
            dev->ep1_33_sampling_pressed = false;
            mk1_emit_button_event(dev, "Auto Write", 0x1c, true);
        }
        return;
    }

    if (high_nibble == 0x60) {
        if (!dev->ep1_33_sampling_pressed) {
            dev->ep1_33_sampling_pressed = true;
            dev->ep1_33_autowrite_pressed = false;
            mk1_emit_button_event(dev, "Sampling", 0x1e, true);
        }
        return;
    }

    if (high_nibble == 0x50) {
        if (dev->ep1_33_autowrite_pressed) {
            dev->ep1_33_autowrite_pressed = false;
            mk1_emit_button_event(dev, "Auto Write", 0x1c, false);
        }
        if (dev->ep1_33_sampling_pressed) {
            dev->ep1_33_sampling_pressed = false;
            mk1_emit_button_event(dev, "Sampling", 0x1e, false);
        }
    }

    memcpy(dev->ep1_len33_prev, data, sizeof(dev->ep1_len33_prev));
    dev->ep1_len33_prev_valid = true;
}

static void mk1_device_process_ep1_button_packet(mk1_device_t *dev,
                                                mk1_pipe_reader_t *reader,
                                                const uint8_t *data,
                                                size_t len)
{
    (void)reader;

    if (!dev || !data || len == 0) {
        return;
    }

    if (len == 8 && data[0] == 0x04) {
        mk1_process_ep1_short_buttons(dev, data);
        return;
    }

    if (len == 33 && data[0] == 0x02) {
        mk1_process_ep1_len33_buttons(dev, data);
        return;
    }
}

static bool get_pipe_endpoint_number(const mk1_device_t *dev,
                                     UInt8 pipe_ref,
                                     UInt8 *endpoint_number)
{
    UInt8 direction = 0;
    UInt8 pipe_endpoint = 0;
    UInt8 transfer_type = 0;
    UInt16 max_packet_size = 0;
    UInt8 interval = 0;

    if (endpoint_number) *endpoint_number = 0;
    if (!dev || !dev->interface || pipe_ref == 0) {
        return false;
    }

    if ((*dev->interface)->GetPipeProperties(dev->interface,
                                             pipe_ref,
                                             &direction,
                                             &pipe_endpoint,
                                             &transfer_type,
                                             &max_packet_size,
                                             &interval) != kIOReturnSuccess) {
        return false;
    }

    if (endpoint_number) *endpoint_number = pipe_endpoint;
    return true;
}

static void mk1_device_handle_ep1_reply(mk1_device_t *dev,
                                        const uint8_t *data,
                                        size_t len)
{
    if (!dev || !data || len == 0) {
        return;
    }

    pthread_mutex_lock(&dev->reply_lock);

    switch (data[0]) {
    case 0x00:
        dev->saw_device_info_reply = true;
        fprintf(stderr,
                "[mk1-usb] EP1 reply type=0x00 len=%zu seq=0x%02x\n",
                len,
                len > 1 ? data[1] : 0);
        break;
    case 0x01:
        if (len >= 1 + sizeof(dev->device_spec_reply)) {
            memcpy(dev->device_spec_reply, data + 1, sizeof(dev->device_spec_reply));
            dev->have_device_spec_reply = true;
        }
        fprintf(stderr, "[mk1-usb] EP1 reply type=0x01 len=%zu (device spec)\n", len);
        break;
    case 0x14:
        if (len >= 1 + sizeof(dev->user_data_reply)) {
            memcpy(dev->user_data_reply, data + 1, sizeof(dev->user_data_reply));
            dev->have_user_data_reply = true;
        }
        fprintf(stderr, "[mk1-usb] EP1 reply type=0x14 len=%zu (user data)\n", len);
        break;
    default:
        fprintf(stderr, "[mk1-usb] EP1 reply type=0x%02x len=%zu\n", data[0], len);
        break;
    }

    if (dev->trace_reports) {
        log_short_bytes("EP1 reply", data, len, 96);
    }

    pthread_mutex_unlock(&dev->reply_lock);
}

static bool mk1_device_drain_ep1_replies(mk1_device_t *dev,
                                         unsigned int timeout_ms,
                                         unsigned int max_packets)
{
    UInt8 pipe_ref = 0;
    unsigned int packets = 0;

    if (!dev || !dev->interface) {
        return false;
    }

    for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
        UInt8 endpoint_number = 0;

        if (!get_pipe_endpoint_number(dev, dev->input_pipes[i], &endpoint_number)) {
            continue;
        }
        if (endpoint_number == 1) {
            pipe_ref = dev->input_pipes[i];
            break;
        }
    }

    if (pipe_ref == 0) {
        return false;
    }

    while (max_packets == 0 || packets < max_packets) {
        uint8_t buffer[MK1_READ_BUFFER_SIZE] = {0};
        UInt32 size = sizeof(buffer);
        IOReturn kr = (*dev->interface)->ReadPipeTO(dev->interface,
                                                    pipe_ref,
                                                    buffer,
                                                    &size,
                                                    timeout_ms,
                                                    timeout_ms);
        if (kr != kIOReturnSuccess) {
            break;
        }
        if (size == 0) {
            break;
        }

        mk1_device_handle_ep1_reply(dev, buffer, size);
        packets++;
    }

    return packets > 0;
}

static size_t mk1_remap_led_payload(const void *input_payload,
                                    size_t input_len,
                                    uint8_t *output_payload,
                                    size_t output_capacity)
{
    static const uint8_t hardware_index_by_logical[32] = {
        0,
        4, 3, 2, 1,
        8, 7, 6, 5,
        12, 11, 10, 9,
        16, 15, 14, 13,
        17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 28, 29, 30,
        31
    };
    size_t copy_len = input_len;

    if (!input_payload || !output_payload || output_capacity == 0) {
        return 0;
    }
    if (copy_len > output_capacity) {
        copy_len = output_capacity;
    }

    memcpy(output_payload, input_payload, copy_len);
    for (size_t i = 0; i < copy_len; i++) {
        output_payload[i] = mk1_normalize_led_brightness(output_payload[i]);
    }

    for (size_t logical_index = 0; logical_index < sizeof(hardware_index_by_logical); logical_index++) {
        size_t hardware_index = hardware_index_by_logical[logical_index];
        if (logical_index >= copy_len || hardware_index >= copy_len) {
            continue;
        }
        output_payload[hardware_index] =
            mk1_normalize_led_brightness(((const uint8_t *)input_payload)[logical_index]);
    }

    return copy_len;
}

static uint8_t mk1_normalize_led_brightness(uint8_t value)
{
    static const uint8_t tiers[] = { 0x13, 0x32, 0x5c };
    uint8_t best = tiers[0];
    unsigned best_distance = 0xff;

    if (value == 0) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        unsigned distance = (value > tiers[i]) ? (unsigned)(value - tiers[i])
                                               : (unsigned)(tiers[i] - value);
        if (distance < best_distance) {
            best = tiers[i];
            best_distance = distance;
        }
    }

    return best;
}

static void mk1_device_dispatch_input_report(mk1_pipe_reader_t *reader,
                                             const uint8_t *data,
                                             size_t len)
{
    mk1_device_t *dev = reader ? reader->device : NULL;

    if (!dev || !data || len == 0) {
        return;
    }

    // Some 64-byte EP4 reports are not pad pressure. The non-pad button/encoder
    // state trace we captured is a duplicated 16-word scan table that steps by
    // 0x1000 each slot. Ignore those for now so we don't misclassify them as
    // pads or forward malformed BTN_DATA payloads into Maschine.
    if (mk1_is_scanned_button_report(data, len)) {
        log_scan_report(reader, data, len);
        return;
    }

    // EP4 pad pressure report: 64 bytes, no report-ID prefix.
    // pcap confirms: pad[i] raw value is at bytes[i*2 .. i*2+1] (LE uint16).
    // Bytes[0..31] = first 16 pad channels; bytes[32..63] = second channel (ignored).
    //
    // The first report received is used as a per-pad baseline (resting level).
    // Pressure is reported as delta above baseline, clamped to [0, 4095].
    // This eliminates false-positive events from non-zero resting values.
    if (len == 64) {
        if (!dev->pad_baseline_set) {
            for (uint8_t i = 0; i < MK1_PAD_COUNT; i++) {
                dev->pad_baseline[i] = read_le16(data + (i * 2));
            }
            dev->pad_baseline_set = true;
            return;  // skip this first report; it is calibration only
        }

        mk1_pad_event_t pads[MK1_PAD_COUNT];
        for (uint8_t i = 0; i < MK1_PAD_COUNT; i++) {
            uint16_t raw    = read_le16(data + (i * 2));
            int32_t  delta  = (int32_t)raw - (int32_t)dev->pad_baseline[i];
            pads[i].index    = i;
            pads[i].pressure = (uint16_t)(delta <= 0 ? 0 : (delta > 4095 ? 4095 : delta));
        }

        if (dev->pad_cb) {
            dev->pad_cb(pads, MK1_PAD_COUNT, dev->cb_context);
        }
        return;
    }

    if (dev->trace_reports) {
        fprintf(stderr,
                "[mk1-usb] ignoring unknown input report len=%zu byte0=0x%02x from pipe=%u endpoint=%u\n",
                len,
                data[0],
                reader ? reader->pipe_ref : 0,
                reader ? reader->endpoint_number : 0);
        log_short_bytes("ignored input report", data, len, 96);
    }

}

static void release_device_interface(IOUSBDeviceInterface **device_interface)
{
    if (device_interface) {
        (*device_interface)->Release(device_interface);
    }
}

static void release_usb_interface(IOUSBInterfaceInterface **interface)
{
    if (interface) {
        (*interface)->Release(interface);
    }
}

static IOUSBDeviceInterface **create_device_interface(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    IOUSBDeviceInterface **device_interface = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(service,
                                                    kIOUSBDeviceUserClientTypeID,
                                                    kIOCFPlugInInterfaceID,
                                                    &plugin,
                                                    &score);
    if (kr != kIOReturnSuccess || !plugin) {
        log_iokit_error("IOCreatePlugInInterfaceForService(device)", kr);
        return NULL;
    }

    HRESULT hr = (*plugin)->QueryInterface(plugin,
                                           CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                           (LPVOID)&device_interface);
    IODestroyPlugInInterface(plugin);
    if (hr || !device_interface) {
        fprintf(stderr, "[mk1-usb] QueryInterface(device) failed: 0x%08x\n", (unsigned int)hr);
        return NULL;
    }

    return device_interface;
}

static IOUSBInterfaceInterface **create_usb_interface(io_service_t service)
{
    IOCFPlugInInterface **plugin = NULL;
    IOUSBInterfaceInterface **interface = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(service,
                                                    kIOUSBInterfaceUserClientTypeID,
                                                    kIOCFPlugInInterfaceID,
                                                    &plugin,
                                                    &score);
    if (kr != kIOReturnSuccess || !plugin) {
        log_iokit_error("IOCreatePlugInInterfaceForService(interface)", kr);
        return NULL;
    }

    HRESULT hr = (*plugin)->QueryInterface(plugin,
                                           CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
                                           (LPVOID)&interface);
    IODestroyPlugInInterface(plugin);
    if (hr || !interface) {
        fprintf(stderr, "[mk1-usb] QueryInterface(interface) failed: 0x%08x\n", (unsigned int)hr);
        return NULL;
    }

    return interface;
}

static io_service_t find_matching_usb_service(char *serial, size_t serial_size)
{
    static const char *classes[] = { "IOUSBDevice", "IOUSBHostDevice" };

    for (size_t class_index = 0; class_index < sizeof(classes) / sizeof(classes[0]); class_index++) {
        CFMutableDictionaryRef matching = IOServiceMatching(classes[class_index]);
        io_iterator_t iterator = IO_OBJECT_NULL;

        if (!matching) continue;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != kIOReturnSuccess) {
            continue;
        }

        io_service_t service = IO_OBJECT_NULL;
        while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            uint32_t vendor = 0;
            uint32_t product = 0;
            char candidate_serial[64] = {0};

            log_usb_candidate(service, classes[class_index]);
            if (!get_device_identity(service, &vendor, &product, candidate_serial, sizeof(candidate_serial))) {
                IOObjectRelease(service);
                continue;
            }
            if (vendor == MK1_VENDOR_ID && product == MK1_PRODUCT_ID) {
                if (serial && serial_size > 0 && candidate_serial[0]) {
                    strlcpy(serial, candidate_serial, serial_size);
                }
                IOObjectRelease(iterator);
                return service;
            }

            IOObjectRelease(service);
        }

        IOObjectRelease(iterator);
    }

    return IO_OBJECT_NULL;
}

static bool open_device_interface(mk1_device_t *dev)
{
    UInt8 configuration = 0;
    IOReturn kr;

    dev->device_interface = create_device_interface(dev->service);
    if (!dev->device_interface) {
        return false;
    }

    kr = (*dev->device_interface)->USBDeviceOpen(dev->device_interface);
    if (kr == kIOReturnExclusiveAccess) {
        kr = (*dev->device_interface)->USBDeviceOpenSeize(dev->device_interface);
    }
    if (kr != kIOReturnSuccess) {
        log_iokit_error("USBDeviceOpen", kr);
        release_device_interface(dev->device_interface);
        dev->device_interface = NULL;
        return false;
    }
    dev->device_open = true;

    kr = (*dev->device_interface)->GetConfiguration(dev->device_interface, &configuration);
    if (kr != kIOReturnSuccess) {
        log_iokit_error("GetConfiguration", kr);
        return true;
    }

    fprintf(stderr, "[mk1-usb] current configuration=%u\n", configuration);
    if (configuration == 0) {
        kr = (*dev->device_interface)->SetConfiguration(dev->device_interface, 1);
        if (kr != kIOReturnSuccess) {
            log_iokit_error("SetConfiguration(1)", kr);
            return false;
        }
        fprintf(stderr, "[mk1-usb] configuration set to 1\n");
    }

    return true;
}

static bool open_interface(mk1_device_t *dev)
{
    IOUSBFindInterfaceRequest request;
    io_iterator_t iterator = IO_OBJECT_NULL;
    io_service_t service = IO_OBJECT_NULL;
    io_service_t fallback_service = IO_OBJECT_NULL;
    IOUSBInterfaceInterface **fallback_interface = NULL;
    UInt8 fallback_alt_setting = 0xff;
    IOReturn kr;

    memset(&request, 0xff, sizeof(request));
    kr = (*dev->device_interface)->CreateInterfaceIterator(dev->device_interface, &request, &iterator);
    if (kr != kIOReturnSuccess) {
        log_iokit_error("CreateInterfaceIterator", kr);
        return false;
    }

    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        IOUSBInterfaceInterface **interface = create_usb_interface(service);
        UInt8 interface_number = 0xff;
        UInt8 alternate_setting = 0xff;
        UInt8 interface_class = 0xff;
        UInt8 interface_subclass = 0xff;
        UInt8 interface_protocol = 0xff;

        if (!interface) {
            IOObjectRelease(service);
            continue;
        }

        (*interface)->GetInterfaceNumber(interface, &interface_number);
        (*interface)->GetAlternateSetting(interface, &alternate_setting);
        (*interface)->GetInterfaceClass(interface, &interface_class);
        (*interface)->GetInterfaceSubClass(interface, &interface_subclass);
        (*interface)->GetInterfaceProtocol(interface, &interface_protocol);

        fprintf(stderr,
                "[mk1-usb] interface candidate iface=%u alt=%u class=0x%02x sub=0x%02x proto=0x%02x\n",
                interface_number,
                alternate_setting,
                interface_class,
                interface_subclass,
                interface_protocol);
        log_service_path(service, kIOServicePlane, "interface");
        log_service_path(service, kIOUSBPlane, "interface");

        if (interface_number != MK1_INTERFACE_NUMBER) {
            release_usb_interface(interface);
            IOObjectRelease(service);
            continue;
        }

        if (alternate_setting == MK1_ALTERNATE_SETTING) {
            dev->interface_service = service;
            dev->interface = interface;
            break;
        }

        if (!fallback_interface) {
            fallback_interface = interface;
            fallback_service = service;
            fallback_alt_setting = alternate_setting;
            continue;
        }

        release_usb_interface(interface);
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    if (!dev->interface) {
        dev->interface = fallback_interface;
        dev->interface_service = fallback_service;
        if (dev->interface) {
            fprintf(stderr,
                    "[mk1-usb] falling back to interface %u alt=%u and will switch to alt %u\n",
                    MK1_INTERFACE_NUMBER,
                    fallback_alt_setting,
                    MK1_ALTERNATE_SETTING);
        }
    } else {
        if (fallback_interface) {
            release_usb_interface(fallback_interface);
        }
        if (fallback_service != IO_OBJECT_NULL) {
            IOObjectRelease(fallback_service);
        }
    }

    if (!dev->interface || dev->interface_service == IO_OBJECT_NULL) {
        fprintf(stderr, "[mk1-usb] failed to locate interface %u\n", MK1_INTERFACE_NUMBER);
        return false;
    }

    kr = (*dev->interface)->USBInterfaceOpen(dev->interface);
    if (kr == kIOReturnExclusiveAccess) {
        kr = (*dev->interface)->USBInterfaceOpenSeize(dev->interface);
    }
    if (kr != kIOReturnSuccess) {
        log_iokit_error("USBInterfaceOpen", kr);
        return false;
    }
    dev->interface_open = true;

    UInt8 alternate_setting = 0xff;
    (*dev->interface)->GetAlternateSetting(dev->interface, &alternate_setting);
    if (alternate_setting != MK1_ALTERNATE_SETTING) {
        kr = (*dev->interface)->SetAlternateInterface(dev->interface, MK1_ALTERNATE_SETTING);
        if (kr != kIOReturnSuccess) {
            log_iokit_error("SetAlternateInterface(1)", kr);
            return false;
        }
        fprintf(stderr, "[mk1-usb] alternate setting switched to %u\n", MK1_ALTERNATE_SETTING);
    }

    return true;
}

static bool enumerate_pipes(mk1_device_t *dev)
{
    UInt8 endpoint_count = 0;
    int input_rank[MK1_MAX_INPUT_PIPES] = {0};
    IOReturn kr = (*dev->interface)->GetNumEndpoints(dev->interface, &endpoint_count);
    if (kr != kIOReturnSuccess) {
        log_iokit_error("GetNumEndpoints", kr);
        return false;
    }

    fprintf(stderr, "[mk1-usb] interface has %u endpoint pipes\n", endpoint_count);
    dev->input_pipe_count = 0;
    dev->output_pipe_count = 0;
    memset(dev->input_pipes, 0, sizeof(dev->input_pipes));
    memset(dev->output_pipes, 0, sizeof(dev->output_pipes));

    for (UInt8 pipe_ref = 1; pipe_ref <= endpoint_count; pipe_ref++) {
        UInt8 direction = 0;
        UInt8 endpoint_number = 0;
        UInt8 transfer_type = 0;
        UInt16 max_packet_size = 0;
        UInt8 interval = 0;

        kr = (*dev->interface)->GetPipeProperties(dev->interface,
                                                  pipe_ref,
                                                  &direction,
                                                  &endpoint_number,
                                                  &transfer_type,
                                                  &max_packet_size,
                                                  &interval);
        if (kr != kIOReturnSuccess) {
            log_iokit_error("GetPipeProperties", kr);
            continue;
        }

        fprintf(stderr,
                "[mk1-usb] pipe=%u endpoint=%u dir=%s type=%s maxPacket=%u interval=%u\n",
                pipe_ref,
                endpoint_number,
                direction_name(direction),
                transfer_type_name(transfer_type),
                max_packet_size,
                interval);

        if (direction != 0 && (transfer_type == 2 || transfer_type == 3)) {
            if (dev->input_pipe_count < MK1_MAX_INPUT_PIPES) {
                UInt8 index = dev->input_pipe_count++;
                dev->input_pipes[index] = pipe_ref;
                input_rank[index] = preferred_input_pipe_rank(pipe_ref, endpoint_number);
            }
        }
        if (direction == 0 && (transfer_type == 2 || transfer_type == 3)) {
            if (dev->output_pipe_count < MK1_MAX_OUTPUT_PIPES) {
                dev->output_pipes[dev->output_pipe_count++] = pipe_ref;
            }
        }
    }

    fprintf(stderr, "[mk1-usb] selected input pipes:");
    for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
        fprintf(stderr, " %u", dev->input_pipes[i]);
    }
    if (dev->input_pipe_count == 0) fprintf(stderr, " <none>");
    fprintf(stderr, "\n");

    fprintf(stderr, "[mk1-usb] selected output pipes:");
    for (UInt8 i = 0; i < dev->output_pipe_count; i++) {
        fprintf(stderr, " %u", dev->output_pipes[i]);
    }
    if (dev->output_pipe_count == 0) fprintf(stderr, " <none>");
    fprintf(stderr, "\n");

    for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
        for (UInt8 j = i + 1; j < dev->input_pipe_count; j++) {
            if (input_rank[j] < input_rank[i]) {
                int rank_tmp = input_rank[i];
                UInt8 pipe_tmp = dev->input_pipes[i];
                input_rank[i] = input_rank[j];
                dev->input_pipes[i] = dev->input_pipes[j];
                input_rank[j] = rank_tmp;
                dev->input_pipes[j] = pipe_tmp;
            }
        }
    }

    fprintf(stderr, "[mk1-usb] preferred input order:");
    for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
        fprintf(stderr, " %u", dev->input_pipes[i]);
    }
    if (dev->input_pipe_count == 0) fprintf(stderr, " <none>");
    fprintf(stderr, "\n");

    return dev->input_pipe_count != 0;
}

static void *read_thread_main(void *context)
{
    mk1_pipe_reader_t *reader = (mk1_pipe_reader_t *)context;
    mk1_device_t *dev = reader->device;
    UInt8 pipe_ref = reader->pipe_ref;

    while (dev->running) {
        uint8_t buffer[MK1_READ_BUFFER_SIZE] = {0};
        UInt32 size = sizeof(buffer);
        IOReturn kr;

        if (!dev->interface || pipe_ref == 0) {
            break;
        }

        kr = (*dev->interface)->ReadPipe(dev->interface, pipe_ref, buffer, &size);
        if (kr == kIOReturnAborted || kr == kIOReturnNotOpen) {
            break;
        }
        if (kr != kIOReturnSuccess) {
            fprintf(stderr, "[mk1-usb] ReadPipe(pipe=%u) failed: 0x%08x\n", pipe_ref, kr);
            usleep(50000);
            continue;
        }
        if (size == 0) {
            continue;
        }

        log_all_pipe_report(reader, buffer, size);
        if (reader->endpoint_number == 1) {
            mk1_device_process_ep1_button_packet(dev, reader, buffer, size);
            mk1_device_handle_ep1_reply(dev, buffer, size);
            continue;
        }

        log_raw_report(reader, buffer, size);
        mk1_device_dispatch_input_report(reader, buffer, size);
    }

    return NULL;
}

mk1_device_t *mk1_device_open(void)
{
    mk1_device_t *dev = calloc(1, sizeof(mk1_device_t));
    if (!dev) return NULL;
    dev->trace_reports = env_flag_enabled("MK1_USB_TRACE");
    dev->trace_scan_reports = env_flag_enabled("MK1_SCAN_TRACE");
    dev->trace_all_pipes = env_flag_enabled("MK1_ALL_PIPE_TRACE");
    pthread_mutex_init(&dev->reply_lock, NULL);

    dev->service = find_matching_usb_service(dev->serial, sizeof(dev->serial));
    if (dev->service == IO_OBJECT_NULL) {
        fprintf(stderr, "[mk1-usb] MK1 not present in USB registry (VID=0x%04x PID=0x%04x)\n",
                MK1_VENDOR_ID, MK1_PRODUCT_ID);
        pthread_mutex_destroy(&dev->reply_lock);
        free(dev);
        return NULL;
    }

    if (dev->serial[0]) {
        fprintf(stderr, "[mk1-usb] device opened via USB registry (serial='%s')\n", dev->serial);
    } else {
        fprintf(stderr, "[mk1-usb] device opened via USB registry (serial unavailable)\n");
    }
    if (dev->trace_reports) {
        fprintf(stderr, "[mk1-usb] raw USB report tracing enabled via MK1_USB_TRACE\n");
    }
    if (dev->trace_scan_reports) {
        fprintf(stderr, "[mk1-usb] scan-frame tracing enabled via MK1_SCAN_TRACE\n");
    }
    if (dev->trace_all_pipes) {
        fprintf(stderr, "[mk1-usb] all-pipe tracing enabled via MK1_ALL_PIPE_TRACE\n");
    }
    log_service_path(dev->service, kIOServicePlane, "device");
    log_service_path(dev->service, kIOUSBPlane, "device");

    if (!open_device_interface(dev) || !open_interface(dev) || !enumerate_pipes(dev)) {
        mk1_device_close(dev);
        return NULL;
    }

    return dev;
}

void mk1_device_close(mk1_device_t *dev)
{
    if (!dev) return;

    mk1_device_stop(dev);

    if (dev->interface) {
        release_usb_interface(dev->interface);
        dev->interface = NULL;
    }
    if (dev->device_interface) {
        release_device_interface(dev->device_interface);
        dev->device_interface = NULL;
    }
    if (dev->interface_service != IO_OBJECT_NULL) {
        IOObjectRelease(dev->interface_service);
        dev->interface_service = IO_OBJECT_NULL;
    }
    if (dev->service != IO_OBJECT_NULL) {
        IOObjectRelease(dev->service);
        dev->service = IO_OBJECT_NULL;
    }

    pthread_mutex_destroy(&dev->reply_lock);
    free(dev);
    fprintf(stderr, "[mk1-usb] device closed\n");
}

bool mk1_device_is_open(const mk1_device_t *dev)
{
    return dev && dev->service != IO_OBJECT_NULL && dev->interface != NULL;
}

bool mk1_device_get_serial(const mk1_device_t *dev, char *serial, size_t len)
{
    if (!dev || !serial || len == 0 || dev->serial[0] == '\0') return false;
    strlcpy(serial, dev->serial, len);
    return true;
}

static UInt8 find_output_pipe_for_endpoint(const mk1_device_t *dev, UInt8 endpoint_number)
{
    if (!dev || !dev->interface) return 0;

    for (UInt8 i = 0; i < dev->output_pipe_count; i++) {
        UInt8 pipe_ref = dev->output_pipes[i];
        UInt8 direction = 0;
        UInt8 pipe_endpoint = 0;
        UInt8 transfer_type = 0;
        UInt16 max_packet_size = 0;
        UInt8 interval = 0;

        if (pipe_ref == 0) continue;
        if ((*dev->interface)->GetPipeProperties(dev->interface,
                                                 pipe_ref,
                                                 &direction,
                                                 &pipe_endpoint,
                                                 &transfer_type,
                                                 &max_packet_size,
                                                 &interval) != kIOReturnSuccess) {
            continue;
        }
        if (direction == 0 && pipe_endpoint == endpoint_number) {
            return pipe_ref;
        }
    }

    return 0;
}

bool mk1_device_write_endpoint(mk1_device_t *dev,
                               uint8_t endpoint_number,
                               const uint8_t *data,
                               size_t len)
{
    UInt8 pipe_ref = 0;
    IOReturn kr;

    if (!dev || !dev->interface || !data || len == 0) {
        return false;
    }

    pipe_ref = find_output_pipe_for_endpoint(dev, endpoint_number);
    if (pipe_ref == 0) {
        fprintf(stderr, "[mk1-usb] no output pipe found for endpoint 0x%02x\n", endpoint_number);
        return false;
    }

    kr = (*dev->interface)->WritePipe(dev->interface, pipe_ref, (void *)data, (UInt32)len);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr,
                "[mk1-usb] WritePipe(pipe=%u endpoint=0x%02x len=%zu) failed: 0x%08x\n",
                pipe_ref,
                endpoint_number,
                len,
                kr);
        log_short_bytes("write payload", data, len, 96);
        return false;
    }

    fprintf(stderr,
            "[mk1-usb] wrote endpoint 0x%02x via pipe %u (%zu bytes)\n",
            endpoint_number,
            pipe_ref,
            len);
    log_short_bytes("write payload", data, len, 64);
    return true;
}

bool mk1_device_init_hardware(mk1_device_t *dev)
{
    // Caiaq protocol init sequence (from Linux snd-usb-caiaq driver + Wireshark capture).
    // Without this, the device stays in post-enumeration idle and never activates.

    if (!dev || !dev->interface || dev->output_pipe_count == 0) {
        fprintf(stderr, "[mk1-usb] init_hardware: no output pipe available\n");
        return false;
    }

    fprintf(stderr, "[mk1-usb] sending caiaq init sequence\n");

    // 1. GET_DEVICE_INFO (0x01) — wakes the controller, triggers device spec reply on EP1 IN
    {
        uint8_t cmd = 0x01;
        if (!mk1_device_write_endpoint(dev, 0x01, &cmd, 1)) {
            fprintf(stderr, "[mk1-usb] init_hardware: GET_DEVICE_INFO failed\n");
            return false;
        }
        mk1_device_drain_ep1_replies(dev, 20, 8);
        usleep(50000); // 50ms — give device time to process and respond
    }

    // 2. AUTO_MSG (0x0b) — enable spontaneous reports for buttons, knobs, encoders
    //    Parameters from macOS kext capture: digital=1, analog=2, erp=5
    {
        uint8_t auto_msg[] = { 0x0b, 0x01, 0x02, 0x05 };
        if (!mk1_device_write_endpoint(dev, 0x01, auto_msg, sizeof(auto_msg))) {
            fprintf(stderr, "[mk1-usb] init_hardware: AUTO_MSG failed\n");
            return false;
        }
        mk1_device_drain_ep1_replies(dev, 5, 4);
        usleep(10000);
    }

    // 3. DIMM_LEDS — clear the full 32-byte LED state frame.
    // The validated EP1 format is [0x0c, phys0..phys31] with no start index.
    {
        uint8_t leds_clear[33];
        memset(leds_clear, 0, sizeof(leds_clear));
        leds_clear[0] = 0x0c;
        if (!mk1_device_write_endpoint(dev, 0x01, leds_clear, sizeof(leds_clear))) {
            fprintf(stderr, "[mk1-usb] init_hardware: DIMM_LEDS clear failed\n");
            return false;
        }
        mk1_device_drain_ep1_replies(dev, 5, 4);
        usleep(2000);
    }

    // 4. EP8 display config — init BOTH displays (0x00=left, 0x02=right)
    //    Controller: ST7529 family (not SSD1327).
    //    17-command sequence confirmed verbatim from usb.pcapng analysis.
    //    Format: [display_idx, len_hi, len_lo, command_data...]
    {
        static const struct { size_t len; uint8_t bytes[8]; } display_cmds[] = {
            // 17-command init sequence (ep08.txt pcap-confirmed, verbatim).
            // 0xbc uses UI-mode scan direction [0x02,0x01,0x01] (not animation [0x00,0x00,0x02]).
            { 1, { 0x30 } },                    // enter extension set (SEC)
            { 4, { 0xca, 0x04, 0x0f, 0x00 } },  // duty / display lines
            { 2, { 0xbb, 0x00 } },               // COM scan direction
            { 1, { 0xd1 } },                    // power on sequence
            { 1, { 0x94 } },                    // sleep out
            { 3, { 0x81, 0x1e, 0x02 } },         // electronic volume (contrast = 0x1e)
            { 2, { 0x20, 0x08 } },               // power control
            { 2, { 0x20, 0x0b } },               // power control
            { 1, { 0xa6 } },                    // normal display, non-inverted
            { 1, { 0x31 } },                    // exit extension set
            { 4, { 0x32, 0x00, 0x00, 0x05 } },   // scroll/scan config
            { 1, { 0x34 } },                    // scroll off
            { 1, { 0x30 } },                    // re-enter extension set
            { 4, { 0xbc, 0x02, 0x01, 0x01 } },   // data scan direction: UI mode (pcap transition)
            { 3, { 0x75, 0x00, 0x3f } },          // row address range: 0–63
            { 3, { 0x15, 0x00, 0x54 } },          // col address range: 0–84 (170px)
            { 3, { 0x81, 0x20, 0x02 } },          // electronic volume (contrast = 0x20)
            { 1, { 0xaf } },                    // display ON (extension mode, cmd 18)
        };
        size_t ep8_ok = 0, ep8_fail = 0;

        // Check if EP8 is available before sending commands
        UInt8 ep8_pipe = find_output_pipe_for_endpoint(dev, 0x08);
        if (ep8_pipe == 0) {
            fprintf(stderr, "[mk1-usb] *** EP8 NOT AVAILABLE — display will not work ***\n");
            fprintf(stderr, "[mk1-usb] output pipes available:");
            for (UInt8 i = 0; i < dev->output_pipe_count; i++) {
                UInt8 dir = 0, epnum = 0, xfer = 0;
                UInt16 maxpkt = 0;
                UInt8 intvl = 0;
                if ((*dev->interface)->GetPipeProperties(dev->interface,
                        dev->output_pipes[i], &dir, &epnum, &xfer, &maxpkt, &intvl)
                        == kIOReturnSuccess) {
                    fprintf(stderr, " EP%u(pipe=%u,max=%u)", epnum, dev->output_pipes[i], maxpkt);
                }
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "[mk1-usb] EP8 found (pipe=%u) — initializing displays\n", ep8_pipe);
        }

        // Send init commands to both displays (0x00=left, 0x02=right)
        for (uint8_t disp = 0; disp <= 2; disp += 2) {
            for (size_t i = 0; i < sizeof(display_cmds) / sizeof(display_cmds[0]); i++) {
                if (mk1_set_display(dev, disp, display_cmds[i].bytes, display_cmds[i].len)) {
                    ep8_ok++;
                } else {
                    ep8_fail++;
                }
                usleep(2000);
            }
        }

        fprintf(stderr, "[mk1-usb] display init: %zu OK, %zu FAILED (sent to both displays)\n",
                ep8_ok, ep8_fail);
    }

    fprintf(stderr, "[mk1-usb] caiaq init sequence complete — device should be active\n");
    return true;
}

bool mk1_device_replay_startup_init(mk1_device_t *dev)
{
    typedef struct {
        uint8_t endpoint_number;
        size_t  len;
        uint8_t bytes[40];
    } mk1_init_packet_t;

    static const mk1_init_packet_t init_packets[] = {
        { 0x01, 1, { 0x01 } },
        { 0x08, 4, { 0x00, 0x00, 0x01, 0x30 } },
        { 0x01, 4, { 0x0b, 0x01, 0x02, 0x05 } },
        { 0x01, 33, {
            0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00
        } },
        { 0x08, 7, { 0x00, 0x00, 0x04, 0xca, 0x04, 0x0f, 0x00 } },
        { 0x08, 5, { 0x00, 0x00, 0x02, 0xbb, 0x00 } },
        { 0x08, 4, { 0x00, 0x00, 0x01, 0xd1 } },
        { 0x08, 4, { 0x00, 0x00, 0x01, 0x94 } },
        { 0x08, 6, { 0x00, 0x00, 0x03, 0x81, 0x1e, 0x02 } },
        { 0x08, 5, { 0x00, 0x00, 0x02, 0x20, 0x08 } },
        { 0x08, 5, { 0x00, 0x00, 0x02, 0x20, 0x0b } },
        { 0x08, 4, { 0x00, 0x00, 0x01, 0xa4 } },
        { 0x08, 4, { 0x00, 0x00, 0x01, 0xaf } },
    };

    if (!dev || !dev->interface || dev->output_pipe_count == 0) {
        fprintf(stderr, "[mk1-usb] startup init unavailable: no output pipe\n");
        return false;
    }

    fprintf(stderr, "[mk1-usb] replaying captured startup init (%zu packets)\n",
            sizeof(init_packets) / sizeof(init_packets[0]));

    for (size_t i = 0; i < sizeof(init_packets) / sizeof(init_packets[0]); i++) {
        if (!mk1_device_write_endpoint(dev,
                                       init_packets[i].endpoint_number,
                                       init_packets[i].bytes,
                                       init_packets[i].len)) {
            fprintf(stderr, "[mk1-usb] startup init stopped at packet %zu\n", i + 1);
            return false;
        }
        usleep(2000);
    }

    replay_capture_file(dev, 0x01, "ep01.txt", 2, 0);
    replay_capture_file(dev, 0x08, "ep08.txt", 0, 0);
    replay_visual_cleanup(dev);

    fprintf(stderr, "[mk1-usb] startup init replay complete\n");
    return true;
}

bool mk1_device_start(mk1_device_t *dev,
                      mk1_pad_callback_t pad_cb,
                      mk1_button_callback_t button_cb,
                      void *context)
{
    if (!dev || dev->running || !dev->interface || dev->input_pipe_count == 0) {
        return false;
    }

    dev->pad_cb = pad_cb;
    dev->button_cb = button_cb;
    dev->cb_context = context;
    dev->running = true;

    for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
        dev->readers[i].device = dev;
        dev->readers[i].pipe_ref = dev->input_pipes[i];
        dev->readers[i].endpoint_number = 0;
        dev->readers[i].started = false;
        get_pipe_endpoint_number(dev, dev->input_pipes[i], &dev->readers[i].endpoint_number);

        if (pthread_create(&dev->readers[i].thread, NULL, read_thread_main, &dev->readers[i]) != 0) {
            dev->running = false;
            fprintf(stderr, "[mk1-usb] failed to create read thread for pipe %u\n",
                    dev->input_pipes[i]);
            for (UInt8 j = 0; j < i; j++) {
                if (dev->readers[j].started) {
                    pthread_join(dev->readers[j].thread, NULL);
                    dev->readers[j].started = false;
                }
            }
            return false;
        }
        dev->readers[i].started = true;
    }

    fprintf(stderr, "[mk1-usb] USB read loops active on");
    for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
        fprintf(stderr, " pipe %u", dev->input_pipes[i]);
    }
    fprintf(stderr, "\n");
    return true;
}

void mk1_device_stop(mk1_device_t *dev)
{
    if (!dev) return;

    if (dev->running) {
        dev->running = false;
        if (dev->interface_open && dev->interface) {
            for (UInt8 i = 0; i < dev->input_pipe_count; i++) {
                if (dev->input_pipes[i] != 0) {
                    (*dev->interface)->AbortPipe(dev->interface, dev->input_pipes[i]);
                }
            }
        }
    }

    for (UInt8 i = 0; i < MK1_MAX_INPUT_PIPES; i++) {
        if (dev->readers[i].started) {
            pthread_join(dev->readers[i].thread, NULL);
            dev->readers[i].started = false;
        }
    }

    if (dev->interface_open && dev->interface) {
        (*dev->interface)->USBInterfaceClose(dev->interface);
        dev->interface_open = false;
    }
    if (dev->device_open && dev->device_interface) {
        (*dev->device_interface)->USBDeviceClose(dev->device_interface);
        dev->device_open = false;
    }
}

bool mk1_set_led(mk1_device_t *dev, uint8_t led_index, uint8_t brightness)
{
    uint8_t packet[33] = {0};

    if (!dev || !dev->interface || dev->output_pipe_count == 0) {
        fprintf(stderr, "[mk1-usb] mk1_set_led unavailable: no output pipe\n");
        return false;
    }
    if (led_index >= 32) {
        fprintf(stderr, "[mk1-usb] mk1_set_led invalid index %u\n", led_index);
        return false;
    }

    // Convenience helper: emit a complete 32-byte DIMM_LEDS frame with one slot set.
    packet[0] = 0x0c;
    packet[1 + led_index] = mk1_normalize_led_brightness(brightness);
    return mk1_device_write_endpoint(dev, 0x01, packet, sizeof(packet));
}

bool mk1_set_display(mk1_device_t *dev, uint8_t display_index,
                     const uint8_t *pixels, size_t len)
{
    uint8_t packet[512];

    if (len > 0x1fc) {
        fprintf(stderr, "[mk1-usb] mk1_set_display payload too large: %zu bytes\n", len);
        return false;
    }

    if (len > sizeof(packet) - 3) {
        fprintf(stderr, "[mk1-usb] mk1_set_display too large: %zu bytes\n", len);
        return false;
    }

    if (!dev || !dev->interface || dev->output_pipe_count == 0) {
        fprintf(stderr, "[mk1-usb] mk1_set_display unavailable: no output pipe\n");
        return false;
    }

    packet[0] = display_index;
    packet[1] = (uint8_t)((len >> 8) & 0xff);
    packet[2] = (uint8_t)(len & 0xff);
    memcpy(packet + 3, pixels, len);
    return mk1_device_write_endpoint(dev, 0x08, packet, len + 3);
}
static int32_t mk1_uc_send_ep1_command(mk1_user_client_t *client,
                                       uint8_t command,
                                       const void *payload,
                                       size_t payload_len,
                                       const char *label)
{
    uint8_t packet[512];

    if (!client || !client->device) return kIOReturnNotOpen;
    if (payload_len >= 0x40) return kIOReturnBadArgument;
    if (payload_len > sizeof(packet) - 1) return kIOReturnBadArgument;

    packet[0] = command;
    if (payload && payload_len > 0) {
        memcpy(packet + 1, payload, payload_len);
    }

    if (!mk1_device_write_endpoint(client->device, 0x01, packet, payload_len + 1)) {
        return kIOReturnError;
    }

    mk1_device_drain_ep1_replies(client->device, 10, 8);

    fprintf(stderr,
            "[mk1-user-client] %s -> EP1 cmd=0x%02x payload=%zu\n",
            label ? label : "command",
            command,
            payload_len);
    return kIOReturnSuccess;
}

static void mk1_uc_fill_device_info(mk1_user_client_t *client, void *buffer, size_t len)
{
    uint8_t *bytes = (uint8_t *)buffer;
    uint16_t vendor_id = MK1_VENDOR_ID;
    uint16_t product_id = MK1_PRODUCT_ID;
    const char *serial = (client && client->serial[0]) ? client->serial : "SN-unknown";
    const char *product = "Maschine Controller";
    const char *vendor = "Native Instruments";

    memset(bytes, 0, len);
    memcpy(bytes + 0x00, &vendor_id, sizeof(vendor_id));
    memcpy(bytes + 0x02, &product_id, sizeof(product_id));
    strncpy((char *)(bytes + 0x04), serial, 0x20);
    strncpy((char *)(bytes + 0x25), product, 0x20);
    strncpy((char *)(bytes + 0x46), vendor, 0x20);
}

static void mk1_uc_fill_device_spec(mk1_user_client_t *client, void *buffer, size_t len)
{
    uint8_t *bytes = (uint8_t *)buffer;
    uint16_t vendor_id = MK1_VENDOR_ID;
    uint16_t product_id = MK1_PRODUCT_ID;

    memset(bytes, 0, len);

    if (client && client->device) {
        pthread_mutex_lock(&client->device->reply_lock);
        if (client->device->have_device_spec_reply) {
            memcpy(bytes, client->device->device_spec_reply, len);
            pthread_mutex_unlock(&client->device->reply_lock);
            return;
        }
        pthread_mutex_unlock(&client->device->reply_lock);
    }

    memcpy(bytes + 0x00, &vendor_id, sizeof(vendor_id));
    memcpy(bytes + 0x02, &product_id, sizeof(product_id));
    bytes[0x04] = 0x01;
    bytes[0x05] = 0x00;
    bytes[0x0b] = 0x01;
}

static mk1_uc_async_registration_t *mk1_uc_async_slot(mk1_user_client_t *client,
                                                       uint32_t selector,
                                                       bool *timestamped)
{
    if (!client) return NULL;
    if (timestamped) *timestamped = false;

    switch (selector) {
    case MK1_UC_ASYNC_MIDI_READ:
        return &client->midi_read;
    case MK1_UC_ASYNC_ANALOG_INPUT_READ:
        return &client->analog_read;
    case MK1_UC_ASYNC_DIGITAL_INPUT_READ:
        return &client->digital_read;
    case MK1_UC_ASYNC_ERP_INPUT_READ:
        return &client->erp_read;
    case MK1_UC_ASYNC_SAMPLE_BUFFER_READ:
        return &client->sample_buffer_read;
    case MK1_UC_ASYNC_ENCODER_INPUT_READ:
        return &client->encoder_read;
    case MK1_UC_ASYNC_MIDI_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &client->midi_read;
    case MK1_UC_ASYNC_ANALOG_INPUT_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &client->analog_read;
    case MK1_UC_ASYNC_DIGITAL_INPUT_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &client->digital_read;
    case MK1_UC_ASYNC_ERP_INPUT_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &client->erp_read;
    case MK1_UC_ASYNC_SAMPLE_BUFFER_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &client->sample_buffer_read;
    default:
        return NULL;
    }
}

mk1_user_client_t *mk1_user_client_open(void)
{
    mk1_user_client_t *client = calloc(1, sizeof(*client));
    if (!client) return NULL;

    client->device = mk1_device_open();
    if (!client->device) {
        free(client);
        return NULL;
    }
    if (!mk1_device_get_serial(client->device, client->serial, sizeof(client->serial))) {
        strlcpy(client->serial, "SN-unknown", sizeof(client->serial));
    }
    if (!mk1_device_init_hardware(client->device)) {
        mk1_user_client_close(client);
        return NULL;
    }

    fprintf(stderr,
            "[mk1-user-client] opened serial='%s' with hardware init\n",
            client->serial);
    return client;
}

void mk1_user_client_close(mk1_user_client_t *client)
{
    if (!client) return;
    for (size_t i = 0; i < 2; i++) {
        free(client->client_memory[i]);
        client->client_memory[i] = NULL;
        client->client_memory_size[i] = 0;
    }
    if (client->device) {
        mk1_device_close(client->device);
        client->device = NULL;
    }
    free(client);
}

bool mk1_user_client_is_open(const mk1_user_client_t *client)
{
    return client && client->device && mk1_device_is_open(client->device);
}

int32_t mk1_user_client_call_method(mk1_user_client_t *client,
                                    uint32_t selector,
                                    const uint64_t *input_scalars,
                                    uint32_t input_scalar_count,
                                    const void *input_struct,
                                    size_t input_struct_count,
                                    uint64_t *output_scalars,
                                    uint32_t *output_scalar_count,
                                    void *output_struct,
                                    size_t *output_struct_count)
{
    (void)output_scalars;
    (void)output_scalar_count;

    if (!mk1_user_client_is_open(client)) return kIOReturnNotOpen;

    switch (selector) {
    case MK1_UC_SELECTOR_WRITE_IO: {
        size_t payload_len = input_struct_count;
        if ((!input_struct || input_struct_count == 0) && input_scalars && input_scalar_count > 0) {
            payload_len = (size_t)input_scalars[0];
        }
        if (!input_struct || payload_len == 0 || payload_len > input_struct_count) {
            return kIOReturnBadArgument;
        }
        return mk1_uc_send_ep1_command(client, 0x05, input_struct, payload_len, "writeIO");
    }
    case MK1_UC_SELECTOR_GET_DEVICE_INFO:
        if (!output_struct || !output_struct_count || *output_struct_count < MK1_UC_DEVICE_INFO_SIZE) {
            return kIOReturnBadArgument;
        }
        mk1_uc_fill_device_info(client, output_struct, MK1_UC_DEVICE_INFO_SIZE);
        *output_struct_count = MK1_UC_DEVICE_INFO_SIZE;
        return kIOReturnSuccess;
    case MK1_UC_SELECTOR_SET_AUTO_MSG: {
        uint8_t payload[3];
        if (!input_scalars || input_scalar_count < 3) return kIOReturnBadArgument;
        payload[0] = (uint8_t)input_scalars[0];
        payload[1] = (uint8_t)input_scalars[1];
        payload[2] = (uint8_t)input_scalars[2];
        return mk1_uc_send_ep1_command(client, 0x0b, payload, sizeof(payload), "setAutoMsg");
    }
    case MK1_UC_SELECTOR_SET_LEDS: {
        uint8_t remapped_payload[32];
        size_t payload_len = input_struct_count;
        if ((!input_struct || input_struct_count == 0) && input_scalars && input_scalar_count > 0) {
            payload_len = (size_t)input_scalars[0];
        }
        if (!input_struct || payload_len == 0) {
            return kIOReturnBadArgument;
        }
        if (payload_len == 33) {
            fprintf(stderr, "[mk1-user-client] setLEDs trimming legacy 33-byte payload to 32 bytes\n");
            payload_len = 32;
        }
        if (payload_len > 32) {
            return kIOReturnBadArgument;
        }
        payload_len = mk1_remap_led_payload(input_struct,
                                            payload_len,
                                            remapped_payload,
                                            sizeof(remapped_payload));
        return mk1_uc_send_ep1_command(client, 0x0c, remapped_payload, payload_len, "setLEDs");
    }
    case MK1_UC_SELECTOR_DISPLAY_COMMAND: {
        uint8_t display_index = 0;
        size_t payload_len = 0;
        if (!input_scalars || input_scalar_count < 2 || !input_struct) return kIOReturnBadArgument;
        display_index = (uint8_t)input_scalars[0];
        payload_len = (size_t)input_scalars[1];
        if (payload_len > input_struct_count) return kIOReturnBadArgument;
        return mk1_set_display(client->device, display_index, input_struct, payload_len)
            ? kIOReturnSuccess : kIOReturnError;
    }
    case MK1_UC_SELECTOR_GET_HARDWARE_BUFFER_SIZE:
    case MK1_UC_SELECTOR_SET_HARDWARE_BUFFER_SIZE:
    case MK1_UC_SELECTOR_MIDI_WRITE:
    case MK1_UC_SELECTOR_MIDI_WRITE_FAKE:
    case MK1_UC_SELECTOR_DIGITAL_INPUT_ARM:
        return kIOReturnSuccess;
    case MK1_UC_SELECTOR_SET_THRESHOLDS: {
        size_t payload_len = input_struct_count;
        if ((!input_struct || input_struct_count == 0) && input_scalars && input_scalar_count > 0) {
            payload_len = (size_t)input_scalars[0];
        }
        if (!input_struct || payload_len == 0 || payload_len > input_struct_count) {
            return kIOReturnBadArgument;
        }
        return mk1_uc_send_ep1_command(client, 0x0f, input_struct, payload_len, "setThresholds");
    }
    case MK1_UC_SELECTOR_DISPLAY_COMMAND_LONG: {
        uint8_t display_index = 0;
        size_t remaining = 0;
        size_t offset = 0;
        if (!input_scalars || input_scalar_count < 3 || !input_struct) return kIOReturnBadArgument;
        display_index = (uint8_t)input_scalars[0];
        remaining = (size_t)input_scalars[2];
        if (remaining > input_struct_count || remaining > 0x10000) return kIOReturnBadArgument;
        while (remaining > 0) {
            size_t chunk_len = remaining > 0x1fc ? 0x1fc : remaining;
            uint8_t chunk_index = offset == 0 ? display_index : (uint8_t)(display_index | 1u);
            if (!mk1_set_display(client->device,
                                 chunk_index,
                                 (const uint8_t *)input_struct + offset,
                                 chunk_len)) {
                return kIOReturnError;
            }
            remaining -= chunk_len;
            offset += chunk_len;
        }
        return kIOReturnSuccess;
    }
    case MK1_UC_SELECTOR_GET_DEVICE_SPEC:
        if (!output_struct || !output_struct_count || *output_struct_count < MK1_UC_DEVICE_SPEC_SIZE) {
            return kIOReturnBadArgument;
        }
        mk1_uc_fill_device_spec(client, output_struct, MK1_UC_DEVICE_SPEC_SIZE);
        *output_struct_count = MK1_UC_DEVICE_SPEC_SIZE;
        return kIOReturnSuccess;
    case MK1_UC_SELECTOR_READ_USER_DATA:
        if (!output_struct || !output_struct_count || *output_struct_count < MK1_UC_USER_DATA_SIZE) {
            return kIOReturnBadArgument;
        }
        if (!input_scalars || input_scalar_count < 1) return kIOReturnBadArgument;
        if (!client->user_data_valid) {
            memset(client->user_data, 0, sizeof(client->user_data));
            client->user_data_valid = true;
        }
        client->user_data[0] = (uint8_t)input_scalars[0];
        memcpy(output_struct, client->user_data, MK1_UC_USER_DATA_SIZE);
        *output_struct_count = MK1_UC_USER_DATA_SIZE;
        return kIOReturnSuccess;
    case MK1_UC_SELECTOR_WRITE_USER_DATA:
        if (!input_struct || input_struct_count != MK1_UC_USER_DATA_SIZE) return kIOReturnBadArgument;
        memcpy(client->user_data, input_struct, MK1_UC_USER_DATA_SIZE);
        client->user_data_valid = true;
        return mk1_uc_send_ep1_command(client, 0x13, input_struct, MK1_UC_USER_DATA_SIZE, "writeUserData");
    default:
        return kIOReturnUnsupported;
    }
}

int32_t mk1_user_client_call_async_method(mk1_user_client_t *client,
                                          uint32_t selector,
                                          const uint64_t *input_scalars,
                                          uint32_t input_scalar_count,
                                          const void *input_struct,
                                          size_t input_struct_count,
                                          bool timestamped)
{
    mk1_uc_async_registration_t *slot = NULL;
    bool inferred_timestamped = false;

    (void)input_struct;

    if (!mk1_user_client_is_open(client)) return kIOReturnNotOpen;

    slot = mk1_uc_async_slot(client, selector, &inferred_timestamped);
    if (!slot) {
        if (selector == MK1_UC_ASYNC_SERIAL_DATA_READ) return kIOReturnSuccess;
        return kIOReturnUnsupported;
    }
    if (slot->registered) return kIOReturnExclusiveAccess;

    if (selector != MK1_UC_ASYNC_SAMPLE_BUFFER_READ &&
        selector != MK1_UC_ASYNC_SAMPLE_BUFFER_READ_TIMESTAMPED) {
        if (!input_scalars || input_scalar_count < 2 || input_scalars[0] == 0 || input_scalars[1] == 0) {
            return kIOReturnBadArgument;
        }
    }

    slot->registered = true;
    slot->timestamped = timestamped || inferred_timestamped;
    slot->input_scalar_count = input_scalar_count;
    slot->input_struct_size = input_struct_count;

    fprintf(stderr,
            "[mk1-user-client] async selector=%u registered timestamped=%s scalars=%u struct=%zu\n",
            selector,
            slot->timestamped ? "true" : "false",
            input_scalar_count,
            input_struct_count);
    return kIOReturnSuccess;
}

bool mk1_user_client_map_memory(mk1_user_client_t *client,
                                uint32_t memory_type,
                                void **address,
                                size_t *size)
{
    if (!mk1_user_client_is_open(client) || !address || !size || memory_type > 1) {
        return false;
    }

    if (!client->client_memory[memory_type]) {
        client->client_memory[memory_type] = calloc(1, MK1_UC_CLIENT_MEM_SIZE);
        if (!client->client_memory[memory_type]) return false;
        client->client_memory_size[memory_type] = MK1_UC_CLIENT_MEM_SIZE;
    }

    *address = client->client_memory[memory_type];
    *size = client->client_memory_size[memory_type];
    return true;
}
