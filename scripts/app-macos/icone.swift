// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// icone.swift — dessine l'icône de POMPPC.app (PNG 1024×1024) :
//   swift scripts/app-macos/icone.swift sortie.png
// Puis sips et iconutil pour obtenir POMPPC.icns, gardé dans le dépôt.
import AppKit

let taille = 1024
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: taille, pixelsHigh: taille,
                           bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                           colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let cadre = NSRect(x: 100, y: 100, width: 824, height: 824)
let fond = NSBezierPath(roundedRect: cadre, xRadius: 185, yRadius: 185)
NSGradient(starting: NSColor(calibratedRed: 0.33, green: 0.56, blue: 0.87, alpha: 1),
           ending: NSColor(calibratedRed: 0.09, green: 0.20, blue: 0.47, alpha: 1))!
    .draw(in: fond, angle: -90)
func texte(_ s: String, _ corps: CGFloat, _ y: CGFloat) {
    let style = NSMutableParagraphStyle(); style.alignment = .center
    let attr: [NSAttributedString.Key: Any] = [
        .font: NSFont.systemFont(ofSize: corps, weight: .heavy),
        .foregroundColor: NSColor.white, .paragraphStyle: style]
    NSAttributedString(string: s, attributes: attr)
        .draw(in: NSRect(x: 100, y: y, width: 824, height: corps * 1.3))
}
texte("POM", 300, 470)
texte("PPC", 300, 170)
NSGraphicsContext.current?.flushGraphics()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
