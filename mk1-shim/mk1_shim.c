// mk1-shim.c
//
// DYLD_INSERT_LIBRARIES shim for debugging NIHardwareAgent's IOKit calls.
//
// Usage:
//   DYLD_INSERT_LIBRARIES=/path/to/libmk1-shim.dylib \
//   DYLD_FORCE_FLAT_NAMESPACE=1 \
//   /path/to/NIHardwareAgent-patched.app/Contents/MacOS/NIHardwareAgent

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

// ---------------------------------------------------------------------------
// Logging — only log things that look NI/USB related
// ---------------------------------------------------------------------------

#define SHIM_LOG(fmt, ...) \
    fprintf(stderr, "[mk1-shim] " fmt "\n", ##__VA_ARGS__)

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

    kern_return_t (*orig)(io_service_t, task_port_t, uint32_t, io_connect_t *) =
        dlsym(RTLD_NEXT, "IOServiceOpen");
    kern_return_t ret = orig(service, owning_task, type, connect);
    SHIM_LOG("  → result=%d connect=%u", ret, connect ? *connect : 0);
    return ret;
}

// ---------------------------------------------------------------------------
// IORegistryEntryGetName — tells us the name of services NIHA finds
// ---------------------------------------------------------------------------

kern_return_t mk1_shim_IORegistryEntryGetName(io_registry_entry_t entry,
                                               io_name_t name)
{
    kern_return_t (*orig)(io_registry_entry_t, io_name_t) =
        dlsym(RTLD_NEXT, "IORegistryEntryGetName");
    kern_return_t ret = orig(entry, name);
    if (ret == 0 && name && is_interesting(name)) {
        SHIM_LOG("IORegistryEntryGetName → \"%s\"", name);
    }
    return ret;
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

    { (void *)mk1_shim_IORegistryEntryGetName,
      (void *)IORegistryEntryGetName },

    { (void *)mk1_shim_CFMessagePortCreateRemote,
      (void *)CFMessagePortCreateRemote },

    { (void *)mk1_shim_CFMessagePortCreateLocal,
      (void *)CFMessagePortCreateLocal },
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

__attribute__((constructor))
static void shim_init(void)
{
    SHIM_LOG("====================================================");
    SHIM_LOG("mk1-shim loaded");
    SHIM_LOG("Filtering noise — only logging NI/USB relevant calls");
    SHIM_LOG("Plug in MK1 now and watch for IOServiceMatching calls");
    SHIM_LOG("====================================================");
}
