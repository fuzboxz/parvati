// ui_drag_helper.swift — scripted active-use driver for tools/profile_ui.sh.
//
// Posts real CGEvent mouse input circling a centre point: leftMouseDown,
// ~80 Hz leftMouseDragged on a circular path for N seconds, leftMouseUp.
// Used to measure the editor's CPU under ACTIVE use (knob drags, hover
// tracking, repaints) — the counterpart to profile_ui.sh's idle measurement.
//
// Args: <cx> <cy> <radius> <seconds>   (screen coordinates, points)
//
// CoreGraphics/CoreFoundation only (no QuartzCore) so it compiles with a
// plain `swiftc -O`. profile_ui.sh compiles it to a temp binary on first
// use; it can also be run by hand:
//   swift tools/ui_drag_helper.swift 640 250 60 5
//
// NOTE: posting CGEvents to other apps requires Accessibility permission
// (System Settings > Privacy & Security > Accessibility) for the calling
// terminal/IDE — this is why the drag-based measurement is a LOCAL/manual
// step and only the headless tests/perf_smoke_test runs in CI.

import CoreGraphics
import Foundation

guard CommandLine.arguments.count >= 5,
      let cx = Double(CommandLine.arguments[1]),
      let cy = Double(CommandLine.arguments[2]),
      let radius = Double(CommandLine.arguments[3]),
      let seconds = Double(CommandLine.arguments[4]) else {
    FileHandle.standardError.write("usage: ui_drag_helper <cx> <cy> <radius> <seconds>\n".data(using: .utf8)!)
    exit(2)
}

func post(_ type: CGEventType, _ point: CGPoint) {
    CGEvent(mouseEventSource: nil, mouseType: type, mouseCursorPosition: point, mouseButton: .left)?
        .post(tap: .cghidEventTap)
}

let centre = CGPoint(x: cx, y: cy)
let start = Date().timeIntervalSince1970
var step = 0.0

post(.leftMouseDown, centre)
while Date().timeIntervalSince1970 - start < seconds {
    // Slow circular sweep (~1 revolution / 6.7 s at 80 Hz) — a long continuous
    // knob drag, crossing several controls per revolution.
    let angle = step * 0.15
    post(.leftMouseDragged, CGPoint(x: cx + radius * cos(angle), y: cy + radius * sin(angle)))
    step += 1
    usleep(12_000)   // ~80 Hz
}
post(.leftMouseUp, centre)
