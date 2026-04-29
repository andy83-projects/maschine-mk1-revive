# maschine-mk1-revive

Userspace driver for the Native Instruments Maschine MK1 on Apple Silicon macOS — no kext, no DriverKit, no special entitlements.

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-andy83-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/andy83)

## What works

- Maschine software detects the MK1 in its controller list
- Both displays render correctly (170×64 grayscale, ST7529)
- All LEDs: pad rubber, group A–H, transport, scene-row, screen area (SA1–SA8)
- Pad input with velocity and pressure; sustain gate eliminates stuck notes
- All buttons and transport controls
- All 11 encoders (Volume, Tempo, Swing + 8 screen area), with jitter and crosstalk filtering
- Physical DIN MIDI Out (hardware-verified); DIN MIDI In (untested)
- USB hot-plug: survives unplug/replug; starts before device is connected
- **MK1 Revive** menu bar app: start/stop/restart the bridge, adjust settings

## Install

Download the latest `.pkg` from [Releases](https://github.com/andy83-projects/maschine-mk1-revive/releases). The installer places:

- **MK1 Revive** app (includes `mk1-bridge`) → `/Applications/MK1 Revive.app`
- LaunchAgent → `~/Library/LaunchAgents/com.dragco.mk1-bridge.plist`

The bridge starts automatically. Open **MK1 Revive** from `/Applications` to manage the service and adjust settings.

### Unsigned package

macOS may warn that the package is from an unidentified developer.

- **Blocked at install:** right-click the `.pkg` → Open, or go to `System Settings → Privacy & Security` and allow it.
- **Quarantined binary after install:** clear quarantine and restart the agent:

```bash
sudo xattr -dr com.apple.quarantine "/Applications/MK1 Revive.app"
launchctl bootout "gui/$(id -u)" ~/Library/LaunchAgents/com.dragco.mk1-bridge.plist 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" ~/Library/LaunchAgents/com.dragco.mk1-bridge.plist
```

### Uninstall

Run the matching uninstaller package, or manually:

```bash
launchctl bootout "gui/$(id -u)" ~/Library/LaunchAgents/com.dragco.mk1-bridge.plist 2>/dev/null || true
rm -f ~/Library/LaunchAgents/com.dragco.mk1-bridge.plist
sudo rm -rf "/Applications/MK1 Revive.app"
sudo pkgutil --forget com.dragco.mk1-bridge
```

## Configuration

Use the **Settings…** panel in the MK1 Revive menu bar app, or set environment variables directly in the launchd plist.

### Pad sensitivity

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_PAD_HIT_ON` | `300` | ADC pressure (0–4095) to trigger a pad strike |
| `MK1_PAD_HIT_OFF` | `150` | ADC pressure must fall below this to register release |
| `MK1_PAD_PRESSURE` | `200` | Minimum change to forward a pressure-update event |
| `MK1_PAD_DEBOUNCE_MS` | `10` | Milliseconds between hit-off and next hit-on |
| `MK1_PAD_SUSTAIN` | `3` | Consecutive above-threshold reports required before hit_on fires |

### Encoder

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_ENCODER_SENSITIVITY` | `10` | Step multiplier in tenths (10 = ×1.0, 5 = ×0.5, 20 = ×2.0) |
| `MK1_ENCODER_MIN_DELTA` | `2` | Minimum byte-delta per report to register a step (filters jitter) |

### Display & integration

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_DISPLAY_TICK_MS` | `16` | Display flush interval in ms (16 ≈ 60 fps) |
| `MK1_AUTO_OPEN_MASCHINE` | off | Auto-launch Maschine when bridge starts |
| `MK1_AUTO_CLOSE_MASCHINE` | off | Auto-quit Maschine when bridge stops |

### Diagnostics

| Env var | Default | Description |
|---------|---------|-------------|
| `MK1_VERBOSE_IO` | off | Log every USB bulk transfer |
| `MK1_TIMING_TRACE` | off | Log IPC and USB timing measurements |

## Build

Requires Xcode 15+ on Apple Silicon macOS 14+.

```bash
xcodebuild -project maschine-mk1-revive.xcodeproj -scheme mk1-bridge -configuration Debug build
bash mk1-menubar/build.sh   # builds MK1 Revive.app → build/MK1 Revive.app
```

Run directly for development:

```bash
./build/Debug/mk1-bridge [--display_tick_ms=16] [--no-partial-display] [--help]
```

## How it works

The MK1 originally required `NIUSBMaschineController.kext` — a kernel extension that never received an ARM64 build. This project replaces it entirely in userspace.

`mk1-bridge` kills the real `NIHardwareAgent` and impersonates it: it registers the `NIHWMainHandler` CFMessagePort itself, performs the full IPC handshake with Maschine software, and owns direct IOKit USB bulk transfers to the hardware.

```
[Maschine.app] ←─CFMessagePort─→ [mk1-bridge] ←─IOKit USB─→ [MK1 hardware]
```

### Components

| Target | Type | Purpose |
|--------|------|---------|
| `mk1-usb` | static lib | Claim USB device, bulk transfers |
| `mk1-ipc` | static lib | CFMessagePort handshake + NI IPC protocol |
| `mk1-bridge` | daemon | Glues USB ↔ IPC, runs as launchd agent |
| `mk1-menubar` | app bundle | Menu bar app: service control + settings |
| `mk1-shim` | dylib | DYLD_INSERT_LIBRARIES shim for debugging |

## Prior art

- **[biappi/Macchina](https://github.com/biappi/Macchina)** — Reverse engineered the CFMessagePort handshake between NIHardwareAgent and Maschine software (MK1, 2012).
- **[SamL98/NIProtocol](https://github.com/SamL98/NIProtocol)** — Full MK2 handshake; `NIHWMainHandler` port name; complete client + server in C. ([article](https://lerner98.medium.com/rage-against-the-maschine-3357be1abc48))
- **[terminar/rebellion](https://github.com/terminar/rebellion)** — Most complete IPC implementation (MK3/KK); key insight that USB and IPC data share the same format.
- **[fzero/maschine-mk1](https://github.com/fzero/maschine-mk1)** — MK1 HID protocol on Linux; confirms VID `0x17CC` / PID `0x0808`.
- **[hansfbaier/open-maschine](https://github.com/hansfbaier/open-maschine)** — MK2 proof-of-concept using hidapi.

## Changelog

### v0.3.3 — 2026-04-24
- Fix invisible tractor beam in idle animation — grey pixel values render as black on this display; replaced with white alternating scanlines

### v0.3.2 — 2026-04-23
- UFO tractor beam idle animation: small cat stands at bottom of left display while a UFO sweeps back and forth, homes in, fires a tractor beam, sucks the cat up, flies around, and returns the cat to the ground (~26.8 s loop)

### v0.3.1 — 2026-04-20
- Fix rogue pad hits from scan table reports leaking into the pad pressure classifier
- Fix orphaned hit-off events caused by `was_active` tracking pressure state instead of sent-event state
- Improve scan table classifier: compare top nibbles only, require both ADC channels to agree

### v0.3.0 — 2026-04-17
- Fix pad stuck notes and bounce: sustain gate, reliable hit_off retry, hit_on conditional on IPC success
- Fix encoder direction reversal; filter single-count jitter; suppress electrical crosstalk between adjacent encoders

### v0.2.1 — 2026-04-16
- Move logs to `/tmp/maschine-mk1-revive/`; cap at 10 MiB with wraparound

### v0.2.0 — 2026-04-14
- MK1 Revive menu bar app: service management and settings panel
- Runtime-configurable pad thresholds and encoder sensitivity via env vars
