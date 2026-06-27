// TrayApp.cpp — Application lifecycle controller + multi-monitor overlay
// coordinator. Faithful port of main.swift / AppDelegate.swift /
// OverlayController.swift (Spec 2).
//
// COORDINATE NOTE (Spec 2 §4): Qt screen geometry is already top-left/+Y-down
// virtual-desktop space, so each overlay is placed with
// setGeometry(screen->geometry()) directly and the "screen under cursor" test
// is geometry().contains(QCursor::pos()) — no bottom-left inversion needed
// (the Swift NSMouseInRect / NSScreen.frame math is dropped).

#include "TrayApp.h"

#include "GlobalHotkey.h"
#include "Localization.h"
#include "OverlayWindow.h"
#include "ScreenCapture.h"
#include "Settings.h"
#include "SettingsWindow.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>

// ===========================================================================
// Construction / lifecycle
// ===========================================================================

TrayApp::TrayApp(QObject* parent) : QObject(parent) {}

TrayApp::~TrayApp() {
    // Tear down any live overlays first so their destructors don't outlive us.
    closeOverlays();
    delete m_settings;
    delete m_menu;     // owns its QActions
    delete m_tray;
    delete m_hotKey;
}

void TrayApp::start() {
    // Equivalent of applicationDidFinishLaunching: build the tray and register
    // the global hotkey. (The hidden AppKit "Edit" menu existed only so an
    // accessory app could route ⌘C/⌘V into NSTextFields; Qt's QTextEdit handles
    // those shortcuts itself, so there is no analog to port.)
    setupTray();

    m_hotKey = new GlobalHotkey(this);
    connect(m_hotKey, &GlobalHotkey::activated, this, &TrayApp::onHotkeyActivated);
    m_hotKey->registerHotkey(Settings::instance().hotKeyCode(),
                             Settings::instance().hotKeyModifiers());
}

// ===========================================================================
// Tray + menu
// ===========================================================================

void TrayApp::setupTray() {
    m_tray = new QSystemTrayIcon(this);
    applyBarIcon();
    m_menu = buildMenu();
    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                onTrayActivated(static_cast<int>(reason));
            });
    m_tray->show();
}

void TrayApp::applyBarIcon() {
    // Custom user-supplied 18x18 path takes precedence; otherwise the named
    // preset icon asset (SF-Symbol replacement). Mirrors AppDelegate.applyBarIcon.
    if (!m_tray) return;

    const std::optional<QString> customPath = Settings::instance().barIconCustomPath();
    if (customPath && QFileInfo::exists(*customPath)) {
        QImage img(*customPath);
        if (!img.isNull()) {
            // Match the menu bar by scaling to 18x18 (as the Swift code did).
            img = img.scaled(18, 18, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            m_tray->setIcon(QIcon(QPixmap::fromImage(img)));
            return;
        }
    }

    // Named asset (e.g. "scissors"). Try the theme first, then a bundled
    // resource of the same name; fall back to the platform default if neither.
    const QString name = Settings::instance().barIcon();
    QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull()) {
        const QString res = QStringLiteral(":/assets/%1.png").arg(name);
        QIcon bundled(res);
        if (!bundled.isNull()) icon = bundled;
    }
    m_tray->setIcon(icon);
}

QMenu* TrayApp::buildMenu() {
    QMenu* menu = new QMenu();

    // Capture: title shows the hotkey as plain text, NO accelerator. The hotkey
    // fires globally via GlobalHotkey, not via this menu item — showing an
    // accelerator here would be misleading (matches the Swift comment).
    m_captureAction = menu->addAction(
        QStringLiteral("%1  (%2)").arg(Loc::t(QStringLiteral("menu.capture")),
                                       Settings::instance().hotKeyDisplay()));
    m_captureAction->setIcon(QIcon::fromTheme(QStringLiteral("camera-photo")));
    connect(m_captureAction, &QAction::triggered, this, &TrayApp::startCapture);

    // Settings — keyEquivalent deliberately omitted (would only work with the
    // menu open, never globally), same reasoning as the source.
    QAction* settings = menu->addAction(Loc::t(QStringLiteral("menu.settings")));
    settings->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system")));
    connect(settings, &QAction::triggered, this, &TrayApp::openSettings);

    menu->addSeparator();

    QAction* quit = menu->addAction(Loc::t(QStringLiteral("menu.quit")));
    quit->setIcon(QIcon::fromTheme(QStringLiteral("application-exit")));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    return menu;
}

void TrayApp::rebuildMenu() {
    QMenu* old = m_menu;
    m_menu = buildMenu();
    if (m_tray) m_tray->setContextMenu(m_menu);
    delete old;     // releases the previous QActions (incl. old m_captureAction)
}

void TrayApp::onTrayActivated(int reason) {
    // On platforms where a left-click does not auto-open the context menu,
    // mirror the macOS status-item behaviour by popping the menu at the cursor.
    if (reason == QSystemTrayIcon::Trigger && m_menu && m_tray) {
        m_menu->popup(QCursor::pos());
    }
}

// ===========================================================================
// Capture pipeline (Spec 2 §2.9)
// ===========================================================================

void TrayApp::onHotkeyActivated() {
    startCapture();
}

void TrayApp::startCapture() {
    // Re-entrancy guard (load-bearing, Spec 2 §2.9): an overlay is already up
    // OR a capture is in flight -> do nothing. m_isCapturing closes the race:
    // it is set synchronously, while m_overlayShown only becomes true once the
    // overlays exist, so a second trigger cannot create a second set.
    if (m_overlayShown || m_isCapturing) return;

    // No Screen Recording permission -> show ONLY the system prompt (no
    // duplicate message of our own) and bail. No-op / true off macOS.
    if (!ScreenCapture::preflightPermission()) {
        ScreenCapture::requestPermission();
        return;
    }

    m_isCapturing = true;

    // HARD ORDERING INVARIANT: capture pixels for every display BEFORE any
    // overlay (dim shield) is shown, so neither the dim nor annotations leak
    // into the grab.
    ScreenCaptureError err = ScreenCaptureError::None;
    std::vector<CapturedScreen> shots = ScreenCapture::captureAllDisplays(err);

    if (err != ScreenCaptureError::None || shots.empty()) {
        m_isCapturing = false;
        presentCaptureError();
        return;
    }

    // Remember what was frontmost (e.g. a fullscreen game) so we can hand focus
    // back on close.
    recordPreviousApp();

    // Create one overlay per captured screen, placed at that screen's geometry.
    for (const CapturedScreen& cap : shots) {
        QScreen* screen = cap.screen;
        OverlayWindow* overlay = new OverlayWindow(cap.image, screen);

        // finished -> tear down ALL overlays and return to IDLE.
        connect(overlay, &OverlayWindow::finished, this, &TrayApp::closeOverlays);
        // beganSelection -> single active selection across monitors.
        connect(overlay, &OverlayWindow::beganSelection, this,
                [this, overlay]() { clearOthers(overlay); });

        if (screen) overlay->setGeometry(screen->geometry());
        m_overlays.append(overlay);
    }

    // Show + raise all overlays, then apply the native shield level once mapped.
    for (OverlayWindow* w : m_overlays) {
        w->show();
        w->raise();
        w->applyShieldLevel();
    }

    // Keyboard focus goes to the overlay under the cursor (Esc / ⌘C etc.).
    const QPoint mouse = QCursor::pos();
    OverlayWindow* active = nullptr;
    for (OverlayWindow* w : m_overlays) {
        if (w->geometry().contains(mouse)) { active = w; break; }
    }
    if (!active && !m_overlays.isEmpty()) active = m_overlays.first();
    if (active) {
        active->activateWindow();
        active->raise();
        active->setFocus();
    }

    // Defeat a hidden cursor (e.g. a UE5 game): force it visible now and again
    // on the next tick to win any race with the window server / the game.
    forceCursorVisible();
    QTimer::singleShot(0, this, [this]() { forceCursorVisible(); });

    // Overlays are up; commit the state transition: overlay != nil, not capturing.
    m_overlayShown = true;
    m_isCapturing = false;
}

void TrayApp::onCaptureFinished() {
    // Single return-to-IDLE point (mirrors the onClose closure): overlays gone,
    // both guards cleared.
    m_overlayShown = false;
    m_isCapturing = false;
}

void TrayApp::presentCaptureError() {
    // Message box + deep-link to the Screen Recording settings pane.
    QMessageBox box;
    box.setIcon(QMessageBox::Warning);
    box.setText(Loc::t(QStringLiteral("error.title")));
    box.setInformativeText(Loc::t(QStringLiteral("error.body")));
    QPushButton* openBtn =
        box.addButton(Loc::t(QStringLiteral("error.openSettings")), QMessageBox::AcceptRole);
    box.addButton(Loc::t(QStringLiteral("error.close")), QMessageBox::RejectRole);
    box.setDefaultButton(openBtn);

    box.exec();
    if (box.clickedButton() == openBtn) {
#if defined(Q_OS_MAC)
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")));
#endif
        // On Windows/Linux there is no per-app screen-capture permission pane,
        // so the deep link is a macOS-only action (no-op elsewhere).
    }
}

// ===========================================================================
// Multi-monitor coordination (Spec 2 §4)
// ===========================================================================

void TrayApp::closeOverlays() {
    // Already idle (e.g. a second finished() arriving) -> nothing to do.
    if (m_overlays.isEmpty() && !m_overlayShown && !m_isCapturing) return;

    for (OverlayWindow* w : m_overlays) {
        w->hide();
        w->deleteLater();
    }
    m_overlays.clear();

    // Return focus to the previously-frontmost app (it re-hides its own cursor
    // in its focus handler, mirroring the game case).
    restorePreviousApp();

    onCaptureFinished();
}

void TrayApp::clearOthers(OverlayWindow* except) {
    for (OverlayWindow* w : m_overlays) {
        if (w != except) w->clearSelectionState();
    }
}

// ===========================================================================
// Platform-specific bits (native helpers live in TrayApp_mac.mm /
// TrayApp_win.cpp / TrayApp_x11.cpp; the cross-platform fallbacks are no-ops).
// The macOS-native helpers are compiled in only when HAVE_MAC_NATIVE is defined
// (and the .mm files are added to the build); by default they stay no-ops so a
// plain Qt build links on macOS without the Objective-C++ translation units.
// ===========================================================================

void TrayApp::forceCursorVisible() {
    // macOS (faithful): CGAssociateMouseAndMouseCursorPosition(1),
    // NSCursor.unhide(), CGDisplayShowCursor for every display — to defeat a
    // game that hid the cursor (NSCursor.hide / CGDisplayHideCursor / mouselook).
    // No reliable cross-app equivalent on Windows/X11 (Spec 2 fidelity gap #5),
    // and unnecessary for normal apps -> no-op fallback. The macOS-native symbol
    // lives in TrayApp_mac.mm and is only linked when HAVE_MAC_NATIVE is defined;
    // without it (default), this is a no-op on macOS too.
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    extern void TrayApp_forceCursorVisible();   // implemented in TrayApp_mac.mm
    TrayApp_forceCursorVisible();
#endif
}

void TrayApp::recordPreviousApp() {
    // macOS: NSWorkspace.shared.frontmostApplication (retained in m_previousApp).
    // No-op where unsupported (and on macOS without HAVE_MAC_NATIVE);
    // m_previousApp stays null.
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    extern void* TrayApp_recordFrontmostApp();
    m_previousApp = TrayApp_recordFrontmostApp();
#endif
}

void TrayApp::restorePreviousApp() {
    // macOS: activate the recorded NSRunningApplication unless it is us, then
    // release it. No-op / clears the handle elsewhere (and on macOS without
    // HAVE_MAC_NATIVE, where m_previousApp is never set).
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    extern void TrayApp_restoreApp(void* app);
    if (m_previousApp) TrayApp_restoreApp(m_previousApp);
#endif
    m_previousApp = nullptr;
}

// ===========================================================================
// Settings
// ===========================================================================

void TrayApp::openSettings() {
    if (!m_settings) {
        m_settings = new SettingsWindow();

        // hotKeyChanged -> re-register the global hotkey + refresh the Capture
        // menu title to show the new combo.
        connect(m_settings, &SettingsWindow::hotKeyChanged, this, [this]() {
            if (m_hotKey) {
                m_hotKey->reregister(Settings::instance().hotKeyCode(),
                                     Settings::instance().hotKeyModifiers());
            }
            if (m_captureAction) {
                m_captureAction->setText(QStringLiteral("%1  (%2)").arg(
                    Loc::t(QStringLiteral("menu.capture")),
                    Settings::instance().hotKeyDisplay()));
            }
        });

        // languageChanged -> rebuild the whole tray menu (retranslate titles).
        connect(m_settings, &SettingsWindow::languageChanged, this,
                [this]() { rebuildMenu(); });

        // barIconChanged -> update the tray icon.
        connect(m_settings, &SettingsWindow::barIconChanged, this,
                [this]() { applyBarIcon(); });
    }

    m_settings->showCentered();
}
