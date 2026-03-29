// mk1-shim.c
//
// DYLD_INSERT_LIBRARIES shim for debugging NIHardwareAgent's IOKit calls.
//
// Usage (disable SIP or use on non-SIP-protected binary):
//   DYLD_INSERT_LIBRARIES=/path/to/mk1-shim.dylib \
//   DYLD_FORCE_FLAT_NAMESPACE=1 \
//   /Library/Application\ Support/Native\ Instruments/Hardware/NIHardwareAgent.app/Contents/MacOS/NIHardwareAgent
//
// What this logs:
//   - IOServiceMatching calls (what service name NIHA looks for)
//   - IOServiceGetMatchingServices (what it finds)
//   - IOServiceOpen (what it tries to open)
//   - CFMessagePortCreateRemote (what IPC ports it connects to)
//   - CFMessagePortCreateLocal (what ports it registers)
//
// This solves the "what IOKit service name does NIHA expect?" question
// without needing an Intel Mac — just run it and read the log.

#include <stdio.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

// ---------------------------------------------------------------------------
// Logging helper
// ---------------------------------------------------------------------------

#define SHIM_LOG(fmt, ...) \
    fprintf(stderr, "[mk1-shim] " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// IOKit interposing
// ---------------------------------------------------------------------------

// IOServiceMatching — what service class is NIHA looking for?
CFMutableDictionaryRef mk1_shim_IOServiceMatching(const char *name)
{
    SHIM_LOG("IOServiceMatching(\"%s\")", name ? name : "(null)");
    // Call original
    CFMutableDictionaryRef (*orig)(const char *) =
        dlsym(RTLD_NEXT, "IOServiceMatching");
    return orig(name);
}

// IOServiceGetMatchingServices — did it find anything?
kern_return_t mk1_shim_IOServiceGetMatchingServices(mach_port_t master,
                                                     CFDictionaryRef matching,
                                                     io_iterator_t *iter)
{
    // Log what's being searched for
    if (matching) {
        CFStringRef desc = CFCopyDescription(matching);
        char buf[512] = {0};
        CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
        SHIM_LOG("IOServiceGetMatchingServices: %s", buf);
        CFRelease(desc);
    }
    kern_return_t (*orig)(mach_port_t, CFDictionaryRef, io_iterator_t *) =
        dlsym(RTLD_NEXT, "IOServiceGetMatchingServices");
    kern_return_t ret = orig(master, matching, iter);
    SHIM_LOG("  → result: %d, iterator: %u", ret, iter ? *iter : 0);
    return ret;
}

// IOServiceOpen — what exactly is it opening?
kern_return_t mk1_shim_IOServiceOpen(io_service_t service,
                                      task_port_t owning_task,
                                      uint32_t type,
                                      io_connect_t *connect)
{
    SHIM_LOG("IOServiceOpen(service=%u, type=%u)", service, type);
    kern_return_t (*orig)(io_service_t, task_port_t, uint32_t, io_connect_t *) =
        dlsym(RTLD_NEXT, "IOServiceOpen");
    kern_return_t ret = orig(service, owning_task, type, connect);
    SHIM_LOG("  → result: %d, connect: %u", ret, connect ? *connect : 0);
    return ret;
}

// ---------------------------------------------------------------------------
// CFMessagePort interposing — confirm port names
// ---------------------------------------------------------------------------

CFMessagePortRef mk1_shim_CFMessagePortCreateRemote(CFAllocatorRef alloc,
                                                      CFStringRef name)
{
    char buf[256] = {0};
    if (name) CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    SHIM_LOG("CFMessagePortCreateRemote(\"%s\")", buf);

    CFMessagePortRef (*orig)(CFAllocatorRef, CFStringRef) =
        dlsym(RTLD_NEXT, "CFMessagePortCreateRemote");
    CFMessagePortRef ret = orig(alloc, name);
    SHIM_LOG("  → %s", ret ? "success" : "FAILED");
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
    SHIM_LOG("CFMessagePortCreateLocal(\"%s\")", buf);

    CFMessagePortRef (*orig)(CFAllocatorRef, CFStringRef,
                              CFMessagePortCallBack,
                              CFMessagePortContext *, Boolean *) =
        dlsym(RTLD_NEXT, "CFMessagePortCreateLocal");
    return orig(alloc, name, cb, ctx, shouldFreeInfo);
}

// ---------------------------------------------------------------------------
// Interpose table — tells dyld to replace the real symbols with ours
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

    { (void *)mk1_shim_CFMessagePortCreateRemote,
      (void *)CFMessagePortCreateRemote },

    { (void *)mk1_shim_CFMessagePortCreateLocal,
      (void *)CFMessagePortCreateLocal },
};

// ---------------------------------------------------------------------------
// Constructor — confirm shim loaded
// ---------------------------------------------------------------------------

__attribute__((constructor))
static void shim_init(void)
{
    SHIM_LOG("loaded — intercepting IOKit and CFMessagePort calls");
    SHIM_LOG("watching for VID=0x17CC PID=0x0808 (Maschine MK1)");
}
