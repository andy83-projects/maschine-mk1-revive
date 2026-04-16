# Handoff — 2026-04-16 — MK1 Revive Menu Bar App

## What was built

A native macOS menu bar app (`MK1 Revive.app`) that lets you start, stop, and
restart the `mk1-bridge` launchd service without touching the terminal.

---

## Files added

| Path | Purpose |
|------|---------|
| `mk1-menubar/main.swift` | Full app source (AppKit, single file) |
| `mk1-menubar/Info.plist` | Bundle metadata; `LSUIElement=true` hides Dock icon |
| `mk1-menubar/build.sh` | Compile + package + ad-hoc sign the `.app` bundle |
| `mk1-menubar/AppIcon.icns` | App icon for Finder/Login Items (all sizes) |
| `mk1-menubar/menubar.png` | Menu bar icon 18×18 @1x (derived from repo photo) |
| `mk1-menubar/menubar@2x.png` | Menu bar icon 18×18 @2x Retina |
| `mk1-menubar/AppIcon.iconset/` | Source iconset used to generate AppIcon.icns |

Source image: `znilcjyseuwhvfgcws5p.jpg` (400×400 MK1 product photo already in repo).

---

## Architecture

Single-file Swift app using AppKit directly (no SwiftUI, no Xcode project needed).

```
NSApplication (accessory policy — no Dock icon)
  └── AppDelegate
        ├── NSStatusItem  ← menu bar button with MK1 photo icon
        └── NSMenu (autoenablesItems=false)
              ├── ● Running / ○ Stopped  (non-interactive status line)
              ├── Start   (disabled when running)
              ├── Stop    (disabled when stopped)
              ├── Restart (always enabled)
              └── Quit
```

The menu is rebuilt from scratch on every `menuWillOpen` so state is always current.

---

## Service control implementation

| Action | launchctl command |
|--------|-------------------|
| Start (not loaded) | `launchctl bootstrap gui/<uid> <plist>` |
| Start (loaded, stopped) | `launchctl start com.dragco.mk1-bridge` |
| Stop | `launchctl bootout gui/<uid> <plist>` |
| Restart | `bootout` + 0.6s sleep + `bootstrap` (off main thread) |
| Status check | `launchctl list com.dragco.mk1-bridge` — running = output contains `"PID"` |

`Stop` uses `bootout` (not `stop`) so that `KeepAlive=true` in the plist does **not**
respawn the process.

### Plist location discovery

The plist is at `/Library/LaunchAgents/com.dragco.mk1-bridge.plist` (system-wide,
installed by our packaging scripts). The code auto-detects this:

```swift
let system = "/Library/LaunchAgents/com.dragco.mk1-bridge.plist"
let user   = NSHomeDirectory() + "/Library/LaunchAgents/com.dragco.mk1-bridge.plist"
return FileManager.default.fileExists(atPath: system) ? system : user
```

**Root cause of original stop/start failure**: the code was defaulting to `~/Library/LaunchAgents/`
which doesn't exist; `bootout` silently returned error 5.

---

## Key lessons learned (gotchas for next session)

### 1. `setActivationPolicy(.accessory)` must be called BEFORE `app.run()`
Calling it inside `applicationDidFinishLaunching` causes repeated
"Dropping transition context because the scene is reconnecting" log spam and the
status item never appears in the menu bar. The correct pattern:

```swift
let app = NSApplication.shared
app.setActivationPolicy(.accessory)   // ← BEFORE app.run()
let delegate = AppDelegate()
app.delegate = delegate
app.run()
```

`LSUIElement=true` in Info.plist alone is NOT sufficient when building/running
outside of Xcode — the `setActivationPolicy` call in the entry point is required.

### 2. `NSMenu.autoenablesItems` defaults to `true`
AppKit auto-enables menu items based on whether the target responds to the action
selector. This overrides any `item.isEnabled = false` you set. Always set:

```swift
menu.autoenablesItems = false
```

### 3. `sips` requires explicit format flag for JPEG→PNG conversion
```sh
sips -s format png -z 18 18 source.jpg --out output.png
```
Without `-s format png`, sips writes JPEG bytes into the `.png` file and `iconutil` fails.

---

## How to build

```sh
bash mk1-menubar/build.sh
```

Output: `build/MK1 Revive.app`

### To install / update

```sh
cp -r "build/MK1 Revive.app" /Applications/
xattr -cr "/Applications/MK1 Revive.app"   # strip OneDrive file-provider xattrs
killall mk1-menubar 2>/dev/null
open "/Applications/MK1 Revive.app"
```

### To auto-launch at login

System Settings → General → Login Items → add `MK1 Revive`

---

## Managing the service from the terminal

```sh
# Check status
launchctl list com.dragco.mk1-bridge

# Stop (prevents KeepAlive respawn)
launchctl bootout gui/$(id -u) /Library/LaunchAgents/com.dragco.mk1-bridge.plist

# Start
launchctl bootstrap gui/$(id -u) /Library/LaunchAgents/com.dragco.mk1-bridge.plist

# Logs
tail -f /tmp/mk1-bridge.log
```
