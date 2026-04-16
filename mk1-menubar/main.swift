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
        menu.addItem(NSMenuItem(title: "Quit", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    @objc private func startTapped()   { startService() }
    @objc private func stopTapped()    { stopService() }
    @objc private func restartTapped() {
        // Run restart off the main thread so the menu closes first
        DispatchQueue.global().async { restartService() }
    }
}

// MARK: - Entry point

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let delegate = AppDelegate()
app.delegate = delegate
app.run()
