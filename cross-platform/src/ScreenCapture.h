#pragma once

// ScreenCapture.h — Per-display screen capture.
//
// Source: ScreenCapture.swift (Spec 1 §2) + usage in Spec 2 §6.
//
// HARD ORDERING INVARIANT: capture pixels FIRST, then show the dimming overlay.
// Neither the dark shield nor the user's annotations may leak into the grab.
// Export = original captured pixels + re-rendered annotations composited at
// export time — never a re-grab with the overlay visible.
//
// RESOLUTION: capture at native pixels = screen points x devicePixelRatio.
// Each CapturedScreen carries its QScreen so the overlay knows geometry + DPR.
// The returned QImage should have its devicePixelRatio() set so painting it at
// logical size keeps it crisp; export rasterizes at points x dpr.
//
// PLATFORM (second genuinely OS-specific piece; backends in .cpp):
//   - Baseline: QScreen::grabWindow(0) per screen (simple; includes cursor;
//     may be blocked/empty on Wayland or without macOS Screen Recording perm).
//   - macOS (faithful): ScreenCaptureKit via native code (correct retina scale,
//     cursor-excluded, TCC-gated). Use CGPreflight/RequestScreenCaptureAccess.
//   - Windows: DXGI Desktop Duplication or BitBlt/PrintWindow per monitor.
//   - X11: XGetImage / QScreen::grabWindow.
//   - Wayland: org.freedesktop.portal.ScreenCast/Screenshot portal (interactive).
// On failure / permission denied -> NoDisplay.

#include <QImage>
#include <QScreen>
#include <vector>

enum class ScreenCaptureError {
    None,
    NoDisplay        // no suitable display / permission denied
};

struct CapturedScreen {
    QImage image;        // full pixel-resolution bitmap (dpr set on the image)
    QScreen* screen = nullptr;  // geometry + devicePixelRatio
};

namespace ScreenCapture {

// macOS Screen Recording permission gate (no-op / true on other platforms).
bool preflightPermission();      // CGPreflightScreenCaptureAccess
void requestPermission();        // CGRequestScreenCaptureAccess (may prompt)

// Capture EVERY connected display (one overlay per monitor). On success,
// outError == None and the vector has one entry per usable screen.
std::vector<CapturedScreen> captureAllDisplays(ScreenCaptureError& outError);

// Capture only the display currently under the cursor (single-monitor mode).
CapturedScreen captureDisplayUnderCursor(ScreenCaptureError& outError);

} // namespace ScreenCapture
