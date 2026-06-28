#pragma once

// GlobalHotkey.h — Platform-abstracted system-wide hotkey.
//
// Source: HotKey.swift (Spec 1 §3) + AppDelegate usage (Spec 2 §5).
//
// Fires even when the app is NOT frontmost ("capture from anywhere").
// There is NO Qt6 built-in global hotkey, so this is one of the two genuinely
// OS-specific pieces. Backends (chosen in GlobalHotkey.cpp via #ifdef):
//   - Windows: RegisterHotKey + WM_HOTKEY (QAbstractNativeEventFilter)
//   - Linux/X11: XGrabKey on the root window + filter XCB_KEY_PRESS
//     (must register all NumLock/CapsLock modifier variants)
//   - macOS: Carbon RegisterEventHotKey + kEventHotKeyPressed
//   - Wayland: no standard protocol; org.freedesktop.portal.GlobalShortcuts
//     where available, else document the limitation (see DESIGN.md).
//
// PORTING NOTES:
//  - keyCode/modifiers are passed as Carbon codes (the persisted format,
//    Settings::hotKeyCode/hotKeyModifiers). Each backend translates Carbon ->
//    native at the edge. Keep ONE internal representation and convert there.
//  - Install exactly ONE process-wide event handler regardless of how many
//    hotkeys exist (the macOS source had a per-instance-handler duplicate-fire
//    bug — do not replicate it).
//  - Registration failure was silently swallowed in the original; here we
//    surface it via registered() / the return of registerHotkey() for better UX.

#include <QObject>
#include <cstdint>

class GlobalHotkey : public QObject {
    Q_OBJECT
public:
    explicit GlobalHotkey(QObject* parent = nullptr);
    ~GlobalHotkey() override;

    // Register the combo (Carbon keyCode + Carbon modifier mask). Returns true
    // on success. Replaces any previous registration for this object.
    bool registerHotkey(uint32_t carbonKeyCode, uint32_t carbonModifiers);

    // Change the combo on the fly (settings change): unregister old, register new.
    bool reregister(uint32_t carbonKeyCode, uint32_t carbonModifiers);

    // Remove the current registration.
    void unregisterHotkey();

    bool registered() const { return m_registered; }

    // Lets the single process-wide native event handler/filter (defined in
    // GlobalHotkey.cpp) emit activated() on the owning instance after routing
    // the OS event by id. A Qt signal can only be emitted from within its own
    // class, so this thin forwarder is the bridge. Not for general call-site use.
    void emitActivated() { emit activated(); }

signals:
    // Emitted on each hotkey press. Wire to TrayApp::startCapture().
    void activated();

private:
    struct Impl;     // platform-specific state (pimpl, defined per-OS in .cpp)
    Impl* d = nullptr;
    bool m_registered = false;
};
