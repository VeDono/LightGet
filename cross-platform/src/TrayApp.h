#pragma once

// TrayApp.h — Application lifecycle controller (the AppDelegate analog) +
//             multi-monitor overlay coordinator (the OverlayController analog).
//
// Source: main.swift / AppDelegate.swift / OverlayController.swift (Spec 2).
//
// Responsibilities:
//   - menu-bar/tray-only app (no taskbar/Dock window): QSystemTrayIcon + QMenu;
//   - global hotkey (default ⇧⌘2) -> startCapture, fires even when backgrounded;
//   - tray menu: Capture (title shows hotkey, NO accelerator), Settings, Quit;
//   - capture-before-overlay ordering + re-entrancy guard (isCapturing / overlay);
//   - permission preflight (macOS TCC) before capture; system prompt on denial;
//   - on success: create one OverlayWindow per CapturedScreen, place each at its
//     QScreen::geometry(), focus the one under the cursor, force the cursor
//     visible (now + next tick), and coordinate single-active-selection across
//     monitors; on close: restore the previously-active app and return to IDLE.
//
// RE-ENTRANCY (load-bearing, Spec 2 §2.9): set m_isCapturing=true synchronously
// before any async capture; on completion (GUI thread) create overlay, set
// m_overlay, clear m_isCapturing; never a window where both are idle while a
// capture/overlay is logically active. onClose is the single return-to-IDLE.
//
// COORDINATE NOTE: Qt screen geometry is already top-left/Y-down virtual-desktop
// space; place overlays with window->setGeometry(screen->geometry()) directly
// (the Swift bottom-left global-frame math is not needed).

#include <QObject>
#include <QVector>

class QSystemTrayIcon;
class QMenu;
class QAction;
class GlobalHotkey;
class OverlayWindow;
class SettingsWindow;

class TrayApp : public QObject {
    Q_OBJECT
public:
    explicit TrayApp(QObject* parent = nullptr);
    ~TrayApp() override;

    // Equivalent of applicationDidFinishLaunching: build tray, register hotkey.
    // Call once from main() after QApplication is constructed.
    void start();

public slots:
    void startCapture();        // hotkey / menu "Capture" trigger
    void openSettings();

private slots:
    void onHotkeyActivated();   // GlobalHotkey::activated -> startCapture
    void onTrayActivated(int reason);  // QSystemTrayIcon::ActivationReason (left-click menu)

private:
    void setupTray();
    void applyBarIcon();        // custom 18x18 path OR named icon asset
    QMenu* buildMenu();         // rebuilt on language change
    void rebuildMenu();

    // Capture pipeline (Spec 2 §2.9).
    void onCaptureFinished();   // overlay closed callback -> overlay=null, isCapturing=false
    void presentCaptureError(); // message box + deep-link to Screen Recording settings

    // Multi-monitor coordination (Spec 2 §4).
    void closeOverlays();                       // tear down all, restore previous app
    void clearOthers(OverlayWindow* except);    // single active selection
    void forceCursorVisible();                  // defeat hidden cursor (games); native

    // Record / restore the app that was frontmost before capture (native; no-op
    // where unsupported). Used to return focus on close (e.g. a fullscreen game).
    void recordPreviousApp();
    void restorePreviousApp();

    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QAction* m_captureAction = nullptr;     // title updated live to show hotkey

    GlobalHotkey* m_hotKey = nullptr;
    SettingsWindow* m_settings = nullptr;   // lazily created, cached

    QVector<OverlayWindow*> m_overlays;     // one per screen; empty when IDLE
    bool m_isCapturing = false;             // guards the async capture window
    bool m_overlayShown = false;            // true while overlays are up (overlay!=nil)

    // Opaque handle to the previously-active app/window (platform-specific).
    void* m_previousApp = nullptr;
};
