# maschine-mk1-revive

A userspace driver shim to make the Native Instruments Maschine MK1 work on Apple Silicon macOS,
with the original Maschine software.

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-andy83-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/andy83)

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

Working as of 2026-04-20:
- Maschine software detects the MK1 in its controller list
- Both displays render correctly (ST7529, EP8 bulk, 170×64 grayscale)
- Status screen ("Open Maschine") shown on both displays at bridge start and when Maschine exits
- All button and transport LEDs confirmed working: pad-row (Scene/Mute/Solo/Select/Duplicate/Navigate/Pad Mode/Pattern), transport (Play/Record/Restart/Erase/Grid/Shift/TransportLeft/TransportRight), screen area (SA1–SA8), Browse, Note Repeat, Snap, Control, Step, Modules Left/Right, Auto Write, Group A–H
- Pad rubber LEDs light correctly per active group and pad hits
- Pads register velocity and pressure (EP4 64-byte reports, 12-bit ADC, IPC forwarded)
  - Pressure updates throttled to ≥5% change threshold to prevent IPC flooding at 700Hz
- Group, transport, and screen buttons registered (EP1 short reports)
- All 11 encoders forwarded: Volume, Tempo, Swing (Master Section) + 8 screen area encoders
- USB hot-plug: bridge can start before device is connected; device can be unplugged and re-plugged
- Physical DIN MIDI Out transport: basic CoreMIDI-to-DIN path working
- Physical DIN MIDI In transport: not yet hardware-verified
- Menu Bar applet to control service - Start, Stop, Restart
- Applet to control user parameters

### Components

| Target | Type | Purpose |
|--------|------|---------|
| `mk1-usb` | static lib | Claim USB device, read/write HID reports |
| `mk1-ipc` | static lib | CFMessagePort handshake + NI IPC protocol |
| `mk1-bridge` | daemon | Glues USB ↔ IPC, runs as launchd agent |
| `mk1-menubar` | app bundle | Menu bar app: start/stop/restart service + settings panel |
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

## DIN MIDI Ports

The MK1 hardware includes physical DIN MIDI In/Out ports, but the Apple Silicon
bridge does not yet treat them as fully validated standard MIDI ports.

Current status:

- virtual CoreMIDI ports are exposed as `Maschine MK1 DIN In` and `Maschine MK1 DIN Out`
- DIN MIDI Out has been hardware-verified at a basic level: DAW MIDI clock sent to
  `Maschine MK1 DIN Out` reached an external device connected to the MK1 physical DIN Out
- `cabl` strongly suggests outbound DIN MIDI uses a vendor packet beginning with `0x07`
- inbound DIN MIDI likely arrives on the EP1 path as packet type `0x06`
- DIN MIDI In has not yet been validated on hardware

DIN MIDI Out is currently the more trustworthy path. DIN MIDI In should still be
considered experimental until it is tested with a real device driving the MK1 DIN In port.

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

The menu bar app is built separately:

```bash
bash mk1-menubar/build.sh
# Output: build/MK1 Revive.app
```

## Running

The bridge runs as a launchd agent. Use the **MK1 Revive** menu bar app (installed to
`/Applications/MK1 Revive.app`) to start, stop, and restart the service, and to adjust
settings without touching the terminal.

To start manually:

```bash
launchctl bootstrap "gui/$(id -u)" /Library/LaunchAgents/com.dragco.mk1-bridge.plist
```

Or run the bridge directly for development:

```bash
./build/Debug/mk1-bridge
```

Useful flags:

```bash
./build/Debug/mk1-bridge --display_tick_ms=16
./build/Debug/mk1-bridge --no-partial-display
./build/Debug/mk1-bridge --help
```

## Configuration

All tunable parameters can be set via the **Settings…** panel in the MK1 Revive menu bar app,
or by setting environment variables in the launchd plist's `EnvironmentVariables` key.

### Pad sensitivity

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_PAD_HIT_ON` | `300` | Minimum ADC pressure (0–4095) to trigger a pad strike |
| `MK1_PAD_HIT_OFF` | `150` | ADC pressure must fall below this to register release (hysteresis) |
| `MK1_PAD_PRESSURE` | `200` | Minimum change to forward a pressure-update event (reduces 700Hz flooding) |
| `MK1_PAD_DEBOUNCE_MS` | `10` | Milliseconds between hit-off and next hit-on (prevents rubber bounce) |

### Display

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_DISPLAY_TICK_MS` | `16` | Display flush timer interval in ms (16 ≈ 60 fps) |

### Integration

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_AUTO_OPEN_MASCHINE` | off | Auto-launch Maschine when bridge starts |
| `MK1_AUTO_CLOSE_MASCHINE` | off | Auto-quit Maschine when bridge stops |
| `MK1_MASCHINE_EXEC` | auto | Full path to Maschine 2 executable |
| `MK1_SUPPRESS_APPLE_PROJECT_DIM_CLUSTER` | off | Suppress spurious LED-dim cluster on Apple Silicon |

### Diagnostics

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_VERBOSE_IO` | off | Log every USB bulk transfer to `/tmp/mk1-bridge.log` |
| `MK1_TIMING_TRACE` | off | Log IPC and USB timing measurements |

## Install

The installer package installs:

- `mk1-bridge` daemon → `/usr/local/bin/mk1-bridge`
- LaunchAgent plist → `/Library/LaunchAgents/com.dragco.mk1-bridge.plist`
- **MK1 Revive** menu bar app → `/Applications/MK1 Revive.app`

A matching uninstaller package is also provided.

### After install

The bridge starts automatically via launchd. Open `/Applications/MK1 Revive.app` to manage
the service and configure settings. To auto-launch the menu bar app at login, add it via:

```
System Settings → General → Login Items → add MK1 Revive
```

### Unsigned Package Install

The current GitHub package release is unsigned. macOS may warn that the package
or installed binary is from an unidentified developer.

If the package opens normally, install it. The LaunchAgent starts automatically.

If macOS blocks the package or binary, use one of these paths:

1. In Finder, right-click the `.pkg` and choose `Open`.
2. If macOS still blocks it, open `System Settings` → `Privacy & Security` and
   allow the blocked package or binary to run.
3. If the installed binary is quarantined, clear quarantine manually and restart
   the LaunchAgent:

```bash
sudo xattr -dr com.apple.quarantine /usr/local/bin/mk1-bridge
sudo xattr -dr com.apple.quarantine "/Applications/MK1 Revive.app"
launchctl bootout "gui/$(id -u)" /Library/LaunchAgents/com.dragco.mk1-bridge.plist 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" /Library/LaunchAgents/com.dragco.mk1-bridge.plist
```

### Manual Install

```bash
sudo install -m 755 ./build/Release/mk1-bridge /usr/local/bin/mk1-bridge
sudo install -m 644 ./mk1-bridge/com.dragco.mk1-bridge.plist /Library/LaunchAgents/com.dragco.mk1-bridge.plist
sudo cp -r "build/MK1 Revive.app" /Applications/
sudo xattr -cr "/Applications/MK1 Revive.app"
launchctl bootstrap "gui/$(id -u)" /Library/LaunchAgents/com.dragco.mk1-bridge.plist
open "/Applications/MK1 Revive.app"
```

## Uninstall

Run the matching uninstaller package, or manually:

```bash
launchctl bootout "gui/$(id -u)" /Library/LaunchAgents/com.dragco.mk1-bridge.plist 2>/dev/null || true
sudo rm -f /Library/LaunchAgents/com.dragco.mk1-bridge.plist
sudo rm -f /usr/local/bin/mk1-bridge
sudo rm -rf "/Applications/MK1 Revive.app"
sudo pkgutil --forget com.dragco.mk1-bridge
```

## Status

- [x] Directory structure and project scaffold
- [x] Intel Mac data collection (pcap, Frida trace, Ghidra kext RE)
- [x] USB device claim (IOKit direct, not HID)
- [x] IPC handshake (full NIHA impersonation including Serial Connect phase)
- [x] Bridge daemon (`mk1-bridge`) — Maschine detects MK1 in controller list
- [x] launchd agent plist (`com.dragco.mk1-bridge.plist`)
- [x] MK1 Revive menu bar app — start/stop/restart service, settings panel
- [x] Display init (EP8, ST7529 17-command sequence)
- [x] LCD display pixel updates — full framebuffer composite + RAMWR; display renders correctly
- [x] LED forwarding — all button/group/transport/pad-row/SA/pad-rubber LEDs confirmed working
- [x] Pad input events — EP4 64-byte reports decoded; pressure/hit-on/off forwarded; rogue hit bugs fixed
- [x] Pad sensitivity tunable at runtime via env vars or Settings panel (no recompile needed)
- [x] Button input events — EP1 short reports decoded; group/transport/screen buttons forwarded
- [x] Display backlight stays on — no longer toggles on button presses (resolved)
- [x] Status screen — "Open Maschine" shown on both displays at start and when Maschine exits
- [x] Master Section knobs — Volume, Tempo, Swing encoder events forwarded via IPC
- [x] Screen area encoders — all 8 encoders mapped
- [x] USB hot-plug — bridge survives device unplug/replug; starts before device is connected
- [~] Bridge reconnect — DEVICE_OFF triggers Maschine re-handshake but Maschine can take 5-30 seconds to reconnect
- [ ] First-launch input miss — after a fresh Maschine launch, the first button or pad press is ignored
- [~ ] Apple Silicon project-load LED anomaly — some projects still assert errant dim group LEDs on load
- [x] Physical DIN MIDI Out — basic CoreMIDI bridge working and hardware-verified with MIDI clock
- [ ] Physical DIN MIDI In — packet path guessed from vendor USB traffic but not yet hardware-verified
- [x] Menubar applet

## LED Mapping Workflow

For Apple Silicon project-state LED mapping, prefer running the bridge with:

```bash
MK1_PROJECT_CAPTURE=1 ./mk1-bridge
```

This opens an interactive capture mode on `/dev/tty` and logs `PROJCAP` entries to
`build/Debug/bridge-logs/led.log`. Each capture records:

- logical-slot diffs from the raw `NI_CMD_LED` payload
- remapped `phys[...]` byte diffs from the current Packet B / `second_block` LED path

Recommended workflow:

1. Load a project first and wait for the initial LED baseline.
2. Use low-fanout controls that should only affect a small, local LED cluster.
3. Prefer controls that flow through the captured Packet B path: `Group A`–`Group H`,
   `Auto Write`, `Snap`, `Modules Left/Right`, `Sampling`, `Browse`, `Control`, `Step`,
   `SA1`–`SA8`, `Note Repeat`.
4. Avoid scene-row / Packet A controls: `Mute`, `Solo`, `Select`, `Duplicate`, `Navigate`,
   `Pad Mode`, `Pattern`, `Scene`, `Shift`, `Erase`, `Grid`, `Record`, `Play`, `TransportRight`.
5. Avoid transport buttons and mode switches known to trigger broad LED refreshes.
6. For direct physical probing, `MK1_LED_PROBE=1` supports `phys[0..32]`.

## Changelog

### v0.3.1 — 2026-04-20
- **Fix rogue pad hits** — EP4 scan table reports at non-zero ring phase (phases 1–15) were
  escaping the classifier and being processed as pad pressure, triggering phantom strikes on
  arbitrary pads. Added a phase=0 guard: pressure reports are physically constrained to phase 0
  (pad 0 rests at 0x0000; max pressure 4095 keeps the top nibble at 0), so any report with
  phase ≠ 0 is discarded as a misidentified scan table.
- **Fix orphaned hit-off events** — `was_active` was derived from `g_prev_pressure ≥ hit_off_threshold`,
  which could become true without a `hit_on` ever being sent (pressure drifting into the 150–299
  hysteresis band). Changed to `g_sent_pressure[idx] > 0` so `was_active` reflects only whether
  a `hit_on` was actually dispatched.
- **Improve scan table classifier** — relaxed the ring-pattern check to compare top nibbles only,
  with an additional guard that both ADC channels agree on the lower 12 bits. This catches
  button-state-encoded scan table entries that previously leaked through.

### v0.2.1 — 2026-04-16
- Move bridge/runtime logs under `/tmp/maschine-mk1-revive/`
- Cap bridge, encoder, and timing logs at 10 MiB with wraparound
- Route bridge stderr-style diagnostics through the capped bridge log

### v0.2.0 — 2026-04-14
- Menu bar app (MK1 Revive) for service management and settings
- Runtime-configurable pad thresholds via env vars and Settings panel
- Encoder sensitivity multiplier control
