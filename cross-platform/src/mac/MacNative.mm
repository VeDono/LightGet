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

    // Make sure the shield overlay can never be resized by the user. A frameless
    // top-level NSWindow is created with NSWindowStyleMaskResizable, and macOS
    // allows edge-drag resizing of a resizable borderless window even though it
    // has no visible border — letting the user shrink the overlay and scale the
    // frozen screenshot into it. Clear the bit so the edges are inert. (Qt also
    // drops it when the widget is given a fixed size; this re-asserts it here.)
    [window setStyleMask:([window styleMask] & ~NSWindowStyleMaskResizable)];

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
// MacNative_confineCursorToScreen — clamp the hardware cursor to the display the
// overlay window lives on, so a selection/resize drag cannot drag the pointer
// onto an adjacent monitor.
//
// Qt's QCursor::setPos() routes through CGWarpMouseCursorPosition but leaves the
// default ~0.25s "local events suppression interval" in place, so during a fast
// drag the OS keeps delivering the pre-warp motion and the cursor visibly slips
// onto the neighbouring screen before the clamp settles. We replicate the Swift
// overlay's approach instead: read the true CG cursor location, clamp it to the
// window's display bounds (CG top-left global coords), warp, and immediately
// re-associate the mouse so movement stays in sync. This confines reliably.
// Same WId->NSView->NSWindow climb and nil guards as the helpers above.
// ---------------------------------------------------------------------------
void MacNative_confineCursorToScreen(WId win) {
    if (win == 0) return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(win);
    if (![view isKindOfClass:[NSView class]]) return;
    NSWindow* window = [view window];
    if (window == nil) return;
    NSScreen* screen = [window screen];
    if (screen == nil) return;
    NSNumber* num = screen.deviceDescription[@"NSScreenNumber"];
    if (num == nil) return;
    CGDirectDisplayID display = (CGDirectDisplayID)[num unsignedIntValue];
    CGRect b = CGDisplayBounds(display);

    CGEventRef ev = CGEventCreate(NULL);
    if (ev == NULL) return;
    CGPoint cur = CGEventGetLocation(ev);
    CFRelease(ev);

    CGPoint p = cur;
    p.x = MIN(MAX(b.origin.x + 1.0, p.x), b.origin.x + b.size.width - 2.0);
    p.y = MIN(MAX(b.origin.y + 1.0, p.y), b.origin.y + b.size.height - 2.0);
    if (p.x != cur.x || p.y != cur.y) {
        CGWarpMouseCursorPosition(p);
        CGAssociateMouseAndMouseCursorPosition(true);   // keep cursor in sync
    }
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

// ---------------------------------------------------------------------------
// MacNative_installEditMenu — give the (LSUIElement) app a standard Edit menu so
// macOS routes the system "Emoji & Symbols" panel (⌃⌘Space) to the focused text
// field while annotating a screenshot. Without an Edit menu carrying
// orderFrontCharacterPalette: the shortcut is a no-op for an accessory app. The
// standard edit items target the first responder (Qt's text view);
// orderFrontCharacterPalette: is handled by NSApplication itself, so the emoji
// item works regardless of which widget has focus.
// ---------------------------------------------------------------------------
void MacNative_installEditMenu() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        NSMenu* mainMenu = [[NSMenu alloc] init];

        // (1) Application menu — the first submenu; required for a valid menu bar.
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [mainMenu addItem:appItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit LightGet"
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];

        // (2) Edit menu — ONLY the Emoji & Symbols panel. We deliberately do NOT
        // add Cut/Copy/Paste/Undo/Redo here: their ⌘X/⌘C/⌘V/⌘Z key-equivalents
        // would be claimed by the menu and shadow the overlay's own shortcuts
        // (⌘C/⌘X copy the screenshot, ⌘Z undoes an annotation). ⌃⌘Space for the
        // character palette conflicts with nothing, and orderFrontCharacterPalette:
        // is handled by NSApplication, so the item is always live.
        NSMenuItem* editItem = [[NSMenuItem alloc] init];
        [mainMenu addItem:editItem];
        NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        NSMenuItem* emoji = [[NSMenuItem alloc] initWithTitle:@"Emoji & Symbols"
                                                       action:@selector(orderFrontCharacterPalette:)
                                                keyEquivalent:@" "];
        [emoji setKeyEquivalentModifierMask:(NSEventModifierFlagControl | NSEventModifierFlagCommand)];
        [editMenu addItem:emoji];
        [editItem setSubmenu:editMenu];

        [app setMainMenu:mainMenu];
    }
}
