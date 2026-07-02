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
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <cmath>

namespace {

// Menu-item glyphs, painted 1:1 with the user's tray-menu design (24x24 viewBox):
//   Capture  -> viewfinder (4 corner brackets + center dot)
//   Settings -> sliders (two rows, each a track + knob)
//   Power    -> power symbol (stem + ~300° ring open at the top)
// macOS has no freedesktop icon theme, so these supply the menu icons. They are
// NOT template masks: we paint an explicit themed-gray pixmap for the Normal
// state and a WHITE pixmap for the Selected/Active state, so the icon turns white
// together with the label when the row is highlighted (accent background).
enum class MenuGlyph { Capture, Settings, Power };

QPixmap paintMenuGlyphPixmap(MenuGlyph kind, const QColor& color, int side) {
    const qreal dpr = 2.0;
    QPixmap pm(int(side * dpr), int(side * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal sc = side / 24.0;     // design viewBox is 24x24
    auto PT = [&](qreal x, qreal y) { return QPointF(x * sc, y * sc); };
    QPen pen(color, 1.6 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case MenuGlyph::Capture: {
        QPainterPath tl; tl.moveTo(PT(3, 8));  tl.lineTo(PT(3, 5.5));  tl.quadTo(PT(3, 4),  PT(4.5, 4));  tl.lineTo(PT(7, 4));
        QPainterPath tr; tr.moveTo(PT(17, 4)); tr.lineTo(PT(19.5, 4)); tr.quadTo(PT(21, 4), PT(21, 5.5)); tr.lineTo(PT(21, 8));
        QPainterPath br; br.moveTo(PT(21, 16));br.lineTo(PT(21, 18.5));br.quadTo(PT(21, 20),PT(19.5, 20));br.lineTo(PT(17, 20));
        QPainterPath bl; bl.moveTo(PT(7, 20)); bl.lineTo(PT(4.5, 20));bl.quadTo(PT(3, 20), PT(3, 18.5)); bl.lineTo(PT(3, 16));
        p.drawPath(tl); p.drawPath(tr); p.drawPath(br); p.drawPath(bl);
        p.drawEllipse(PT(12, 12), 2.6 * sc, 2.6 * sc);
        break;
    }
    case MenuGlyph::Settings: {
        p.drawLine(PT(4, 7),  PT(11, 7));
        p.drawLine(PT(15, 7), PT(20, 7));
        p.drawEllipse(PT(13, 7), 2 * sc, 2 * sc);
        p.drawLine(PT(4, 17), PT(9, 17));
        p.drawLine(PT(13, 17),PT(20, 17));
        p.drawEllipse(PT(11, 17), 2 * sc, 2 * sc);
        break;
    }
    case MenuGlyph::Power: {
        // Power symbol: a vertical stem entering a ring that is OPEN AT THE TOP
        // (design M12 3v8 + M7.5 6.5 a7 7 0 1 0 9 0). Drawn as a polyline so the
        // gap is unambiguously at the top (independent of arc-angle conventions).
        p.drawLine(PT(12, 3), PT(12, 11));
        const QPointF cc = PT(12, 11.86);
        const qreal rr = 7.0 * sc;
        QPainterPath ring;
        bool first = true;
        for (int deg = -50; deg <= 230; deg += 5) {     // gap ~80° centred on top
            const qreal a = deg * M_PI / 180.0;
            const QPointF pt(cc.x() + rr * std::cos(a), cc.y() + rr * std::sin(a));
            if (first) { ring.moveTo(pt); first = false; } else ring.lineTo(pt);
        }
        p.drawPath(ring);
        break;
    }
    }
    p.end();
    return pm;
}

QIcon makeMenuGlyph(MenuGlyph kind, const QColor& normalColor) {
    const int side = 16;
    QIcon icon;
    icon.addPixmap(paintMenuGlyphPixmap(kind, normalColor, side), QIcon::Normal, QIcon::Off);
    // White for the highlighted row (Qt styles use Selected or Active for the
    // active menu item — add both so the icon whitens on hover in every style).
    const QPixmap white = paintMenuGlyphPixmap(kind, QColor(Qt::white), side);
    icon.addPixmap(white, QIcon::Selected, QIcon::Off);
    icon.addPixmap(white, QIcon::Active, QIcon::Off);
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
QString menuStyleSheet(bool dark, bool rounded) {
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

// Effective dark/light for the tray menu. The app's Appearance (auto/light/dark)
// is applied via QStyleHints::setColorScheme (see main.cpp / SettingsWindow), so
// reading colorScheme() back is the single source of truth — "auto" resolves to
// the OS scheme. This is what keeps the tray menu in sync with the app theme.
bool appIsDark() {
    if (auto* h = QGuiApplication::styleHints())
        return h->colorScheme() == Qt::ColorScheme::Dark;
    return false;
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
    applyHotkey(Settings::instance().hotKeyCode(),
                Settings::instance().hotKeyModifiers(), /*userInitiated*/ false);
}

void TrayApp::refreshCaptureShortcutLabel() {
    if (!m_captureAction) return;
    // Keep the '\t' right-aligned-hint form so the combo stays in the shortcut
    // column (see buildMenu()).
    m_captureAction->setText(QStringLiteral("%1\t%2").arg(
        Loc::t(QStringLiteral("menu.capture")),
        Settings::instance().hotKeyDisplay()));
}

void TrayApp::applyHotkey(uint32_t code, uint32_t mods, bool userInitiated) {
    if (!m_hotKey) return;

    if (m_hotKey->registerHotkey(code, mods)) {
        // Remember the working combo so a later failed change can roll back.
        m_activeHotKeyCode = code;
        m_activeHotKeyMods = mods;
        m_activeHotKeyDisplay = Settings::instance().hotKeyDisplay();
        return;
    }

    // Registration failed: the combo is owned by another app, or the key can't
    // be mapped by this platform's backend. GlobalHotkey::registerHotkey already
    // tore down any previous registration, so we must not just leave it dead.
    const QString attempted = Settings::instance().hotKeyDisplay();

    if (userInitiated && (m_activeHotKeyCode != 0 || m_activeHotKeyMods != 0)) {
        // Roll Settings + registration back to the last combo that worked.
        Settings& s = Settings::instance();
        s.setHotKeyCode(m_activeHotKeyCode);
        s.setHotKeyModifiers(m_activeHotKeyMods);
        s.setHotKeyDisplay(m_activeHotKeyDisplay);
        m_hotKey->registerHotkey(m_activeHotKeyCode, m_activeHotKeyMods);
        refreshCaptureShortcutLabel();
        if (m_settings) m_settings->refreshHotKeyDisplay();
    }

    // Tell the user (non-blocking): a modal on every launch would be obnoxious,
    // and a tray notification is enough to explain why the shortcut is inert.
    if (m_tray && QSystemTrayIcon::supportsMessages()) {
        m_tray->showMessage(QStringLiteral("LightGet"),
                            Loc::t(QStringLiteral("hotkey.conflict")).arg(attempted),
                            QSystemTrayIcon::Warning, 6000);
    }
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
    // Theme (stylesheet + icon tints) is applied by applyMenuTheme() — both here
    // at build time and again on every popup, so the menu always matches the
    // app's active light/dark scheme (fixes the wrong-theme-on-open bug).

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
    settings->setIconVisibleInMenu(true);
    connect(settings, &QAction::triggered, this, &TrayApp::openSettings);
    m_settingsAction = settings;

    menu->addSeparator();

    // Quit — standard ⌘Q (QKeySequence::Quit), right-aligned like the design and
    // wired to actually quit the app.
    QAction* quit = menu->addAction(Loc::t(QStringLiteral("menu.quit")));
    quit->setShortcut(QKeySequence(QKeySequence::Quit));
    quit->setIconVisibleInMenu(true);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
    m_quitAction = quit;

    applyMenuTheme();   // initial stylesheet + icon tints for the active theme
    return menu;
}

// Apply the active light/dark scheme to the tray menu: stylesheet + the three
// item icons' resting tint. Driven by QStyleHints::colorScheme() (the same source
// the rest of the app uses for the Appearance setting), so the menu can never
// open in a scheme that disagrees with the app theme. Re-run on every popup.
void TrayApp::applyMenuTheme() {
    if (!m_menu) return;
    const bool dark = appIsDark();
    const bool rounded = (QGuiApplication::platformName() != QLatin1String("xcb"));
    m_menu->setStyleSheet(menuStyleSheet(dark, rounded));
    // Resting icon tint (themed gray); the highlighted row swaps to white
    // automatically via the icon's Selected/Active pixmaps (see makeMenuGlyph).
    const QColor iconColor = dark ? QColor("#c7c7cc") : QColor("#4a4a4f");
    if (m_captureAction)  m_captureAction->setIcon(makeMenuGlyph(MenuGlyph::Capture, iconColor));
    if (m_settingsAction) m_settingsAction->setIcon(makeMenuGlyph(MenuGlyph::Settings, iconColor));
    if (m_quitAction)     m_quitAction->setIcon(makeMenuGlyph(MenuGlyph::Power, iconColor));
}

void TrayApp::rebuildMenu() {
    QMenu* old = m_menu;
    m_menu = buildMenu();
    delete old;     // releases the previous QActions (incl. old m_captureAction)
}

void TrayApp::onTrayActivated(int reason) {
    Q_UNUSED(reason);   // any activation (left/right/etc.) opens the menu
    if (!m_menu || !m_tray) return;
    applyMenuTheme();   // re-sync to the current theme before showing
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

#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    // LSUIElement (accessory) app: it is NOT the active app on a mere status-item
    // click, and onTrayActivated runs INSIDE macOS's status-item mouse-tracking
    // handler. The tray menu is a styled Qt QMenu — a real frameless/translucent
    // NSWindow, NOT a native NSMenu — so if we order it front right now it never
    // becomes key, and the FIRST click only dismisses it without firing ANY
    // QAction (the "first tray action does nothing" bug — hit Capture, Settings
    // AND Quit). activateIgnoringOtherApps: only POSTS the activation; it settles
    // once the tracking handler unwinds. So activate NOW and pop the menu one
    // event-loop turn later (singleShot 0): by then the app is active and the
    // popup's window becomes key on creation, so the first click fires. raise() +
    // activateWindow() force-key it as belt-and-suspenders (mirrors the overlay's
    // makeKeyAndOrderFront). Preserves the styled, centered QMenu (no native-NSMenu
    // regression) and matches the system-activated native Swift menu.
    extern void MacNative_activateApp();   // implemented in MacNative.mm
    MacNative_activateApp();
    QTimer::singleShot(0, this, [this, pos]() {
        if (!m_menu) return;
        m_menu->popup(pos);
        m_menu->raise();
        m_menu->activateWindow();
    });
#else
    m_menu->popup(pos);
#endif
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

    // Tear the status-item menu down synchronously before grabbing pixels or
    // showing the shield. When a capture is launched from the tray menu the menu
    // popup is still on screen as triggered() fires; hiding it now keeps the
    // translucent popup out of the grabbed screenshot AND removes the transient
    // popup window that otherwise races the overlay's app-activation on the very
    // first capture (the "first menu capture does nothing" bug). Harmless no-op
    // for the hotkey path (no menu is open) and off macOS.
    if (m_menu) m_menu->hide();

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
    //
    // The same next-tick pass ALSO re-asserts the active overlay's shield level +
    // key/active state. The very first capture launched from the status-item menu
    // can land while the app is not yet frontmost (an LSUIElement accessory app is
    // not active on a mere menu click) and/or while the just-dismissed popup is
    // still being torn down, so the initial activateIgnoringOtherApps +
    // makeKeyAndOrderFront (in applyShieldLevel) gets dropped and the overlay never
    // becomes key — i.e. the first menu capture appears to do nothing. Re-running
    // applyShieldLevel() + focus on the next event-loop turn (a clean context, the
    // menu fully gone) self-heals that miss without needing a second click. Only
    // the cursor overlay is re-asserted, so multi-monitor keyboard focus still
    // lands on the screen under the pointer. m_overlays.contains() guards against
    // the gesture having already finished (overlays torn down) before the tick.
    forceCursorVisible();
    QTimer::singleShot(0, this, [this, active]() {
        forceCursorVisible();
        if (active && m_overlays.contains(active)) {
            active->applyShieldLevel();
            active->activateWindow();
            active->raise();
            active->setFocus();
        }
    });

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
            // applyHotkey surfaces a conflict and rolls back to the last working
            // combo on failure, so the user is never silently left hotkey-less.
            applyHotkey(Settings::instance().hotKeyCode(),
                        Settings::instance().hotKeyModifiers(), /*userInitiated*/ true);
            refreshCaptureShortcutLabel();
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
