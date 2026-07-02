// ScreenCapture.cpp — Per-display pixel capture (portable Qt baseline).
//
// Faithful port of ScreenCapture.swift (which used ScreenCaptureKit on macOS 14+).
// The Swift module exposed three pieces, mirrored 1:1 here:
//   - captureAllDisplays()        — one CapturedScreen per connected screen
//   - captureDisplayUnderCursor() — the single screen under the cursor
//   - displayID helper            — replaced by QScreen identity below
//
// HARD ORDERING INVARIANT (DESIGN.md §3.2): capture pixels FIRST, before any
// overlay/dimming window is shown. This file only grabs; the caller is
// responsible for showing overlays only after these functions return.
//
// COORDINATES: Qt is top-left origin, +Y down (DESIGN.md §4). Swift used
// bottom-left NSEvent.mouseLocation / NSMouseInRect for the under-cursor screen;
// here QCursor::pos() + QScreen::geometry().contains() needs no inversion.
//
// RESOLUTION: capture at native pixels = points × devicePixelRatio. The returned
// QImage carries setDevicePixelRatio(dpr) so it paints crisp at logical size and
// exports at full pixel resolution (matches Swift's display.width × backingScale).
//
// PLATFORM BACKENDS (dispatched here; no #ifdef leaks to call sites):
//   - Baseline (all OSes): QScreen::grabWindow(0). Simple; includes the cursor;
//     may be empty on Wayland or without macOS Screen Recording permission.
//   - macOS (faithful) TODO: ScreenCaptureKit (SCShareableContent +
//     SCScreenshotManager) in a .mm translation unit for retina-correct,
//     cursor-excluded grabs gated by CGPreflight/RequestScreenCaptureAccess.
//     Until that .mm exists, the baseline grab below is used on macOS too.
//   - Windows: DXGI Desktop Duplication / BitBlt per monitor (TODO; baseline OK).
//   - X11: XGetImage / QScreen::grabWindow (baseline OK).
//   - Wayland: org.freedesktop.portal ScreenCast/Screenshot portal (TODO).

#include "ScreenCapture.h"

#include <QGuiApplication>
#include <QCursor>
#include <QPixmap>

#if defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>  // CGPreflight/RequestScreenCaptureAccess
#endif

// macOS-native helpers (Objective-C++ in src/mac/MacNative.mm). Declared as plain
// `extern` C++ symbols, mirroring the other MacNative externs (TrayApp_*, etc.).
// Only linked / called when HAVE_MAC_NATIVE is defined.
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
extern bool MacNative_hasScreenCapturePermission();
extern void MacNative_requestScreenCapturePermission();
#endif

// Wayland: capture goes through the xdg-desktop-portal Screenshot backend
// (WaylandPortal.cpp, Linux+QtDBus only), since grabWindow(0) returns null there.
#if defined(Q_OS_LINUX) && defined(HAVE_QTDBUS)
extern QImage LightGet_captureDesktopViaPortal();
#endif

namespace ScreenCapture {

#if defined(Q_OS_LINUX) && defined(HAVE_QTDBUS)
static bool isWaylandSession() {
    return QGuiApplication::platformName()
        .startsWith(QLatin1String("wayland"), Qt::CaseInsensitive);
}

// Grab the whole desktop via the portal, then crop one image per QScreen so the
// per-screen overlay model is preserved. The portal returns a single PNG of the
// virtual desktop in native pixels; we map each screen's logical geometry into it
// with a uniform scale (exact for uniform-DPI setups; approximate but safe for
// mixed-DPI). Returns empty on failure.
static std::vector<CapturedScreen> captureViaPortal(ScreenCaptureError& outError) {
    std::vector<CapturedScreen> result;
    const QImage desktop = LightGet_captureDesktopViaPortal();
    if (desktop.isNull()) { outError = ScreenCaptureError::NoDisplay; return {}; }

    QScreen* prim = QGuiApplication::primaryScreen();
    const QRect virt = prim ? prim->virtualGeometry() : desktop.rect();
    const double sx = virt.width()  > 0 ? double(desktop.width())  / virt.width()  : 1.0;
    const double sy = virt.height() > 0 ? double(desktop.height()) / virt.height() : 1.0;

    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect g = screen->geometry();
        QRect cropPx(qRound((g.x() - virt.x()) * sx), qRound((g.y() - virt.y()) * sy),
                     qRound(g.width() * sx), qRound(g.height() * sy));
        cropPx = cropPx.intersected(desktop.rect());
        if (cropPx.isEmpty()) continue;
        QImage sub = desktop.copy(cropPx);
        sub.setDevicePixelRatio(screen->devicePixelRatio());
        result.push_back(CapturedScreen{ sub, screen });
    }
    if (result.empty() && prim) {
        // Fallback: hand the whole grab to the primary screen.
        QImage d = desktop;
        d.setDevicePixelRatio(prim->devicePixelRatio());
        result.push_back(CapturedScreen{ d, prim });
    }
    if (result.empty()) outError = ScreenCaptureError::NoDisplay;
    return result;
}
#endif

// ---------------------------------------------------------------------------
// Permission gate.
//
// On macOS the faithful path requires Screen Recording (TCC) permission. Swift's
// SCShareableContent throws on denial; here we expose the CoreGraphics preflight
// pair declared in the header. On every other platform these are no-ops and
// preflight returns true (DESIGN.md: "no per-app permission gate" off macOS).
// ---------------------------------------------------------------------------

bool preflightPermission() {
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    // Faithful path: ask the native helper (CGPreflightScreenCaptureAccess).
    // Returns false until the user grants Screen Recording in
    // System Settings → Privacy & Security. This is the authoritative gate
    // TrayApp::startCapture checks BEFORE showing any dim/overlay (task 5).
    return MacNative_hasScreenCapturePermission();
#elif defined(Q_OS_MACOS)
    // macOS without the native .mm: call CoreGraphics directly. Available on
    // macOS 10.15+.
    return CGPreflightScreenCaptureAccess();
#else
    // No per-app screen-capture permission gate off macOS.
    return true;
#endif
}

void requestPermission() {
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    // Faithful path: ask the native helper (CGRequestScreenCaptureAccess) to show
    // the system prompt. preflightPermission() remains the authoritative gate.
    MacNative_requestScreenCapturePermission();
#elif defined(Q_OS_MACOS)
    // Triggers the system prompt the first time; thereafter a no-op returning
    // the current grant state (which we ignore — preflightPermission() is the
    // authoritative gate the caller checks).
    (void)CGRequestScreenCaptureAccess();
#endif
    // No-op elsewhere.
}

// ---------------------------------------------------------------------------
// Internal: grab a single QScreen at native pixel resolution.
//
// Equivalent to the Swift per-screen body:
//   scale = screen.backingScaleFactor
//   config.width  = display.width  * scale
//   config.height = display.height * scale
//   cgImage = SCScreenshotManager.captureImage(...)
//
// QScreen::grabWindow(0) already returns a pixmap at the device's native
// resolution on HiDPI screens, so we convert to QImage and stamp the dpr so the
// rest of the app can paint it at logical size and export at full pixels.
//
// Returns a null QImage on failure (empty/locked screen, Wayland, denied perm).
// ---------------------------------------------------------------------------
static QImage grabScreenPixels(QScreen* screen) {
    if (!screen)
        return QImage();

    // grabWindow(0) on a QScreen captures that screen's full desktop. The
    // returned QPixmap is at native pixel resolution; dpr is baked into size.
    const QPixmap pix = screen->grabWindow(0);
    if (pix.isNull())
        return QImage();

    QImage image = pix.toImage();
    if (image.isNull())
        return QImage();

    // Stamp the device pixel ratio so painting at logical (point) size stays
    // crisp and export rasterizes at points × dpr (header contract).
    image.setDevicePixelRatio(screen->devicePixelRatio());
    return image;
}

// ---------------------------------------------------------------------------
// Capture EVERY connected display — one CapturedScreen per usable screen.
//
// Mirrors Swift captureAllDisplays(): iterate NSScreen.screens, skip any that
// fail to grab, and throw .noDisplay if the result is empty. Here "throw" maps
// to setting outError = NoDisplay and returning an empty vector.
// ---------------------------------------------------------------------------
std::vector<CapturedScreen> captureAllDisplays(ScreenCaptureError& outError) {
    outError = ScreenCaptureError::None;

#if defined(Q_OS_LINUX) && defined(HAVE_QTDBUS)
    if (isWaylandSession())
        return captureViaPortal(outError);
#endif

    std::vector<CapturedScreen> result;

    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        QImage image = grabScreenPixels(screen);
        if (image.isNull())
            continue;  // mirrors Swift's `guard ... else { continue }`
        result.push_back(CapturedScreen{ image, screen });
    }

    if (result.empty()) {
        outError = ScreenCaptureError::NoDisplay;  // Swift: throw .noDisplay
        return {};
    }
    return result;
}

// ---------------------------------------------------------------------------
// Capture only the display under the cursor (single-monitor mode).
//
// Mirrors Swift captureDisplayUnderCursor(): find the screen whose frame
// contains the mouse, else fall back to the primary screen. QCursor::pos() and
// QScreen::geometry() are both top-left virtual-desktop coords — no inversion
// (DESIGN.md §4). On no screen or a failed grab → NoDisplay.
// ---------------------------------------------------------------------------
CapturedScreen captureDisplayUnderCursor(ScreenCaptureError& outError) {
    outError = ScreenCaptureError::None;

#if defined(Q_OS_LINUX) && defined(HAVE_QTDBUS)
    if (isWaylandSession()) {
        auto all = captureViaPortal(outError);
        if (all.empty()) return CapturedScreen{};
        QScreen* target = QGuiApplication::screenAt(QCursor::pos());
        for (const auto& cs : all)
            if (cs.screen == target) return cs;
        return all.front();
    }
#endif

    const QPoint mouse = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(mouse);
    if (!screen)
        screen = QGuiApplication::primaryScreen();  // Swift: ?? NSScreen.main

    if (!screen) {
        outError = ScreenCaptureError::NoDisplay;
        return CapturedScreen{};
    }

    QImage image = grabScreenPixels(screen);
    if (image.isNull()) {
        outError = ScreenCaptureError::NoDisplay;  // empty grab / denied perm
        return CapturedScreen{};
    }

    return CapturedScreen{ image, screen };
}

} // namespace ScreenCapture
