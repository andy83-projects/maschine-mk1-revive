import AppKit

// MARK: - Service management

private let kLabel    = "com.dragco.mk1-bridge"
// Check system-wide /Library/LaunchAgents first (our installer puts it there),
// fall back to user-level ~/Library/LaunchAgents.
private let kPlist: String = {
    let system = "/Library/LaunchAgents/com.dragco.mk1-bridge.plist"
    let user   = NSHomeDirectory() + "/Library/LaunchAgents/com.dragco.mk1-bridge.plist"
    return FileManager.default.fileExists(atPath: system) ? system : user
}()
private let kUIDDomain = "gui/\(getuid())"

/// Returns true if the service is loaded AND has a live PID.
func serviceIsRunning() -> Bool {
    let pipe = Pipe()
    let errPipe = Pipe()
    let task = Process()
    task.executableURL = URL(fileURLWithPath: "/bin/launchctl")
    task.arguments = ["list", kLabel]
    task.standardOutput = pipe
    task.standardError = errPipe
    try? task.run()
    task.waitUntilExit()
    if task.terminationStatus != 0 { return false }
    let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    return out.contains("\"PID\"")
}

/// Returns true if the service plist is loaded (running or stopped).
func serviceIsLoaded() -> Bool {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: "/bin/launchctl")
    task.arguments = ["list", kLabel]
    task.standardOutput = Pipe()
    task.standardError = Pipe()
    try? task.run()
    task.waitUntilExit()
    return task.terminationStatus == 0
}

@discardableResult
func shell(_ args: String...) -> Int32 {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: "/bin/launchctl")
    task.arguments = Array(args)
    task.standardOutput = Pipe()
    task.standardError = Pipe()
    try? task.run()
    task.waitUntilExit()
    return task.terminationStatus
}

func startService() {
    if serviceIsLoaded() {
        shell("start", kLabel)
    } else {
        shell("bootstrap", kUIDDomain, kPlist)
    }
}

func stopService() {
    if serviceIsLoaded() {
        // bootout prevents KeepAlive from respawning
        shell("bootout", kUIDDomain, kPlist)
    }
}

func restartService() {
    stopService()
    // Brief pause so launchd fully releases the service before re-bootstrapping
    Thread.sleep(forTimeInterval: 0.6)
    startService()
}

// MARK: - Menu bar icon

func makeMenuBarIcon() -> NSImage {
    // Load the MK1 photo from the app bundle (menubar.png / menubar@2x.png)
    if let url = Bundle.main.url(forResource: "menubar", withExtension: "png"),
       let img = NSImage(contentsOf: url) {
        img.isTemplate = false  // show in full colour
        return img
    }
    // Fallback: SF Symbol
    if #available(macOS 11.0, *),
       let sym = NSImage(systemSymbolName: "square.grid.2x2.fill", accessibilityDescription: "MK1 Revive") {
        sym.isTemplate = true
        return sym
    }
    return NSImage()
}

// MARK: - Settings model

private struct SliderParam {
    let envKey: String
    let label: String
    let min: Int
    let max: Int
    let defaultVal: Int
    let unit: String
    let tooltip: String
}

private struct BoolParam {
    let envKey: String
    let label: String
    let defaultVal: Bool
    let tooltip: String
}

private struct StringParam {
    let envKey: String
    let label: String
    let placeholder: String
    let tooltip: String
}

private let kPadSliders: [SliderParam] = [
    SliderParam(envKey: "MK1_PAD_HIT_ON",        label: "Hit-on threshold",         min: 50,  max: 800, defaultVal: 300, unit: "ADC",
                tooltip: "Minimum pressure (0–4095) to trigger a pad strike. Raise to reduce crosstalk; lower to detect lighter taps."),
    SliderParam(envKey: "MK1_PAD_HIT_OFF",       label: "Hit-off threshold",        min: 25,  max: 400, defaultVal: 150, unit: "ADC",
                tooltip: "Pressure must fall below this to register a pad release (hysteresis). Keep below hit-on threshold."),
    SliderParam(envKey: "MK1_PAD_PRESSURE",      label: "Pressure update threshold", min: 25,  max: 400, defaultVal: 200, unit: "ADC",
                tooltip: "Minimum pressure change required to forward a pressure_update event. Reduces ADC jitter flooding at 700 Hz."),
    SliderParam(envKey: "MK1_PAD_DEBOUNCE_MS",   label: "Debounce window",           min: 1,   max: 50,  defaultVal: 10,  unit: "ms",
                tooltip: "Minimum time between hit-off and next hit-on for the same pad. Eliminates rubber-bounce double triggers."),
]

private let kDisplaySliders: [SliderParam] = [
    SliderParam(envKey: "MK1_DISPLAY_TICK_MS",   label: "Display refresh interval",  min: 8,   max: 100, defaultVal: 16,  unit: "ms",
                tooltip: "How often the bridge flushes pending display frames to the hardware (16 ms ≈ 60 fps)."),
]

private let kIntegrationBools: [BoolParam] = [
    BoolParam(envKey: "MK1_AUTO_OPEN_MASCHINE",  label: "Auto-launch Maschine when bridge starts", defaultVal: false,
              tooltip: "Bridge opens the Maschine application automatically at startup."),
    BoolParam(envKey: "MK1_AUTO_CLOSE_MASCHINE", label: "Auto-close Maschine when bridge stops",   defaultVal: false,
              tooltip: "Bridge quits the Maschine application when the bridge daemon exits."),
    BoolParam(envKey: "MK1_SUPPRESS_APPLE_PROJECT_DIM_CLUSTER", label: "Suppress Apple project dim cluster", defaultVal: false,
              tooltip: "Suppress the spurious LED-dim cluster sent by some Maschine versions on Apple Silicon."),
]

private let kIntegrationStrings: [StringParam] = [
    StringParam(envKey: "MK1_MASCHINE_EXEC", label: "Maschine executable path",
                placeholder: "(auto-detected — leave blank for default)",
                tooltip: "Full path to the Maschine 2 executable. Leave blank to use the default search path."),
]

private let kDiagBools: [BoolParam] = [
    BoolParam(envKey: "MK1_VERBOSE_IO",    label: "Verbose USB I/O logging",  defaultVal: false,
              tooltip: "Log every USB bulk transfer to /tmp/mk1-bridge.log. High volume — use only for debugging."),
    BoolParam(envKey: "MK1_TIMING_TRACE",  label: "Timing trace logging",     defaultVal: false,
              tooltip: "Log IPC and USB timing measurements. Used to diagnose latency."),
]

// MARK: - Plist helpers

/// Read EnvironmentVariables dict from the launchd plist. Returns [:] if absent or unreadable.
private func readEnvVarsFromPlist() -> [String: String] {
    guard let dict = NSDictionary(contentsOfFile: kPlist) as? [String: Any],
          let envVars = dict["EnvironmentVariables"] as? [String: String] else { return [:] }
    return envVars
}

/// Write a new EnvironmentVariables dict into the launchd plist.
/// Merges with any keys already present that we don't manage.
/// Returns nil on success, or an error string.
private func writeEnvVarsToPlist(_ newVars: [String: String]) -> String? {
    // 1. Read current plist
    guard var dict = NSMutableDictionary(contentsOfFile: kPlist) as? [String: Any] else {
        return "Cannot read plist at \(kPlist)"
    }

    // 2. Merge: start from existing env vars, overwrite with new ones
    var existing = (dict["EnvironmentVariables"] as? [String: String]) ?? [:]
    for (k, v) in newVars {
        if v.isEmpty {
            existing.removeValue(forKey: k)
        } else {
            existing[k] = v
        }
    }
    dict["EnvironmentVariables"] = existing

    // 3. Serialize to plist data
    guard let data = try? PropertyListSerialization.data(
        fromPropertyList: dict, format: .xml, options: 0) else {
        return "Failed to serialize plist"
    }

    // 4. Write — directly if user-writable, via osascript + admin auth for system path
    let isSystem = kPlist.hasPrefix("/Library/")
    if isSystem {
        // Write to temp file, then copy with admin privileges
        let tmp = NSTemporaryDirectory() + "mk1-bridge-settings.plist"
        do { try data.write(to: URL(fileURLWithPath: tmp)) }
        catch { return "Failed to write temp file: \(error)" }

        let escaped = kPlist.replacingOccurrences(of: "'", with: "'\\''")
        let tmpEscaped = tmp.replacingOccurrences(of: "'", with: "'\\''")
        let shellCmd = "cp '\(tmpEscaped)' '\(escaped)'"
        let script = "do shell script \"\(shellCmd.replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"

        var err: NSDictionary?
        NSAppleScript(source: script)?.executeAndReturnError(&err)
        if let err = err {
            return "Admin copy failed: \(err[NSAppleScript.errorMessage as NSString] ?? "unknown error")"
        }
        try? FileManager.default.removeItem(atPath: tmp)
    } else {
        do { try data.write(to: URL(fileURLWithPath: kPlist)) }
        catch { return "Failed to write plist: \(error)" }
    }
    return nil
}

// MARK: - Settings window controller

/// NSView subclass with flipped coordinate system so we can lay out top-to-bottom.
private final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

final class SettingsWindowController: NSObject, NSWindowDelegate {

    static let shared = SettingsWindowController()

    private var panel: NSPanel?
    private var sliderControls: [String: NSSlider]    = [:]
    private var sliderLabels:   [String: NSTextField]  = [:]
    private var checkControls:  [String: NSButton]     = [:]
    private var textControls:   [String: NSTextField]  = [:]

    func show() {
        if panel == nil { buildPanel() }
        loadCurrentValues()
        panel!.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: - Layout constants

    private let W:      CGFloat = 520   // content width
    private let margin: CGFloat = 20    // left/right inset
    private let labelW: CGFloat = 185   // label column width
    private let valueW: CGFloat = 72    // value readout width
    private let rowH:   CGFloat = 22    // control row height
    private let rowGap: CGFloat = 10    // gap between rows
    private let secGap: CGFloat = 18    // gap between sections

    // MARK: - Build

    private func buildPanel() {
        // --- build content in a flipped view so y=0 is the top ---
        let canvas = FlippedView()
        var y: CGFloat = margin

        addSectionHeader("Pad Sensitivity", to: canvas, y: &y)
        for p in kPadSliders    { addSliderRow(p, to: canvas, y: &y) }

        y += secGap
        addSectionHeader("Display", to: canvas, y: &y)
        for p in kDisplaySliders { addSliderRow(p, to: canvas, y: &y) }

        y += secGap
        addSectionHeader("Integration", to: canvas, y: &y)
        for p in kIntegrationBools   { addCheckRow(p, to: canvas, y: &y) }
        for p in kIntegrationStrings { addStringRow(p, to: canvas, y: &y) }

        y += secGap
        addSectionHeader("Diagnostics", to: canvas, y: &y)
        for p in kDiagBools { addCheckRow(p, to: canvas, y: &y) }

        y += secGap

        // button bar — built separately so it fills the full width
        let btnBar = makeButtonBar()
        btnBar.frame = NSRect(x: 0, y: y, width: W, height: 32)
        canvas.addSubview(btnBar)
        y += 32 + margin

        let totalH = y
        canvas.frame = NSRect(x: 0, y: 0, width: W, height: totalH)

        // --- wrap in scroll view in case window is resized small ---
        let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: W, height: min(totalH, 620)))
        scroll.documentView = canvas
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false

        let p = NSPanel(
            contentRect: scroll.frame,
            styleMask: [.titled, .closable, .resizable, .utilityWindow],
            backing: .buffered, defer: false)
        p.title = "MK1 Bridge — Settings"
        p.isFloatingPanel = false
        p.delegate = self
        p.contentView = scroll
        p.center()
        self.panel = p
    }

    // MARK: - Row adders

    private func addSectionHeader(_ title: String, to v: NSView, y: inout CGFloat) {
        let lbl = NSTextField(labelWithString: title)
        lbl.font = NSFont.boldSystemFont(ofSize: NSFont.systemFontSize)
        lbl.frame = NSRect(x: margin, y: y, width: W - margin * 2, height: 18)
        v.addSubview(lbl)

        // thin separator line underneath
        let sep = NSBox()
        sep.boxType = .separator
        sep.frame = NSRect(x: margin, y: y + 20, width: W - margin * 2, height: 1)
        v.addSubview(sep)

        y += 28
    }

    private func addSliderRow(_ param: SliderParam, to v: NSView, y: inout CGFloat) {
        let x = margin
        let sliderX = x + labelW + 8
        let sliderW = W - margin - labelW - 8 - 8 - valueW

        let lbl = NSTextField(labelWithString: param.label + ":")
        lbl.alignment = .left
        lbl.toolTip = param.tooltip
        lbl.frame = NSRect(x: x, y: y, width: labelW, height: rowH)
        v.addSubview(lbl)

        let slider = NSSlider(value: Double(param.defaultVal),
                              minValue: Double(param.min),
                              maxValue: Double(param.max),
                              target: self, action: #selector(sliderChanged(_:)))
        slider.identifier = NSUserInterfaceItemIdentifier(param.envKey)
        slider.toolTip = param.tooltip
        slider.isContinuous = true
        slider.frame = NSRect(x: sliderX, y: y, width: sliderW, height: rowH)
        v.addSubview(slider)
        sliderControls[param.envKey] = slider

        let valLbl = NSTextField(labelWithString: "\(param.defaultVal) \(param.unit)")
        valLbl.alignment = .left
        valLbl.font = NSFont.monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        valLbl.frame = NSRect(x: sliderX + sliderW + 8, y: y, width: valueW, height: rowH)
        v.addSubview(valLbl)
        sliderLabels[param.envKey] = valLbl

        y += rowH + rowGap
    }

    private func addCheckRow(_ param: BoolParam, to v: NSView, y: inout CGFloat) {
        let cb = NSButton(checkboxWithTitle: param.label, target: nil, action: nil)
        cb.state = param.defaultVal ? .on : .off
        cb.toolTip = param.tooltip
        cb.identifier = NSUserInterfaceItemIdentifier(param.envKey)
        cb.frame = NSRect(x: margin, y: y, width: W - margin * 2, height: rowH)
        v.addSubview(cb)
        checkControls[param.envKey] = cb
        y += rowH + rowGap
    }

    private func addStringRow(_ param: StringParam, to v: NSView, y: inout CGFloat) {
        let lbl = NSTextField(labelWithString: param.label + ":")
        lbl.alignment = .left
        lbl.toolTip = param.tooltip
        lbl.frame = NSRect(x: margin, y: y, width: labelW, height: rowH)
        v.addSubview(lbl)

        let field = NSTextField()
        field.placeholderString = param.placeholder
        field.toolTip = param.tooltip
        field.identifier = NSUserInterfaceItemIdentifier(param.envKey)
        let fieldX = margin + labelW + 8
        field.frame = NSRect(x: fieldX, y: y, width: W - fieldX - margin, height: rowH)
        v.addSubview(field)
        textControls[param.envKey] = field

        y += rowH + rowGap
    }

    private func makeButtonBar() -> NSView {
        let bar = NSView()

        func btn(_ title: String, action: Selector, key: String = "") -> NSButton {
            let b = NSButton(title: title, target: self, action: action)
            b.bezelStyle = .rounded
            b.keyEquivalent = key
            b.sizeToFit()
            return b
        }

        let reset        = btn("Reset to Defaults", action: #selector(resetTapped))
        let cancel       = btn("Cancel",            action: #selector(cancelTapped),        key: "\u{1b}")
        let apply        = btn("Apply",             action: #selector(applyTapped))
        let applyRestart = btn("Apply & Restart",   action: #selector(applyAndRestartTapped), key: "\r")

        // Right-align: Apply & Restart | Apply | Cancel, Reset on left
        // Layout right-to-left
        var rx = W - margin
        for b in [applyRestart, apply, cancel] {
            rx -= b.frame.width
            b.frame.origin = NSPoint(x: rx, y: 0)
            rx -= 8
            bar.addSubview(b)
        }
        reset.frame.origin = NSPoint(x: margin, y: 0)
        bar.addSubview(reset)

        return bar
    }

    // MARK: Load / Save

    private func loadCurrentValues() {
        let env = readEnvVarsFromPlist()

        for p in kPadSliders + kDisplaySliders {
            let intVal = env[p.envKey].flatMap(Int.init) ?? p.defaultVal
            sliderControls[p.envKey]?.integerValue = intVal
            sliderLabels[p.envKey]?.stringValue = "\(intVal) \(p.unit)"
        }

        for p in kIntegrationBools + kDiagBools {
            let isSet = env[p.envKey].map { $0 != "0" && !$0.isEmpty } ?? p.defaultVal
            checkControls[p.envKey]?.state = isSet ? .on : .off
        }

        for p in kIntegrationStrings {
            textControls[p.envKey]?.stringValue = env[p.envKey] ?? ""
        }
    }

    private func collectEnvVars() -> [String: String] {
        var vars: [String: String] = [:]

        for p in kPadSliders + kDisplaySliders {
            if let s = sliderControls[p.envKey] {
                vars[p.envKey] = "\(s.integerValue)"
            }
        }

        for p in kIntegrationBools + kDiagBools {
            if let cb = checkControls[p.envKey] {
                // Omit (delete) if matching the default to keep plist clean
                let on = cb.state == .on
                if on == p.defaultVal && !on {
                    vars[p.envKey] = ""   // empty → remove from plist
                } else {
                    vars[p.envKey] = on ? "1" : "0"
                }
            }
        }

        for p in kIntegrationStrings {
            vars[p.envKey] = textControls[p.envKey]?.stringValue.trimmingCharacters(in: .whitespaces) ?? ""
        }

        return vars
    }

    private func applySettings() -> Bool {
        let vars = collectEnvVars()
        if let err = writeEnvVarsToPlist(vars) {
            let alert = NSAlert()
            alert.messageText = "Failed to save settings"
            alert.informativeText = err
            alert.alertStyle = .critical
            alert.runModal()
            return false
        }
        return true
    }

    // MARK: Actions

    @objc private func sliderChanged(_ sender: NSSlider) {
        guard let key = sender.identifier?.rawValue else { return }
        // Find matching param for unit
        let all = kPadSliders + kDisplaySliders
        if let p = all.first(where: { $0.envKey == key }) {
            sliderLabels[key]?.stringValue = "\(sender.integerValue) \(p.unit)"
        }
    }

    @objc private func resetTapped() {
        for p in kPadSliders + kDisplaySliders {
            sliderControls[p.envKey]?.integerValue = p.defaultVal
            sliderLabels[p.envKey]?.stringValue = "\(p.defaultVal) \(p.unit)"
        }
        for p in kIntegrationBools + kDiagBools {
            checkControls[p.envKey]?.state = p.defaultVal ? .on : .off
        }
        for p in kIntegrationStrings {
            textControls[p.envKey]?.stringValue = ""
        }
    }

    @objc private func cancelTapped() {
        panel?.orderOut(nil)
    }

    @objc private func applyTapped() {
        if applySettings() {
            panel?.orderOut(nil)
            if serviceIsRunning() {
                let alert = NSAlert()
                alert.messageText = "Settings saved"
                alert.informativeText = "Restart the bridge (Restart in menu bar) to apply the new values."
                alert.alertStyle = .informational
                alert.addButton(withTitle: "OK")
                alert.addButton(withTitle: "Restart Now")
                if alert.runModal() == .alertSecondButtonReturn {
                    DispatchQueue.global().async { restartService() }
                }
            }
        }
    }

    @objc private func applyAndRestartTapped() {
        if applySettings() {
            panel?.orderOut(nil)
            DispatchQueue.global().async { restartService() }
        }
    }

    // MARK: NSWindowDelegate

    func windowWillClose(_ notification: Notification) {
        // nothing — keep controller alive for next show()
    }
}

// MARK: - App delegate

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private var menu: NSMenu!

    func applicationDidFinishLaunching(_ notification: Notification) {
        // LSUIElement=true in Info.plist handles accessory mode — no setActivationPolicy needed
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let btn = statusItem.button {
            btn.image = makeMenuBarIcon()
            btn.imageScaling = .scaleProportionallyDown
            btn.toolTip = "MK1 Revive"
        }

        menu = NSMenu()
        menu.autoenablesItems = false
        menu.delegate = self
        statusItem.menu = menu
    }

    // Rebuild the menu fresh each time it opens so status is always current.
    func menuWillOpen(_ menu: NSMenu) {
        rebuildMenu(running: serviceIsRunning())
    }

    private func rebuildMenu(running: Bool) {
        menu.removeAllItems()

        // Status indicator (non-interactive)
        let statusLabel = NSMenuItem(title: running ? "● Running" : "○ Stopped", action: nil, keyEquivalent: "")
        statusLabel.isEnabled = false
        if running {
            statusLabel.attributedTitle = NSAttributedString(
                string: "● Running",
                attributes: [.foregroundColor: NSColor.systemGreen]
            )
        }
        menu.addItem(statusLabel)
        menu.addItem(.separator())

        // Start — disabled when running
        let start = NSMenuItem(title: "Start", action: #selector(startTapped), keyEquivalent: "")
        start.target = self
        start.isEnabled = !running
        menu.addItem(start)

        // Stop — disabled when stopped
        let stop = NSMenuItem(title: "Stop", action: #selector(stopTapped), keyEquivalent: "")
        stop.target = self
        stop.isEnabled = running
        menu.addItem(stop)

        // Restart — always available
        let restart = NSMenuItem(title: "Restart", action: #selector(restartTapped), keyEquivalent: "r")
        restart.target = self
        menu.addItem(restart)

        menu.addItem(.separator())

        // Settings panel
        let settings = NSMenuItem(title: "Settings\u{2026}", action: #selector(settingsTapped), keyEquivalent: ",")
        settings.target = self
        menu.addItem(settings)

        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    @objc private func startTapped()    { startService() }
    @objc private func stopTapped()     { stopService() }
    @objc private func restartTapped()  {
        // Run restart off the main thread so the menu closes first
        DispatchQueue.global().async { restartService() }
    }
    @objc private func settingsTapped() { SettingsWindowController.shared.show() }
}

// MARK: - Entry point

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
