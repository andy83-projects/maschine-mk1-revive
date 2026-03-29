// mk1-ipc-test
//
// Standalone test that connects to NIHardwareAgent and attempts
// the handshake. No USB device needed — just NIHA running.
//
// Start NIHA first, then run this binary:
//   ~/Desktop/NIHardwareAgent-patched.app/Contents/MacOS/NIHardwareAgent &
//   sleep 2
//   ./mk1-ipc-test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

#include "../mk1-ipc/mk1_ipc.h"

static mk1_ipc_connection_t *g_conn = NULL;
static int g_msg_count = 0;

// ---------------------------------------------------------------------------
// IPC callback — log everything NIHA sends us
// ---------------------------------------------------------------------------

static void on_ipc_message(SInt32 msgid, CFDataRef data, void *ctx)
{
    (void)ctx;
    g_msg_count++;

    CFIndex len = data ? CFDataGetLength(data) : 0;
    printf("\n[test] === message #%d from NIHA ===\n", g_msg_count);
    printf("[test] msgid = 0x%x (%d)\n", (unsigned)msgid, (int)msgid);
    printf("[test] length = %ld bytes\n", (long)len);

    if (data && len > 0) {
        const uint8_t *bytes = CFDataGetBytePtr(data);

        // Hex dump
        printf("[test] hex:   ");
        for (CFIndex i = 0; i < len && i < 64; i++) {
            printf("%02x ", bytes[i]);
            if ((i + 1) % 16 == 0) printf("\n              ");
        }
        if (len > 64) printf("... (%ld more bytes)", len - 64);
        printf("\n");

        // ASCII dump
        printf("[test] ascii: ");
        for (CFIndex i = 0; i < len && i < 64; i++) {
            printf("%c", (bytes[i] >= 0x20 && bytes[i] < 0x7f) ? bytes[i] : '.');
        }
        printf("\n");

        // uint32 fields
        if (len >= 4) {
            printf("[test] u32s:  ");
            for (CFIndex i = 0; i + 3 < len && i < 32; i += 4) {
                uint32_t val;
                memcpy(&val, bytes + i, 4);
                printf("0x%08x ", val);
            }
            printf("\n");
        }
    }
    printf("[test] ===================================\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------

static void on_signal(int sig)
{
    printf("\n[test] caught signal %d, exiting\n", sig);
    CFRunLoopStop(CFRunLoopGetMain());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("[test] mk1-ipc-test\n");
    printf("[test] connecting to NIHardwareAgent on port '%s'...\n\n",
           NIHA_BOOTSTRAP_PORT);

    g_conn = mk1_ipc_connect(on_ipc_message, NULL);
    if (!g_conn) {
        printf("[test] FAILED — NIHardwareAgent is not running\n");
        printf("[test] start it first:\n");
        printf("[test]   ~/Desktop/NIHardwareAgent-patched.app"
               "/Contents/MacOS/NIHardwareAgent &\n");
        return 1;
    }

    printf("\n[test] connected — attempting handshake...\n\n");

    bool ok = mk1_ipc_handshake(g_conn);

    printf("\n[test] handshake returned: %s\n", ok ? "OK" : "FAILED");

    const char *serial = mk1_ipc_get_serial(g_conn);
    if (serial && strlen(serial) > 0)
        printf("[test] device serial: '%s'\n", serial);
    else
        printf("[test] no serial received yet\n");

    printf("\n[test] listening for further NIHA messages (Ctrl-C to quit)...\n\n");

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    CFRunLoopRun();

    printf("\n[test] total messages received from NIHA: %d\n", g_msg_count);
    mk1_ipc_disconnect(g_conn);
    return 0;
}
