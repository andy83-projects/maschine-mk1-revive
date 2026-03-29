# Xcode Project Setup Instructions

The source files are all present. Create the Xcode workspace manually:

## Step 1 — Create Workspace

1. Open Xcode
2. File → New → Workspace
3. Save as `maschine-mk1-revive.xcworkspace` in the repo root

## Step 2 — Add hidapi as a submodule

In Terminal, from the repo root:

```bash
git submodule add https://github.com/libusb/hidapi vendor/hidapi
```

## Step 3 — Create `mk1-usb` target (Static Library)

1. File → New → Project → macOS → Library
2. Name: `mk1-usb`, Language: C, Type: Static
3. Save inside the `mk1-usb/` directory
4. Add to workspace
5. Add `mk1_device.h` and `mk1_device.c` to target
6. Add `vendor/hidapi/mac/hid.c` to target (hidapi macOS backend)
7. Under Build Settings:
   - Header Search Paths: `$(SRCROOT)/../vendor/hidapi/hidapi`
8. Under Build Phases → Link Binary With Libraries:
   - `IOKit.framework`
   - `CoreFoundation.framework`

## Step 4 — Create `mk1-ipc` target (Static Library)

1. File → New → Project → macOS → Library
2. Name: `mk1-ipc`, Language: C, Type: Static
3. Save inside `mk1-ipc/` directory
4. Add to workspace
5. Add `mk1_ipc.h` and `mk1_ipc.c`
6. Link: `CoreFoundation.framework`

## Step 5 — Create `mk1-bridge` target (Command Line Tool)

1. File → New → Project → macOS → Command Line Tool
2. Name: `mk1-bridge`, Language: C
3. Save inside `mk1-bridge/` directory
4. Add to workspace
5. Add `main.c`
6. Under Build Phases → Link Binary With Libraries:
   - `mk1-usb.a` (drag from Products)
   - `mk1-ipc.a` (drag from Products)
   - `IOKit.framework`
   - `CoreFoundation.framework`
7. Under Build Settings:
   - Header Search Paths: `$(SRCROOT)/../mk1-usb $(SRCROOT)/../mk1-ipc`

## Step 6 — Create `mk1-shim` target (Dynamic Library)

1. File → New → Project → macOS → Library
2. Name: `mk1-shim`, Language: C, Type: Dynamic
3. Save inside `mk1-shim/` directory
4. Add to workspace
5. Add `mk1_shim.c`
6. Under Build Phases → Link Binary With Libraries:
   - `IOKit.framework`
   - `CoreFoundation.framework`
7. Under Build Settings:
   - Other Linker Flags: `-undefined dynamic_lookup`
     (required for DYLD_INSERT_LIBRARIES interposing)

## Step 7 — Scheme order

Build order should be: `mk1-usb` → `mk1-ipc` → `mk1-bridge`
`mk1-shim` builds independently.

## Code Signing

For local development:
- Signing Certificate: Sign to Run Locally (ad-hoc)
- No entitlements needed for the shim or bridge at this stage

## First Build Goal

Get `mk1-shim.dylib` building and injecting into NIHardwareAgent — 
this tells us what IOKit service name NIHA expects, solving the biggest
unknown without needing the Intel Mac.
