// mk1-shim.c
//
// DYLD_INSERT_LIBRARIES shim for recreating the minimal NIUSBUserClient subset
// Maschine needs for MK1 output control.
//
// Usage:
//   DYLD_INSERT_LIBRARIES=/path/to/libmk1-shim.dylib \
//   DYLD_FORCE_FLAT_NAMESPACE=1 \
//   /path/to/NIHardwareAgent-patched.app/Contents/MacOS/NIHardwareAgent

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "../mk1-usb/mk1_device.h"

// ---------------------------------------------------------------------------
// Logging — only log things that look NI/USB related
// ---------------------------------------------------------------------------

#define SHIM_LOG(fmt, ...) \
    fprintf(stderr, "[mk1-shim] " fmt "\n", ##__VA_ARGS__)

#define MK1_FAKE_ITERATOR ((io_iterator_t)0x4d4b4954u)
#define MK1_FAKE_SERVICE  ((io_service_t)0x4d4b5356u)
#define MK1_FAKE_CONNECT  ((io_connect_t)0x4d4b434eu)
#define MK1_DEVICE_INFO_SIZE 0x6e
#define MK1_CLIENT_MEMORY_SIZE 0x10000
#define MK1_USER_DATA_SIZE 0x21

enum {
    MK1_SELECTOR_I2C_READ = 0,
    MK1_SELECTOR_I2C_WRITE = 1,
    MK1_SELECTOR_WRITE_IO = 2,
    MK1_SELECTOR_GET_DEVICE_INFO = 3,
    MK1_SELECTOR_MIDI_WRITE = 4,
    MK1_SELECTOR_SET_AUTO_MSG = 5,
    MK1_SELECTOR_SET_LEDS = 6,
    MK1_SELECTOR_DISPLAY_COMMAND = 7,
    MK1_SELECTOR_GET_HARDWARE_BUFFER_SIZE = 8,
    MK1_SELECTOR_SET_HARDWARE_BUFFER_SIZE = 9,
    MK1_SELECTOR_SET_THRESHOLDS = 10,
    MK1_SELECTOR_MIDI_WRITE_FAKE = 12,
    MK1_SELECTOR_DISPLAY_COMMAND_LONG = 17,
    MK1_SELECTOR_GET_DEVICE_SPEC = 18,
    MK1_SELECTOR_READ_USER_DATA = 19,
    MK1_SELECTOR_WRITE_USER_DATA = 20,
    MK1_SELECTOR_DIGITAL_INPUT_ARM = 21,
};

enum {
    MK1_ASYNC_SELECTOR_MIDI_READ = 0,
    MK1_ASYNC_SELECTOR_ANALOG_INPUT_READ = 1,
    MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ = 2,
    MK1_ASYNC_SELECTOR_ERP_INPUT_READ = 3,
    MK1_ASYNC_SELECTOR_SERIAL_DATA_READ = 4,
    MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ = 5,
    MK1_ASYNC_SELECTOR_MIDI_READ_TIMESTAMPED = 6,
    MK1_ASYNC_SELECTOR_ANALOG_INPUT_READ_TIMESTAMPED = 7,
    MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ_TIMESTAMPED = 8,
    MK1_ASYNC_SELECTOR_ERP_INPUT_READ_TIMESTAMPED = 9,
    MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ_TIMESTAMPED = 10,
    MK1_ASYNC_SELECTOR_ENCODER_INPUT_READ = 11,
};

typedef struct {
    bool registered;
    bool timestamped;
    mach_port_t wake_port;
    uint32_t reference_count;
    uint32_t input_scalar_count;
    size_t input_struct_size;
} mk1_async_registration_t;

typedef struct {
    pthread_mutex_t lock;
    bool iterator_live;
    bool iterator_yielded_service;
    bool service_live;
    bool connect_live;
    mk1_device_t *device;
    bool device_ready;
    char serial[64];
    mk1_async_registration_t midi_read;
    mk1_async_registration_t analog_read;
    mk1_async_registration_t digital_read;
    mk1_async_registration_t erp_read;
    mk1_async_registration_t sample_buffer_read;
    mk1_async_registration_t encoder_read;
    mach_vm_address_t client_memory[2];
    mach_vm_size_t client_memory_size[2];
    uint8_t user_data[MK1_USER_DATA_SIZE];
    bool user_data_valid;
} mk1_shim_state_t;

static mk1_shim_state_t g_mk1_shim_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static const char *mk1_selector_name(uint32_t selector)
{
    switch (selector) {
    case MK1_SELECTOR_I2C_READ: return "I2CRead";
    case MK1_SELECTOR_I2C_WRITE: return "I2CWrite";
    case MK1_SELECTOR_WRITE_IO: return "writeIO";
    case MK1_SELECTOR_GET_DEVICE_INFO: return "getDeviceInfo";
    case MK1_SELECTOR_MIDI_WRITE: return "MIDIWrite";
    case MK1_SELECTOR_SET_AUTO_MSG: return "setAutoMsg";
    case MK1_SELECTOR_SET_LEDS: return "setLEDs";
    case MK1_SELECTOR_DISPLAY_COMMAND: return "displayCommand";
    case MK1_SELECTOR_GET_HARDWARE_BUFFER_SIZE: return "getHardwareBufferSize";
    case MK1_SELECTOR_SET_HARDWARE_BUFFER_SIZE: return "setHardwareBufferSize";
    case MK1_SELECTOR_SET_THRESHOLDS: return "setThresholds";
    case MK1_SELECTOR_MIDI_WRITE_FAKE: return "MIDIWriteFake";
    case MK1_SELECTOR_DISPLAY_COMMAND_LONG: return "displayCommandLong";
    case MK1_SELECTOR_GET_DEVICE_SPEC: return "getDeviceSpec";
    case MK1_SELECTOR_READ_USER_DATA: return "readUserData";
    case MK1_SELECTOR_WRITE_USER_DATA: return "writeUserData";
    case MK1_SELECTOR_DIGITAL_INPUT_ARM: return "digitalInputArm";
    default: return "unknown";
    }
}

static const char *mk1_async_selector_name(uint32_t selector)
{
    switch (selector) {
    case MK1_ASYNC_SELECTOR_MIDI_READ: return "MIDIRead";
    case MK1_ASYNC_SELECTOR_ANALOG_INPUT_READ: return "analogInputRead";
    case MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ: return "digitalInputRead";
    case MK1_ASYNC_SELECTOR_ERP_INPUT_READ: return "ERPInputRead";
    case MK1_ASYNC_SELECTOR_SERIAL_DATA_READ: return "serialDataRead";
    case MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ: return "sampleBufferRead";
    case MK1_ASYNC_SELECTOR_MIDI_READ_TIMESTAMPED: return "MIDIReadTimestamped";
    case MK1_ASYNC_SELECTOR_ANALOG_INPUT_READ_TIMESTAMPED: return "analogInputReadTimestamped";
    case MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ_TIMESTAMPED: return "digitalInputReadTimestamped";
    case MK1_ASYNC_SELECTOR_ERP_INPUT_READ_TIMESTAMPED: return "ERPInputReadTimestamped";
    case MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ_TIMESTAMPED: return "sampleBufferReadTimestamped";
    case MK1_ASYNC_SELECTOR_ENCODER_INPUT_READ: return "encoderInputRead";
    default: return "unknown";
    }
}

static mk1_async_registration_t *mk1_async_registration_for_selector(uint32_t selector, bool *timestamped)
{
    if (timestamped) *timestamped = false;

    switch (selector) {
    case MK1_ASYNC_SELECTOR_MIDI_READ:
        return &g_mk1_shim_state.midi_read;
    case MK1_ASYNC_SELECTOR_ANALOG_INPUT_READ:
        return &g_mk1_shim_state.analog_read;
    case MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ:
        return &g_mk1_shim_state.digital_read;
    case MK1_ASYNC_SELECTOR_ERP_INPUT_READ:
        return &g_mk1_shim_state.erp_read;
    case MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ:
        return &g_mk1_shim_state.sample_buffer_read;
    case MK1_ASYNC_SELECTOR_ENCODER_INPUT_READ:
        return &g_mk1_shim_state.encoder_read;
    case MK1_ASYNC_SELECTOR_MIDI_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &g_mk1_shim_state.midi_read;
    case MK1_ASYNC_SELECTOR_ANALOG_INPUT_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &g_mk1_shim_state.analog_read;
    case MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &g_mk1_shim_state.digital_read;
    case MK1_ASYNC_SELECTOR_ERP_INPUT_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &g_mk1_shim_state.erp_read;
    case MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ_TIMESTAMPED:
        if (timestamped) *timestamped = true;
        return &g_mk1_shim_state.sample_buffer_read;
    default:
        return NULL;
    }
}

static bool is_fake_iterator(io_object_t object)
{
    return object == MK1_FAKE_ITERATOR;
}

static bool is_fake_service(io_object_t object)
{
    return object == MK1_FAKE_SERVICE;
}

static bool is_fake_connect(io_object_t object)
{
    return object == MK1_FAKE_CONNECT;
}

static bool matching_targets_mk1_driver(CFDictionaryRef matching)
{
    char buf[1024] = {0};
    CFStringRef desc = NULL;

    if (!matching) return false;
    desc = CFCopyDescription(matching);
    if (!desc) return false;
    CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(desc);

    return strstr(buf, "NIUSBMaschineControllerDriver") != NULL ||
           strstr(buf, "NIUSBUserClient") != NULL;
}

static bool mk1_shim_ensure_device_ready(void)
{
    bool ok = false;

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    if (g_mk1_shim_state.device_ready && g_mk1_shim_state.device) {
        pthread_mutex_unlock(&g_mk1_shim_state.lock);
        return true;
    }
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    mk1_device_t *device = mk1_device_open();
    if (!device) {
        SHIM_LOG("failed to open MK1 USB device");
        return false;
    }

    if (!mk1_device_init_hardware(device)) {
        SHIM_LOG("failed to run minimal MK1 hardware init");
        mk1_device_close(device);
        return false;
    }

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    if (!g_mk1_shim_state.device_ready) {
        g_mk1_shim_state.device = device;
        g_mk1_shim_state.device_ready = true;
        if (!mk1_device_get_serial(device, g_mk1_shim_state.serial, sizeof(g_mk1_shim_state.serial))) {
            g_mk1_shim_state.serial[0] = '\0';
        }
        SHIM_LOG("MK1 USB device ready for fake user client");
        device = NULL;
        ok = true;
    } else {
        ok = true;
    }
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    if (device) {
        mk1_device_close(device);
    }
    return ok;
}

static size_t mk1_shim_remap_led_payload(const void *input_payload,
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

    for (size_t logical_index = 0; logical_index < sizeof(hardware_index_by_logical); logical_index++) {
        size_t hardware_index = hardware_index_by_logical[logical_index];
        if (logical_index >= copy_len || hardware_index >= copy_len) {
            continue;
        }
        output_payload[hardware_index] = ((const uint8_t *)input_payload)[logical_index];
    }

    return copy_len;
}

static void mk1_shim_fill_device_info(void *buffer, size_t len)
{
    uint8_t *bytes = (uint8_t *)buffer;
    const char *serial = g_mk1_shim_state.serial[0] ? g_mk1_shim_state.serial : "SN-unknown";
    const char *product = "Maschine Controller";
    const char *vendor = "Native Instruments";
    uint16_t vendor_id = MK1_VENDOR_ID;
    uint16_t product_id = MK1_PRODUCT_ID;

    memset(bytes, 0, len);
    memcpy(bytes + 0x00, &vendor_id, sizeof(vendor_id));
    memcpy(bytes + 0x02, &product_id, sizeof(product_id));
    strncpy((char *)(bytes + 0x04), serial, 0x20);
    strncpy((char *)(bytes + 0x25), product, 0x20);
    strncpy((char *)(bytes + 0x46), vendor, 0x20);
}

static kern_return_t mk1_shim_handle_get_device_info(void *outputStruct, size_t *outputStructCnt)
{
    if (!outputStruct || !outputStructCnt || *outputStructCnt < MK1_DEVICE_INFO_SIZE) {
        return kIOReturnBadArgument;
    }
    mk1_shim_fill_device_info(outputStruct, MK1_DEVICE_INFO_SIZE);
    *outputStructCnt = MK1_DEVICE_INFO_SIZE;
    SHIM_LOG("selector 3 getDeviceInfo -> %zu bytes", *outputStructCnt);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_send_ep1_command(uint8_t command,
                                               const void *payload,
                                               size_t payload_len,
                                               const char *label)
{
    uint8_t packet[512];

    if (!mk1_shim_ensure_device_ready()) {
        return kIOReturnNotOpen;
    }
    if (payload_len > sizeof(packet) - 1) {
        return kIOReturnBadArgument;
    }

    packet[0] = command;
    if (payload_len > 0 && payload) {
        memcpy(packet + 1, payload, payload_len);
    }

    if (!mk1_device_write_endpoint(g_mk1_shim_state.device, 0x01, packet, payload_len + 1)) {
        return kIOReturnIOError;
    }

    SHIM_LOG("%s -> EP1 cmd=0x%02x payload=%zu", label, command, payload_len);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_write_io(const uint64_t *inputScalars,
                                              uint32_t inputScalarCnt,
                                              const void *inputStruct,
                                              size_t inputStructCnt)
{
    size_t payload_len = inputStructCnt;

    if ((!inputStruct || inputStructCnt == 0) && inputScalars && inputScalarCnt > 0) {
        payload_len = (size_t)inputScalars[0];
    }
    if (!inputStruct || payload_len == 0 || payload_len > inputStructCnt) {
        return kIOReturnBadArgument;
    }

    return mk1_shim_send_ep1_command(0x05, inputStruct, payload_len, "selector 2 writeIO");
}

static kern_return_t mk1_shim_handle_set_auto_msg(const uint64_t *inputScalars, uint32_t inputScalarCnt)
{
    uint8_t packet[4];

    if (!mk1_shim_ensure_device_ready()) {
        return kIOReturnNotOpen;
    }
    if (!inputScalars || inputScalarCnt < 3) {
        return kIOReturnBadArgument;
    }

    packet[0] = 0x0b;
    packet[1] = (uint8_t)inputScalars[0];
    packet[2] = (uint8_t)inputScalars[1];
    packet[3] = (uint8_t)inputScalars[2];

    if (!mk1_device_write_endpoint(g_mk1_shim_state.device, 0x01, packet, sizeof(packet))) {
        return kIOReturnIOError;
    }

    SHIM_LOG("selector 5 setAutoMsg [%u, %u, %u]",
             packet[1], packet[2], packet[3]);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_set_thresholds(const uint64_t *inputScalars,
                                                    uint32_t inputScalarCnt,
                                                    const void *inputStruct,
                                                    size_t inputStructCnt)
{
    size_t payload_len = inputStructCnt;

    if ((!inputStruct || inputStructCnt == 0) && inputScalars && inputScalarCnt > 0) {
        payload_len = (size_t)inputScalars[0];
    }
    if (!inputStruct || payload_len == 0 || payload_len > inputStructCnt) {
        return kIOReturnBadArgument;
    }

    (void)inputScalarCnt;
    return mk1_shim_send_ep1_command(0x0f, inputStruct, payload_len, "selector 10 setThresholds");
}

static kern_return_t mk1_shim_handle_set_leds(const uint64_t *inputScalars,
                                              uint32_t inputScalarCnt,
                                              const void *inputStruct,
                                              size_t inputStructCnt)
{
    uint8_t packet[64];
    uint8_t remapped_payload[32];
    size_t payload_len = inputStructCnt;

    if (!mk1_shim_ensure_device_ready()) {
        return kIOReturnNotOpen;
    }
    if ((!inputStruct || inputStructCnt == 0) && (!inputScalars || inputScalarCnt == 0 || inputScalars[0] == 0)) {
        return kIOReturnBadArgument;
    }
    if (payload_len == 0 && inputScalars && inputScalarCnt > 0) {
        payload_len = (size_t)inputScalars[0];
    }
    if (!inputStruct || payload_len == 0 || payload_len > sizeof(packet) - 1) {
        return kIOReturnBadArgument;
    }
    payload_len = mk1_shim_remap_led_payload(inputStruct,
                                             payload_len,
                                             remapped_payload,
                                             sizeof(remapped_payload));

    packet[0] = 0x0c;
    memcpy(packet + 1, remapped_payload, payload_len);
    if (!mk1_device_write_endpoint(g_mk1_shim_state.device, 0x01, packet, payload_len + 1)) {
        return kIOReturnIOError;
    }

    SHIM_LOG("selector 6 setLEDs (%zu bytes)", payload_len);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_display_command(const uint64_t *inputScalars,
                                                     uint32_t inputScalarCnt,
                                                     const void *inputStruct,
                                                     size_t inputStructCnt)
{
    uint8_t display_index = 0;
    size_t payload_len = 0;

    if (!mk1_shim_ensure_device_ready()) {
        return kIOReturnNotOpen;
    }
    if (!inputScalars || inputScalarCnt < 2 || !inputStruct) {
        return kIOReturnBadArgument;
    }

    display_index = (uint8_t)inputScalars[0];
    payload_len = (size_t)inputScalars[1];
    if (payload_len > inputStructCnt) {
        return kIOReturnBadArgument;
    }

    if (!mk1_set_display(g_mk1_shim_state.device, display_index, inputStruct, payload_len)) {
        return kIOReturnIOError;
    }

    SHIM_LOG("selector 7 displayCommand display=%u len=%zu", display_index, payload_len);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_display_command_long(const uint64_t *inputScalars,
                                                          uint32_t inputScalarCnt,
                                                          const void *inputStruct,
                                                          size_t inputStructCnt)
{
    uint8_t display_index = 0;
    size_t remaining = 0;
    size_t offset = 0;

    if (!mk1_shim_ensure_device_ready()) {
        return kIOReturnNotOpen;
    }
    if (!inputScalars || inputScalarCnt < 3 || !inputStruct) {
        return kIOReturnBadArgument;
    }

    display_index = (uint8_t)inputScalars[0];
    remaining = (size_t)inputScalars[2];
    if (remaining > inputStructCnt || remaining > 0x10000) {
        return kIOReturnBadArgument;
    }

    while (remaining > 0) {
        size_t chunk_len = remaining > 0x1fc ? 0x1fc : remaining;
        uint8_t chunk_index = offset == 0 ? display_index : (uint8_t)(display_index | 1u);

        if (!mk1_set_display(g_mk1_shim_state.device,
                             chunk_index,
                             (const uint8_t *)inputStruct + offset,
                             chunk_len)) {
            return kIOReturnIOError;
        }

        remaining -= chunk_len;
        offset += chunk_len;
    }

    SHIM_LOG("selector 17 displayCommandLong display=%u total=%zu", display_index, offset);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_get_device_spec(void *outputStruct, size_t *outputStructCnt)
{
    uint8_t *bytes = (uint8_t *)outputStruct;
    uint16_t vendor_id = MK1_VENDOR_ID;
    uint16_t product_id = MK1_PRODUCT_ID;

    if (!outputStruct || !outputStructCnt || *outputStructCnt < 0x0e) {
        return kIOReturnBadArgument;
    }

    memset(bytes, 0, 0x0e);
    memcpy(bytes + 0x00, &vendor_id, sizeof(vendor_id));
    memcpy(bytes + 0x02, &product_id, sizeof(product_id));
    bytes[0x04] = 0x01;
    bytes[0x05] = 0x00;
    bytes[0x0b] = 0x01;
    *outputStructCnt = 0x0e;

    SHIM_LOG("selector 18 getDeviceSpec -> %zu bytes", *outputStructCnt);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_read_user_data(const uint64_t *inputScalars,
                                                    uint32_t inputScalarCnt,
                                                    void *outputStruct,
                                                    size_t *outputStructCnt)
{
    uint8_t selector_value = 0;

    if (!outputStruct || !outputStructCnt || *outputStructCnt < MK1_USER_DATA_SIZE) {
        return kIOReturnBadArgument;
    }
    if (!inputScalars || inputScalarCnt < 1) {
        return kIOReturnBadArgument;
    }

    selector_value = (uint8_t)inputScalars[0];

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    if (!g_mk1_shim_state.user_data_valid) {
        memset(g_mk1_shim_state.user_data, 0, sizeof(g_mk1_shim_state.user_data));
        g_mk1_shim_state.user_data[0] = selector_value;
        g_mk1_shim_state.user_data_valid = true;
    } else {
        g_mk1_shim_state.user_data[0] = selector_value;
    }
    memcpy(outputStruct, g_mk1_shim_state.user_data, sizeof(g_mk1_shim_state.user_data));
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    *outputStructCnt = MK1_USER_DATA_SIZE;
    SHIM_LOG("selector 19 readUserData id=0x%02x -> %zu bytes", selector_value, *outputStructCnt);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_write_user_data(const void *inputStruct,
                                                     size_t inputStructCnt)
{
    if (!inputStruct || inputStructCnt != MK1_USER_DATA_SIZE) {
        return kIOReturnBadArgument;
    }

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    memcpy(g_mk1_shim_state.user_data, inputStruct, MK1_USER_DATA_SIZE);
    g_mk1_shim_state.user_data_valid = true;
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    return mk1_shim_send_ep1_command(0x13, inputStruct, MK1_USER_DATA_SIZE, "selector 20 writeUserData");
}

static kern_return_t mk1_shim_handle_hardware_buffer_selector(uint32_t selector)
{
    SHIM_LOG("selector %u %s -> success", selector, mk1_selector_name(selector));
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_passthrough_success(uint32_t selector)
{
    SHIM_LOG("selector %u %s -> stub success", selector, mk1_selector_name(selector));
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_handle_async_selector(uint32_t selector,
                                                    mach_port_t wakePort,
                                                    uint64_t *reference,
                                                    uint32_t referenceCnt,
                                                    const uint64_t *inputScalars,
                                                    uint32_t inputScalarCnt,
                                                    const void *inputStruct,
                                                    size_t inputStructCnt)
{
    bool timestamped = false;
    mk1_async_registration_t *registration = NULL;

    if (!mk1_shim_ensure_device_ready()) {
        return kIOReturnNotOpen;
    }

    if (selector == MK1_ASYNC_SELECTOR_SERIAL_DATA_READ) {
        SHIM_LOG("async selector 4 serialDataRead -> success");
        return KERN_SUCCESS;
    }

    registration = mk1_async_registration_for_selector(selector, &timestamped);
    if (!registration) {
        SHIM_LOG("async selector %u %s -> unsupported", selector, mk1_async_selector_name(selector));
        return kIOReturnUnsupported;
    }

    if (registration->registered) {
        SHIM_LOG("async selector %u %s -> already registered", selector, mk1_async_selector_name(selector));
        return kIOReturnExclusiveAccess;
    }

    if (selector == MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ ||
        selector == MK1_ASYNC_SELECTOR_DIGITAL_INPUT_READ_TIMESTAMPED) {
        if (!inputScalars || inputScalarCnt < 2 || inputScalars[0] == 0 || inputScalars[1] == 0) {
            return kIOReturnBadArgument;
        }
    } else if (selector == MK1_ASYNC_SELECTOR_MIDI_READ ||
               selector == MK1_ASYNC_SELECTOR_MIDI_READ_TIMESTAMPED) {
        if (!inputScalars || inputScalarCnt < 2 || inputScalars[0] == 0 || inputScalars[1] == 0) {
            return kIOReturnBadArgument;
        }
    } else if (selector == MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ ||
               selector == MK1_ASYNC_SELECTOR_SAMPLE_BUFFER_READ_TIMESTAMPED) {
        // Decompile shows this only stores async reference and succeeds if device exists.
    } else {
        if (!inputScalars || inputScalarCnt < 2 || inputScalars[0] == 0 || inputScalars[1] == 0) {
            return kIOReturnBadArgument;
        }
    }

    registration->registered = true;
    registration->timestamped = timestamped;
    registration->wake_port = wakePort;
    registration->reference_count = referenceCnt;
    registration->input_scalar_count = inputScalarCnt;
    registration->input_struct_size = inputStructCnt;

    SHIM_LOG("async selector %u %s registered timestamped=%s wakePort=%u refCnt=%u scalars=%u struct=%zu firstRef=%llu",
             selector,
             mk1_async_selector_name(selector),
             timestamped ? "true" : "false",
             wakePort,
             referenceCnt,
             inputScalarCnt,
             inputStructCnt,
             (reference && referenceCnt > 0) ? (unsigned long long)reference[0] : 0ull);
    return KERN_SUCCESS;
}

static kern_return_t mk1_shim_ensure_client_memory(uint32_t memoryType,
                                                   mach_vm_address_t *address,
                                                   mach_vm_size_t *size)
{
    kern_return_t kr = KERN_SUCCESS;

    if (memoryType > 1 || !address || !size) {
        return kIOReturnBadArgument;
    }

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    if (g_mk1_shim_state.client_memory[memoryType] == 0) {
        mach_vm_address_t allocated = 0;
        kr = mach_vm_allocate(mach_task_self(), &allocated, MK1_CLIENT_MEMORY_SIZE, VM_FLAGS_ANYWHERE);
        if (kr == KERN_SUCCESS) {
            memset((void *)(uintptr_t)allocated, 0, MK1_CLIENT_MEMORY_SIZE);
            g_mk1_shim_state.client_memory[memoryType] = allocated;
            g_mk1_shim_state.client_memory_size[memoryType] = MK1_CLIENT_MEMORY_SIZE;
            SHIM_LOG("allocated fake client memory type=%u addr=0x%llx size=%llu",
                     memoryType,
                     (unsigned long long)allocated,
                     (unsigned long long)MK1_CLIENT_MEMORY_SIZE);
        }
    }
    if (kr == KERN_SUCCESS) {
        *address = g_mk1_shim_state.client_memory[memoryType];
        *size = g_mk1_shim_state.client_memory_size[memoryType];
    }
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    return kr == KERN_SUCCESS ? KERN_SUCCESS : kIOReturnNoMemory;
}

// Returns 1 if the string contains anything NI or USB related
static int is_interesting(const char *s)
{
    if (!s) return 0;
    // Always log these
    if (strstr(s, "17cc"))   return 1;  // NI vendor ID
    if (strstr(s, "17CC"))   return 1;
    if (strstr(s, "0808"))   return 1;  // MK1 product ID
    if (strstr(s, "Maschine")) return 1;
    if (strstr(s, "maschine")) return 1;
    if (strstr(s, "Native"))  return 1;
    if (strstr(s, "NIUSB"))   return 1;
    if (strstr(s, "NIHardware")) return 1;
    if (strstr(s, "USB"))     return 1;
    if (strstr(s, "HID"))     return 1;
    if (strstr(s, "IOUSBDevice")) return 1;
    if (strstr(s, "IOUSBInterface")) return 1;
    if (strstr(s, "IOHIDDevice")) return 1;
    if (strstr(s, "idVendor")) return 1;
    if (strstr(s, "idProduct")) return 1;
    // Ignore disk/filesystem noise
    if (strstr(s, "BSD Name")) return 0;
    if (strstr(s, "disk"))    return 0;
    // Log everything else that isn't disk noise
    return 1;
}

// ---------------------------------------------------------------------------
// IOServiceMatching
// ---------------------------------------------------------------------------

CFMutableDictionaryRef mk1_shim_IOServiceMatching(const char *name)
{
    if (name) {
        // Always log IOServiceMatching — it shows what class NIHA searches for
        SHIM_LOG("IOServiceMatching(\"%s\")", name);
    }
    CFMutableDictionaryRef (*orig)(const char *) =
        dlsym(RTLD_NEXT, "IOServiceMatching");
    return orig(name);
}

// ---------------------------------------------------------------------------
// IOServiceGetMatchingServices
// ---------------------------------------------------------------------------

kern_return_t mk1_shim_IOServiceGetMatchingServices(mach_port_t master,
                                                     CFDictionaryRef matching,
                                                     io_iterator_t *iter)
{
    kern_return_t (*orig)(mach_port_t, CFDictionaryRef, io_iterator_t *) =
        dlsym(RTLD_NEXT, "IOServiceGetMatchingServices");

    if (iter && matching_targets_mk1_driver(matching)) {
        pthread_mutex_lock(&g_mk1_shim_state.lock);
        g_mk1_shim_state.iterator_live = true;
        g_mk1_shim_state.iterator_yielded_service = false;
        g_mk1_shim_state.service_live = true;
        *iter = MK1_FAKE_ITERATOR;
        pthread_mutex_unlock(&g_mk1_shim_state.lock);
        SHIM_LOG("IOServiceGetMatchingServices: returning fake MK1 driver iterator");
        return KERN_SUCCESS;
    }

    kern_return_t ret = orig(master, matching, iter);

    if (matching) {
        CFStringRef desc = CFCopyDescription(matching);
        char buf[1024] = {0};
        CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(desc);

        if (is_interesting(buf)) {
            SHIM_LOG("IOServiceGetMatchingServices: %s", buf);
            SHIM_LOG("  → result=%d iterator=%u", ret, iter ? *iter : 0);
        }
    }
    return ret;
}

io_object_t mk1_shim_IOIteratorNext(io_iterator_t iterator)
{
    io_object_t (*orig)(io_iterator_t) = dlsym(RTLD_NEXT, "IOIteratorNext");

    if (!is_fake_iterator(iterator)) {
        return orig(iterator);
    }

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    if (!g_mk1_shim_state.iterator_live || g_mk1_shim_state.iterator_yielded_service) {
        pthread_mutex_unlock(&g_mk1_shim_state.lock);
        return IO_OBJECT_NULL;
    }
    g_mk1_shim_state.iterator_yielded_service = true;
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    SHIM_LOG("IOIteratorNext(fake) -> fake MK1 service");
    return MK1_FAKE_SERVICE;
}

// ---------------------------------------------------------------------------
// IOServiceOpen
// ---------------------------------------------------------------------------

kern_return_t mk1_shim_IOServiceOpen(io_service_t service,
                                      task_port_t owning_task,
                                      uint32_t type,
                                      io_connect_t *connect)
{
    // Always log this — it's only called when something is actually found
    SHIM_LOG("*** IOServiceOpen(service=%u, type=%u) ***", service, type);

    if (is_fake_service(service) && connect) {
        if (!mk1_shim_ensure_device_ready()) {
            return kIOReturnNotOpen;
        }
        pthread_mutex_lock(&g_mk1_shim_state.lock);
        g_mk1_shim_state.connect_live = true;
        *connect = MK1_FAKE_CONNECT;
        pthread_mutex_unlock(&g_mk1_shim_state.lock);
        SHIM_LOG("*** IOServiceOpen(fake service, type=%u) -> fake connect ***", type);
        return KERN_SUCCESS;
    }

    kern_return_t (*orig)(io_service_t, task_port_t, uint32_t, io_connect_t *) =
        dlsym(RTLD_NEXT, "IOServiceOpen");
    kern_return_t ret = orig(service, owning_task, type, connect);
    SHIM_LOG("  → result=%d connect=%u", ret, connect ? *connect : 0);
    return ret;
}

kern_return_t mk1_shim_IOObjectRelease(io_object_t object)
{
    kern_return_t (*orig)(io_object_t) = dlsym(RTLD_NEXT, "IOObjectRelease");

    if (!is_fake_iterator(object) && !is_fake_service(object) && !is_fake_connect(object)) {
        return orig(object);
    }

    pthread_mutex_lock(&g_mk1_shim_state.lock);
    if (is_fake_iterator(object)) {
        g_mk1_shim_state.iterator_live = false;
    } else if (is_fake_service(object)) {
        g_mk1_shim_state.service_live = false;
    } else if (is_fake_connect(object)) {
        g_mk1_shim_state.connect_live = false;
        for (uint32_t i = 0; i < 2; i++) {
            if (g_mk1_shim_state.client_memory[i] != 0) {
                mach_vm_deallocate(mach_task_self(),
                                   g_mk1_shim_state.client_memory[i],
                                   g_mk1_shim_state.client_memory_size[i]);
                g_mk1_shim_state.client_memory[i] = 0;
                g_mk1_shim_state.client_memory_size[i] = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_mk1_shim_state.lock);

    SHIM_LOG("IOObjectRelease(fake=%u)", object);
    return KERN_SUCCESS;
}

// ---------------------------------------------------------------------------
// IORegistryEntryGetName — tells us the name of services NIHA finds
// ---------------------------------------------------------------------------

kern_return_t mk1_shim_IORegistryEntryGetName(io_registry_entry_t entry,
                                               io_name_t name)
{
    if (is_fake_service(entry) && name) {
        strlcpy(name, "NIUSBMaschineControllerDriver", sizeof(io_name_t));
        SHIM_LOG("IORegistryEntryGetName(fake) -> \"%s\"", name);
        return KERN_SUCCESS;
    }

    kern_return_t (*orig)(io_registry_entry_t, io_name_t) =
        dlsym(RTLD_NEXT, "IORegistryEntryGetName");
    kern_return_t ret = orig(entry, name);
    if (ret == 0 && name && is_interesting(name)) {
        SHIM_LOG("IORegistryEntryGetName → \"%s\"", name);
    }
    return ret;
}

kern_return_t mk1_shim_IOObjectGetClass(io_object_t object, io_name_t className)
{
    kern_return_t (*orig)(io_object_t, io_name_t) = dlsym(RTLD_NEXT, "IOObjectGetClass");

    if (!is_fake_service(object) || !className) {
        return orig(object, className);
    }

    strlcpy(className, "com_caiaq_driver_MaschineController_NIUSBUserClient", sizeof(io_name_t));
    SHIM_LOG("IOObjectGetClass(fake) -> \"%s\"", className);
    return KERN_SUCCESS;
}

kern_return_t mk1_shim_IOConnectCallMethod(io_connect_t connection,
                                           uint32_t selector,
                                           const uint64_t *inputScalars,
                                           uint32_t inputScalarCnt,
                                           const void *inputStruct,
                                           size_t inputStructCnt,
                                           uint64_t *outputScalars,
                                           uint32_t *outputScalarCnt,
                                           void *outputStruct,
                                           size_t *outputStructCnt)
{
    kern_return_t (*orig)(io_connect_t, uint32_t, const uint64_t *, uint32_t,
                          const void *, size_t, uint64_t *, uint32_t *,
                          void *, size_t *) = dlsym(RTLD_NEXT, "IOConnectCallMethod");

    if (!is_fake_connect(connection)) {
        return orig(connection, selector, inputScalars, inputScalarCnt,
                    inputStruct, inputStructCnt, outputScalars, outputScalarCnt,
                    outputStruct, outputStructCnt);
    }

    (void)outputScalars;
    (void)outputScalarCnt;

    SHIM_LOG("IOConnectCallMethod(fake) selector=%u %s scalars=%u struct=%zu",
             selector,
             mk1_selector_name(selector),
             inputScalarCnt,
             inputStructCnt);

    switch (selector) {
    case MK1_SELECTOR_WRITE_IO:
        return mk1_shim_handle_write_io(inputScalars, inputScalarCnt, inputStruct, inputStructCnt);
    case MK1_SELECTOR_GET_DEVICE_INFO:
        return mk1_shim_handle_get_device_info(outputStruct, outputStructCnt);
    case MK1_SELECTOR_MIDI_WRITE:
    case MK1_SELECTOR_MIDI_WRITE_FAKE:
        return mk1_shim_handle_passthrough_success(selector);
    case MK1_SELECTOR_SET_AUTO_MSG:
        return mk1_shim_handle_set_auto_msg(inputScalars, inputScalarCnt);
    case MK1_SELECTOR_SET_LEDS:
        return mk1_shim_handle_set_leds(inputScalars, inputScalarCnt, inputStruct, inputStructCnt);
    case MK1_SELECTOR_DISPLAY_COMMAND:
        return mk1_shim_handle_display_command(inputScalars, inputScalarCnt, inputStruct, inputStructCnt);
    case MK1_SELECTOR_GET_HARDWARE_BUFFER_SIZE:
    case MK1_SELECTOR_SET_HARDWARE_BUFFER_SIZE:
        return mk1_shim_handle_hardware_buffer_selector(selector);
    case MK1_SELECTOR_SET_THRESHOLDS:
        return mk1_shim_handle_set_thresholds(inputScalars, inputScalarCnt, inputStruct, inputStructCnt);
    case MK1_SELECTOR_DISPLAY_COMMAND_LONG:
        return mk1_shim_handle_display_command_long(inputScalars, inputScalarCnt, inputStruct, inputStructCnt);
    case MK1_SELECTOR_GET_DEVICE_SPEC:
        return mk1_shim_handle_get_device_spec(outputStruct, outputStructCnt);
    case MK1_SELECTOR_READ_USER_DATA:
        return mk1_shim_handle_read_user_data(inputScalars, inputScalarCnt, outputStruct, outputStructCnt);
    case MK1_SELECTOR_WRITE_USER_DATA:
        return mk1_shim_handle_write_user_data(inputStruct, inputStructCnt);
    case MK1_SELECTOR_DIGITAL_INPUT_ARM:
        return mk1_shim_handle_passthrough_success(selector);
    default:
        SHIM_LOG("unknown fake selector %u %s (scalars=%u struct=%zu)",
                 selector, mk1_selector_name(selector), inputScalarCnt, inputStructCnt);
        return kIOReturnUnsupported;
    }
}

kern_return_t mk1_shim_IOConnectCallAsyncMethod(io_connect_t connection,
                                                uint32_t selector,
                                                mach_port_t wakePort,
                                                uint64_t *reference,
                                                uint32_t referenceCnt,
                                                const uint64_t *inputScalars,
                                                uint32_t inputScalarCnt,
                                                const void *inputStruct,
                                                size_t inputStructCnt,
                                                uint64_t *outputScalars,
                                                uint32_t *outputScalarCnt,
                                                void *outputStruct,
                                                size_t *outputStructCnt)
{
    kern_return_t (*orig)(io_connect_t, uint32_t, mach_port_t, uint64_t *, uint32_t,
                          const uint64_t *, uint32_t, const void *, size_t,
                          uint64_t *, uint32_t *, void *, size_t *) =
        dlsym(RTLD_NEXT, "IOConnectCallAsyncMethod");

    if (!is_fake_connect(connection)) {
        return orig(connection, selector, wakePort, reference, referenceCnt,
                    inputScalars, inputScalarCnt, inputStruct, inputStructCnt,
                    outputScalars, outputScalarCnt, outputStruct, outputStructCnt);
    }

    (void)wakePort;
    (void)reference;
    (void)referenceCnt;
    (void)outputScalars;
    (void)outputScalarCnt;
    (void)outputStruct;
    (void)outputStructCnt;

    SHIM_LOG("IOConnectCallAsyncMethod(fake) selector=%u %s scalars=%u struct=%zu",
             selector,
             mk1_async_selector_name(selector),
             inputScalarCnt,
             inputStructCnt);

    return mk1_shim_handle_async_selector(selector, wakePort, reference, referenceCnt,
                                          inputScalars, inputScalarCnt, inputStruct, inputStructCnt);
}

kern_return_t mk1_shim_IOConnectMapMemory64(io_connect_t connection,
                                            uint32_t memoryType,
                                            task_port_t intoTask,
                                            mach_vm_address_t *atAddress,
                                            mach_vm_size_t *ofSize,
                                            IOOptionBits options)
{
    kern_return_t (*orig)(io_connect_t, uint32_t, task_port_t, mach_vm_address_t *,
                          mach_vm_size_t *, IOOptionBits) =
        dlsym(RTLD_NEXT, "IOConnectMapMemory64");

    if (!is_fake_connect(connection)) {
        return orig(connection, memoryType, intoTask, atAddress, ofSize, options);
    }

    (void)intoTask;
    (void)options;

    if (memoryType > 1 || !atAddress || !ofSize) {
        return kIOReturnBadArgument;
    }

    return mk1_shim_ensure_client_memory(memoryType, atAddress, ofSize);
}

kern_return_t mk1_shim_IOConnectUnmapMemory64(io_connect_t connection,
                                              uint32_t memoryType,
                                              task_port_t fromTask,
                                              mach_vm_address_t atAddress)
{
    kern_return_t (*orig)(io_connect_t, uint32_t, task_port_t, mach_vm_address_t) =
        dlsym(RTLD_NEXT, "IOConnectUnmapMemory64");

    if (!is_fake_connect(connection)) {
        return orig(connection, memoryType, fromTask, atAddress);
    }

    (void)fromTask;
    (void)atAddress;

    if (memoryType > 1) {
        return kIOReturnBadArgument;
    }

    SHIM_LOG("IOConnectUnmapMemory64(fake) type=%u", memoryType);
    return KERN_SUCCESS;
}

// ---------------------------------------------------------------------------
// CFMessagePort — confirm port names (these are always interesting)
// ---------------------------------------------------------------------------

CFMessagePortRef mk1_shim_CFMessagePortCreateRemote(CFAllocatorRef alloc,
                                                      CFStringRef name)
{
    char buf[256] = {0};
    if (name) CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    SHIM_LOG("*** CFMessagePortCreateRemote(\"%s\") ***", buf);

    CFMessagePortRef (*orig)(CFAllocatorRef, CFStringRef) =
        dlsym(RTLD_NEXT, "CFMessagePortCreateRemote");
    CFMessagePortRef ret = orig(alloc, name);
    SHIM_LOG("  → %s", ret ? "SUCCESS" : "FAILED (port not found)");
    return ret;
}

CFMessagePortRef mk1_shim_CFMessagePortCreateLocal(CFAllocatorRef alloc,
                                                     CFStringRef name,
                                                     CFMessagePortCallBack cb,
                                                     CFMessagePortContext *ctx,
                                                     Boolean *shouldFreeInfo)
{
    char buf[256] = {0};
    if (name) CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    SHIM_LOG("*** CFMessagePortCreateLocal(\"%s\") ***", buf);

    CFMessagePortRef (*orig)(CFAllocatorRef, CFStringRef,
                              CFMessagePortCallBack,
                              CFMessagePortContext *, Boolean *) =
        dlsym(RTLD_NEXT, "CFMessagePortCreateLocal");
    return orig(alloc, name, cb, ctx, shouldFreeInfo);
}

// ---------------------------------------------------------------------------
// Interpose table
// ---------------------------------------------------------------------------

typedef struct {
    const void *replacement;
    const void *replacee;
} interpose_t;

__attribute__((used))
static const interpose_t interposers[]
    __attribute__((section("__DATA,__interpose"))) =
{
    { (void *)mk1_shim_IOServiceMatching,
      (void *)IOServiceMatching },

    { (void *)mk1_shim_IOServiceGetMatchingServices,
      (void *)IOServiceGetMatchingServices },

    { (void *)mk1_shim_IOServiceOpen,
      (void *)IOServiceOpen },

    { (void *)mk1_shim_IOIteratorNext,
      (void *)IOIteratorNext },

    { (void *)mk1_shim_IOObjectRelease,
      (void *)IOObjectRelease },

    { (void *)mk1_shim_IORegistryEntryGetName,
      (void *)IORegistryEntryGetName },

    { (void *)mk1_shim_IOObjectGetClass,
      (void *)IOObjectGetClass },

    { (void *)mk1_shim_CFMessagePortCreateRemote,
      (void *)CFMessagePortCreateRemote },

    { (void *)mk1_shim_CFMessagePortCreateLocal,
      (void *)CFMessagePortCreateLocal },

    { (void *)mk1_shim_IOConnectCallMethod,
      (void *)IOConnectCallMethod },

    { (void *)mk1_shim_IOConnectCallAsyncMethod,
      (void *)IOConnectCallAsyncMethod },

    { (void *)mk1_shim_IOConnectMapMemory64,
      (void *)IOConnectMapMemory64 },

    { (void *)mk1_shim_IOConnectUnmapMemory64,
      (void *)IOConnectUnmapMemory64 },
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

__attribute__((constructor))
static void shim_init(void)
{
    SHIM_LOG("====================================================");
    SHIM_LOG("mk1-shim loaded");
    SHIM_LOG("Recreating minimal NIUSBUserClient subset for MK1");
    SHIM_LOG("Fake selectors enabled: 3 getDeviceInfo, 5 setAutoMsg, 6 setLEDs, 7 displayCommand, 17 displayCommandLong");
    SHIM_LOG("====================================================");
}
