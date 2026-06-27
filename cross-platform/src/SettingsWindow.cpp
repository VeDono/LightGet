// SettingsWindow.cpp — Tabbed settings window (General + Features) + hotkey
// recorder. Faithful Qt6 port of SettingsWindowController.swift, restyled to the
// "LightGet — Direction B" design (settings_window.dc.html / settings_canvas.html).
//
// STRUCTURE (Direction B):
//   window  -> title-row handled by the native title bar
//           -> centered rounded tab buttons (General / Features) over a
//              QStackedWidget (instant switch).
//   General -> one white "card" with flat, left-aligned rows
//              (Language, Menu-bar icon, Shortcut, Save folder, Dim level,
//               Animated dimming, Downscale, Launch at login) + the About card +
//               copyright/version footer.
//   Features-> grouped section cards (Tools / Interface / Text) with painted
//              checkbox rows.
//
// Colors come from a palette-derived token set (DesignTokens) so LIGHT and DARK
// both look right; the accent is #007AFF (light) / #0A84FF (dark) per the design.
// Every setting, signal/slot, handler and localization string is preserved — only
// layout/styling changed.

#include "SettingsWindow.h"

#include "Settings.h"
#include "Localization.h"
#include "Annotation.h"

#include <QApplication>
#include <QStackedWidget>
#include <QWidget>
#include <QLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QSlider>
#include <QFrame>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QResizeEvent>
#include <QDate>
#include <QScreen>
#include <QGuiApplication>
#include <QCursor>
#include <QSignalBlocker>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QSizePolicy>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QEvent>
#include <QStyle>
#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// Local helpers / data
// ---------------------------------------------------------------------------
namespace {

// Languages: (title, code). English first (default language).
struct LangEntry { const char* title; const char* code; };
const std::vector<LangEntry> kLanguages = {
    {"English", "en"}, {"Русский", "ru"}, {"Українська", "uk"}
};

// Preset bar icons (asset names mirroring SF Symbols), exactly 5 (Spec 5 §5.2).
const QStringList kBarIcons = {
    "scissors", "camera.viewfinder", "crop", "rectangle.dashed",
    "paintbrush.pointed.fill"
};

// Tools shown on the Features tab (Select is always-on, never listed).
const std::vector<Tool> kFeatureTools = {
    Tool::Arrow, Tool::Line, Tool::Rectangle, Tool::FilledRect,
    Tool::Pen, Tool::Text
};

// Right-column control width from the design tokens (240px field width).
constexpr int kFieldWidth   = 240;
// Trailing reset-slot width (keeps rows aligned whether a reset is present).
constexpr int kResetSlot    = 28;
// Card row content padding (11px vertical / 14px horizontal in the design).
constexpr int kRowPadH      = 14;
constexpr int kRowPadV      = 9;
constexpr int kLabelGap     = 14;

QString colCss(const QColor& c) {
    // rgba() so alpha survives in QSS strings.
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// Recolor an alpha-only (white-on-transparent) glyph pixmap to a solid `tint`,
// preserving the alpha channel. The bundled assets are white-on-transparent
// (see assets.qrc), invisible on the LIGHT settings background — this stamps
// them in a readable color (icon/text token) instead.
QPixmap tintPixmap(const QPixmap& src, const QColor& tint) {
    if (src.isNull()) return src;
    QPixmap out(src.size());
    out.setDevicePixelRatio(src.devicePixelRatio());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPixmap(0, 0, src);                       // lay down the alpha shape
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), tint);                  // recolor opaque areas to tint
    p.end();
    return out;
}

// Resolve a preset bar-icon asset to a QIcon. SF Symbols are macOS-only, so we
// load a bundled replacement asset from the qrc; fall back to a themed icon,
// then an empty icon, so the UI still builds.
QIcon barIconAsset(const QString& name) {
    QIcon icon(QStringLiteral(":/assets/%1.png").arg(name));
    if (!icon.isNull()) return icon;
    QIcon theme = QIcon::fromTheme(name);
    if (!theme.isNull()) return theme;
    return QIcon();
}

// Same as barIconAsset() but recolored to a tint visible on the settings
// background. The qrc PNGs are white-on-transparent (invisible by default), so
// the menu-bar-icon chooser / reset glyphs MUST be tinted. Renders a crisp 2x
// pixmap so the recolor stays sharp on Retina. `size` is in points.
QIcon barIconAssetTinted(const QString& name, const QColor& tint, int size = 18) {
    QIcon base = barIconAsset(name);
    if (base.isNull()) {
        QIcon theme = QIcon::fromTheme(name);
        if (!theme.isNull()) base = theme;
    }
    if (base.isNull()) return QIcon();
    const qreal dpr = 2.0;
    QPixmap pm = base.pixmap(QSize(int(size * dpr), int(size * dpr)));
    pm.setDevicePixelRatio(dpr);
    return QIcon(tintPixmap(pm, tint));
}

// Open a URL in the default browser (mirrors NSWorkspace.shared.open).
void openExternal(const QString& url) {
    QDesktopServices::openUrl(QUrl(url));
}

// ---- Social card link button (blue link text, pointing-hand cursor) --------
// Mirrors Swift LinkButton: link-colored text, opens URL on click; dims on hover
// (the design's link :hover -> opacity 0.65).
class LinkButton : public QLabel {
public:
    LinkButton(const QString& text, const QString& url, const QColor& linkColor,
               Qt::Alignment align, QWidget* parent = nullptr)
        : QLabel(parent), m_url(url), m_link(linkColor) {
        setText(text);
        setAlignment(align | Qt::AlignVCenter);
        setCursor(Qt::PointingHandCursor);
        setTextInteractionFlags(Qt::NoTextInteraction);
        applyColor(false);
    }
protected:
    void enterEvent(QEnterEvent*) override { applyColor(true); }
    void leaveEvent(QEvent*) override { applyColor(false); }
    void mousePressEvent(QMouseEvent*) override { openExternal(m_url); }
private:
    void applyColor(bool hover) {
        QColor c = m_link;
        if (hover) c.setAlphaF(0.65);
        setStyleSheet(QStringLiteral("color:%1;").arg(colCss(c)));
    }
    QString m_url;
    QColor  m_link;
};

// ---- Inline copyright name link --------------------------------------------
// Mirrors Swift InlineLinkField: gray + underlined, blue on hover, opens URL.
class InlineLinkLabel : public QLabel {
public:
    InlineLinkLabel(const QString& text, const QString& url, int sizePt,
                    const QColor& base, const QColor& linkColor,
                    QWidget* parent = nullptr)
        : QLabel(text, parent), m_url(url), m_base(base), m_link(linkColor) {
        QFont f = font();
        f.setPointSize(sizePt);
        f.setUnderline(true);
        setFont(f);
        setCursor(Qt::PointingHandCursor);
        applyColor(false);
    }
protected:
    void enterEvent(QEnterEvent*) override { applyColor(true); }
    void leaveEvent(QEvent*) override { applyColor(false); }
    void mousePressEvent(QMouseEvent*) override { openExternal(m_url); }
private:
    void applyColor(bool hover) {
        setStyleSheet(QStringLiteral("color:%1;")
                          .arg(colCss(hover ? m_link : m_base)));
    }
    QString m_url;
    QColor  m_base, m_link;
};

// Version + edition (set as compile definitions in CMakeLists.txt). Defaults
// keep the file self-contained if a definition is ever missing.
#ifndef LIGHTGET_VERSION
#define LIGHTGET_VERSION "1.0.0"
#endif
#ifndef LIGHTGET_EDITION
#define LIGHTGET_EDITION "Cross-platform (Qt 6)"
#endif

} // namespace

// ===========================================================================
// ToggleSwitch — painted on/off pill switch
// ===========================================================================
ToggleSwitch::ToggleSwitch(const DesignTokens& tk, QWidget* parent)
    : QAbstractButton(parent), m_tk(tk) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(38, 22);
    m_knobPos = isChecked() ? 1.0 : 0.0;
    // Animate the knob slide on every toggle (180ms ease-out per the design).
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        auto* anim = new QPropertyAnimation(this, "knobPos", this);
        anim->setDuration(180);
        anim->setStartValue(m_knobPos);
        anim->setEndValue(on ? 1.0 : 0.0);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    });
}

QSize ToggleSwitch::sizeHint() const { return QSize(38, 22); }

void ToggleSwitch::setKnobPos(qreal v) {
    m_knobPos = v;
    update();
}

void ToggleSwitch::enterEvent(QEnterEvent*) { m_hover = true; update(); }
void ToggleSwitch::leaveEvent(QEvent*)      { m_hover = false; update(); }

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2.0;

    // Track: off -> toggleOff, on -> toggleOn (accent). Slight dim when disabled.
    QColor track = (m_knobPos > 0.5) ? m_tk.toggleOn : m_tk.toggleOff;
    // Interpolate so the color crossfades with the knob during the slide.
    track = QColor::fromRgbF(
        m_tk.toggleOff.redF()   + (m_tk.toggleOn.redF()   - m_tk.toggleOff.redF())   * m_knobPos,
        m_tk.toggleOff.greenF() + (m_tk.toggleOn.greenF() - m_tk.toggleOff.greenF()) * m_knobPos,
        m_tk.toggleOff.blueF()  + (m_tk.toggleOn.blueF()  - m_tk.toggleOff.blueF())  * m_knobPos);
    if (!isEnabled()) track.setAlphaF(0.4);
    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(r, radius, radius);

    // Knob: 18px circle, slides from left(2px) to right.
    const qreal knobD = 18.0;
    const qreal travel = r.width() - knobD - 4.0;   // 2px inset each side
    const qreal kx = r.left() + 2.0 + travel * m_knobPos;
    const qreal ky = r.top() + (r.height() - knobD) / 2.0;
    QRectF knob(kx, ky, knobD, knobD);

    // Soft drop shadow under the knob.
    p.setBrush(QColor(0, 0, 0, 45));
    p.drawEllipse(knob.translated(0, 0.8));
    p.setBrush(m_tk.knob);
    p.drawEllipse(knob);
}

// ===========================================================================
// CheckRow — clickable row with a painted rounded checkbox + label + trailing
// ===========================================================================
CheckRow::CheckRow(const DesignTokens& tk, const QString& label,
                   bool checkedState, QWidget* trailing, QWidget* parent)
    : QAbstractButton(parent), m_tk(tk), m_label(label), m_trailing(trailing) {
    setCheckable(true);
    setChecked(checkedState);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setMinimumHeight(38);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (m_trailing) {
        m_trailing->setParent(this);
        m_trailing->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    connect(this, &QAbstractButton::toggled, this, [this]() { update(); });
}

QSize CheckRow::sizeHint() const { return QSize(260, 38); }

void CheckRow::enterEvent(QEnterEvent*) { m_hover = true; update(); }
void CheckRow::leaveEvent(QEvent*)      { m_hover = false; update(); }

void CheckRow::resizeEvent(QResizeEvent*) {
    if (m_trailing) {
        QSize sz = m_trailing->sizeHint();
        if (!sz.isValid() || sz.isEmpty()) sz = m_trailing->size();
        const int x = width() - kRowPadH - sz.width();
        const int y = (height() - sz.height()) / 2;
        m_trailing->setGeometry(x, y, sz.width(), sz.height());
    }
}

void CheckRow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Hover wash across the whole row (accent-weak), as in the design.
    if (m_hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_tk.accentWeak);
        p.drawRoundedRect(rect().adjusted(2, 1, -2, -1), 7, 7);
    }

    // Checkbox: 20px rounded square. On -> accent fill + white check;
    // off -> bordered empty box.
    const int box = 20;
    const int bx = kRowPadH;
    const int by = (height() - box) / 2;
    QRectF boxR(bx, by, box, box);
    if (isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_tk.checkOn);
        p.drawRoundedRect(boxR, 6, 6);
        // Checkmark.
        QPen cp(Qt::white, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(cp);
        QPainterPath path;
        path.moveTo(bx + box * 0.24, by + box * 0.52);
        path.lineTo(bx + box * 0.43, by + box * 0.72);
        path.lineTo(bx + box * 0.78, by + box * 0.30);
        p.drawPath(path);
    } else {
        QPen bp(m_tk.dark ? m_tk.border : QColor(0xc4, 0xc4, 0xc9), 1.5);
        p.setPen(bp);
        p.setBrush(m_tk.control);
        p.drawRoundedRect(boxR.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

    // Label.
    p.setPen(m_tk.text);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() > 0 ? f.pointSizeF() : 10.0);
    p.setFont(f);
    const int textX = bx + box + 11;
    int textRight = width() - kRowPadH;
    if (m_trailing) textRight = m_trailing->x() - 10;
    QRect textR(textX, 0, textRight - textX, height());
    p.drawText(textR, Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(m_label, Qt::ElideRight, textR.width()));
}

// ===========================================================================
// HotkeyRecorder
// ===========================================================================
HotkeyRecorder::HotkeyRecorder(QWidget* parent) : QPushButton(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCheckable(false);
    setCursor(Qt::PointingHandCursor);
    connect(this, &QPushButton::clicked, this, &HotkeyRecorder::startRecording);
}

// Style the recorder field to match the surrounding card (control fill, rounded
// 8px, accent border on hover/focus, accent focus ring on recording) using the
// resolved theme tokens. Called from buildUI() so light/dark both look right.
void HotkeyRecorder::applyTokens(const DesignTokens& tk) {
    const QString acc      = colCss(tk.accent);
    const QString accWeak  = colCss(tk.accentWeak);
    const QString border   = colCss(tk.border);
    const QString control  = colCss(tk.control);
    const QString text     = colCss(tk.text);

    m_idleStyle = QStringLiteral(
        "HotkeyRecorder {"
        " background-color:%1; color:%2;"
        " border:1px solid %3; border-radius:8px;"
        " padding:4px 10px; font-size:14px; text-align:center; }"
        "HotkeyRecorder:hover { border-color:%4; }")
        .arg(control, text, border, acc);

    m_recordingStyle = QStringLiteral(
        "HotkeyRecorder {"
        " background-color:%1; color:%2;"
        " border:1px solid %3; border-radius:8px;"
        " padding:4px 10px; font-size:14px; text-align:center;"
        " outline:none; }")
        .arg(control, text, acc);
    Q_UNUSED(accWeak);

    setStyleSheet(m_recording ? m_recordingStyle : m_idleStyle);
}

void HotkeyRecorder::startRecording() {
    m_recording = true;
    setStyleSheet(m_recordingStyle);
    setText(Loc::t("recorder.press"));
    setFocus(Qt::OtherFocusReason);
}

void HotkeyRecorder::keyPressEvent(QKeyEvent* event) {
    if (!m_recording) { QPushButton::keyPressEvent(event); return; }

    // Esc — cancel recording, restore previous display + idle style.
    if (event->key() == Qt::Key_Escape) {
        m_recording = false;
        setStyleSheet(m_idleStyle);
        setText(Settings::instance().hotKeyDisplay());
        return;
    }

    // Ignore lone modifier presses; wait for an actual key.
    const int k = event->key();
    if (k == Qt::Key_Control || k == Qt::Key_Shift || k == Qt::Key_Alt ||
        k == Qt::Key_Meta || k == Qt::Key_AltGr || k == 0 ||
        k == Qt::Key_unknown) {
        return;
    }

    const Qt::KeyboardModifiers mods = event->modifiers();
    const uint32_t carbonMods = carbonModifiers(mods);
    if (carbonMods == 0) {            // at least one modifier required
        QApplication::beep();
        return;                      // keep recording
    }

    // Key text ignoring modifiers, uppercased (charactersIgnoringModifiers).
    QString keyText = event->text();
    if (!keyText.isEmpty() && keyText.at(0).isPrint())
        keyText = keyText.toUpper();
    else
        keyText = QKeySequence(k).toString(QKeySequence::NativeText).toUpper();

    const uint32_t carbonCode = carbonKeyCode(k, event->nativeVirtualKey());
    const QString display = displayString(mods, keyText);

    m_recording = false;
    setStyleSheet(m_idleStyle);
    setText(display);
    emit captured(carbonCode, carbonMods, display);
}

uint32_t HotkeyRecorder::carbonModifiers(Qt::KeyboardModifiers mods) {
    using namespace CarbonKeys;
    uint32_t m = 0;
    // Qt::ControlModifier maps to Cmd on macOS, Ctrl elsewhere — treat it as the
    // command-key for persistence parity with the Swift app's cmdKey.
    if (mods & Qt::ControlModifier) m |= cmdKey;
    if (mods & Qt::AltModifier)     m |= optionKey;
    if (mods & Qt::MetaModifier)    m |= controlKey;
    if (mods & Qt::ShiftModifier)   m |= shiftKey;
    return m;
}

QString HotkeyRecorder::displayString(Qt::KeyboardModifiers mods,
                                      const QString& keyText) {
    // Delegate to the shared, platform-aware formatter so the recorder, the reset
    // action, and the default all render identically. Convert Qt modifiers to the
    // Carbon mask first (the same mask that is persisted/registered): on macOS the
    // result is glyphs (⌃⌥⇧⌘), on Windows/Linux spelled-out names (Ctrl/Alt/Shift)
    // joined with "+", matching the actually-registered keys.
    return Settings::hotKeyDisplayString(carbonModifiers(mods), keyText);
}

// Translate a Qt key (+ native VK fallback) to a Carbon virtual-key code so the
// persisted value matches the macOS app's defaults file (kVK_* constants).
uint32_t HotkeyRecorder::carbonKeyCode(int qtKey, quint32 nativeVK) {
#ifdef Q_OS_MAC
    // On macOS, nativeVirtualKey() already IS the Carbon kVK_* code.
    if (nativeVK != 0) return nativeVK;
#else
    Q_UNUSED(nativeVK);
#endif
    // Portable Qt::Key -> Carbon kVK_ANSI_* table (ANSI US layout positions).
    switch (qtKey) {
    case Qt::Key_A: return 0x00; case Qt::Key_B: return 0x0B;
    case Qt::Key_C: return 0x08; case Qt::Key_D: return 0x02;
    case Qt::Key_E: return 0x0E; case Qt::Key_F: return 0x03;
    case Qt::Key_G: return 0x05; case Qt::Key_H: return 0x04;
    case Qt::Key_I: return 0x22; case Qt::Key_J: return 0x26;
    case Qt::Key_K: return 0x28; case Qt::Key_L: return 0x25;
    case Qt::Key_M: return 0x2E; case Qt::Key_N: return 0x2D;
    case Qt::Key_O: return 0x1F; case Qt::Key_P: return 0x23;
    case Qt::Key_Q: return 0x0C; case Qt::Key_R: return 0x0F;
    case Qt::Key_S: return 0x01; case Qt::Key_T: return 0x11;
    case Qt::Key_U: return 0x20; case Qt::Key_V: return 0x09;
    case Qt::Key_W: return 0x0D; case Qt::Key_X: return 0x07;
    case Qt::Key_Y: return 0x10; case Qt::Key_Z: return 0x06;
    case Qt::Key_1: return 0x12; case Qt::Key_2: return 0x13;
    case Qt::Key_3: return 0x14; case Qt::Key_4: return 0x15;
    case Qt::Key_5: return 0x17; case Qt::Key_6: return 0x16;
    case Qt::Key_7: return 0x1A; case Qt::Key_8: return 0x1C;
    case Qt::Key_9: return 0x19; case Qt::Key_0: return 0x1D;
    case Qt::Key_Space:  return 0x31;
    case Qt::Key_Return: return 0x24;
    case Qt::Key_Tab:    return 0x30;
    case Qt::Key_Escape: return CarbonKeys::kVK_Escape;
    case Qt::Key_Minus:  return 0x1B;
    case Qt::Key_Equal:  return 0x18;
    case Qt::Key_BracketLeft:  return 0x21;
    case Qt::Key_BracketRight: return 0x1E;
    case Qt::Key_Backslash:    return 0x2A;
    case Qt::Key_Semicolon:    return 0x29;
    case Qt::Key_Apostrophe:   return 0x27;
    case Qt::Key_Comma:        return 0x2B;
    case Qt::Key_Period:       return 0x2F;
    case Qt::Key_Slash:        return 0x2C;
    case Qt::Key_QuoteLeft:    return 0x32;
    case Qt::Key_F1: return 0x7A; case Qt::Key_F2: return 0x78;
    case Qt::Key_F3: return 0x63; case Qt::Key_F4: return 0x76;
    case Qt::Key_F5: return 0x60; case Qt::Key_F6: return 0x61;
    case Qt::Key_F7: return 0x62; case Qt::Key_F8: return 0x64;
    case Qt::Key_F9: return 0x65; case Qt::Key_F10: return 0x6D;
    case Qt::Key_F11: return 0x67; case Qt::Key_F12: return 0x6F;
    default: break;
    }
    // Unknown key: persist the native VK if any, else 0.
    return static_cast<uint32_t>(nativeVK);
}

// ===========================================================================
// SettingsWindow
// ===========================================================================

SettingsWindow::SettingsWindow(QWidget* parent) : QDialog(parent) {
    // Fixed-size window, not resizable, persists across open/close (the owner
    // keeps the instance alive). Title bar with close only. Sized to the design's
    // 500-wide layout; tall enough that the grouped cards + About footer breathe.
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setFixedSize(500, 700);

    m_recorder = new HotkeyRecorder(this);   // reused member across rebuilds
    connect(m_recorder, &HotkeyRecorder::captured, this,
            [this](uint32_t code, uint32_t mods, const QString& display) {
        Settings& s = Settings::instance();
        s.setHotKeyCode(code);
        s.setHotKeyModifiers(mods);
        s.setHotKeyDisplay(display);
        emit hotKeyChanged();
    });

    buildUI();
}

void SettingsWindow::showCentered() {
    // Center on the screen under the cursor (accessory-app focus dance).
    if (QScreen* scr = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect g = scr->availableGeometry();
        move(g.center() - rect().center());
    }
    show();
    raise();
    activateWindow();
}

void SettingsWindow::changeEvent(QEvent* e) {
    QDialog::changeEvent(e);
    // The OS switched light<->dark (or the app palette changed): rebuild so all
    // painted widgets (toggles, checkboxes, cards) pick up the new tokens. Guard
    // against rebuilding during teardown.
    if (e->type() == QEvent::PaletteChange ||
        e->type() == QEvent::ApplicationPaletteChange) {
        if (m_stack) reloadUI();
    }
}

// ---------------------------------------------------------------------------
// Theme tokens
// ---------------------------------------------------------------------------
void SettingsWindow::resolveTokens() {
    // Decide light vs. dark from the window color's lightness (works on every
    // platform without an explicit app theme setting).
    const QColor win = palette().color(QPalette::Window);
    const bool dark = win.lightness() < 128;
    DesignTokens& t = m_tk;
    t.dark = dark;
    if (dark) {
        t.bg          = QColor("#1a1a1c");
        t.card        = QColor("#262629");
        t.control     = QColor("#2f2f33");
        t.controlFill = QColor("#37373b");
        t.border      = QColor("#3a3a3e");
        t.separator   = QColor("#343438");
        t.text        = QColor("#f2f2f4");
        t.text2       = QColor("#98989e");
        t.text3       = QColor("#6e6e75");
        t.accent      = QColor("#0a84ff");
        t.accentWeak  = QColor(10, 132, 255, 61);   // rgba(10,132,255,0.24)
        t.link        = QColor("#3b9cff");
        t.toggleOff   = QColor("#48484c");
        t.toggleOn    = QColor("#0a84ff");
        t.knob        = QColor("#ffffff");
        t.resetFg     = QColor("#8e8e95");
        t.icon        = QColor("#c7c7cc");
        t.checkOn     = QColor("#0a84ff");
    } else {
        t.bg          = QColor("#f3f3f5");
        t.card        = QColor("#ffffff");
        t.control     = QColor("#ffffff");
        t.controlFill = QColor("#eef0f2");
        t.border      = QColor("#dcdce0");
        t.separator   = QColor("#e8e8eb");
        t.text        = QColor("#1d1d1f");
        t.text2       = QColor("#6e6e73");
        t.text3       = QColor("#9b9ba1");
        t.accent      = QColor("#007aff");
        t.accentWeak  = QColor(0, 122, 255, 31);    // rgba(0,122,255,0.12)
        t.link        = QColor("#007aff");
        t.toggleOff   = QColor("#d9d9de");
        t.toggleOn    = QColor("#007aff");
        t.knob        = QColor("#ffffff");
        t.resetFg     = QColor("#8a8a8f");
        t.icon        = QColor("#44454a");
        t.checkOn     = QColor("#007aff");
    }
}

// ---------------------------------------------------------------------------
// Reusable styled pieces
// ---------------------------------------------------------------------------
QFrame* SettingsWindow::makeCard() {
    auto* card = new QFrame;
    card->setObjectName("card");
    card->setStyleSheet(QStringLiteral(
        "#card { background-color:%1; border:1px solid %2; border-radius:10px; }")
        .arg(colCss(m_tk.card), colCss(m_tk.border)));
    return card;
}

QFrame* SettingsWindow::makeSeparator() {
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background-color:%1; border:none;")
                            .arg(colCss(m_tk.separator)));
    return line;
}

QWidget* SettingsWindow::makeRow(const QString& label, QWidget* control,
                                 QPushButton* reset) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(kRowPadH, kRowPadV, kRowPadH, kRowPadV);
    h->setSpacing(kLabelGap);

    auto* lab = new QLabel(label);
    lab->setStyleSheet(QStringLiteral("color:%1; font-size:13px;")
                           .arg(colCss(m_tk.text)));
    h->addWidget(lab, 0, Qt::AlignVCenter);
    h->addStretch(1);
    h->addWidget(control, 0, Qt::AlignVCenter | Qt::AlignRight);

    // Trailing reset slot (fixed width so rows with/without a reset line up).
    auto* slot = new QWidget;
    slot->setFixedWidth(kResetSlot);
    auto* sh = new QHBoxLayout(slot);
    sh->setContentsMargins(0, 0, 0, 0);
    if (reset) sh->addWidget(reset, 0, Qt::AlignCenter);
    h->addWidget(slot, 0, Qt::AlignVCenter);

    return row;
}

QWidget* SettingsWindow::makeToggleRow(const QString& label, QWidget* toggle,
                                       QPushButton* reset) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(kRowPadH, kRowPadV, kRowPadH, kRowPadV);
    h->setSpacing(kLabelGap);

    auto* lab = new QLabel(label);
    lab->setWordWrap(true);
    lab->setStyleSheet(QStringLiteral("color:%1; font-size:13px;")
                           .arg(colCss(m_tk.text)));
    h->addWidget(lab, 1, Qt::AlignVCenter);
    h->addWidget(toggle, 0, Qt::AlignVCenter);

    auto* slot = new QWidget;
    slot->setFixedWidth(kResetSlot);
    auto* sh = new QHBoxLayout(slot);
    sh->setContentsMargins(0, 0, 0, 0);
    if (reset) sh->addWidget(reset, 0, Qt::AlignCenter);
    h->addWidget(slot, 0, Qt::AlignVCenter);

    return row;
}

void SettingsWindow::reloadUI() {
    // Full teardown + rebuild of both tabs. The recorder is a reused member
    // (reparented into the rebuilt tab), so detach it before tearing down so it
    // survives deletion of the old stack.
    if (m_recorder) m_recorder->setParent(this);
    if (m_stack) {
        m_stack->hide();
        m_stack->setParent(nullptr);
        m_stack->deleteLater();
        m_stack = nullptr;
    }
    // The tab buttons live in the root layout; clearing the layout below deletes
    // their container, so drop our dangling pointers.
    m_tabGeneral = nullptr;
    m_tabFeatures = nullptr;
    if (QLayout* old = layout()) {
        delete old;                   // detaches; recorder reparented above
    }
    buildUI();
}

void SettingsWindow::buildUI() {
    setWindowTitle(Loc::t("settings.title"));
    resolveTokens();

    // Window background = panel bg token. (Most platforms already match via the
    // system palette, but set it so the card/border contrast is exact.)
    setStyleSheet(QStringLiteral("SettingsWindow { background-color:%1; }")
                      .arg(colCss(m_tk.bg)));

    // ---- Centered rounded tab buttons (General / Features) ----
    auto* tabBar = new QWidget;
    auto* tabRow = new QHBoxLayout(tabBar);
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(6);

    auto makeTab = [&](const QString& text) {
        auto* b = new QPushButton(text);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(QStringLiteral(
            "QPushButton {"
            " padding:7px 26px; font-size:13px; font-weight:600;"
            " color:%1; background-color:%2;"
            " border:1px solid %3; border-radius:9px; }"
            "QPushButton:hover:!checked { color:%4; }"
            "QPushButton:checked {"
            " color:%5; background-color:%6;"
            " border:1px solid %7; }")
            .arg(colCss(m_tk.text2),          // 1 inactive text
                 colCss(m_tk.dark ? QColor("#202023") : QColor("#e7e7ea")),  // 2 inactive bg
                 colCss(m_tk.border),         // 3 border
                 colCss(m_tk.text),           // 4 hover text
                 colCss(m_tk.accent),         // 5 active text (accent)
                 colCss(m_tk.accentWeak),     // 6 active bg
                 colCss(m_tk.accent)));       // 7 active border
        return b;
    };
    m_tabGeneral  = makeTab(Loc::t("tab.general"));
    m_tabFeatures = makeTab(Loc::t("tab.features"));

    auto* tabGroup = new QButtonGroup(tabBar);
    tabGroup->setExclusive(true);
    tabGroup->addButton(m_tabGeneral, 0);
    tabGroup->addButton(m_tabFeatures, 1);

    tabRow->addStretch(1);
    tabRow->addWidget(m_tabGeneral);
    tabRow->addWidget(m_tabFeatures);
    tabRow->addStretch(1);

    // ---- Stacked pages inside a rounded panel ----
    m_stack = new QStackedWidget;
    m_stack->setObjectName("pagePane");
    m_stack->setStyleSheet(QStringLiteral(
        "#pagePane { background-color:%1; border:1px solid %2; border-radius:10px; }")
        .arg(colCss(m_tk.bg), colCss(m_tk.border)));
    m_stack->addWidget(buildGeneralTab());
    m_stack->addWidget(buildFeaturesTab());

    connect(tabGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_currentTab = id;
        m_stack->setCurrentIndex(id);
    });

    // Restore the previously selected tab across rebuilds.
    if (m_currentTab == 1) { m_tabFeatures->setChecked(true); m_stack->setCurrentIndex(1); }
    else                   { m_tabGeneral->setChecked(true);  m_stack->setCurrentIndex(0); }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);
    root->addWidget(tabBar);
    root->addWidget(m_stack, 1);
}

// ---------------------------------------------------------------------------
// General tab
// ---------------------------------------------------------------------------
QWidget* SettingsWindow::buildGeneralTab() {
    Settings& s = Settings::instance();

    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 20, 20, 18);
    outer->setSpacing(0);

    // The settings card holds every General row, separated by 1px lines.
    auto* card = makeCard();
    auto* col = new QVBoxLayout(card);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    bool firstRow = true;
    auto addRowWidget = [&](QWidget* row) {
        if (!firstRow) col->addWidget(makeSeparator());
        firstRow = false;
        col->addWidget(row);
    };

    // --- Language (combo, no reset) ---
    auto* langCombo = new QComboBox;
    for (const auto& l : kLanguages) langCombo->addItem(QString::fromUtf8(l.title));
    int langIdx = 0;
    for (size_t i = 0; i < kLanguages.size(); ++i)
        if (s.language() == QString::fromUtf8(kLanguages[i].code)) { langIdx = int(i); break; }
    langCombo->setCurrentIndex(langIdx);
    langCombo->setMinimumWidth(150);
    langCombo->setFixedHeight(30);
    langCombo->setCursor(Qt::PointingHandCursor);
    langCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background-color:%1; color:%2; border:1px solid %3;"
        " border-radius:8px; padding:0 8px 0 12px; font-size:13px; }"
        "QComboBox:hover { border-color:%4; }"
        "QComboBox::drop-down { border:none; width:20px; }"
        "QComboBox QAbstractItemView { background-color:%1; color:%2;"
        " border:1px solid %3; selection-background-color:%5;"
        " selection-color:%2; }")
        .arg(colCss(m_tk.control), colCss(m_tk.text), colCss(m_tk.border),
             colCss(m_tk.accent), colCss(m_tk.accentWeak)));
    connect(langCombo, QOverload<int>::of(&QComboBox::activated),
            this, &SettingsWindow::onLanguageChanged);
    addRowWidget(makeRow(Loc::t("settings.language"), langCombo, nullptr));

    // --- Menu-bar icon: segmented preset chooser + separate custom-image button ---
    {
        auto* iconWrap = new QWidget;
        auto* h = new QHBoxLayout(iconWrap);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);

        // Segmented group: 5 presets joined in one rounded, bordered strip.
        auto* segStrip = new QWidget;
        segStrip->setObjectName("segStrip");
        segStrip->setStyleSheet(QStringLiteral(
            "#segStrip { background-color:%1; border:1px solid %2; border-radius:8px; }")
            .arg(colCss(m_tk.control), colCss(m_tk.border)));
        auto* sh = new QHBoxLayout(segStrip);
        sh->setContentsMargins(0, 0, 0, 0);
        sh->setSpacing(0);

        // Segment buttons: tinted to the icon token (visible on both themes); the
        // selected one gets the accent-weak wash + accent glyph.
        const QString segStyle = QStringLiteral(
            "QToolButton { border:none; border-right:1px solid %1;"
            " background:transparent; }"
            "QToolButton:hover { background-color:%2; }"
            "QToolButton:checked { background-color:%2; }")
            .arg(colCss(m_tk.border), colCss(m_tk.accentWeak));

        auto* group = new QButtonGroup(segStrip);
        group->setExclusive(true);
        const bool usingCustom = s.barIconCustomPath().has_value();
        int selectedSeg = -1;
        if (!usingCustom) {
            selectedSeg = kBarIcons.indexOf(s.barIcon());
            if (selectedSeg < 0) selectedSeg = 0;
        }
        for (int i = 0; i < kBarIcons.size(); ++i) {
            auto* b = new QToolButton(segStrip);
            b->setCheckable(true);
            b->setFixedSize(34, 32);
            b->setIconSize(QSize(17, 17));
            b->setCursor(Qt::PointingHandCursor);
            const QColor tint = (i == selectedSeg) ? m_tk.accent : m_tk.icon;
            b->setIcon(barIconAssetTinted(kBarIcons.at(i), tint, 17));
            b->setToolTip(kBarIcons.at(i));
            // Drop the right border on the last segment.
            QString st = segStyle;
            if (i == kBarIcons.size() - 1)
                st += " QToolButton { border-right:none; }";
            b->setStyleSheet(st);
            if (i == selectedSeg) b->setChecked(true);
            group->addButton(b, i);
            sh->addWidget(b);
        }
        connect(group, &QButtonGroup::idClicked, this, &SettingsWindow::onBarIconSegment);
        h->addWidget(segStrip, 0, Qt::AlignVCenter);

        // Thin divider, then the SEPARATE custom-image button.
        auto* div = new QFrame;
        div->setFixedSize(1, 24);
        div->setStyleSheet(QStringLiteral("background-color:%1; border:none;")
                               .arg(colCss(m_tk.border)));
        h->addWidget(div, 0, Qt::AlignVCenter);

        auto* custom = new QToolButton;
        custom->setCheckable(true);
        custom->setFixedSize(34, 32);
        custom->setIconSize(QSize(17, 17));
        custom->setCursor(Qt::PointingHandCursor);
        const QColor customTint = usingCustom ? m_tk.accent : m_tk.icon;
        custom->setIcon(barIconAssetTinted("photo", customTint, 17));
        custom->setStyleSheet(QStringLiteral(
            "QToolButton { background-color:%1; border:1px solid %2;"
            " border-radius:8px; }"
            "QToolButton:hover { background-color:%3; border-color:%4; }"
            "QToolButton:checked { background-color:%3; border-color:%4; }")
            .arg(colCss(m_tk.control), colCss(m_tk.border),
                 colCss(m_tk.accentWeak), colCss(m_tk.accent)));
        custom->setChecked(usingCustom);
        custom->setToolTip(Loc::t("settings.customIcon"));
        connect(custom, &QToolButton::clicked, this, &SettingsWindow::chooseCustomIcon);
        h->addWidget(custom, 0, Qt::AlignVCenter);

        addRowWidget(makeRow(Loc::t("settings.barIcon"), iconWrap, nullptr));
    }

    // --- Hotkey recorder + reset ---
    m_recorder->applyTokens(m_tk);
    m_recorder->setText(s.hotKeyDisplay());
    m_recorder->setFixedSize(kFieldWidth, 32);
    addRowWidget(makeRow(Loc::t("settings.hotkey"), m_recorder,
                         makeResetButton(ResetTarget::Hotkey)));

    // Hint under the hotkey row (small tertiary text, right-aligned over the field).
    {
        auto* hintRow = new QWidget;
        auto* hh = new QHBoxLayout(hintRow);
        hh->setContentsMargins(kRowPadH, 2, kRowPadH, 11);
        hh->setSpacing(kLabelGap);
        hh->addStretch(1);
        auto* hint = new QLabel(Loc::t("settings.hint"));
        hint->setWordWrap(true);
        hint->setFixedWidth(kFieldWidth);
        hint->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                                .arg(colCss(m_tk.text3)));
        hh->addWidget(hint, 0);
        auto* slot = new QWidget; slot->setFixedWidth(kResetSlot);
        hh->addWidget(slot, 0);
        // Attach directly (no separator before the hint — it belongs to the row above).
        col->addWidget(hintRow);
    }

    // --- Save folder picker + reset ---
    {
        auto* folderBtn = new QPushButton(saveFolderTitle());
        folderBtn->setToolTip(saveFolderTitle());
        folderBtn->setFixedSize(kFieldWidth, 32);
        folderBtn->setCursor(Qt::PointingHandCursor);
        folderBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background-color:%1; color:%2; border:1px solid %3;"
            " border-radius:8px; font-size:13px; }"
            "QPushButton:hover { border-color:%4; }")
            .arg(colCss(m_tk.controlFill), colCss(m_tk.text),
                 colCss(m_tk.border), colCss(m_tk.accent)));
        connect(folderBtn, &QPushButton::clicked, this, &SettingsWindow::chooseFolder);
        addRowWidget(makeRow(Loc::t("settings.saveFolder"), folderBtn,
                             makeResetButton(ResetTarget::SaveFolder)));
    }

    // --- Dim slider (0.10 .. 0.85) + reset ---
    {
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setMinimum(10);
        slider->setMaximum(85);
        slider->setValue(int(s.dimOpacity() * 100.0 + 0.5));
        slider->setFixedWidth(kFieldWidth);
        slider->setCursor(Qt::PointingHandCursor);
        slider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height:4px; border-radius:2px;"
            " background:%1; }"
            "QSlider::sub-page:horizontal { height:4px; border-radius:2px;"
            " background:%2; }"
            "QSlider::add-page:horizontal { height:4px; border-radius:2px;"
            " background:%1; }"
            "QSlider::handle:horizontal { width:16px; height:16px;"
            " margin:-6px 0; border-radius:8px; background:%3;"
            " border:1px solid %4; }"
            "QSlider::handle:horizontal:hover { border-color:%2; }")
            .arg(colCss(m_tk.dark ? QColor("#48484c") : QColor("#d9d9de")),
                 colCss(m_tk.accent), colCss(m_tk.knob), colCss(m_tk.border)));
        connect(slider, &QSlider::valueChanged, this,
                [this](int v) { onDimChanged(double(v) / 100.0); });
        addRowWidget(makeRow(Loc::t("settings.dim"), slider,
                             makeResetButton(ResetTarget::Dim)));
    }

    // --- Animated dimming (toggle, no reset) — moved into General per Direction B.
    //     Same Settings::animatedDim() field + handler as before. ---
    {
        auto* tog = new ToggleSwitch(m_tk);
        tog->setChecked(s.animatedDim());
        connect(tog, &QAbstractButton::toggled, this, [](bool on) {
            Settings::instance().setAnimatedDim(on);
        });
        addRowWidget(makeToggleRow(Loc::t("features.animatedDim"), tog, nullptr));
    }

    // --- Downscale toggle + reset ---
    {
        auto* tog = new ToggleSwitch(m_tk);
        tog->setChecked(s.downscaleRetina());
        connect(tog, &QAbstractButton::toggled, this, &SettingsWindow::onDownscaleToggled);
        addRowWidget(makeToggleRow(Loc::t("settings.downscale"), tog,
                                   makeResetButton(ResetTarget::Downscale)));
    }

    // --- Launch at login (toggle, no reset) ---
    {
        auto* tog = new ToggleSwitch(m_tk);
        tog->setChecked(isLaunchAtLoginEnabled());
        connect(tog, &QAbstractButton::toggled, this, &SettingsWindow::onLaunchAtLoginToggled);
        addRowWidget(makeToggleRow(Loc::t("settings.launchAtLogin"), tog, nullptr));
    }

    outer->addWidget(card);
    outer->addStretch(1);

    // About section pinned at the bottom of the General tab.
    addAboutSection(outer);

    // Wrap the page in a scroll-friendly plain widget (kept simple; fixed-size
    // window means it won't normally scroll, but the layout won't clip).
    return page;
}

// ---------------------------------------------------------------------------
// Features tab
// ---------------------------------------------------------------------------
QWidget* SettingsWindow::buildFeaturesTab() {
    Settings& s = Settings::instance();

    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(20, 20, 20, 20);
    v->setSpacing(18);

    // A titled section: bold caption above a card of CheckRows.
    auto addSection = [&](const QString& title,
                          const std::vector<std::function<CheckRow*()>>& makers) {
        auto* block = new QVBoxLayout;
        block->setContentsMargins(0, 0, 0, 0);
        block->setSpacing(8);

        auto* cap = new QLabel(title);
        cap->setStyleSheet(QStringLiteral(
            "color:%1; font-size:13px; font-weight:600; padding-left:4px;")
            .arg(colCss(m_tk.text)));
        block->addWidget(cap);

        auto* card = makeCard();
        auto* col = new QVBoxLayout(card);
        col->setContentsMargins(0, 4, 0, 4);
        col->setSpacing(0);
        for (auto& mk : makers) col->addWidget(mk());

        block->addWidget(card);
        v->addLayout(block);
    };

    // Small swatch/preview widget builder for the trailing chip in a row.
    auto previewChip = [&](QWidget* inner) -> QWidget* {
        auto* chip = new QWidget;
        chip->setObjectName("chip");
        chip->setStyleSheet(QStringLiteral(
            "#chip { background-color:%1; border:1px solid %2; border-radius:7px; }")
            .arg(colCss(m_tk.controlFill), colCss(m_tk.border)));
        auto* ch = new QHBoxLayout(chip);
        ch->setContentsMargins(11, 0, 11, 0);
        ch->setSpacing(5);
        chip->setFixedHeight(26);
        ch->addWidget(inner);
        return chip;
    };
    auto coloredText = [&](const QString& txt, const QString& cssExtra) -> QLabel* {
        auto* l = new QLabel(txt);
        l->setStyleSheet(QStringLiteral("color:%1;%2").arg(colCss(m_tk.text), cssExtra));
        return l;
    };

    // ===== Tools in the toolbar =====
    {
        std::vector<std::function<CheckRow*()>> makers;
        for (Tool t : kFeatureTools) {
            makers.push_back([this, t, &s]() {
                const QString key = QStringLiteral("tool.%1")
                                        .arg(QString::fromUtf8(toolKey(t)));
                auto* row = new CheckRow(m_tk, Loc::t(key), s.isToolEnabled(t));
                connect(row, &QAbstractButton::toggled, this, [t](bool on) {
                    Settings::instance().setToolEnabled(t, on);
                });
                return row;
            });
        }
        addSection(Loc::t("features.toolsTitle"), makers);
    }

    // ===== Interface =====
    {
        std::vector<std::function<CheckRow*()>> makers;
        // Show color palette + a row of swatches as the trailing preview.
        makers.push_back([this, &s, &previewChip]() {
            auto* swatches = new QWidget;
            auto* sw = new QHBoxLayout(swatches);
            sw->setContentsMargins(0, 0, 0, 0);
            sw->setSpacing(5);
            const QStringList cols = {"#ff453a", "#32d74b", "#0a84ff", "#ffd60a", "#ffffff"};
            for (const QString& c : cols) {
                auto* dot = new QLabel;
                dot->setFixedSize(11, 11);
                QString extra = (c == "#ffffff")
                    ? QStringLiteral("border:1px solid %1;").arg(colCss(m_tk.border))
                    : QString();
                dot->setStyleSheet(QStringLiteral(
                    "background-color:%1; border-radius:5px; %2").arg(c, extra));
                sw->addWidget(dot);
            }
            auto* chip = previewChip(swatches);
            auto* row = new CheckRow(m_tk, Loc::t("features.showColors"),
                                     s.showColorPalette(), chip);
            connect(row, &QAbstractButton::toggled, this, [](bool on) {
                Settings::instance().setShowColorPalette(on);
            });
            return row;
        });
        addSection(Loc::t("features.interfaceTitle"), makers);
    }

    // ===== Text options =====
    {
        std::vector<std::function<CheckRow*()>> makers;
        // Text alignment row with an alignment-glyph preview chip.
        makers.push_back([this, &s, &previewChip, &coloredText]() {
            auto* glyph = coloredText(QStringLiteral("≡"),
                                      QStringLiteral("font-size:15px; font-weight:600;"));
            auto* chip = previewChip(glyph);
            auto* row = new CheckRow(m_tk, Loc::t("features.textAlignment"),
                                     s.textAlignmentEnabled(), chip);
            connect(row, &QAbstractButton::toggled, this, [](bool on) {
                Settings::instance().setTextAlignmentEnabled(on);
            });
            return row;
        });
        // Text background row with a highlighted-"Aa" preview chip.
        makers.push_back([this, &s, &previewChip]() {
            auto* hl = new QLabel(QStringLiteral("Aa"));
            hl->setStyleSheet(QStringLiteral(
                "background-color:#ffd60a; color:#1c1c1e; padding:1px 6px;"
                " border-radius:4px; font-weight:600; font-size:13px;"));
            auto* chip = previewChip(hl);
            auto* row = new CheckRow(m_tk, Loc::t("features.textBackground"),
                                     s.textBackgroundEnabled(), chip);
            connect(row, &QAbstractButton::toggled, this, [](bool on) {
                Settings::instance().setTextBackgroundEnabled(on);
            });
            return row;
        });
        addSection(Loc::t("features.textTitle"), makers);
    }

    v->addStretch(1);

    // Footer hint at the very bottom (tertiary, small).
    auto* hint = new QLabel(Loc::t("features.hint"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                            .arg(colCss(m_tk.text3)));
    v->addWidget(hint);

    return page;
}

// ---------------------------------------------------------------------------
// About section (bottom of General tab)
// ---------------------------------------------------------------------------
void SettingsWindow::addAboutSection(QVBoxLayout* generalCol) {
    generalCol->addSpacing(18);

    // Social card.
    struct Social { const char* title; const char* handle; const char* url; };
    const std::vector<Social> socials = {
        {"GitHub",      "@VeDono",          "https://github.com/VeDono"},
        {"LinkedIn",    "Sergey Emelyanov", "https://www.linkedin.com/in/sergey-emelyanov-18082b27a/"},
        {"X / Twitter", "@SergeyEDev",      "https://x.com/SergeyEDev"},
    };

    auto* card = makeCard();
    auto* cardCol = new QVBoxLayout(card);
    cardCol->setContentsMargins(0, 0, 0, 0);
    cardCol->setSpacing(0);

    for (size_t i = 0; i < socials.size(); ++i) {
        const Social& sc = socials[i];
        if (i > 0) cardCol->addWidget(makeSeparator());

        auto* rowW = new QWidget;
        auto* h = new QHBoxLayout(rowW);
        h->setContentsMargins(kRowPadH, 11, kRowPadH, 11);
        auto* title = new QLabel(QString::fromUtf8(sc.title));
        title->setStyleSheet(QStringLiteral("color:%1; font-size:13px;")
                                 .arg(colCss(m_tk.text)));
        auto* link = new LinkButton(QString::fromUtf8(sc.handle),
                                    QString::fromUtf8(sc.url), m_tk.link,
                                    Qt::AlignRight);
        h->addWidget(title, 0, Qt::AlignLeft);
        h->addStretch(1);
        h->addWidget(link, 0, Qt::AlignRight);
        cardCol->addWidget(rowW);
    }
    generalCol->addWidget(card);

    // Copyright: "© Sergey Emelyanov YYYY · Made in Ukraine 🇺🇦", with the name a
    // GitHub link (underlined, blue on hover).
    const int year = QDate::currentDate().year();
    auto* copyRow = new QWidget;
    auto* ch = new QHBoxLayout(copyRow);
    ch->setContentsMargins(0, 12, 0, 0);
    ch->setSpacing(0);

    auto grayText = [this](const QString& text, int sizePt) {
        auto* l = new QLabel(text);
        l->setStyleSheet(QStringLiteral("color:%1; font-size:%2px;")
                             .arg(colCss(m_tk.text3)).arg(sizePt));
        return l;
    };
    const int fs = 11;
    auto* pre = grayText(QStringLiteral("© "), fs);
    auto* nameLink = new InlineLinkLabel(QStringLiteral("Sergey Emelyanov"),
                                         QStringLiteral("https://github.com/VeDono"),
                                         9, m_tk.text3, m_tk.link);
    auto* post = grayText(QStringLiteral(" %1 · %2")
                              .arg(year).arg(Loc::t("settings.madeInUkraine")), fs);

    ch->addStretch(1);
    ch->addWidget(pre);
    ch->addWidget(nameLink);
    ch->addWidget(post);
    ch->addStretch(1);
    generalCol->addWidget(copyRow);

    // Version + edition line (small tertiary, centered) so this build is easy to
    // tell apart from the native one: "LightGet 1.0.0 · Cross-platform (Qt 6)".
    auto* version = new QLabel(
        QStringLiteral("LightGet %1 · %2")
            .arg(QString::fromUtf8(LIGHTGET_VERSION),
                 QString::fromUtf8(LIGHTGET_EDITION)));
    version->setAlignment(Qt::AlignHCenter);
    version->setStyleSheet(QStringLiteral("color:%1; font-size:10px;")
                               .arg(colCss(m_tk.text3)));
    generalCol->addSpacing(2);
    generalCol->addWidget(version);
}

// ---------------------------------------------------------------------------
// Reset button + handler
// ---------------------------------------------------------------------------
QPushButton* SettingsWindow::makeResetButton(ResetTarget target) {
    // 28px circular reset button with a "counterclockwise arrow" glyph. Hover:
    // accent-weak wash + accent border/glyph. Click: spin the glyph -360°.
    auto* b = new QPushButton;
    b->setFixedSize(28, 28);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);

    // Tint the glyph to the reset-fg token; refresh to accent on hover via icon
    // swap (QSS can't recolor a pixmap, so we keep the base tint and rely on the
    // accent border/background to signal hover). The glyph stays visible in both
    // themes because resetFg is theme-appropriate.
    const QColor tint = m_tk.resetFg;
    QIcon icon = barIconAssetTinted("arrow.counterclockwise", tint, 15);
    if (icon.isNull()) icon = QIcon::fromTheme("edit-undo");
    b->setIcon(icon);
    b->setIconSize(QSize(15, 15));
    if (icon.isNull()) b->setText(QStringLiteral("↺"));  // ↺ fallback glyph
    b->setToolTip(Loc::t("reset.tooltip"));
    b->setStyleSheet(QStringLiteral(
        "QPushButton { border:1px solid %1; border-radius:14px;"
        " background:transparent; color:%2; }"
        "QPushButton:hover { background-color:%3; border-color:%4; }")
        .arg(colCss(m_tk.border), colCss(m_tk.resetFg),
             colCss(m_tk.accentWeak), colCss(m_tk.accent)));

    connect(b, &QPushButton::clicked, this, [this, b, target]() {
        // Spin the icon -360° (500ms) for the same feedback as the design.
        if (!b->icon().isNull()) {
            const QIcon base = b->icon();
            const QSize isz = b->iconSize();
            auto* spin = new QVariantAnimation(b);
            spin->setStartValue(0.0);
            spin->setEndValue(-360.0);
            spin->setDuration(500);
            spin->setEasingCurve(QEasingCurve::OutBack);
            const qreal dpr = b->devicePixelRatioF();
            QPixmap basePm = base.pixmap(isz * dpr);
            basePm.setDevicePixelRatio(dpr);
            connect(spin, &QVariantAnimation::valueChanged, b,
                    [b, basePm, isz, dpr](const QVariant& v) {
                QPixmap rot(QSize(int(isz.width() * dpr), int(isz.height() * dpr)));
                rot.setDevicePixelRatio(dpr);
                rot.fill(Qt::transparent);
                QPainter p(&rot);
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.translate(rot.deviceIndependentSize().width() / 2.0,
                            rot.deviceIndependentSize().height() / 2.0);
                p.rotate(v.toReal());
                p.translate(-rot.deviceIndependentSize().width() / 2.0,
                            -rot.deviceIndependentSize().height() / 2.0);
                p.drawPixmap(0, 0, basePm);
                p.end();
                b->setIcon(QIcon(rot));
            });
            connect(spin, &QVariantAnimation::finished, b,
                    [b, base]() { b->setIcon(base); });
            spin->start(QAbstractAnimation::DeleteWhenStopped);
        }
        resetTapped(target);
    });
    return b;
}

void SettingsWindow::resetTapped(ResetTarget target) {
    // Confirmation dialog (Confirm / Cancel).
    QMessageBox box(this);
    box.setIcon(QMessageBox::NoIcon);
    box.setText(Loc::t("reset.title"));
    box.setInformativeText(Loc::t("reset.body"));
    QPushButton* confirm = box.addButton(Loc::t("reset.confirm"), QMessageBox::AcceptRole);
    box.addButton(Loc::t("reset.cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(confirm);
    box.exec();
    if (box.clickedButton() != confirm) return;

    Settings& s = Settings::instance();
    switch (target) {
    case ResetTarget::Language:
        s.setLanguage(QStringLiteral("en"));
        emit languageChanged();
        break;
    case ResetTarget::Hotkey:
        s.setHotKeyCode(CarbonKeys::kVK_ANSI_2);
        s.setHotKeyModifiers(CarbonKeys::cmdKey | CarbonKeys::shiftKey);
        // Platform-correct default display: "⇧⌘2" on macOS, "Ctrl+Shift+2" else.
        s.setHotKeyDisplay(Settings::hotKeyDisplayString(
            CarbonKeys::cmdKey | CarbonKeys::shiftKey, QStringLiteral("2")));
        emit hotKeyChanged();
        break;
    case ResetTarget::Dim:
        s.setDimOpacity(0.45);
        break;
    case ResetTarget::Downscale:
        s.setDownscaleRetina(false);
        break;
    case ResetTarget::SaveFolder:
        s.setSaveFolderPath(std::nullopt);
        break;
    }
    reloadUI();
}

// ---------------------------------------------------------------------------
// General-tab action handlers
// ---------------------------------------------------------------------------
void SettingsWindow::chooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, Loc::t("settings.chooseFolder"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        Settings::instance().setSaveFolderPath(dir);
        reloadUI();
    }
}

void SettingsWindow::chooseCustomIcon() {
    const QString file = QFileDialog::getOpenFileName(
        this, Loc::t("settings.chooseIcon"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tiff *.webp)"));
    if (!file.isEmpty()) {
        Settings::instance().setBarIconCustomPath(file);
        emit barIconChanged();
        reloadUI();
    } else {
        // User cancelled the custom-image picker: the custom button stays checked
        // visually only if a custom path is already active; otherwise rebuild so
        // the previously-selected preset segment reads as checked again.
        reloadUI();
    }
}

void SettingsWindow::onBarIconSegment(int index) {
    if (index < 0 || index >= kBarIcons.size()) return;
    Settings& s = Settings::instance();
    s.setBarIconCustomPath(std::nullopt);     // choosing a preset clears custom image
    s.setBarIcon(kBarIcons.at(index));
    emit barIconChanged();
    reloadUI();   // refresh tints (selected = accent, custom button unchecks)
}

void SettingsWindow::onLanguageChanged(int index) {
    if (index >= 0 && index < int(kLanguages.size())) {
        const QString code = QString::fromUtf8(kLanguages[size_t(index)].code);
        Settings::instance().setLanguage(code);
        // Persist AppleLanguages-equivalent so native dialogs follow next launch.
        // main.cpp reads this key before constructing QApplication.
        QSettings().setValue(QStringLiteral("AppleLanguages"), QStringList{code});
    }
    reloadUI();
    emit languageChanged();
}

void SettingsWindow::onDimChanged(double value) {
    Settings::instance().setDimOpacity(value);
}

void SettingsWindow::onDownscaleToggled(bool on) {
    Settings::instance().setDownscaleRetina(on);
}

void SettingsWindow::onLaunchAtLoginToggled(bool on) {
    if (setLaunchAtLogin(on)) return;

    // Failure: revert the toggle and explain (often the cause is running from a
    // build folder). Find the sender switch and flip it back silently.
    if (auto* tog = qobject_cast<ToggleSwitch*>(sender())) {
        QSignalBlocker block(tog);
        tog->setChecked(!on);
        tog->setKnobPos(!on ? 1.0 : 0.0);
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::NoIcon);
    box.setText(Loc::t("launch.errorTitle"));
    box.setInformativeText(Loc::t("launch.errorBody"));
    box.exec();
}

QString SettingsWindow::saveFolderTitle() const {
    const auto path = Settings::instance().saveFolderPath();
    if (!path.has_value()) return Loc::t("settings.askEachTime");
    return QFileInfo(*path).fileName();
}

// ---------------------------------------------------------------------------
// Launch-at-login platform shim (Spec 6 §7: registry / autostart / SMAppService)
// ---------------------------------------------------------------------------
bool SettingsWindow::isLaunchAtLoginEnabled() const {
#if defined(Q_OS_WIN)
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    return run.contains(QStringLiteral("LightGet"));
#elif defined(Q_OS_LINUX)
    const QString dir = QDir::homePath() + "/.config/autostart";
    return QFileInfo::exists(dir + "/LightGet.desktop");
#else
    // macOS: SMAppService status via the native shim (macOS 13+).
#if defined(HAVE_MAC_NATIVE)
    extern bool MacNative_isLaunchAtLoginEnabled();
    return MacNative_isLaunchAtLoginEnabled();
#else
    return false;
#endif
#endif
}

bool SettingsWindow::setLaunchAtLogin(bool enabled) {
#if defined(Q_OS_WIN)
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled) {
        const QString exe = QDir::toNativeSeparators(QApplication::applicationFilePath());
        run.setValue(QStringLiteral("LightGet"), QStringLiteral("\"%1\"").arg(exe));
    } else {
        run.remove(QStringLiteral("LightGet"));
    }
    run.sync();
    return run.status() == QSettings::NoError;
#elif defined(Q_OS_LINUX)
    const QString dir = QDir::homePath() + "/.config/autostart";
    const QString file = dir + "/LightGet.desktop";
    if (enabled) {
        if (!QDir().mkpath(dir)) return false;
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        const QString exe = QApplication::applicationFilePath();
        QByteArray data =
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=LightGet\n"
            "Exec=" + exe.toUtf8() + "\n"
            "X-GNOME-Autostart-enabled=true\n";
        f.write(data);
        f.close();
        return true;
    } else {
        if (!QFileInfo::exists(file)) return true;
        return QFile::remove(file);
    }
#else
    // macOS: SMAppService register/unregister via the native shim (macOS 13+).
#if defined(HAVE_MAC_NATIVE)
    extern bool MacNative_setLaunchAtLogin(bool);
    return MacNative_setLaunchAtLogin(enabled);
#else
    Q_UNUSED(enabled);
    return false;
#endif
#endif
}
