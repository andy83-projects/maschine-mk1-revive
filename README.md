# maschine-mk1-revive

A userspace driver shim to make the Native Instruments Maschine MK1 work on Apple Silicon macOS,
with the original Maschine software.

## Problem

The MK1 relied on `NIUSBMaschineController.kext` — a kernel extension that:
1. Matched the USB device (VID `0x17CC`, PID `0x0808`)
2. Published an IOKit service that `NIHardwareAgent` could open
3. Gave `NIHardwareAgent` exclusive USB interface access

Kernel extensions are dead on Apple Silicon. The kext never received an ARM64 build.

## Approach

Entirely userspace. No DriverKit, no kernel code, no special entitlements.

```
[Maschine.app] ←─CFMessagePort─→ [NIHardwareAgent]
                                        ↑
                                  [mk1-bridge]  ← our daemon
                                        ↑
                               IOUSBHost / hidapi
                                        ↑
                                  [MK1 hardware]
```

`NIHardwareAgent` speaks `CFMessagePort` IPC to the Maschine software.
Our bridge daemon claims the USB device first (before NIHardwareAgent can),
reads HID reports from the hardware, and forwards them over IPC — making
NIHardwareAgent believe it has a working device.

### Components

| Target | Type | Purpose |
|--------|------|---------|
| `mk1-usb` | static lib | Claim USB device, read/write HID reports |
| `mk1-ipc` | static lib | CFMessagePort handshake + NI IPC protocol |
| `mk1-bridge` | daemon | Glues USB ↔ IPC, runs as launchd agent |
| `mk1-shim` | dylib | DYLD_INSERT_LIBRARIES shim for logging/debugging IOKit calls |

## Prior Art

This project leans heavily on reverse engineering work by others:

- **[biappi/Macchina](https://github.com/biappi/Macchina)** — MK1-era (2012). Reverse engineered
  the CFMessagePort handshake between NIHardwareAgent and Maschine software. Implemented both
  client (connect to NIHardwareAgent) and server (impersonate NIHardwareAgent).

- **[SamL98/NIProtocol](https://github.com/SamL98/NIProtocol)** — MK2. Full handshake documented,
  `CFMessagePort` port name `NIHWMainHandler`, complete client + server in C.
  Article: [Rage Against the Maschine](https://lerner98.medium.com/rage-against-the-maschine-3357be1abc48)

- **[terminar/rebellion](https://github.com/terminar/rebellion)** — MK3/KK. Most complete IPC
  implementation. Masquerades as Maschine software, connects to NIHA, takes over device.
  Key insight: USB data and IPC data share the same format.

- **[fzero/maschine-mk1](https://github.com/fzero/maschine-mk1)** — MK1 HID protocol on Linux.
  Confirms VID `0x17CC` / PID `0x0808`, HID report structure.

- **[hansfbaier/open-maschine](https://github.com/hansfbaier/open-maschine)** — MK2 proof-of-concept
  using hidapi. Report IDs: `0x10` buttons/encoders, `0x20` pads (continuous).

## USB / HID Protocol (MK1)

- **VID:** `0x17CC`
- **PID:** `0x0808`
- **Interfaces:** HID (input) + DFU (firmware upgrade, ignore)
- **Report ID `0x10`:** Button and encoder state
- **Report ID `0x20`:** Pad pressure data (sent continuously)
- **Output reports:** LED states, display content (2x monochrome LCDs)

## IPC Protocol

NIHardwareAgent communicates via `CFMessagePort` (Mach-based).
- Port name: `NIHWMainHandler`
- Handshake sequence: TBD (see `docs/ipc-protocol.md`)
- Message format: TBD (fill in from Intel Mac capture session)

## Intel Mac Data Collection

Before the bridge can work, we need to capture from a working system:

```bash
# 1. kext Info.plist — matching config and IOKit class names
cat /Library/Extensions/NIUSBMaschineController.kext/Contents/Info.plist

# 2. IORegistry entry — what the kext publishes (device must be plugged in)
ioreg -l -w 0 -p IOService | grep -B 2 -A 80 "Maschine"

# 3. Confirm kext loaded
kextstat | grep -i "native\|NIUSB\|maschine"

# 4. USB descriptor
system_profiler SPUSBDataType | grep -A 20 "Maschine"

# 5. What NIHardwareAgent has open (run while Maschine software is running)
lsof -p $(pgrep NIHardwareAgent) | grep -i "usb\|hid\|ni"
```

## Build

Requires Xcode 15+ on Apple Silicon macOS 14+.

Open `maschine-mk1-revive.xcworkspace` in Xcode.

Targets build order: `mk1-usb` → `mk1-ipc` → `mk1-bridge`

The `mk1-shim` target builds independently.

## Status

- [ ] Directory structure and project scaffold
- [ ] Intel Mac data collection
- [ ] USB device claim (IOUSBHost)
- [ ] HID report parsing (mk1-usb)
- [ ] IPC handshake (mk1-ipc)
- [ ] Bridge daemon (mk1-bridge)
- [ ] launchd agent plist
- [ ] End-to-end test with Maschine software
