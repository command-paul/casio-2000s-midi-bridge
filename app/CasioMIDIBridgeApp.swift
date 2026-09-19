// CasioMIDIBridgeApp.swift — the macOS app front end for casio-midi-bridge.
// One window, quitting (or closing the window) disconnects the keyboard. MIT license.
import SwiftUI
import AppKit
import ServiceManagement

let agentLabel = "com.github.casio-midi-bridge"

@main
struct CasioMIDIBridgeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = BridgeModel()

    var body: some Scene {
        Window("Casio MIDI Bridge", id: "main") {
            ContentView().environmentObject(model)
        }
        .windowResizability(.contentSize)
        .commands { CommandGroup(replacing: .newItem) {} }

        // Menu bar icon: filled keys while a keyboard is connected, outline otherwise.
        MenuBarExtra {
            MenuBarContent().environmentObject(model)
        } label: {
            Image(systemName: model.stats.connected != 0 ? "pianokeys.inverse" : "pianokeys")
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    // Closing the window keeps the bridge alive in the menu bar; Quit disconnects.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) { bridge_stop() }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // `--snapshot <file.png>`: wait for the keyboard, render the window to a PNG, quit.
        // Used by `make screenshot` for the README; needs no screen-recording permission
        // because an app may always draw its own views.
        if let path = snapshotPath() {
            NSApp.activate(ignoringOtherApps: true)
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.5) {
                if let window = NSApp.windows.first(where: { $0.isVisible && $0.title == "Casio MIDI Bridge" }),
                   let content = window.contentView {
                    let view = content.superview ?? content          // superview includes the title bar
                    if let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                        view.cacheDisplay(in: view.bounds, to: rep)
                        if let png = rep.representation(using: .png, properties: [:]) {
                            try? png.write(to: URL(fileURLWithPath: path))
                            print("wrote \(path)")
                        }
                    }
                }
                NSApp.terminate(nil)
            }
        }
    }
}

func snapshotPath() -> String? {
    let args = CommandLine.arguments
    guard let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count else { return nil }
    return args[i + 1]
}

struct MenuBarContent: View {
    @EnvironmentObject var model: BridgeModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        let s = model.stats
        let connected = s.connected != 0
        Text(connected ? "Keyboard connected" : (s.busy != 0 ? "Keyboard in use by another program" : "Waiting for keyboard"))
        if connected {
            Text("Port: \(cString(s.port_name))")
            Text("\(s.msgs_in) received · \(s.msgs_out) sent")
        }
        Divider()
        Button("Show Window") {
            openWindow(id: "main")
            NSApp.activate(ignoringOtherApps: true)
        }
        Button("Play Test Notes on Keyboard") { model.playTestNotes() }.disabled(!connected)
        Divider()
        Button("Quit Casio MIDI Bridge") { NSApp.terminate(nil) }
    }
}

// MARK: - Model

final class BridgeModel: ObservableObject {
    @Published var stats = bridge_stats()
    @Published var agentRunning = false
    @Published var launchAtLogin = false
    @Published var portName: String
    private var timer: Timer?
    private var ticks = 0

    init() {
        portName = UserDefaults.standard.string(forKey: "portName") ?? "Casio USB MIDI"
        launchAtLogin = SMAppService.mainApp.status == .enabled
        bridge_start(portName, -1, -1, 0)
        checkAgent()
        let t = Timer(timeInterval: 0.2, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        if snapshotPath() != nil {   // give the screenshot some activity to show
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in self?.playTestNotes() }
        }
    }

    func refresh() {
        var s = bridge_stats()
        bridge_get_stats(&s)
        stats = s
        ticks += 1
        if ticks % 10 == 0 { checkAgent() }
    }

    func rename(to name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed != portName else { return }
        portName = trimmed
        UserDefaults.standard.set(trimmed, forKey: "portName")
        bridge_stop()
        bridge_start(trimmed, -1, -1, 0)
    }

    func checkAgent() {
        agentRunning = launchctl(["print", "gui/\(getuid())/\(agentLabel)"]) == 0
    }

    /// Stops and removes the headless LaunchAgent installed by `make install`.
    func stopAgent() {
        _ = launchctl(["bootout", "gui/\(getuid())/\(agentLabel)"])
        let plist = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/\(agentLabel).plist")
        try? FileManager.default.removeItem(at: plist)
        checkAgent()
    }

    @discardableResult
    private func launchctl(_ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run(); p.waitUntilExit(); return p.terminationStatus } catch { return -1 }
    }

    func setLaunchAtLogin(_ on: Bool) {
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            NSLog("login item change failed: \(error)")
        }
        launchAtLogin = SMAppService.mainApp.status == .enabled
    }

    /// Plays a short C-major arpeggio on the keyboard's own sounds.
    func playTestNotes() {
        let notes: [UInt8] = [60, 64, 67, 72]
        for (i, n) in notes.enumerated() {
            let t = Double(i) * 0.22
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { bridge_send([0x90, n, 100], 3) }
            DispatchQueue.main.asyncAfter(deadline: .now() + t + 0.18) { bridge_send([0x80, n, 0], 3) }
        }
    }

    func openAudioMIDISetup() {
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app"))
    }
}

// MARK: - Helpers

/// Reads a fixed-size C char array (imported into Swift as a tuple) as a String.
func cString<T>(_ tuple: T) -> String {
    withUnsafePointer(to: tuple) { p in
        p.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) { String(cString: $0) }
    }
}

func noteName(_ n: UInt8) -> String {
    let names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"]
    return "\(names[Int(n) % 12])\(Int(n) / 12 - 1)"
}

func describe(_ b: (UInt8, UInt8, UInt8), _ len: UInt8) -> String {
    guard len > 0 else { return "—" }
    let ch = Int(b.0 & 0x0F) + 1
    switch b.0 & 0xF0 {
    case 0x90 where b.2 > 0: return "Note on \(noteName(b.1)), velocity \(b.2), ch \(ch)"
    case 0x80, 0x90: return "Note off \(noteName(b.1)), ch \(ch)"
    case 0xA0: return "Poly pressure \(noteName(b.1)) = \(b.2), ch \(ch)"
    case 0xB0: return "Controller \(b.1) = \(b.2), ch \(ch)"
    case 0xC0: return "Program \(b.1), ch \(ch)"
    case 0xD0: return "Channel pressure \(b.1), ch \(ch)"
    case 0xE0: return "Pitch bend \(Int(b.2) << 7 | Int(b.1)), ch \(ch)"
    default:
        if b.0 == 0xF0 { return "SysEx" }
        if b.0 == 0xF8 { return "Clock" }
        return String(format: "%02X %02X %02X", b.0, b.1, b.2)
    }
}

// MARK: - View

struct ContentView: View {
    @EnvironmentObject var model: BridgeModel
    @State private var nameField = ""

    private var s: bridge_stats { model.stats }
    private var connected: Bool { s.connected != 0 }
    private var version: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev" }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header
            if model.agentRunning { agentBanner }
            portBox
            activityBox
            HStack {
                Button("Play test notes on keyboard") { model.playTestNotes() }.disabled(!connected)
                Button("Open Audio MIDI Setup") { model.openAudioMIDISetup() }
                Spacer()
            }
            Toggle("Open at login", isOn: Binding(get: { model.launchAtLogin }, set: { model.setLaunchAtLogin($0) }))
            Text("Closing this window keeps the bridge running in the menu bar (look for the piano keys icon). Quit to disconnect the keyboard.  ·  Not affiliated with Casio  ·  v\(version)")
                .font(.footnote).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .frame(width: 460)
        .onAppear { nameField = model.portName }
    }

    private var header: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(connected ? Color.green : (s.busy != 0 ? Color.orange : Color.gray.opacity(0.5)))
                .frame(width: 14, height: 14)
            VStack(alignment: .leading, spacing: 2) {
                Text(connected ? "Keyboard connected" : (s.busy != 0 ? "Keyboard is in use by another program" : "Waiting for keyboard"))
                    .font(.title2).bold()
                if connected || s.busy != 0 {
                    Text("\(cString(s.device_desc))  ·  USB \(String(format: "%04X:%04X", s.vid, s.pid))")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    Text("Plug the keyboard in with a USB cable and switch it on.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            Spacer()
        }
    }

    private var agentBanner: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 4) {
                Text("The background service is also installed.").bold()
                Text("Only one program can own the keyboard at a time. Stop the service to let this app handle it; you can bring it back later with `make install`.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Button("Stop background service") { model.stopAgent() }
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.orange.opacity(0.12)))
    }

    private var portBox: some View {
        GroupBox("MIDI port") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    TextField("Port name", text: $nameField)
                        .textFieldStyle(.roundedBorder)
                        .onSubmit { model.rename(to: nameField) }
                    Button("Rename") { model.rename(to: nameField) }
                        .disabled(nameField.trimmingCharacters(in: .whitespaces) == model.portName)
                }
                Text(connected
                     ? "Pick “\(model.portName)” as the input in GarageBand, Logic or any other music app."
                     : "This port appears in music apps as soon as the keyboard is connected.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.top, 4)
        }
    }

    private var activityBox: some View {
        GroupBox("Activity") {
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 6) {
                GridRow {
                    Text("Connected for").foregroundStyle(.secondary)
                    if connected {
                        Text(Date(timeIntervalSinceReferenceDate: s.connected_since), style: .relative)
                    } else { Text("—") }
                }
                GridRow {
                    Text("From keyboard").foregroundStyle(.secondary)
                    Text("\(s.msgs_in) messages, \(s.notes_in) notes")
                }
                GridRow {
                    Text("To keyboard").foregroundStyle(.secondary)
                    Text("\(s.msgs_out) messages")
                }
                GridRow {
                    Text("Last received").foregroundStyle(.secondary)
                    Text(describe(s.last_in, s.last_in_len)).monospacedDigit()
                }
                GridRow {
                    Text("Last sent").foregroundStyle(.secondary)
                    Text(describe(s.last_out, s.last_out_len)).monospacedDigit()
                }
                if s.errors > 0 {
                    GridRow {
                        Text("Last error").foregroundStyle(.secondary)
                        Text(cString(s.last_error)).foregroundStyle(.red)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 4)
        }
    }
}
