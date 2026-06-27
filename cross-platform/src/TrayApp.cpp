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
#include <QKeySequence>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <cmath>

namespace {

// Small monochrome menu-item glyphs painted with QPainter into ~16px template
// pixmaps. macOS has no freedesktop icon theme, so QIcon::fromTheme(...) returns
// null and the menu items show no icon; these give "Take Screenshot" / "Settings"
// / "Quit" a consistent subtle glyph. setIsMask(true) marks them as templates so
// the menu tints them to the native appearance-adaptive gray.
enum class MenuGlyph { Camera, Gear, Power };

QIcon makeMenuGlyph(MenuGlyph kind) {
    const qreal dpr = 2.0;
    const int side = 16;
    QPixmap pm(int(side * dpr), int(side * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Paint in black; setIsMask reinterprets alpha as the template shape so the
    // tint color is supplied by the menu, not by us.
    QColor ink(Qt::black);
    QPen pen(ink);
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    const qreal s = side;            // logical drawing size (dpr handled by pm)
    auto px = [s](qreal f) { return f * s; };

    switch (kind) {
    case MenuGlyph::Camera: {
        // Rounded camera body + a small "bump" for the viewfinder + a lens ring.
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF body(px(0.12), px(0.30), px(0.76), px(0.52));
        p.drawRoundedRect(body, px(0.10), px(0.10));
        QPainterPath bump;
        bump.moveTo(px(0.36), px(0.30));
        bump.lineTo(px(0.42), px(0.20));
        bump.lineTo(px(0.58), px(0.20));
        bump.lineTo(px(0.64), px(0.30));
        p.drawPath(bump);
        p.drawEllipse(QPointF(px(0.50), px(0.56)), px(0.15), px(0.15));
        break;
    }
    case MenuGlyph::Gear: {
        // Toothed ring (8 spokes) + a hub hole — a simple settings gear.
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QPointF c(px(0.50), px(0.50));
        const qreal rOuter = px(0.34), rInner = px(0.22), rHub = px(0.12);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * (M_PI / 4.0);
            p.drawLine(QPointF(c.x() + rInner * std::cos(a), c.y() + rInner * std::sin(a)),
                       QPointF(c.x() + rOuter * std::cos(a), c.y() + rOuter * std::sin(a)));
        }
        p.drawEllipse(c, rInner, rInner);
        p.drawEllipse(c, rHub, rHub);
        break;
    }
    case MenuGlyph::Power: {
        // Power symbol: a ~300° ring open at the top + a vertical stem.
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QRectF ring(px(0.24), px(0.28), px(0.52), px(0.52));
        // Start near the top gap, sweep clockwise around (Qt angles: 16ths deg,
        // CCW positive). Open ~60° at the top.
        p.drawArc(ring, int((90 + 30) * 16), int(-300 * 16));
        p.drawLine(QPointF(px(0.50), px(0.18)), QPointF(px(0.50), px(0.50)));
        break;
    }
    }
    p.end();

    QIcon icon(pm);
    icon.setIsMask(true);
    return icon;
}

// QSS for the tray context menu, modelled on tray_menu.dc.html. The design is a
// light-theme mockup; its tokens map straight to the light branch here, and a
// dark branch derives the equivalent values so the menu reads on both menu-bar
// appearances. We branch on the menu palette's window lightness rather than
// hard-coding one theme. Tokens (design -> QSS):
//   --c-menu-bg     #ffffff   menu background
//   --c-menu-border #dcdce0   1px outer border + 11px radius
//   --c-menu-sep    #ececef   separator hairline
//   --c-text        #1d1d1f   item text
//   --c-accent      #007aff   hover/selected highlight (white text on it)
// Item geometry mirrors the mockup: 8px/12px item padding, 6px item radius,
// ~11px icon-to-label gap, comfortable left icon column.
QString menuStyleSheet(const QPalette& pal, bool rounded) {
    const bool dark = pal.color(QPalette::Window).lightness() < 128;

    // Light = design tokens verbatim. Dark = readable equivalents (system dark
    // menu surface, slightly lighter border/sep, near-white text); the accent
    // (#007aff) is shared and reads white-on-blue in both.
    const QString menuBg     = dark ? QStringLiteral("#2b2b2e") : QStringLiteral("#ffffff");
    const QString menuBorder = dark ? QStringLiteral("#3a3a3d") : QStringLiteral("#dcdce0");
    const QString menuSep    = dark ? QStringLiteral("#3a3a3d") : QStringLiteral("#ececef");
    const QString text       = dark ? QStringLiteral("#f2f2f5") : QStringLiteral("#1d1d1f");
    const QString accent     = QStringLiteral("#007aff");
    const QString onAccent   = QStringLiteral("#ffffff");

    // The 11px card radius only paints cleanly when the menu window is
    // translucent (so the area outside the rounded corners is transparent rather
    // than filled with an opaque square). On platforms where we cannot guarantee
    // translucency (X11 without a compositor) we fall back to a square card
    // (radius 0) — see buildMenu(); the 1px border still gives a clean edge.
    const QString cardRadius = rounded ? QStringLiteral("11px") : QStringLiteral("0px");

    return QStringLiteral(
        "QMenu {"
        " background: %1;"
        " border: 1px solid %2;"
        " border-radius: %7;"
        " padding: 6px;"
        " color: %4;"
        "}"
        "QMenu::item {"
        " background: transparent;"
        " padding: 8px 12px 8px 11px;"
        " margin: 1px 0;"
        " border-radius: 6px;"
        " color: %4;"
        "}"
        // Keep the painted template icons; just give them the design's 11px gap
        // to the label and a roomy left column so they don't crowd the edge.
        "QMenu::icon {"
        " padding-left: 11px;"
        "}"
        "QMenu::item:selected {"
        " background: %3;"
        " color: %5;"
        "}"
        "QMenu::item:disabled {"
        " color: %4;"
        "}"
        "QMenu::separator {"
        " height: 1px;"
        " background: %6;"
        " margin: 5px 9px;"
        "}")
        .arg(menuBg, menuBorder, accent, text, onAccent, menuSep, cardRadius);
}

} // namespace

// ===========================================================================
// Construction / lifecycle
// ===========================================================================

TrayApp::TrayApp(QObject* parent) : QObject(parent) {}

TrayApp::~TrayApp() {
    // Tear down any live overlays first so their destructors don't outlive us.
    // Bypass the animated-dim deferral guard: on shutdown there is no time for a
    // fade, so delete the overlays synchronously regardless of m_closing.
    m_closing = false;
    for (OverlayWindow* w : m_overlays) {
        w->hide();
        w->deleteLater();
    }
    m_overlays.clear();
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
    // No setContextMenu(): we pop m_menu MANUALLY in onTrayActivated so we can
    // center it horizontally under the icon. The native context menu can't be
    // repositioned (the OS anchors it to the status item).
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
            QIcon icon(QPixmap::fromImage(img));
            // Treat as a macOS template so the system tints it to the menu-bar
            // appearance (gray / light depending on theme) instead of leaving a
            // flat black bitmap. Harmless no-op off macOS.
            icon.setIsMask(true);
            m_tray->setIcon(icon);
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
    // Mark the bar icon as a template (mask) so macOS treats it as a menu-bar
    // template image: it adapts to light/dark menu bars and renders the correct
    // appearance-adaptive gray instead of pure black. No-op on other platforms.
    icon.setIsMask(true);
    m_tray->setIcon(icon);
}

QMenu* TrayApp::buildMenu() {
    QMenu* menu = new QMenu();

    // Style the popup to match tray_menu.dc.html (rounded card, accent hover,
    // hairline separators) while keeping every item/icon/action below intact.
    //
    // Frameless + translucent background lets the QSS 11px corner radius show
    // without a square native frame painting opaque corners behind it. BUT a
    // translucent top-level window only composites to transparent where the
    // window system has a compositor: on X11 without one, WA_TranslucentBackground
    // yields a BLACK square behind the rounded card. macOS, Windows and Wayland
    // always composite, so enable the rounded translucent card there; on plain X11
    // (xcb) fall back to an opaque, square — but clean — card (no translucency, no
    // corner radius). The frameless hint is safe everywhere (it just drops the
    // native window chrome, which a popup menu never has anyway).
    const QString platform = QGuiApplication::platformName();
    const bool roundedCard = (platform != QLatin1String("xcb"));   // not bare X11
    menu->setWindowFlag(Qt::FramelessWindowHint, true);
    menu->setWindowFlag(Qt::NoDropShadowWindowHint, false);
    menu->setAttribute(Qt::WA_TranslucentBackground, roundedCard);
    menu->setStyleSheet(menuStyleSheet(menu->palette(), roundedCard));

    // Capture: the label shows the global-hotkey combo as a RIGHT-ALIGNED hint
    // (design: "Take Screenshot" … "⇧⌘2"), matching the look of the real ⌘,/⌘Q
    // shortcuts on the other rows. We deliberately do NOT set a QKeySequence here:
    // the hotkey fires GLOBALLY via GlobalHotkey, so a menu accelerator would
    // double-fire (and showing one would be misleading — matches the Swift
    // comment). The trick: a '\t' in a QAction's text makes a Qt-rendered QMenu
    // draw the trailing text in the shortcut column (right-aligned), so the combo
    // lands in the same column as the QKeySequence hints below. This relies on the
    // menu being Qt-drawn (QSS + frameless + translucent below force that, instead
    // of a native NSMenu where '\t' would render literally).
    m_captureAction = menu->addAction(
        QStringLiteral("%1\t%2").arg(Loc::t(QStringLiteral("menu.capture")),
                                     Settings::instance().hotKeyDisplay()));
    // QIcon::fromTheme(...) returns null on macOS (no freedesktop theme), so paint
    // small template glyphs instead — gives each item a subtle native-style icon.
    m_captureAction->setIcon(makeMenuGlyph(MenuGlyph::Camera));
    m_captureAction->setIconVisibleInMenu(true);   // macOS hides menu-item icons by default
    connect(m_captureAction, &QAction::triggered, this, &TrayApp::startCapture);

    // Settings — standard macOS Preferences shortcut (⌘,). QKeySequence::Preferences
    // resolves to the platform-standard combo (⌘, on macOS; no default elsewhere)
    // and is drawn RIGHT-ALIGNED in the shortcut column automatically, matching the
    // design. Unlike the global capture hotkey this IS a real menu accelerator, so
    // it actually fires (while the menu has focus) and opens Settings — no
    // double-fire risk because nothing else listens for ⌘,.
    QAction* settings = menu->addAction(Loc::t(QStringLiteral("menu.settings")));
    settings->setShortcut(QKeySequence(QKeySequence::Preferences));
    settings->setIcon(makeMenuGlyph(MenuGlyph::Gear));
    settings->setIconVisibleInMenu(true);
    connect(settings, &QAction::triggered, this, &TrayApp::openSettings);

    menu->addSeparator();

    // Quit — standard ⌘Q (QKeySequence::Quit), right-aligned like the design and
    // wired to actually quit the app.
    QAction* quit = menu->addAction(Loc::t(QStringLiteral("menu.quit")));
    quit->setShortcut(QKeySequence(QKeySequence::Quit));
    quit->setIcon(makeMenuGlyph(MenuGlyph::Power));
    quit->setIconVisibleInMenu(true);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    return menu;
}

void TrayApp::rebuildMenu() {
    QMenu* old = m_menu;
    m_menu = buildMenu();
    delete old;     // releases the previous QActions (incl. old m_captureAction)
}

void TrayApp::onTrayActivated(int reason) {
    Q_UNUSED(reason);   // any activation (left/right/etc.) opens the menu
    if (!m_menu || !m_tray) return;
    // Show the menu CENTERED horizontally under the tray icon: the menu's center
    // sits right below the icon's center. Use the icon's screen rect when the
    // platform reports it; otherwise center on the click position (some platforms,
    // notably macOS, may not expose the status-item geometry).
    const int menuW = m_menu->sizeHint().width();
    const QRect iconRect = m_tray->geometry();
    QPoint pos;
    if (!iconRect.isEmpty())
        pos = QPoint(iconRect.center().x() - menuW / 2, iconRect.bottom() + 2);
    else
        pos = QCursor::pos() - QPoint(menuW / 2, 0);
    m_menu->popup(pos);
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
    const bool animateDim = Settings::instance().animatedDim();
    for (OverlayWindow* w : m_overlays) {
        w->show();
        w->raise();
        w->applyShieldLevel();
        // Optional smooth fade-in of the dim layer (default OFF -> instant dim).
        if (animateDim) w->startDimFadeIn();
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
    // Already idle (e.g. a second finished() arriving) -> nothing to do. The
    // m_closing guard also swallows extra finished() signals that arrive while a
    // deferred animated-dim teardown is already scheduled.
    if (m_closing) return;
    if (m_overlays.isEmpty() && !m_overlayShown && !m_isCapturing) return;

    // The actual teardown: hide + delete every overlay, restore focus, go IDLE.
    // The copy/save actions have already completed before finished() fired, so
    // nothing here is reordered relative to the capture result.
    auto teardown = [this]() {
        for (OverlayWindow* w : m_overlays) {
            w->hide();
            w->deleteLater();
        }
        m_overlays.clear();

        // Return focus to the previously-frontmost app (it re-hides its own
        // cursor in its focus handler, mirroring the game case).
        restorePreviousApp();

        m_closing = false;
        onCaptureFinished();
    };

    if (Settings::instance().animatedDim() && !m_overlays.isEmpty()) {
        // Animated path: fade the dim out on every overlay, then tear down after
        // the fade. m_closing blocks any further closeOverlays() until done.
        m_closing = true;
        for (OverlayWindow* w : m_overlays) w->startDimFadeOut();
        QTimer::singleShot(180, this, [teardown]() { teardown(); });
        return;
    }

    // Default (animatedDim off): instant teardown, byte-identical to before.
    teardown();
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
                // Keep the right-aligned-hint form ('\t') so the refreshed combo
                // stays in the shortcut column (see buildMenu()).
                m_captureAction->setText(QStringLiteral("%1\t%2").arg(
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
