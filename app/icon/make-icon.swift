// make-icon.swift — renders the app icon (a piano keyboard on a dark tile) into an .iconset
// directory. Run via `make icon`. Part of casio-midi-bridge. MIT license.
import AppKit

func render(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)
    let tile = NSRect(x: 0, y: 0, width: s, height: s).insetBy(dx: s * 0.05, dy: s * 0.05)
    let bg = NSBezierPath(roundedRect: tile, xRadius: s * 0.225, yRadius: s * 0.225)
    NSGradient(starting: NSColor(red: 0.20, green: 0.24, blue: 0.36, alpha: 1),
               ending: NSColor(red: 0.07, green: 0.08, blue: 0.14, alpha: 1))!.draw(in: bg, angle: -90)
    // white keys
    let n = 7
    let kx = tile.minX + s * 0.11, kw = tile.width - s * 0.22, ky = tile.minY + s * 0.16, kh = tile.height * 0.50
    let ww = kw / CGFloat(n)
    for i in 0..<n {
        let r = NSRect(x: kx + CGFloat(i) * ww + s * 0.006, y: ky, width: ww - s * 0.012, height: kh)
        NSColor(white: 0.96, alpha: 1).setFill()
        NSBezierPath(roundedRect: r, xRadius: s * 0.02, yRadius: s * 0.02).fill()
    }
    // black keys (C# D#  F# G# A#)
    for i in [0, 1, 3, 4, 5] {
        let bx = kx + CGFloat(i + 1) * ww - ww * 0.32
        NSColor(white: 0.08, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: bx, y: ky + kh * 0.42, width: ww * 0.64, height: kh * 0.58),
                     xRadius: s * 0.012, yRadius: s * 0.012).fill()
    }
    // "link" dot: connection indicator
    NSColor(red: 0.25, green: 0.85, blue: 0.45, alpha: 1).setFill()
    NSBezierPath(ovalIn: NSRect(x: tile.midX - s * 0.06, y: tile.maxY - s * 0.20, width: s * 0.12, height: s * 0.12)).fill()
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    try! render(base).write(to: URL(fileURLWithPath: "\(out)/icon_\(base)x\(base).png"))
    try! render(base * 2).write(to: URL(fileURLWithPath: "\(out)/icon_\(base)x\(base)@2x.png"))
}
print("wrote \(out)")
