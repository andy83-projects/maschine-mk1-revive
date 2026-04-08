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
[Maschine.app] ←─CFMessagePort─→ [mk1-bridge]  ← our daemon (impersonates NIHA)
                                        ↑
                                    IOKit USB (bulk transfers)
                                        ↑
                                  [MK1 hardware]
```

`mk1-bridge` impersonates `NIHardwareAgent` entirely — it kills the real NIHA and
registers the `NIHWMainHandler` CFMessagePort itself. It owns both ends: full IPC
handshake with the Maschine app and direct IOKit USB bulk transfers to the hardware.

Working as of 2026-04-08:
- Maschine software detects the MK1 in its controller list
- Both displays render correctly (ST7529, EP8 bulk, 170×64 grayscale)
- All LEDs respond (buttons, groups, transport, pad rubber LEDs via EP1 DIMM_LEDS)
- Pads register velocity and pressure (EP4 64-byte reports, 12-bit ADC, IPC forwarded)
- Group, transport, and screen buttons registered (EP1 short reports)
- All 11 encoders forwarded: Volume, Tempo, Swing (Master Section) + 8 screen area encoders
- USB hot-plug: bridge can start before device is connected; device can be unplugged and re-plugged

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

`mk1-bridge` impersonates NIHardwareAgent via `CFMessagePort` (Mach-based).
- Port name: `NIHWMainHandler` (registered by bridge, kills real NIHA first)
- Full handshake: GetServiceVersion → PID_CONNECT → ACK_NOTIF_PORT → SERIAL_CONNECT → START
- Input events: pad pressure (NI_EVT_PAD_DATA), button state (NI_EVT_BTN_DATA)
- Output commands: LED brightness (NI_CMD_LED → EP1 DIMM_LEDS), display frames (EP8 RAMWR)
- Protocol fully documented in `CLAUDE.md` and `mk1-ipc/mk1_ipc.h`

## Build

Requires Xcode 15+ on Apple Silicon macOS 14+.

Open `maschine-mk1-revive.xcworkspace` in Xcode.

Targets build order: `mk1-usb` → `mk1-ipc` → `mk1-bridge`

The `mk1-shim` target builds independently.

## Status

- [x] Directory structure and project scaffold
- [x] Intel Mac data collection (pcap, Frida trace, Ghidra kext RE)
- [x] USB device claim (IOKit direct, not HID)
- [x] IPC handshake (full NIHA impersonation including Serial Connect phase)
- [x] Bridge daemon skeleton (`mk1-bridge`) — Maschine detects MK1 in controller list
- [x] Display init (EP8, ST7529 17-command sequence; UI-mode scan direction `0xbc [0x02,0x01,0x01]`)
- [x] LCD display pixel updates — full framebuffer composite + RAMWR; display renders correctly
- [~] LED forwarding — button/group/transport/pad rubber LEDs all confirmed working
- [x] Pad input events — EP4 64-byte reports decoded; pressure, hit-on/off forwarded via IPC
- [x] Button input events — EP1 short reports decoded; group/transport/screen buttons forwarded
- [~] Display backlight stays on — toggles briefly on certain button presses (under investigation)
- [x] Master Section knobs — Volume, Tempo, Swing encoder events forwarded via IPC
- [x] Screen area encoders — all 8 encoders mapped (byte pairs confirmed from hardware capture)
- [x] USB hot-plug — bridge survives device unplug/replug; starts before device is connected
- [~] Bridge reconnect — sends DEVICE_OFF via saved session state to trigger Maschine re-handshake on bridge restart (under test)
- [ ] launchd agent plist
- [ ] End-to-end test with Maschine software (controller detected; pads, LEDs, display functional)
