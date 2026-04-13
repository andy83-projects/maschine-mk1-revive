#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGGER_PATH_ENV "MK1_CALL_LOGGER_PATH"
#define LOGGER_DEFAULT_PATH "/tmp/mk1-call-logger.log"
#define LOGGER_PREVIEW_BYTES 96
#define MK1_SELECTOR_SET_LEDS 6u
#define MK1_LED_LOGICAL_BYTES 32u

typedef kern_return_t (*io_service_open_fn)(io_service_t, task_port_t, uint32_t, io_connect_t *);
typedef kern_return_t (*io_service_close_fn)(io_connect_t);
typedef io_service_t (*io_iterator_next_fn)(io_iterator_t);
typedef kern_return_t (*io_object_release_fn)(io_object_t);
typedef CFMutableDictionaryRef (*io_service_matching_fn)(const char *);
typedef kern_return_t (*io_service_get_matching_services_fn)(mach_port_t, CFDictionaryRef, io_iterator_t *);
typedef kern_return_t (*io_registry_entry_create_cfproperties_fn)(io_registry_entry_t, CFMutableDictionaryRef *, CFAllocatorRef, IOOptionBits);
typedef kern_return_t (*io_connect_call_method_fn)(mach_port_t, uint32_t, const uint64_t *, uint32_t, const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
typedef kern_return_t (*io_connect_call_scalar_method_fn)(mach_port_t, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
typedef kern_return_t (*io_connect_call_struct_method_fn)(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);
typedef kern_return_t (*io_connect_call_async_method_fn)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t, const uint64_t *, uint32_t, const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
typedef kern_return_t (*io_connect_call_async_scalar_method_fn)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t, const uint64_t *, uint32_t, uint64_t *, uint32_t *);
typedef kern_return_t (*io_connect_call_async_struct_method_fn)(mach_port_t, uint32_t, mach_port_t, uint64_t *, uint32_t, const void *, size_t, void *, size_t *);
typedef kern_return_t (*io_connect_map_memory64_fn)(io_connect_t, uint32_t, task_port_t, mach_vm_address_t *, mach_vm_size_t *, IOOptionBits);
typedef kern_return_t (*io_connect_method_scalari_scalaro_fn)(mach_port_t, int, io_scalar_inband_t, mach_msg_type_number_t, io_scalar_inband_t, mach_msg_type_number_t *);
typedef kern_return_t (*io_connect_method_scalari_structureo_fn)(mach_port_t, int, io_scalar_inband_t, mach_msg_type_number_t, io_struct_inband_t, mach_msg_type_number_t *);
typedef kern_return_t (*io_connect_method_scalari_structurei_fn)(mach_port_t, int, io_scalar_inband_t, mach_msg_type_number_t, io_struct_inband_t, mach_msg_type_number_t);
typedef kern_return_t (*io_connect_method_structurei_structureo_fn)(mach_port_t, int, io_struct_inband_t, mach_msg_type_number_t, io_struct_inband_t, mach_msg_type_number_t *);
typedef kern_return_t (*io_async_method_scalari_scalaro_fn)(mach_port_t, mach_port_t, io_async_ref_t, mach_msg_type_number_t, int, io_scalar_inband_t, mach_msg_type_number_t, io_scalar_inband_t, mach_msg_type_number_t *);
typedef kern_return_t (*io_async_method_scalari_structureo_fn)(mach_port_t, mach_port_t, io_async_ref_t, mach_msg_type_number_t, int, io_scalar_inband_t, mach_msg_type_number_t, io_struct_inband_t, mach_msg_type_number_t *);
typedef kern_return_t (*io_async_method_scalari_structurei_fn)(mach_port_t, mach_port_t, io_async_ref_t, mach_msg_type_number_t, int, io_scalar_inband_t, mach_msg_type_number_t, io_struct_inband_t, mach_msg_type_number_t);
typedef kern_return_t (*io_async_method_structurei_structureo_fn)(mach_port_t, mach_port_t, io_async_ref_t, mach_msg_type_number_t, int, io_struct_inband_t, mach_msg_type_number_t, io_struct_inband_t, mach_msg_type_number_t *);
typedef IOHIDManagerRef (*iohid_manager_create_fn)(CFAllocatorRef, IOOptionBits);
typedef IOReturn (*iohid_manager_open_fn)(IOHIDManagerRef, IOOptionBits);
typedef IOReturn (*iohid_manager_close_fn)(IOHIDManagerRef, IOOptionBits);
typedef void (*iohid_manager_set_device_matching_fn)(IOHIDManagerRef, CFDictionaryRef);
typedef void (*iohid_manager_set_device_matching_multiple_fn)(IOHIDManagerRef, CFArrayRef);
typedef CFSetRef (*iohid_manager_copy_devices_fn)(IOHIDManagerRef);
typedef void (*iohid_manager_register_device_matching_callback_fn)(IOHIDManagerRef, IOHIDDeviceCallback, void *);
typedef void (*iohid_manager_register_input_value_callback_fn)(IOHIDManagerRef, IOHIDValueCallback, void *);
typedef void (*iohid_manager_register_input_report_callback_fn)(IOHIDManagerRef, IOHIDReportCallback, void *);
typedef IOHIDDeviceRef (*iohid_device_create_fn)(CFAllocatorRef, io_service_t);
typedef IOReturn (*iohid_device_open_fn)(IOHIDDeviceRef, IOOptionBits);
typedef IOReturn (*iohid_device_close_fn)(IOHIDDeviceRef, IOOptionBits);
typedef CFTypeRef (*iohid_device_get_property_fn)(IOHIDDeviceRef, CFStringRef);
typedef void (*iohid_device_register_input_value_callback_fn)(IOHIDDeviceRef, IOHIDValueCallback, void *);
typedef void (*iohid_device_register_input_report_callback_fn)(IOHIDDeviceRef, uint8_t *, CFIndex, IOHIDReportCallback, void *);
typedef IOReturn (*iohid_device_set_report_fn)(IOHIDDeviceRef, IOHIDReportType, CFIndex, const uint8_t *, CFIndex);
typedef IOReturn (*iohid_device_get_report_fn)(IOHIDDeviceRef, IOHIDReportType, CFIndex, uint8_t *, CFIndex *);

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_fp = NULL;
static uint8_t g_last_led_payload[MK1_LED_LOGICAL_BYTES];
static bool g_last_led_payload_valid = false;

static io_service_open_fn real_IOServiceOpen = NULL;
static io_service_close_fn real_IOServiceClose = NULL;
static io_iterator_next_fn real_IOIteratorNext = NULL;
static io_object_release_fn real_IOObjectRelease = NULL;
static io_service_matching_fn real_IOServiceMatching = NULL;
static io_service_get_matching_services_fn real_IOServiceGetMatchingServices = NULL;
static io_registry_entry_create_cfproperties_fn real_IORegistryEntryCreateCFProperties = NULL;
static io_connect_call_method_fn real_IOConnectCallMethod = NULL;
static io_connect_call_scalar_method_fn real_IOConnectCallScalarMethod = NULL;
static io_connect_call_struct_method_fn real_IOConnectCallStructMethod = NULL;
static io_connect_call_async_method_fn real_IOConnectCallAsyncMethod = NULL;
static io_connect_call_async_scalar_method_fn real_IOConnectCallAsyncScalarMethod = NULL;
static io_connect_call_async_struct_method_fn real_IOConnectCallAsyncStructMethod = NULL;
static io_connect_map_memory64_fn real_IOConnectMapMemory64 = NULL;
static io_connect_method_scalari_scalaro_fn real_io_connect_method_scalarI_scalarO = NULL;
static io_connect_method_scalari_structureo_fn real_io_connect_method_scalarI_structureO = NULL;
static io_connect_method_scalari_structurei_fn real_io_connect_method_scalarI_structureI = NULL;
static io_connect_method_structurei_structureo_fn real_io_connect_method_structureI_structureO = NULL;
static io_async_method_scalari_scalaro_fn real_io_async_method_scalarI_scalarO = NULL;
static io_async_method_scalari_structureo_fn real_io_async_method_scalarI_structureO = NULL;
static io_async_method_scalari_structurei_fn real_io_async_method_scalarI_structureI = NULL;
static io_async_method_structurei_structureo_fn real_io_async_method_structureI_structureO = NULL;
static iohid_manager_create_fn real_IOHIDManagerCreate = NULL;
static iohid_manager_open_fn real_IOHIDManagerOpen = NULL;
static iohid_manager_close_fn real_IOHIDManagerClose = NULL;
static iohid_manager_set_device_matching_fn real_IOHIDManagerSetDeviceMatching = NULL;
static iohid_manager_set_device_matching_multiple_fn real_IOHIDManagerSetDeviceMatchingMultiple = NULL;
static iohid_manager_copy_devices_fn real_IOHIDManagerCopyDevices = NULL;
static iohid_manager_register_device_matching_callback_fn real_IOHIDManagerRegisterDeviceMatchingCallback = NULL;
static iohid_manager_register_input_value_callback_fn real_IOHIDManagerRegisterInputValueCallback = NULL;
static iohid_manager_register_input_report_callback_fn real_IOHIDManagerRegisterInputReportCallback = NULL;
static iohid_device_create_fn real_IOHIDDeviceCreate = NULL;
static iohid_device_open_fn real_IOHIDDeviceOpen = NULL;
static iohid_device_close_fn real_IOHIDDeviceClose = NULL;
static iohid_device_get_property_fn real_IOHIDDeviceGetProperty = NULL;
static iohid_device_register_input_value_callback_fn real_IOHIDDeviceRegisterInputValueCallback = NULL;
static iohid_device_register_input_report_callback_fn real_IOHIDDeviceRegisterInputReportCallback = NULL;
static iohid_device_set_report_fn real_IOHIDDeviceSetReport = NULL;
static iohid_device_get_report_fn real_IOHIDDeviceGetReport = NULL;

static void logger_timestamp(char *out, size_t out_len)
{
    struct timespec ts;
    struct tm tm_local;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_local);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &tm_local);
    snprintf(out + strlen(out), out_len - strlen(out), ".%03ld", ts.tv_nsec / 1000000L);
}

static FILE *logger_fp(void)
{
    const char *path = NULL;

    pthread_mutex_lock(&g_log_lock);
    if (!g_log_fp) {
        path = getenv(LOGGER_PATH_ENV);
        if (!path || !path[0]) {
            path = LOGGER_DEFAULT_PATH;
        }
        g_log_fp = fopen(path, "a");
        if (!g_log_fp) {
            g_log_fp = stderr;
        }
    }
    pthread_mutex_unlock(&g_log_lock);
    return g_log_fp;
}

static void logger_writef(const char *fmt, ...)
{
    char stamp[64];
    va_list args;
    FILE *fp = logger_fp();

    if (!fp) return;

    logger_timestamp(stamp, sizeof(stamp));

    pthread_mutex_lock(&g_log_lock);
    fprintf(fp, "[%s] [mk1-call-logger] ", stamp);
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fputc('\n', fp);
    fflush(fp);
    pthread_mutex_unlock(&g_log_lock);
}

static void logger_hex_preview(const char *label, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t preview = len < LOGGER_PREVIEW_BYTES ? len : LOGGER_PREVIEW_BYTES;
    FILE *fp = logger_fp();

    if (!fp || !data || len == 0) return;

    pthread_mutex_lock(&g_log_lock);
    fprintf(fp, "    %s len=%zu", label, len);
    for (size_t i = 0; i < preview; i++) {
        if ((i % 16u) == 0u) {
            fprintf(fp, "\n    ");
        }
        fprintf(fp, "%02x ", bytes[i]);
    }
    if (preview < len) {
        fprintf(fp, "\n    ... %zu more bytes", len - preview);
    }
    fputc('\n', fp);
    fflush(fp);
    pthread_mutex_unlock(&g_log_lock);
}

static void logger_hex_line_locked(const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        fprintf(logger_fp(), "%02x%s", bytes[i], (i + 1u < len) ? " " : "");
    }
}

static void logger_led_delta_locked(const uint8_t *payload, size_t len)
{
    if (!g_last_led_payload_valid) {
        fprintf(logger_fp(), "    logical delta: <initial snapshot>\n");
        return;
    }

    bool any = false;
    fprintf(logger_fp(), "    logical delta:");
    for (size_t i = 0; i < len; i++) {
        if (g_last_led_payload[i] == payload[i]) continue;
        fprintf(logger_fp(), " logical[%zu]:%02x->%02x",
                i,
                g_last_led_payload[i],
                payload[i]);
        any = true;
    }
    if (!any) {
        fprintf(logger_fp(), " <no change>");
    }
    fputc('\n', logger_fp());
}

static void logger_led_payload(const char *call_name,
                               uint32_t selector,
                               const void *input_struct,
                               size_t input_struct_cnt)
{
    if (selector != MK1_SELECTOR_SET_LEDS || !input_struct) return;
    if (input_struct_cnt < MK1_LED_LOGICAL_BYTES) return;

    const uint8_t *payload = (const uint8_t *)input_struct;
    FILE *fp = logger_fp();
    if (!fp) return;

    pthread_mutex_lock(&g_log_lock);
    fprintf(fp, "[mk1-call-logger] [%s] [LED sel=%u len=%u] ",
            call_name,
            selector,
            MK1_LED_LOGICAL_BYTES);
    logger_hex_line_locked(payload, MK1_LED_LOGICAL_BYTES);
    fputc('\n', fp);
    logger_led_delta_locked(payload, MK1_LED_LOGICAL_BYTES);
    memcpy(g_last_led_payload, payload, MK1_LED_LOGICAL_BYTES);
    g_last_led_payload_valid = true;
    fflush(fp);
    pthread_mutex_unlock(&g_log_lock);
}

static void logger_cf_dictionary_summary(const char *label, CFDictionaryRef dict)
{
    CFStringRef desc;
    char buf[2048];

    if (!dict) return;
    desc = CFCopyDescription(dict);
    if (!desc) return;
    if (CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8)) {
        logger_writef("%s %s", label, buf);
    }
    CFRelease(desc);
}

static void logger_cf_summary(const char *label, CFTypeRef obj)
{
    CFStringRef desc;
    char buf[2048];

    if (!obj) return;
    desc = CFCopyDescription(obj);
    if (!desc) return;
    if (CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8)) {
        logger_writef("%s %s", label, buf);
    }
    CFRelease(desc);
}

static void resolve_symbols(void)
{
    static dispatch_once_t once_token;
    dispatch_once(&once_token, ^{
        real_IOServiceOpen = (io_service_open_fn)dlsym(RTLD_NEXT, "IOServiceOpen");
        real_IOServiceClose = (io_service_close_fn)dlsym(RTLD_NEXT, "IOServiceClose");
        real_IOIteratorNext = (io_iterator_next_fn)dlsym(RTLD_NEXT, "IOIteratorNext");
        real_IOObjectRelease = (io_object_release_fn)dlsym(RTLD_NEXT, "IOObjectRelease");
        real_IOServiceMatching = (io_service_matching_fn)dlsym(RTLD_NEXT, "IOServiceMatching");
        real_IOServiceGetMatchingServices = (io_service_get_matching_services_fn)dlsym(RTLD_NEXT, "IOServiceGetMatchingServices");
        real_IORegistryEntryCreateCFProperties = (io_registry_entry_create_cfproperties_fn)dlsym(RTLD_NEXT, "IORegistryEntryCreateCFProperties");
        real_IOConnectCallMethod = (io_connect_call_method_fn)dlsym(RTLD_NEXT, "IOConnectCallMethod");
        real_IOConnectCallScalarMethod = (io_connect_call_scalar_method_fn)dlsym(RTLD_NEXT, "IOConnectCallScalarMethod");
        real_IOConnectCallStructMethod = (io_connect_call_struct_method_fn)dlsym(RTLD_NEXT, "IOConnectCallStructMethod");
        real_IOConnectCallAsyncMethod = (io_connect_call_async_method_fn)dlsym(RTLD_NEXT, "IOConnectCallAsyncMethod");
        real_IOConnectCallAsyncScalarMethod = (io_connect_call_async_scalar_method_fn)dlsym(RTLD_NEXT, "IOConnectCallAsyncScalarMethod");
        real_IOConnectCallAsyncStructMethod = (io_connect_call_async_struct_method_fn)dlsym(RTLD_NEXT, "IOConnectCallAsyncStructMethod");
        real_IOConnectMapMemory64 = (io_connect_map_memory64_fn)dlsym(RTLD_NEXT, "IOConnectMapMemory64");
        real_io_connect_method_scalarI_scalarO = (io_connect_method_scalari_scalaro_fn)dlsym(RTLD_NEXT, "io_connect_method_scalarI_scalarO");
        real_io_connect_method_scalarI_structureO = (io_connect_method_scalari_structureo_fn)dlsym(RTLD_NEXT, "io_connect_method_scalarI_structureO");
        real_io_connect_method_scalarI_structureI = (io_connect_method_scalari_structurei_fn)dlsym(RTLD_NEXT, "io_connect_method_scalarI_structureI");
        real_io_connect_method_structureI_structureO = (io_connect_method_structurei_structureo_fn)dlsym(RTLD_NEXT, "io_connect_method_structureI_structureO");
        real_io_async_method_scalarI_scalarO = (io_async_method_scalari_scalaro_fn)dlsym(RTLD_NEXT, "io_async_method_scalarI_scalarO");
        real_io_async_method_scalarI_structureO = (io_async_method_scalari_structureo_fn)dlsym(RTLD_NEXT, "io_async_method_scalarI_structureO");
        real_io_async_method_scalarI_structureI = (io_async_method_scalari_structurei_fn)dlsym(RTLD_NEXT, "io_async_method_scalarI_structureI");
        real_io_async_method_structureI_structureO = (io_async_method_structurei_structureo_fn)dlsym(RTLD_NEXT, "io_async_method_structureI_structureO");
        real_IOHIDManagerCreate = (iohid_manager_create_fn)dlsym(RTLD_NEXT, "IOHIDManagerCreate");
        real_IOHIDManagerOpen = (iohid_manager_open_fn)dlsym(RTLD_NEXT, "IOHIDManagerOpen");
        real_IOHIDManagerClose = (iohid_manager_close_fn)dlsym(RTLD_NEXT, "IOHIDManagerClose");
        real_IOHIDManagerSetDeviceMatching = (iohid_manager_set_device_matching_fn)dlsym(RTLD_NEXT, "IOHIDManagerSetDeviceMatching");
        real_IOHIDManagerSetDeviceMatchingMultiple = (iohid_manager_set_device_matching_multiple_fn)dlsym(RTLD_NEXT, "IOHIDManagerSetDeviceMatchingMultiple");
        real_IOHIDManagerCopyDevices = (iohid_manager_copy_devices_fn)dlsym(RTLD_NEXT, "IOHIDManagerCopyDevices");
        real_IOHIDManagerRegisterDeviceMatchingCallback = (iohid_manager_register_device_matching_callback_fn)dlsym(RTLD_NEXT, "IOHIDManagerRegisterDeviceMatchingCallback");
        real_IOHIDManagerRegisterInputValueCallback = (iohid_manager_register_input_value_callback_fn)dlsym(RTLD_NEXT, "IOHIDManagerRegisterInputValueCallback");
        real_IOHIDManagerRegisterInputReportCallback = (iohid_manager_register_input_report_callback_fn)dlsym(RTLD_NEXT, "IOHIDManagerRegisterInputReportCallback");
        real_IOHIDDeviceCreate = (iohid_device_create_fn)dlsym(RTLD_NEXT, "IOHIDDeviceCreate");
        real_IOHIDDeviceOpen = (iohid_device_open_fn)dlsym(RTLD_NEXT, "IOHIDDeviceOpen");
        real_IOHIDDeviceClose = (iohid_device_close_fn)dlsym(RTLD_NEXT, "IOHIDDeviceClose");
        real_IOHIDDeviceGetProperty = (iohid_device_get_property_fn)dlsym(RTLD_NEXT, "IOHIDDeviceGetProperty");
        real_IOHIDDeviceRegisterInputValueCallback = (iohid_device_register_input_value_callback_fn)dlsym(RTLD_NEXT, "IOHIDDeviceRegisterInputValueCallback");
        real_IOHIDDeviceRegisterInputReportCallback = (iohid_device_register_input_report_callback_fn)dlsym(RTLD_NEXT, "IOHIDDeviceRegisterInputReportCallback");
        real_IOHIDDeviceSetReport = (iohid_device_set_report_fn)dlsym(RTLD_NEXT, "IOHIDDeviceSetReport");
        real_IOHIDDeviceGetReport = (iohid_device_get_report_fn)dlsym(RTLD_NEXT, "IOHIDDeviceGetReport");
    });
}

__attribute__((constructor))
static void logger_init(void)
{
    resolve_symbols();
    logger_writef("initialized pid=%d path=%s", getpid(), getenv(LOGGER_PATH_ENV) ? getenv(LOGGER_PATH_ENV) : LOGGER_DEFAULT_PATH);
}

__attribute__((destructor))
static void logger_fini(void)
{
    logger_writef("shutting down");
    if (g_log_fp && g_log_fp != stderr) {
        fclose(g_log_fp);
    }
    g_log_fp = NULL;
}

CFMutableDictionaryRef IOServiceMatching(const char *name)
{
    CFMutableDictionaryRef result;

    resolve_symbols();
    result = real_IOServiceMatching(name);
    logger_writef("IOServiceMatching(name=%s) -> %p", name ? name : "(null)", result);
    logger_cf_dictionary_summary("matching", result);
    return result;
}

kern_return_t IOServiceGetMatchingServices(mach_port_t mainPort, CFDictionaryRef matching, io_iterator_t *existing)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOServiceGetMatchingServices(mainPort=0x%x, matching=%p)", mainPort, matching);
    logger_cf_dictionary_summary("matching", matching);
    kr = real_IOServiceGetMatchingServices(mainPort, matching, existing);
    logger_writef("IOServiceGetMatchingServices -> kr=0x%x iterator=0x%x", kr, existing ? *existing : 0);
    return kr;
}

io_service_t IOIteratorNext(io_iterator_t iterator)
{
    io_service_t service;

    resolve_symbols();
    service = real_IOIteratorNext(iterator);
    logger_writef("IOIteratorNext(iterator=0x%x) -> service=0x%x", iterator, service);
    return service;
}

kern_return_t IOObjectRelease(io_object_t object)
{
    kern_return_t kr;

    resolve_symbols();
    kr = real_IOObjectRelease(object);
    logger_writef("IOObjectRelease(object=0x%x) -> kr=0x%x", object, kr);
    return kr;
}

kern_return_t IORegistryEntryCreateCFProperties(io_registry_entry_t entry, CFMutableDictionaryRef *properties, CFAllocatorRef allocator, IOOptionBits options)
{
    kern_return_t kr;

    resolve_symbols();
    kr = real_IORegistryEntryCreateCFProperties(entry, properties, allocator, options);
    logger_writef("IORegistryEntryCreateCFProperties(entry=0x%x, options=0x%x) -> kr=0x%x properties=%p",
                  entry,
                  options,
                  kr,
                  properties ? *properties : NULL);
    if (properties && *properties) {
        logger_cf_dictionary_summary("properties", *properties);
    }
    return kr;
}

kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect)
{
    kern_return_t kr;

    resolve_symbols();
    kr = real_IOServiceOpen(service, owningTask, type, connect);
    logger_writef("IOServiceOpen(service=0x%x, type=%u) -> kr=0x%x connect=0x%x",
                  service,
                  type,
                  kr,
                  connect ? *connect : 0);
    return kr;
}

kern_return_t IOServiceClose(io_connect_t connect)
{
    kern_return_t kr;

    resolve_symbols();
    kr = real_IOServiceClose(connect);
    logger_writef("IOServiceClose(connect=0x%x) -> kr=0x%x", connect, kr);
    return kr;
}

kern_return_t IOConnectMapMemory64(io_connect_t connect,
                                   uint32_t memoryType,
                                   task_port_t intoTask,
                                   mach_vm_address_t *atAddress,
                                   mach_vm_size_t *ofSize,
                                   IOOptionBits options)
{
    kern_return_t kr;

    resolve_symbols();
    kr = real_IOConnectMapMemory64(connect, memoryType, intoTask, atAddress, ofSize, options);
    logger_writef("IOConnectMapMemory64(connect=0x%x, memoryType=%u, options=0x%x) -> kr=0x%x address=0x%llx size=0x%llx",
                  connect,
                  memoryType,
                  options,
                  kr,
                  atAddress ? (unsigned long long)*atAddress : 0ull,
                  ofSize ? (unsigned long long)*ofSize : 0ull);
    return kr;
}

kern_return_t IOConnectCallMethod(mach_port_t connection,
                                  uint32_t selector,
                                  const uint64_t *inputScalars,
                                  uint32_t inputScalarCount,
                                  const void *inputStruct,
                                  size_t inputStructCnt,
                                  uint64_t *outputScalars,
                                  uint32_t *outputScalarCount,
                                  void *outputStruct,
                                  size_t *outputStructCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOConnectCallMethod(connection=0x%x, selector=%u, inputScalarCount=%u, inputStructCnt=%zu)",
                  connection,
                  selector,
                  inputScalarCount,
                  inputStructCnt);
    if (inputScalars && inputScalarCount) {
        logger_hex_preview("inputScalars", inputScalars, inputScalarCount * sizeof(uint64_t));
    }
    if (inputStruct && inputStructCnt) {
        logger_hex_preview("inputStruct", inputStruct, inputStructCnt);
        logger_led_payload("IOConnectCallMethod", selector, inputStruct, inputStructCnt);
    }

    kr = real_IOConnectCallMethod(connection,
                                  selector,
                                  inputScalars,
                                  inputScalarCount,
                                  inputStruct,
                                  inputStructCnt,
                                  outputScalars,
                                  outputScalarCount,
                                  outputStruct,
                                  outputStructCnt);

    logger_writef("IOConnectCallMethod -> kr=0x%x outputScalarCount=%u outputStructCnt=%zu",
                  kr,
                  outputScalarCount ? *outputScalarCount : 0,
                  outputStructCnt ? *outputStructCnt : 0);
    if (outputScalars && outputScalarCount && *outputScalarCount) {
        logger_hex_preview("outputScalars", outputScalars, (*outputScalarCount) * sizeof(uint64_t));
    }
    if (outputStruct && outputStructCnt && *outputStructCnt) {
        logger_hex_preview("outputStruct", outputStruct, *outputStructCnt);
    }
    return kr;
}

kern_return_t IOConnectCallScalarMethod(mach_port_t connection,
                                        uint32_t selector,
                                        const uint64_t *input,
                                        uint32_t inputCnt,
                                        uint64_t *output,
                                        uint32_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOConnectCallScalarMethod(connection=0x%x, selector=%u, inputCnt=%u)",
                  connection,
                  selector,
                  inputCnt);
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(uint64_t));
    }

    kr = real_IOConnectCallScalarMethod(connection, selector, input, inputCnt, output, outputCnt);
    logger_writef("IOConnectCallScalarMethod -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputScalars", output, (*outputCnt) * sizeof(uint64_t));
    }
    return kr;
}

kern_return_t IOConnectCallStructMethod(mach_port_t connection,
                                        uint32_t selector,
                                        const void *inputStruct,
                                        size_t inputStructCnt,
                                        void *outputStruct,
                                        size_t *outputStructCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOConnectCallStructMethod(connection=0x%x, selector=%u, inputStructCnt=%zu)",
                  connection,
                  selector,
                  inputStructCnt);
    if (inputStruct && inputStructCnt) {
        logger_hex_preview("inputStruct", inputStruct, inputStructCnt);
        logger_led_payload("IOConnectCallStructMethod", selector, inputStruct, inputStructCnt);
    }

    kr = real_IOConnectCallStructMethod(connection, selector, inputStruct, inputStructCnt, outputStruct, outputStructCnt);
    logger_writef("IOConnectCallStructMethod -> kr=0x%x outputStructCnt=%zu",
                  kr,
                  outputStructCnt ? *outputStructCnt : 0);
    if (outputStruct && outputStructCnt && *outputStructCnt) {
        logger_hex_preview("outputStruct", outputStruct, *outputStructCnt);
    }
    return kr;
}

kern_return_t IOConnectCallAsyncMethod(mach_port_t connection,
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
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOConnectCallAsyncMethod(connection=0x%x, selector=%u, wakePort=0x%x, referenceCnt=%u, inputScalarCnt=%u, inputStructCnt=%zu)",
                  connection,
                  selector,
                  wakePort,
                  referenceCnt,
                  inputScalarCnt,
                  inputStructCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(uint64_t));
    }
    if (inputScalars && inputScalarCnt) {
        logger_hex_preview("inputScalars", inputScalars, inputScalarCnt * sizeof(uint64_t));
    }
    if (inputStruct && inputStructCnt) {
        logger_hex_preview("inputStruct", inputStruct, inputStructCnt);
        logger_led_payload("IOConnectCallAsyncMethod", selector, inputStruct, inputStructCnt);
    }

    kr = real_IOConnectCallAsyncMethod(connection,
                                       selector,
                                       wakePort,
                                       reference,
                                       referenceCnt,
                                       inputScalars,
                                       inputScalarCnt,
                                       inputStruct,
                                       inputStructCnt,
                                       outputScalars,
                                       outputScalarCnt,
                                       outputStruct,
                                       outputStructCnt);

    logger_writef("IOConnectCallAsyncMethod -> kr=0x%x outputScalarCnt=%u outputStructCnt=%zu",
                  kr,
                  outputScalarCnt ? *outputScalarCnt : 0,
                  outputStructCnt ? *outputStructCnt : 0);
    if (outputScalars && outputScalarCnt && *outputScalarCnt) {
        logger_hex_preview("outputScalars", outputScalars, (*outputScalarCnt) * sizeof(uint64_t));
    }
    if (outputStruct && outputStructCnt && *outputStructCnt) {
        logger_hex_preview("outputStruct", outputStruct, *outputStructCnt);
    }
    return kr;
}

kern_return_t IOConnectCallAsyncScalarMethod(mach_port_t connection,
                                             uint32_t selector,
                                             mach_port_t wake_port,
                                             uint64_t *reference,
                                             uint32_t referenceCnt,
                                             const uint64_t *input,
                                             uint32_t inputCnt,
                                             uint64_t *output,
                                             uint32_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOConnectCallAsyncScalarMethod(connection=0x%x, selector=%u, wakePort=0x%x, referenceCnt=%u, inputCnt=%u)",
                  connection, selector, wake_port, referenceCnt, inputCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(uint64_t));
    }
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(uint64_t));
    }

    kr = real_IOConnectCallAsyncScalarMethod(connection, selector, wake_port, reference, referenceCnt, input, inputCnt, output, outputCnt);
    logger_writef("IOConnectCallAsyncScalarMethod -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputScalars", output, (*outputCnt) * sizeof(uint64_t));
    }
    return kr;
}

kern_return_t IOConnectCallAsyncStructMethod(mach_port_t connection,
                                             uint32_t selector,
                                             mach_port_t wake_port,
                                             uint64_t *reference,
                                             uint32_t referenceCnt,
                                             const void *inputStruct,
                                             size_t inputStructCnt,
                                             void *outputStruct,
                                             size_t *outputStructCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("IOConnectCallAsyncStructMethod(connection=0x%x, selector=%u, wakePort=0x%x, referenceCnt=%u, inputStructCnt=%zu)",
                  connection, selector, wake_port, referenceCnt, inputStructCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(uint64_t));
    }
    if (inputStruct && inputStructCnt) {
        logger_hex_preview("inputStruct", inputStruct, inputStructCnt);
        logger_led_payload("IOConnectCallAsyncStructMethod", selector, inputStruct, inputStructCnt);
    }

    kr = real_IOConnectCallAsyncStructMethod(connection, selector, wake_port, reference, referenceCnt, inputStruct, inputStructCnt, outputStruct, outputStructCnt);
    logger_writef("IOConnectCallAsyncStructMethod -> kr=0x%x outputStructCnt=%zu", kr, outputStructCnt ? *outputStructCnt : 0);
    if (outputStruct && outputStructCnt && *outputStructCnt) {
        logger_hex_preview("outputStruct", outputStruct, *outputStructCnt);
    }
    return kr;
}

kern_return_t io_connect_method_scalarI_scalarO(mach_port_t connection,
                                                int selector,
                                                io_scalar_inband_t input,
                                                mach_msg_type_number_t inputCnt,
                                                io_scalar_inband_t output,
                                                mach_msg_type_number_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_connect_method_scalarI_scalarO(connection=0x%x, selector=%d, inputCnt=%u)",
                  connection, selector, inputCnt);
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(input[0]));
    }

    kr = real_io_connect_method_scalarI_scalarO(connection, selector, input, inputCnt, output, outputCnt);
    logger_writef("io_connect_method_scalarI_scalarO -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputScalars", output, (*outputCnt) * sizeof(output[0]));
    }
    return kr;
}

kern_return_t io_connect_method_scalarI_structureO(mach_port_t connection,
                                                   int selector,
                                                   io_scalar_inband_t input,
                                                   mach_msg_type_number_t inputCnt,
                                                   io_struct_inband_t output,
                                                   mach_msg_type_number_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_connect_method_scalarI_structureO(connection=0x%x, selector=%d, inputCnt=%u)",
                  connection, selector, inputCnt);
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(input[0]));
    }

    kr = real_io_connect_method_scalarI_structureO(connection, selector, input, inputCnt, output, outputCnt);
    logger_writef("io_connect_method_scalarI_structureO -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputStruct", output, *outputCnt);
    }
    return kr;
}

kern_return_t io_connect_method_scalarI_structureI(mach_port_t connection,
                                                   int selector,
                                                   io_scalar_inband_t input,
                                                   mach_msg_type_number_t inputCnt,
                                                   io_struct_inband_t inputStruct,
                                                   mach_msg_type_number_t inputStructCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_connect_method_scalarI_structureI(connection=0x%x, selector=%d, inputCnt=%u, inputStructCnt=%u)",
                  connection, selector, inputCnt, inputStructCnt);
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(input[0]));
    }
    if (inputStruct && inputStructCnt) {
        logger_hex_preview("inputStruct", inputStruct, inputStructCnt);
        logger_led_payload("io_connect_method_scalarI_structureI", (uint32_t)selector, inputStruct, inputStructCnt);
    }

    kr = real_io_connect_method_scalarI_structureI(connection, selector, input, inputCnt, inputStruct, inputStructCnt);
    logger_writef("io_connect_method_scalarI_structureI -> kr=0x%x", kr);
    return kr;
}

kern_return_t io_connect_method_structureI_structureO(mach_port_t connection,
                                                      int selector,
                                                      io_struct_inband_t input,
                                                      mach_msg_type_number_t inputCnt,
                                                      io_struct_inband_t output,
                                                      mach_msg_type_number_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_connect_method_structureI_structureO(connection=0x%x, selector=%d, inputCnt=%u)",
                  connection, selector, inputCnt);
    if (input && inputCnt) {
        logger_hex_preview("inputStruct", input, inputCnt);
        logger_led_payload("io_connect_method_structureI_structureO", (uint32_t)selector, input, inputCnt);
    }

    kr = real_io_connect_method_structureI_structureO(connection, selector, input, inputCnt, output, outputCnt);
    logger_writef("io_connect_method_structureI_structureO -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputStruct", output, *outputCnt);
    }
    return kr;
}

kern_return_t io_async_method_scalarI_scalarO(mach_port_t connection,
                                              mach_port_t wake_port,
                                              io_async_ref_t reference,
                                              mach_msg_type_number_t referenceCnt,
                                              int selector,
                                              io_scalar_inband_t input,
                                              mach_msg_type_number_t inputCnt,
                                              io_scalar_inband_t output,
                                              mach_msg_type_number_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_async_method_scalarI_scalarO(connection=0x%x, wakePort=0x%x, selector=%d, referenceCnt=%u, inputCnt=%u)",
                  connection, wake_port, selector, referenceCnt, inputCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(io_user_reference_t));
    }
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(input[0]));
    }

    kr = real_io_async_method_scalarI_scalarO(connection, wake_port, reference, referenceCnt, selector, input, inputCnt, output, outputCnt);
    logger_writef("io_async_method_scalarI_scalarO -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputScalars", output, (*outputCnt) * sizeof(output[0]));
    }
    return kr;
}

kern_return_t io_async_method_scalarI_structureO(mach_port_t connection,
                                                 mach_port_t wake_port,
                                                 io_async_ref_t reference,
                                                 mach_msg_type_number_t referenceCnt,
                                                 int selector,
                                                 io_scalar_inband_t input,
                                                 mach_msg_type_number_t inputCnt,
                                                 io_struct_inband_t output,
                                                 mach_msg_type_number_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_async_method_scalarI_structureO(connection=0x%x, wakePort=0x%x, selector=%d, referenceCnt=%u, inputCnt=%u)",
                  connection, wake_port, selector, referenceCnt, inputCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(io_user_reference_t));
    }
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(input[0]));
    }

    kr = real_io_async_method_scalarI_structureO(connection, wake_port, reference, referenceCnt, selector, input, inputCnt, output, outputCnt);
    logger_writef("io_async_method_scalarI_structureO -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputStruct", output, *outputCnt);
    }
    return kr;
}

kern_return_t io_async_method_scalarI_structureI(mach_port_t connection,
                                                 mach_port_t wake_port,
                                                 io_async_ref_t reference,
                                                 mach_msg_type_number_t referenceCnt,
                                                 int selector,
                                                 io_scalar_inband_t input,
                                                 mach_msg_type_number_t inputCnt,
                                                 io_struct_inband_t inputStruct,
                                                 mach_msg_type_number_t inputStructCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_async_method_scalarI_structureI(connection=0x%x, wakePort=0x%x, selector=%d, referenceCnt=%u, inputCnt=%u, inputStructCnt=%u)",
                  connection, wake_port, selector, referenceCnt, inputCnt, inputStructCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(io_user_reference_t));
    }
    if (input && inputCnt) {
        logger_hex_preview("inputScalars", input, inputCnt * sizeof(input[0]));
    }
    if (inputStruct && inputStructCnt) {
        logger_hex_preview("inputStruct", inputStruct, inputStructCnt);
        logger_led_payload("io_async_method_scalarI_structureI", (uint32_t)selector, inputStruct, inputStructCnt);
    }

    kr = real_io_async_method_scalarI_structureI(connection, wake_port, reference, referenceCnt, selector, input, inputCnt, inputStruct, inputStructCnt);
    logger_writef("io_async_method_scalarI_structureI -> kr=0x%x", kr);
    return kr;
}

kern_return_t io_async_method_structureI_structureO(mach_port_t connection,
                                                    mach_port_t wake_port,
                                                    io_async_ref_t reference,
                                                    mach_msg_type_number_t referenceCnt,
                                                    int selector,
                                                    io_struct_inband_t input,
                                                    mach_msg_type_number_t inputCnt,
                                                    io_struct_inband_t output,
                                                    mach_msg_type_number_t *outputCnt)
{
    kern_return_t kr;

    resolve_symbols();
    logger_writef("io_async_method_structureI_structureO(connection=0x%x, wakePort=0x%x, selector=%d, referenceCnt=%u, inputCnt=%u)",
                  connection, wake_port, selector, referenceCnt, inputCnt);
    if (reference && referenceCnt) {
        logger_hex_preview("reference", reference, referenceCnt * sizeof(io_user_reference_t));
    }
    if (input && inputCnt) {
        logger_hex_preview("inputStruct", input, inputCnt);
        logger_led_payload("io_async_method_structureI_structureO", (uint32_t)selector, input, inputCnt);
    }

    kr = real_io_async_method_structureI_structureO(connection, wake_port, reference, referenceCnt, selector, input, inputCnt, output, outputCnt);
    logger_writef("io_async_method_structureI_structureO -> kr=0x%x outputCnt=%u", kr, outputCnt ? *outputCnt : 0);
    if (output && outputCnt && *outputCnt) {
        logger_hex_preview("outputStruct", output, *outputCnt);
    }
    return kr;
}

IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef allocator, IOOptionBits options)
{
    IOHIDManagerRef manager;
    resolve_symbols();
    manager = real_IOHIDManagerCreate(allocator, options);
    logger_writef("IOHIDManagerCreate(options=0x%x) -> %p", options, manager);
    return manager;
}

IOReturn IOHIDManagerOpen(IOHIDManagerRef manager, IOOptionBits options)
{
    IOReturn kr;
    resolve_symbols();
    kr = real_IOHIDManagerOpen(manager, options);
    logger_writef("IOHIDManagerOpen(manager=%p, options=0x%x) -> 0x%x", manager, options, kr);
    return kr;
}

IOReturn IOHIDManagerClose(IOHIDManagerRef manager, IOOptionBits options)
{
    IOReturn kr;
    resolve_symbols();
    kr = real_IOHIDManagerClose(manager, options);
    logger_writef("IOHIDManagerClose(manager=%p, options=0x%x) -> 0x%x", manager, options, kr);
    return kr;
}

void IOHIDManagerSetDeviceMatching(IOHIDManagerRef manager, CFDictionaryRef matching)
{
    resolve_symbols();
    logger_writef("IOHIDManagerSetDeviceMatching(manager=%p, matching=%p)", manager, matching);
    if (matching) logger_cf_dictionary_summary("hidManagerMatching", matching);
    real_IOHIDManagerSetDeviceMatching(manager, matching);
}

void IOHIDManagerSetDeviceMatchingMultiple(IOHIDManagerRef manager, CFArrayRef multiple)
{
    resolve_symbols();
    logger_writef("IOHIDManagerSetDeviceMatchingMultiple(manager=%p, multiple=%p)", manager, multiple);
    if (multiple) logger_cf_summary("hidManagerMatchingMultiple", multiple);
    real_IOHIDManagerSetDeviceMatchingMultiple(manager, multiple);
}

CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef manager)
{
    CFSetRef devices;
    resolve_symbols();
    devices = real_IOHIDManagerCopyDevices(manager);
    logger_writef("IOHIDManagerCopyDevices(manager=%p) -> %p", manager, devices);
    if (devices) logger_cf_summary("hidManagerDevices", devices);
    return devices;
}

void IOHIDManagerRegisterDeviceMatchingCallback(IOHIDManagerRef manager, IOHIDDeviceCallback callback, void *context)
{
    resolve_symbols();
    logger_writef("IOHIDManagerRegisterDeviceMatchingCallback(manager=%p, callback=%p, context=%p)", manager, callback, context);
    real_IOHIDManagerRegisterDeviceMatchingCallback(manager, callback, context);
}

void IOHIDManagerRegisterInputValueCallback(IOHIDManagerRef manager, IOHIDValueCallback callback, void *context)
{
    resolve_symbols();
    logger_writef("IOHIDManagerRegisterInputValueCallback(manager=%p, callback=%p, context=%p)", manager, callback, context);
    real_IOHIDManagerRegisterInputValueCallback(manager, callback, context);
}

void IOHIDManagerRegisterInputReportCallback(IOHIDManagerRef manager, IOHIDReportCallback callback, void *context)
{
    resolve_symbols();
    logger_writef("IOHIDManagerRegisterInputReportCallback(manager=%p, callback=%p, context=%p)", manager, callback, context);
    real_IOHIDManagerRegisterInputReportCallback(manager, callback, context);
}

IOHIDDeviceRef IOHIDDeviceCreate(CFAllocatorRef allocator, io_service_t service)
{
    IOHIDDeviceRef device;
    resolve_symbols();
    device = real_IOHIDDeviceCreate(allocator, service);
    logger_writef("IOHIDDeviceCreate(service=0x%x) -> %p", service, device);
    return device;
}

IOReturn IOHIDDeviceOpen(IOHIDDeviceRef device, IOOptionBits options)
{
    IOReturn kr;
    resolve_symbols();
    kr = real_IOHIDDeviceOpen(device, options);
    logger_writef("IOHIDDeviceOpen(device=%p, options=0x%x) -> 0x%x", device, options, kr);
    return kr;
}

IOReturn IOHIDDeviceClose(IOHIDDeviceRef device, IOOptionBits options)
{
    IOReturn kr;
    resolve_symbols();
    kr = real_IOHIDDeviceClose(device, options);
    logger_writef("IOHIDDeviceClose(device=%p, options=0x%x) -> 0x%x", device, options, kr);
    return kr;
}

CFTypeRef IOHIDDeviceGetProperty(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef value;
    resolve_symbols();
    value = real_IOHIDDeviceGetProperty(device, key);
    logger_writef("IOHIDDeviceGetProperty(device=%p, key=%p) -> %p", device, key, value);
    if (key) logger_cf_summary("hidDevicePropertyKey", key);
    if (value) logger_cf_summary("hidDevicePropertyValue", value);
    return value;
}

void IOHIDDeviceRegisterInputValueCallback(IOHIDDeviceRef device, IOHIDValueCallback callback, void *context)
{
    resolve_symbols();
    logger_writef("IOHIDDeviceRegisterInputValueCallback(device=%p, callback=%p, context=%p)", device, callback, context);
    real_IOHIDDeviceRegisterInputValueCallback(device, callback, context);
}

void IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef device, uint8_t *report, CFIndex reportLength, IOHIDReportCallback callback, void *context)
{
    resolve_symbols();
    logger_writef("IOHIDDeviceRegisterInputReportCallback(device=%p, report=%p, reportLength=%ld, callback=%p, context=%p)",
                  device, report, (long)reportLength, callback, context);
    real_IOHIDDeviceRegisterInputReportCallback(device, report, reportLength, callback, context);
}

IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef device, IOHIDReportType reportType, CFIndex reportID, const uint8_t *report, CFIndex reportLength)
{
    IOReturn kr;
    resolve_symbols();
    logger_writef("IOHIDDeviceSetReport(device=%p, type=%u, reportID=%ld, reportLength=%ld)",
                  device, (unsigned)reportType, (long)reportID, (long)reportLength);
    if (report && reportLength > 0) {
        logger_hex_preview("hidSetReport", report, (size_t)reportLength);
    }
    kr = real_IOHIDDeviceSetReport(device, reportType, reportID, report, reportLength);
    logger_writef("IOHIDDeviceSetReport -> 0x%x", kr);
    return kr;
}

IOReturn IOHIDDeviceGetReport(IOHIDDeviceRef device, IOHIDReportType reportType, CFIndex reportID, uint8_t *report, CFIndex *reportLength)
{
    IOReturn kr;
    resolve_symbols();
    logger_writef("IOHIDDeviceGetReport(device=%p, type=%u, reportID=%ld, requestedLength=%ld)",
                  device,
                  (unsigned)reportType,
                  (long)reportID,
                  reportLength ? (long)*reportLength : -1L);
    kr = real_IOHIDDeviceGetReport(device, reportType, reportID, report, reportLength);
    logger_writef("IOHIDDeviceGetReport -> 0x%x reportID=%ld reportLength=%ld",
                  kr,
                  (long)reportID,
                  reportLength ? (long)*reportLength : -1L);
    if (report && reportLength && *reportLength > 0) {
        logger_hex_preview("hidGetReport", report, (size_t)*reportLength);
    }
    return kr;
}
