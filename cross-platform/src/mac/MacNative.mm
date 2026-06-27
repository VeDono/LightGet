// MacNative.mm — Objective-C++ implementations of the macOS-native helpers the
// pure-Qt code declares as plain `extern` (C++ linkage) and calls behind
// `#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)`.
//
// Compiled (and linked) ONLY when HAVE_MAC_NATIVE is defined and this file is
// added to the target (see CMakeLists.txt, APPLE branch). The matching call
// sites live in:
//   - OverlayWindow.cpp : OverlayWindow_applyShieldLevel(WId)
//   - TrayApp.cpp       : TrayApp_forceCursorVisible(), TrayApp_recordFrontmostApp(),
//                         TrayApp_restoreApp(void*)
//
// LINKAGE: every declaration at the call sites is a bare `extern void/void* ...`
// (NO `extern "C"`), so these definitions must use plain C++ linkage too — i.e.
// they are NOT wrapped in `extern "C"`. The C++ name mangling on both sides then
// matches and the symbols resolve. Signatures are copied verbatim from the call
// sites; do not change them without changing the declarations there.
//
// This translation unit uses ARC (-fobjc-arc, set in CMakeLists.txt for this
// file). The recorded frontmost app is handed back to C++ as an opaque void* via
// a +1 retained bridge (CFBridgingRetain) and released on restore
// (CFBridgingRelease), so it survives across the Qt event loop.

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ServiceManagement/ServiceManagement.h>
#include <CoreGraphics/CoreGraphics.h>

// Pull in Qt's real WId typedef so the symbol mangling here matches the call
// site's `extern void OverlayWindow_applyShieldLevel(WId)` EXACTLY. On macOS a
// top-level QWidget's winId() is the NSView* of its backing window.
#include <qwindowdefs.h>

// ---------------------------------------------------------------------------
// OverlayWindow_applyShieldLevel — raise the overlay above the menu bar / Dock.
//
// The WId is the overlay QWidget's NSView*. We climb to its NSWindow and:
//   - set the window level to CGShieldingWindowLevel() (above the menu bar), and
//   - give it a collection behavior that lets it sit on every Space, stay put,
//     and coexist with full-screen apps.
// This is exactly what allows the selection overlay to cover the macOS menu bar.
// ---------------------------------------------------------------------------
void OverlayWindow_applyShieldLevel(WId win) {
    if (win == 0) return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(win);
    if (![view isKindOfClass:[NSView class]]) return;
    NSWindow* window = [view window];
    if (window == nil) return;

    [window setLevel:(NSInteger)CGShieldingWindowLevel()];
    [window setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces |
                                   NSWindowCollectionBehaviorStationary |
                                   NSWindowCollectionBehaviorFullScreenAuxiliary)];
    // Belt-and-suspenders: suppress the default macOS window-appear animation
    // (the "screen zooms out and back" scale effect) here as well, in case this
    // is the first place the NSWindow becomes addressable. The dedicated early
    // hook (OverlayWindow_disableShowAnimation, called before show()) is the
    // primary defense; this re-asserts it.
    [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
    // Make sure it can become key so it receives the keyboard (Esc / ⌘C) even at
    // shield level, and bring it to the very front of its level.
    [window setHidesOnDeactivate:NO];
    [window orderFrontRegardless];
    // CRITICAL: actually make the overlay the KEY window so it receives keyboard
    // (Esc / ⌘C) and mouse events. An accessory (LSUIElement) app is not "active"
    // by default, so activate the app first, THEN make this window key. Without
    // this the shield-level overlay shows but gets no events — the user can't
    // select or press Esc, and the dimmed screen is effectively trapped.
    // (Requires the Qt overlay to be a real Window, not Qt::Tool, so its NSWindow
    // can become key — see OverlayWindow.cpp.)
    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
}

// ---------------------------------------------------------------------------
// OverlayWindow_disableShowAnimation — suppress the default macOS window-appear
// animation for the overlay's NSWindow.
//
// When a frameless window is first shown, AppKit plays a subtle scale/fade
// "appear" animation. For a full-screen shield overlay this reads as the whole
// screen briefly zooming out and back. Setting animationBehavior = .none makes
// the overlay snap in instantly (matching the native app). Must be called BEFORE
// the window is shown (the call site uses winId(), which forces native NSWindow
// creation so this hook can reach it early). Same WId->NSView->NSWindow climb and
// nil guards as OverlayWindow_applyShieldLevel.
// ---------------------------------------------------------------------------
void OverlayWindow_disableShowAnimation(WId win) {
    if (win == 0) return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(win);
    if (![view isKindOfClass:[NSView class]]) return;
    NSWindow* window = [view window];
    if (window == nil) return;

    [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
}

// ---------------------------------------------------------------------------
// TrayApp_forceCursorVisible — defeat an app that hid the system cursor
// (e.g. a full-screen game using NSCursor.hide / CGDisplayHideCursor / mouselook)
// so the crosshair is usable on the overlay. Re-associates the mouse and unhides
// the cursor on every active display.
// ---------------------------------------------------------------------------
void TrayApp_forceCursorVisible() {
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
    CGDisplayShowCursor(kCGDirectMainDisplay);

    // Also unhide on every online display, in case the previous app hid the
    // cursor on a non-main screen.
    const uint32_t kMaxDisplays = 16;
    CGDirectDisplayID displays[kMaxDisplays];
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(kMaxDisplays, displays, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; ++i) {
            CGDisplayShowCursor(displays[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// TrayApp_recordFrontmostApp — remember the app that was frontmost before we
// took over (so focus can be handed back on close). Returns an opaque, +1
// retained handle (NSRunningApplication*) that TrayApp_restoreApp must consume.
// Returns nullptr if there is nothing to record.
// ---------------------------------------------------------------------------
void* TrayApp_recordFrontmostApp() {
    NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
    if (app == nil) return nullptr;
    // +1 retain across the C++ boundary; balanced by CFBridgingRelease in restore.
    return (void*)CFBridgingRetain(app);
}

// ---------------------------------------------------------------------------
// TrayApp_restoreApp — re-activate the previously-frontmost app and release the
// handle taken by TrayApp_recordFrontmostApp. No-op (but still releases) if the
// recorded app is us.
// ---------------------------------------------------------------------------
void TrayApp_restoreApp(void* handle) {
    if (handle == nullptr) return;
    // Consume the +1 retain taken in record (transfers ownership back to ARC).
    NSRunningApplication* app = (NSRunningApplication*)CFBridgingRelease(handle);
    if (app == nil) return;
    if ([app isEqual:[NSRunningApplication currentApplication]]) return;
    [app activateWithOptions:NSApplicationActivateAllWindows];
}

// ---------------------------------------------------------------------------
// MacNative_hasScreenCapturePermission — true iff this app already holds the
// Screen Recording (TCC) grant. Non-prompting: it only reports the current
// state, so the caller can decide whether to bail before showing any overlay.
// CGPreflightScreenCaptureAccess is available on macOS 10.15+.
// ---------------------------------------------------------------------------
bool MacNative_hasScreenCapturePermission() {
    return CGPreflightScreenCaptureAccess();
}

// ---------------------------------------------------------------------------
// MacNative_requestScreenCapturePermission — trigger the system Screen Recording
// permission prompt (the first time; thereafter a no-op that just returns the
// current grant). The boolean result is ignored here — preflight is the
// authoritative gate the caller re-checks.
// ---------------------------------------------------------------------------
void MacNative_requestScreenCapturePermission() {
    (void)CGRequestScreenCaptureAccess();
}

// ---------------------------------------------------------------------------
// Launch-at-login via SMAppService (macOS 13+). The app registers ITSELF as a
// login item (no separate helper). Works reliably only when the app is in
// /Applications. Returns false on macOS < 13 (caller surfaces the explanation).
// ---------------------------------------------------------------------------
bool MacNative_isLaunchAtLoginEnabled() {
    if (@available(macOS 13.0, *)) {
        return [[SMAppService mainAppService] status] == SMAppServiceStatusEnabled;
    }
    return false;
}

bool MacNative_setLaunchAtLogin(bool enabled) {
    if (@available(macOS 13.0, *)) {
        NSError* err = nil;
        SMAppService* svc = [SMAppService mainAppService];
        BOOL ok = enabled ? [svc registerAndReturnError:&err]
                          : [svc unregisterAndReturnError:&err];
        if (!ok) {
            NSLog(@"LightGet: SMAppService %@ failed: %@",
                  enabled ? @"register" : @"unregister", err);
        }
        return ok ? true : false;
    }
    return false;
}
