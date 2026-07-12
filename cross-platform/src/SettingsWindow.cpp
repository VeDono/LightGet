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
#include <QScrollArea>
#include <QStyleHints>
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
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Local helpers / data
// ---------------------------------------------------------------------------
namespace {

// Languages: (title, code). English first (default language).
struct LangEntry { const char* title; const char* code; };
const std::vector<LangEntry> kLanguages = {
    {"English", "en"}, {"Русский", "ru"}, {"Українська", "uk"}
};

// Preset bar icons. "feather" is the brand default (matches the app logo's tray
// silhouette); the rest mirror the design's menu-bar-icon chooser glyphs. Stored
// by name as Settings::barIcon(); painted by paintPresetGlyph().
const QStringList kBarIcons = {
    "feather", "scissors", "camera.viewfinder", "crop", "rectangle.dashed",
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
constexpr int kRowPadV      = 11;
constexpr int kLabelGap     = 14;

QString colCss(const QColor& c) {
    // rgba() so alpha survives in QSS strings.
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
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
    InlineLinkLabel(const QString& text, const QString& url, int sizePx,
                    const QColor& base, const QColor& linkColor,
                    QWidget* parent = nullptr)
        : QLabel(text, parent), m_url(url), m_base(base), m_link(linkColor) {
        QFont f = font();
        f.setPixelSize(sizePx);   // px so it matches the px-sized copyright text
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

// ---- Tool-glyph preview (Features tab) -------------------------------------
// Paints the red tool glyph shown in each Features row's preview chip, matching
// the design SVGs: arrow / line / rectangle / filled rectangle / pen squiggle.
// The "Text" tool uses a red "Abc" label instead (handled inline). Colour is the
// design's red #ff453a regardless of theme (the chip bg supplies the contrast).
class ToolGlyph : public QWidget {
public:
    explicit ToolGlyph(Tool tool, QWidget* parent = nullptr)
        : QWidget(parent), m_tool(tool) {
        setFixedSize(36, 18);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    QSize sizeHint() const override { return QSize(36, 18); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor red("#ff453a");
        const qreal w = width(), h = height(), cy = h / 2.0;
        switch (m_tool) {
        case Tool::Arrow: {
            QPen pen(red, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.drawLine(QPointF(4, cy), QPointF(w - 11, cy));
            // Filled arrowhead.
            p.setPen(Qt::NoPen);
            p.setBrush(red);
            QPolygonF head;
            head << QPointF(w - 3, cy)
                 << QPointF(w - 12, cy - 4.6)
                 << QPointF(w - 12, cy + 4.6);
            p.drawPolygon(head);
            break;
        }
        case Tool::Line: {
            QPen pen(red, 2.4, Qt::SolidLine, Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(QPointF(3, cy), QPointF(w - 3, cy));
            break;
        }
        case Tool::Rectangle: {
            QPen pen(red, 2.2);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(3, 3, w - 6, h - 6), 2.5, 2.5);
            break;
        }
        case Tool::FilledRect: {
            p.setPen(Qt::NoPen);
            p.setBrush(red);
            p.drawRoundedRect(QRectF(3, 3, w - 6, h - 6), 2.5, 2.5);
            break;
        }
        case Tool::Pen: {
            QPen pen(red, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            QPainterPath path;
            const qreal x0 = 3, x1 = w - 3;
            path.moveTo(x0, cy + 2.2);
            path.cubicTo(x0 + (x1 - x0) * 0.15, cy - 4.4,
                         x0 + (x1 - x0) * 0.32, cy - 4.4,
                         (x0 + x1) / 2.0, cy);
            path.cubicTo(x0 + (x1 - x0) * 0.68, cy + 4.4,
                         x0 + (x1 - x0) * 0.85, cy + 4.4,
                         x1, cy - 1.6);
            p.drawPath(path);
            break;
        }
        default: break;
        }
    }
private:
    Tool m_tool;
};

// ---- Alignment glyph (Features "Text alignment" preview chip) ---------------
// Three horizontal lines of varying length (left-aligned paragraph), stroked in
// the row's text color, matching the design's align icon.
class AlignGlyph : public QWidget {
public:
    explicit AlignGlyph(const QColor& color, QWidget* parent = nullptr)
        : QWidget(parent), m_color(color) {
        setFixedSize(15, 15);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    QSize sizeHint() const override { return QSize(15, 15); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(m_color, 1.9, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        const qreal x = 1.5, w = width() - 3.0;
        p.drawLine(QPointF(x, 4), QPointF(x + w, 4));
        p.drawLine(QPointF(x, 7.5), QPointF(x + w * 0.62, 7.5));
        p.drawLine(QPointF(x, 11), QPointF(x + w * 0.80, 11));
    }
private:
    QColor m_color;
};

// ---- Palette glyph (Features "Show color palette" preview chip) -------------
// Custom-painted fixed-size widget (mirrors ToolGlyph) that draws the 5 palette
// dots — red / green / blue / yellow / white — evenly spaced and CENTERED both
// horizontally and vertically within its own bounds. Painting (vs a QLabel
// QHBoxLayout) is what guarantees the cluster is centered: the earlier layout
// version left the dots in the left of an inflated chip and ~7px low. The white
// dot gets a 1px border (the chip bg supplies contrast for the others). The
// fixed size is chosen so the resulting chip is the same compact width as a
// tool chip (ToolGlyph is 36px; this is 40px to fit five dots legibly).
class PaletteGlyph : public QWidget {
public:
    explicit PaletteGlyph(const QColor& borderColor, QWidget* parent = nullptr)
        : QWidget(parent), m_border(borderColor) {
        setFixedSize(40, 18);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    QSize sizeHint() const override { return QSize(40, 18); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        static const char* cols[5] =
            { "#ff453a", "#32d74b", "#0a84ff", "#ffd60a", "#ffffff" };
        const int n = 5;
        const qreal d = 6.0;          // dot diameter
        const qreal gap = 2.5;        // gap between dots
        const qreal clusterW = n * d + (n - 1) * gap;
        // Center the cluster horizontally + vertically within the widget bounds.
        const qreal x0 = (width() - clusterW) / 2.0;
        const qreal cy = height() / 2.0;
        for (int i = 0; i < n; ++i) {
            const qreal cx = x0 + i * (d + gap) + d / 2.0;
            const QColor c(QString::fromUtf8(cols[i]));
            if (i == n - 1) {         // white dot: outline so it reads on the chip
                p.setPen(QPen(m_border, 1.0));
                p.setBrush(c);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(c);
            }
            p.drawEllipse(QPointF(cx, cy), d / 2.0, d / 2.0);
        }
    }
private:
    QColor m_border;
};

// ---- Preset glyph painter --------------------------------------------------
// Paints the menu-bar-icon preset glyphs (and the custom-image button glyph)
// procedurally, 1:1 with the user's design SVGs, so the chooser reads exactly
// like the mockup AND clips cleanly inside the rounded strip. `name` is the
// stored preset key; `box` is the target square; strokes use `color`.
//   feather (brand)           -> own 96x172 viewBox, FILLED
//   scissors / target / crop / dashed-rect / pencil / photo -> 24x24, stroked
void paintPresetGlyph(QPainter& p, const QString& name, const QRectF& box,
                      const QColor& color, qreal strokeW = 1.6) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    if (name == QLatin1String("feather")) {
        // Brand feather (mono silhouette), viewBox x[62,158] y[14,186]. Keep
        // aspect, center in the box; filled (no stroke).
        const qreal vbW = 96.0, vbH = 172.0, vbX = 62.0, vbY = 14.0;
        const qreal sc = qMin(box.width() / vbW, box.height() / vbH);
        const qreal ox = box.center().x() - (vbX + vbW / 2.0) * sc;
        const qreal oy = box.center().y() - (vbY + vbH / 2.0) * sc;
        auto FX = [&](qreal x) { return ox + x * sc; };
        auto FY = [&](qreal y) { return oy + y * sc; };
        QPainterPath body;
        body.moveTo(FX(100), FY(24));
        body.cubicTo(FX(113), FY(44),  FX(126), FY(68),  FX(128), FY(96));
        body.cubicTo(FX(129), FY(114), FX(124), FY(130), FX(113), FY(142));
        body.cubicTo(FX(110), FY(145), FX(107), FY(147), FX(104), FY(148));
        body.cubicTo(FX(100), FY(146), FX(96),  FY(143), FX(92),  FY(138));
        body.cubicTo(FX(85),  FY(128), FX(76),  FY(110), FX(74),  FY(90));
        body.cubicTo(FX(73),  FY(70),  FX(84),  FY(46),  FX(100), FY(24));
        body.closeSubpath();
        QPainterPath nib;
        nib.addRoundedRect(QRectF(FX(98), FY(150), 12 * sc, 6 * sc), 2.6 * sc, 2.6 * sc);
        QPainterPath tip;
        tip.moveTo(FX(99.5), FY(157));
        tip.lineTo(FX(108.5), FY(157));
        tip.lineTo(FX(104), FY(179));
        tip.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(body);
        p.drawPath(nib);
        p.drawPath(tip);
        p.restore();
        return;
    }

    // 24x24 stroked glyphs.
    const qreal sc = qMin(box.width(), box.height()) / 24.0;
    const qreal ox = box.center().x() - 12.0 * sc;
    const qreal oy = box.center().y() - 12.0 * sc;
    auto PT = [&](qreal x, qreal y) { return QPointF(ox + x * sc, oy + y * sc); };
    QPen pen(color, strokeW * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    if (name == QLatin1String("scissors")) {
        p.drawEllipse(PT(6, 6), 2.4 * sc, 2.4 * sc);
        p.drawEllipse(PT(6, 18), 2.4 * sc, 2.4 * sc);
        p.drawLine(PT(8, 7.7), PT(20, 18));
        p.drawLine(PT(8, 16.3), PT(20, 6));
    } else if (name == QLatin1String("camera.viewfinder")) {
        // Design glyph for this slot is a crosshair / target.
        p.drawEllipse(PT(12, 12), 3.2 * sc, 3.2 * sc);
        p.drawLine(PT(12, 3),    PT(12, 6.2));
        p.drawLine(PT(12, 17.8), PT(12, 21));
        p.drawLine(PT(3, 12),    PT(6.2, 12));
        p.drawLine(PT(17.8, 12), PT(21, 12));
    } else if (name == QLatin1String("crop")) {
        QPainterPath a;
        a.moveTo(PT(6.5, 2.5)); a.lineTo(PT(6.5, 15.5));
        a.quadTo(PT(6.5, 17.5), PT(8.5, 17.5)); a.lineTo(PT(21.5, 17.5));
        QPainterPath b;
        b.moveTo(PT(2.5, 6.5)); b.lineTo(PT(15.5, 6.5));
        b.quadTo(PT(17.5, 6.5), PT(17.5, 8.5)); b.lineTo(PT(17.5, 21.5));
        p.drawPath(a);
        p.drawPath(b);
    } else if (name == QLatin1String("rectangle.dashed")) {
        QPen dp(color, 1.7 * sc, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
        dp.setDashPattern(QVector<qreal>{ 3.0, 3.2 });
        p.setPen(dp);
        p.drawRoundedRect(QRectF(PT(3.5, 3.5), PT(20.5, 20.5)), 2.5 * sc, 2.5 * sc);
    } else if (name == QLatin1String("paintbrush.pointed.fill") ||
               name == QLatin1String("pencil")) {
        p.drawLine(PT(15.5, 5.5), PT(18.5, 8.5));
        QPainterPath body;
        body.moveTo(PT(5, 19));
        body.lineTo(PT(5.5, 15.8));
        body.lineTo(PT(14.5, 6.8));
        body.lineTo(PT(17.5, 9.8));
        body.lineTo(PT(8.5, 18.8));
        body.closeSubpath();
        p.drawPath(body);
    } else if (name == QLatin1String("photo")) {
        p.drawRoundedRect(QRectF(PT(3.5, 4.5), PT(20.5, 19.5)), 2.5 * sc, 2.5 * sc);
        p.setBrush(color);
        p.drawEllipse(PT(8.5, 9.5), 1.4 * sc, 1.4 * sc);
        p.setBrush(Qt::NoBrush);
        QPainterPath mt;
        mt.moveTo(PT(4.5, 17));
        mt.lineTo(PT(9, 13));
        mt.lineTo(PT(12, 15.5));
        mt.lineTo(PT(15.5, 12));
        mt.lineTo(PT(19.5, 16));
        p.drawPath(mt);
    }
    p.restore();
}

// Render a preset glyph to a QIcon (for QToolButton-based controls).
QIcon presetGlyphIcon(const QString& name, const QColor& color, int size) {
    const qreal dpr = 2.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintPresetGlyph(p, name, QRectF(0, 0, size, size), color);
    p.end();
    return QIcon(pm);
}

// Paint the design's "reset" reload glyph (counterclockwise ring open at the
// upper-left + an open-corner arrowhead) into `box`, stroked in `color`.
// Mirrors the SVG: arc "M3.5 12 a8.5 8.5 0 1 0 2.6-6.1" + corner "M3 4.5V9h4.5".
void paintReloadGlyph(QPainter& p, const QRectF& box, const QColor& color) {
    const qreal sc = qMin(box.width(), box.height()) / 24.0;
    const qreal ox = box.center().x() - 12.0 * sc;
    const qreal oy = box.center().y() - 12.0 * sc;
    auto PT = [&](qreal x, qreal y) { return QPointF(ox + x * sc, oy + y * sc); };
    QPen pen(color, 2.0 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    // Ring: center (12,12) r8.5; sweep the long way (bottom->right->top) leaving
    // the gap at the upper-left where the arrowhead sits.
    QRectF ring(PT(3.5, 3.5), PT(20.5, 20.5));
    p.drawArc(ring, 180 * 16, -292 * 16);
    QPainterPath head;
    head.moveTo(PT(3, 4.5));
    head.lineTo(PT(3, 9));
    head.lineTo(PT(7.5, 9));
    p.drawPath(head);
}

// ---- ResetButton -----------------------------------------------------------
// 28px circular per-setting reset (design "Reset" state): transparent with a 1px
// border; on hover the ring fills accent-weak and the border + glyph turn accent;
// on click the reload glyph spins -360deg over 500ms. Painted (not an icon asset)
// so the glyph is always crisp and present in both themes.
class ResetButton : public QPushButton {
public:
    explicit ResetButton(const DesignTokens& tk, QWidget* parent = nullptr)
        : QPushButton(parent), m_tk(tk) {
        setFixedSize(28, 28);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        connect(this, &QPushButton::clicked, this, [this]() {
            auto* a = new QVariantAnimation(this);
            a->setStartValue(0.0);
            a->setEndValue(-360.0);
            a->setDuration(500);
            a->setEasingCurve(QEasingCurve::OutCubic);
            connect(a, &QVariantAnimation::valueChanged, this,
                    [this](const QVariant& v) { m_spin = v.toReal(); update(); });
            connect(a, &QVariantAnimation::finished, this,
                    [this]() { m_spin = 0.0; update(); });
            a->start(QAbstractAnimation::DeleteWhenStopped);
        });
    }
protected:
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; update(); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(m_hover ? m_tk.accent : m_tk.border, 1.0));
        p.setBrush(m_hover ? QBrush(m_tk.accentWeak) : QBrush(Qt::NoBrush));
        p.drawEllipse(r);
        const QColor gc = m_hover ? m_tk.accent : m_tk.resetFg;
        p.save();
        p.translate(QRectF(rect()).center());
        p.rotate(m_spin);
        const qreal g = 15.0;
        paintReloadGlyph(p, QRectF(-g / 2.0, -g / 2.0, g, g), gc);
        p.restore();
    }
private:
    DesignTokens m_tk;
    qreal m_spin = 0.0;
    bool  m_hover = false;
};

// ---- IconSegment -----------------------------------------------------------
// The menu-bar-icon preset chooser: a single rounded, bordered strip of equal
// icon cells. The selected cell + hover wash are CLIPPED to the rounded 8px
// container (fixes the bug where the selection's square corners overflowed the
// strip's rounded corners). Glyphs are painted via paintPresetGlyph so they are
// 1:1 with the design. Click calls onSelect(index); the owner reloads the UI.
class IconSegment : public QWidget {
public:
    IconSegment(const DesignTokens& tk, const QStringList& names, int selected,
                std::function<void(int)> onSelect, QWidget* parent = nullptr)
        : QWidget(parent), m_tk(tk), m_names(names),
          m_sel(selected), m_onSelect(std::move(onSelect)) {
        setFixedSize(kCellW * m_names.size() + 1, 32);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }
    QSize sizeHint() const override { return QSize(kCellW * m_names.size() + 1, 32); }
protected:
    void leaveEvent(QEvent*) override { m_hover = -1; update(); }
    void mouseMoveEvent(QMouseEvent* e) override {
        const int h = hit(e->pos());
        if (h != m_hover) { m_hover = h; update(); }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int h = hit(e->pos());
        if (h >= 0 && m_onSelect) m_onSelect(h);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal rad = 8.0;
        const int n = m_names.size();
        const qreal cw = r.width() / n;

        QPainterPath clip;
        clip.addRoundedRect(r, rad, rad);
        p.save();
        p.setClipPath(clip);                 // clip washes to the rounded strip
        p.fillRect(r, m_tk.control);
        for (int i = 0; i < n; ++i) {
            if (i == m_sel || i == m_hover) {
                QRectF cell(r.left() + i * cw, r.top(), cw, r.height());
                p.fillRect(cell, m_tk.accentWeak);
            }
        }
        p.setPen(QPen(m_tk.border, 1.0));
        for (int i = 1; i < n; ++i) {
            const qreal x = r.left() + i * cw;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        for (int i = 0; i < n; ++i) {
            QRectF cell(r.left() + i * cw, r.top(), cw, r.height());
            const QColor c = (i == m_sel) ? m_tk.accent : m_tk.icon;
            const qreal g = 17.0;
            paintPresetGlyph(p, m_names[i],
                             QRectF(cell.center().x() - g / 2.0,
                                    cell.center().y() - g / 2.0, g, g), c);
        }
        p.restore();
        p.setPen(QPen(m_tk.border, 1.0));     // crisp outer border on top
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, rad, rad);
    }
private:
    int hit(const QPoint& pt) const {
        const int n = m_names.size();
        if (n == 0) return -1;
        const qreal cw = qreal(width()) / n;
        return qBound(0, int(pt.x() / cw), n - 1);
    }
    static constexpr int kCellW = 34;
    DesignTokens m_tk;
    QStringList  m_names;
    int m_sel = 0;
    int m_hover = -1;
    std::function<void(int)> m_onSelect;
};

// ---- LanguageCombo ---------------------------------------------------------
// QComboBox restyled to the design: rounded control + a custom up/down double-
// chevron (the native single triangle is hidden via QSS). The popup view is
// styled (rounded, padded items, accent-weak selection) by the caller's QSS.
class LanguageCombo : public QComboBox {
public:
    explicit LanguageCombo(const DesignTokens& tk, QWidget* parent = nullptr)
        : QComboBox(parent), m_tk(tk) {}
protected:
    void paintEvent(QPaintEvent* e) override {
        QComboBox::paintEvent(e);            // frame + text via QSS (arrow hidden)
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(m_tk.text2, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        const qreal cx = width() - 14.0, cy = height() / 2.0;
        QPainterPath up;
        up.moveTo(cx - 3.5, cy - 1.6);
        up.lineTo(cx, cy - 5.0);
        up.lineTo(cx + 3.5, cy - 1.6);
        QPainterPath down;
        down.moveTo(cx - 3.5, cy + 1.6);
        down.lineTo(cx, cy + 5.0);
        down.lineTo(cx + 3.5, cy + 1.6);
        p.drawPath(up);
        p.drawPath(down);
    }
private:
    DesignTokens m_tk;
};

// Version + edition (set as compile definitions in CMakeLists.txt). Defaults
// keep the file self-contained if a definition is ever missing.
#ifndef LIGHTGET_VERSION
#define LIGHTGET_VERSION "1.0.5"
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
    // Knob behaviour: animate the 180ms slide ONLY for a real user toggle; snap
    // for any programmatic setChecked() (initial build, reloadUI on an unrelated
    // change, theme switch). Without this every ON toggle re-slides from off->on
    // each rebuild, which reads as the toggles "twitching" on any settings change.
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        const qreal target = on ? 1.0 : 0.0;
        if (m_inUserClick) {
            auto* anim = new QPropertyAnimation(this, "knobPos", this);
            anim->setDuration(180);
            anim->setStartValue(m_knobPos);
            anim->setEndValue(target);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        } else {
            m_knobPos = target;   // snap — no slide
            update();
        }
    });
}

QSize ToggleSwitch::sizeHint() const { return QSize(38, 22); }

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* e) {
    // QAbstractButton flips the checked state (and emits toggled) inside the base
    // release handler; wrap it so the toggled handler above knows this change is
    // user-initiated and should animate.
    m_inUserClick = true;
    QAbstractButton::mouseReleaseEvent(e);
    m_inUserClick = false;
}

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
// AppearanceSegment — 3-way Auto / Light / Dark segmented switch
// ===========================================================================
AppearanceSegment::AppearanceSegment(const DesignTokens& tk, int initial,
                                     const QString& autoText, const QString& lightText,
                                     const QString& darkText, QWidget* parent)
    : QWidget(parent), m_tk(tk) {
    m_labels[0] = autoText;
    m_labels[1] = lightText;
    m_labels[2] = darkText;
    m_index = qBound(0, initial, 2);
    m_pillPos = m_index;
    setFixedSize(240, 32);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

QSize AppearanceSegment::sizeHint() const { return QSize(240, 32); }

void AppearanceSegment::setPillPos(qreal v) { m_pillPos = v; update(); }

void AppearanceSegment::setIndex(int i, bool animate) {
    i = qBound(0, i, 2);
    if (i == m_index) return;
    m_index = i;
    if (animate) {
        auto* a = new QPropertyAnimation(this, "pillPos", this);
        a->setDuration(240);
        a->setStartValue(m_pillPos);
        a->setEndValue(qreal(m_index));
        a->setEasingCurve(QEasingCurve::OutCubic);
        a->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        m_pillPos = m_index;
        update();
    }
    emit selected(m_index);
}

int AppearanceSegment::hitTest(const QPoint& p) const {
    const qreal seg = width() / 3.0;
    int i = int(p.x() / seg);
    return qBound(0, i, 2);
}

void AppearanceSegment::mousePressEvent(QMouseEvent* e) {
    setIndex(hitTest(e->pos()));
}

void AppearanceSegment::mouseMoveEvent(QMouseEvent* e) {
    const int h = hitTest(e->pos());
    if (h != m_hover) { m_hover = h; update(); }
}

void AppearanceSegment::leaveEvent(QEvent*) { m_hover = -1; update(); }

void AppearanceSegment::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Outer control: rounded 8px, control bg + border.
    p.setPen(QPen(m_tk.border, 1.0));
    p.setBrush(m_tk.control);
    p.drawRoundedRect(r, 8, 8);

    // Sliding pill under the active segment: accent-weak, 6px radius, 3px inset.
    const qreal segW = r.width() / 3.0;
    const qreal pillW = segW - 4.0;
    const qreal pillX = r.left() + 2.0 + m_pillPos * segW;
    QRectF pill(pillX, r.top() + 3.0, pillW, r.height() - 6.0);
    p.setPen(Qt::NoPen);
    p.setBrush(m_tk.accentWeak);
    p.drawRoundedRect(pill, 6, 6);

    // Each segment: glyph + label, accent if active else dim (text2).
    QFont f = font();
    f.setPointSizeF(9.0);
    f.setWeight(QFont::Medium);
    p.setFont(f);

    for (int i = 0; i < 3; ++i) {
        const bool active = (i == m_index);
        const QColor c = active ? m_tk.accent : m_tk.text2;
        const QRectF segR(r.left() + i * segW, r.top(), segW, r.height());

        // Measure label so we can lay glyph + gap + text centered together.
        const QString label = m_labels[i];
        const int textW = p.fontMetrics().horizontalAdvance(label);
        const qreal glyphSz = 14.0;
        const qreal gap = 5.0;
        const qreal totalW = glyphSz + gap + textW;
        const qreal startX = segR.center().x() - totalW / 2.0;
        const qreal cy = segR.center().y();

        // Glyph drawn 1:1 from the design (24x24 viewBox) into the 14px box.
        QRectF g(startX, cy - glyphSz / 2.0, glyphSz, glyphSz);
        const qreal gsc = glyphSz / 24.0;
        auto GP = [&](qreal x, qreal y) {
            return QPointF(g.left() + x * gsc, g.top() + y * gsc);
        };
        QPen gp(c, 1.7 * gsc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(gp);
        p.setBrush(Qt::NoBrush);
        if (i == 0) {
            // Auto: monitor — rect 3,4 18x13 r2 + base bar + stem (design).
            p.drawRoundedRect(QRectF(GP(3, 4), GP(21, 17)), 2 * gsc, 2 * gsc);
            p.drawLine(GP(8, 21), GP(16, 21));
            p.drawLine(GP(12, 17), GP(12, 21));
        } else if (i == 1) {
            // Light: sun — circle r4 + 8 detached rays (radius 8->10), per design.
            const QPointF ctr = g.center();
            p.drawEllipse(ctr, 4 * gsc, 4 * gsc);
            for (int a = 0; a < 8; ++a) {
                const qreal ang = a * (M_PI / 4.0);
                const qreal dx = std::cos(ang), dy = std::sin(ang);
                p.drawLine(QPointF(ctr.x() + dx * 8 * gsc,  ctr.y() + dy * 8 * gsc),
                           QPointF(ctr.x() + dx * 10 * gsc, ctr.y() + dy * 10 * gsc));
            }
        } else {
            // Dark: crescent moon (design M21 12.8 A8.5..11.2 3 A6.6..Z) — a stroked
            // crescent = big disc minus an upper-right-offset disc.
            QPainterPath outer; outer.addEllipse(GP(12, 12),   8.5 * gsc, 8.5 * gsc);
            QPainterPath cut;   cut.addEllipse(GP(16.5, 7.7),  8.2 * gsc, 8.2 * gsc);
            p.drawPath(outer.subtracted(cut));
        }

        // Label.
        p.setPen(c);
        const QRectF tR(startX + glyphSz + gap, segR.top(), textW + 2, segR.height());
        p.drawText(tR, Qt::AlignVCenter | Qt::AlignLeft, label);
    }
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

void CheckRow::positionTrailing() {
    if (!m_trailing) return;
    // Use the chip's REAL size, not sizeHint(): the chips are fixed-size
    // (setFixedSize) so their min==max==actual height (26), but QWidget::sizeHint()
    // ignores the fixed size and reports the layout's natural height (~18, driven
    // by the 18px inner glyph). Centering against the 18px hint while the chip
    // paints 26px tall is exactly what pushed every chip ~4px below the checkbox.
    // For a fixed-size widget minimumSize()==maximumSize()==the locked size.
    QSize sz = m_trailing->maximumSize();
    if (sz.width() >= QWIDGETSIZE_MAX || sz.height() >= QWIDGETSIZE_MAX ||
        !sz.isValid() || sz.isEmpty()) {
        sz = m_trailing->size();
        if (!sz.isValid() || sz.isEmpty()) sz = m_trailing->sizeHint();
    }
    // Right-aligned to the shared chip column; vertically centered on height()/2,
    // the exact reference the checkbox is painted against (by = (height()-20)/2,
    // center = height()/2). With the true 26px height these centers coincide.
    const int x = width() - kRowPadH - sz.width();
    const int y = (height() - sz.height()) / 2;
    if (m_trailing->geometry() != QRect(x, y, sz.width(), sz.height()))
        m_trailing->setGeometry(x, y, sz.width(), sz.height());
}

void CheckRow::resizeEvent(QResizeEvent*) {
    positionTrailing();
}

void CheckRow::paintEvent(QPaintEvent*) {
    // Reposition against the FINAL height() before painting, so the chip's
    // vertical center can't drift from the checkbox's (resizeEvent may have run
    // at a transient height during layout / offscreen grab).
    positionTrailing();

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

void HotkeyRecorder::focusOutEvent(QFocusEvent* event) {
    if (m_recording) {   // clicked away / rebuilt while armed: cancel cleanly
        m_recording = false;
        setStyleSheet(m_idleStyle);
        setText(Settings::instance().hotKeyDisplay());
    }
    QPushButton::focusOutEvent(event);
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
    // Require at least one of Cmd/Ctrl/Alt — Shift ALONE is not enough. A
    // Shift+<letter> global hotkey would swallow that capital letter in every
    // app (typing "A" would fire the capture and eat the keystroke).
    if ((carbonMods & ~CarbonKeys::shiftKey) == 0) {
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
    if (carbonCode == 0) {   // key this platform's hotkey backend can't map
        QApplication::beep();
        return;              // keep recording; don't persist an unregisterable combo
    }
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
    // Unknown key -> 0 (unmappable). On macOS a valid Carbon code was already
    // returned above from nativeVK; off macOS the native VK is NOT a Carbon code,
    // and persisting it as one silently mis-registered the hotkey (e.g. Windows
    // VK_HOME 0x24 collides with Carbon kVK_Return, so Ctrl+Home registered
    // Ctrl+Enter). Returning 0 makes the caller reject the key instead.
    return 0;
}

// ===========================================================================
// SettingsWindow
// ===========================================================================

SettingsWindow::SettingsWindow(QWidget* parent) : QDialog(parent) {
    // Frameless, fixed-size window with a custom 46px title bar (traffic lights +
    // centered title). Persists across open/close (the owner keeps the instance
    // alive). Sized to the design's 500-wide layout; tall enough that the grouped
    // card + About footer breathe (design canvas is 500x894).
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);   // rounded corners show through
    // Design height is 840, but on small displays (1366x768 laptops — common on
    // Windows/Linux) that exceeds the work area: the frameless window has no OS
    // resize, so the title bar (the only move/close control) lands off-screen and
    // the bottom rows are cut off with no way to reach them. Clamp the height to
    // the available work area; buildUI() wraps the body in a QScrollArea so any
    // overflow scrolls instead of being lost.
    int availH = 840;
    if (QScreen* scr = QGuiApplication::screenAt(QCursor::pos()))
        availH = scr->availableGeometry().height();
    else if (QScreen* pr = QGuiApplication::primaryScreen())
        availH = pr->availableGeometry().height();
    setFixedSize(500, qBound(420, availH - 48, 840));

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

void SettingsWindow::refreshHotKeyDisplay() {
    if (m_recorder) m_recorder->setText(Settings::instance().hotKeyDisplay());
}

void SettingsWindow::showCentered() {
    // Center on the screen under the cursor (accessory-app focus dance), but
    // clamp so the window never spills past the work area — the title bar is the
    // only move/close control, so it must stay on-screen on small displays.
    if (QScreen* scr = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect g = scr->availableGeometry();
        QPoint p = g.center() - rect().center();
        p.setX(qBound(g.left(), p.x(), g.right() - width() + 1));
        p.setY(qBound(g.top(),  p.y(), g.bottom() - height() + 1));
        move(p);
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
// Frameless chrome painting + title-bar drag
// ---------------------------------------------------------------------------
void SettingsWindow::paintEvent(QPaintEvent*) {
    // The window is frameless + translucent; paint the rounded 12px body bg here
    // so the panel/cards sit on the design's window background. The title bar
    // (its own widget) paints the top with matching top-rounded corners.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(r, 12, 12);
    p.fillPath(path, m_tk.bg);
    p.setPen(QPen(m_tk.border, 1.0));
    p.drawPath(path);
}

void SettingsWindow::mousePressEvent(QMouseEvent* e) {
    // Begin a window drag only when the press lands on the custom title bar (and
    // not on one of its child controls, e.g. the traffic-light dots).
    if (e->button() == Qt::LeftButton && m_titleBar) {
        const QPoint inBar = m_titleBar->mapFrom(this, e->pos());
        if (m_titleBar->rect().contains(inBar)) {
            QWidget* child = m_titleBar->childAt(inBar);
            // Allow drag from the bar itself or the (transparent) title label.
            if (!child || qobject_cast<QLabel*>(child)) {
                m_dragging = true;
                m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
                e->accept();
                return;
            }
        }
    }
    QDialog::mousePressEvent(e);
}

void SettingsWindow::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        move(e->globalPosition().toPoint() - m_dragOffset);
        e->accept();
        return;
    }
    QDialog::mouseMoveEvent(e);
}

void SettingsWindow::mouseReleaseEvent(QMouseEvent* e) {
    m_dragging = false;
    QDialog::mouseReleaseEvent(e);
}

// ---------------------------------------------------------------------------
// Appearance — apply chosen color scheme to the whole app
// ---------------------------------------------------------------------------
void SettingsWindow::applyAppearance(const QString& mode) {
    if (auto* hints = QGuiApplication::styleHints()) {
        if (mode == QStringLiteral("light"))
            hints->setColorScheme(Qt::ColorScheme::Light);
        else if (mode == QStringLiteral("dark"))
            hints->setColorScheme(Qt::ColorScheme::Dark);
        else
            hints->setColorScheme(Qt::ColorScheme::Unknown);   // Auto = follow OS
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
        // Deleting a QLayout does NOT delete the widgets it managed. The old title
        // bar + scroll/body tree would leak (and keep painting underneath) on
        // EVERY rebuild — language change, bar-icon click, per-setting reset, and
        // two rebuilds per light/dark flip — accumulating across a long tray
        // session. Collect the top-level managed widgets and delete them too
        // (m_recorder was reparented to `this` above, so it is not among them).
        QList<QWidget*> stale;
        for (int i = 0; i < old->count(); ++i)
            if (QWidget* w = old->itemAt(i)->widget()) stale.append(w);
        delete old;
        for (QWidget* w : stale) w->deleteLater();
    }
    buildUI();
}

void SettingsWindow::buildUI() {
    setWindowTitle(Loc::t("settings.title"));
    resolveTokens();

    // The frameless window background is painted in paintEvent (rounded 12px).
    // Keep the stylesheet empty here so it doesn't override that paint.
    setStyleSheet(QString());

    // ===== Custom title bar (46px chrome) =====
    m_titleBar = buildTitleBar();

    // ===== Folder-style tabs (General / Features) =====
    // Top-rounded "folder tabs" sitting on the panel: the active tab uses the
    // panel bg and drops its bottom border (so it visually merges with the
    // panel below), inactive tabs use the muted tab-inactive bg.
    auto* tabBar = new QWidget;
    auto* tabRow = new QHBoxLayout(tabBar);
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(6);

    const QColor inactiveBg = m_tk.dark ? QColor("#202023") : QColor("#e7e7ea");
    // Folder tab style applied EXPLICITLY per active/inactive (not via :checked).
    // The :checked pseudo-state is not re-evaluated by Qt's QSS engine in the
    // offscreen --render-dump grab (no event loop), which made the active-tab
    // highlight look stuck on General. Setting the full stylesheet on each switch
    // repaints reliably in both the live app and the render.
    auto styleTab = [this, inactiveBg](QPushButton* b, bool active) {
        b->setStyleSheet(QStringLiteral(
            "QPushButton {"
            " padding:8px 26px; font-size:13px; font-weight:%1;"
            " color:%2; background-color:%3;"
            " border:1px solid %4; border-bottom:none;"
            " border-top-left-radius:9px; border-top-right-radius:9px;"
            " border-bottom-left-radius:0; border-bottom-right-radius:0; }"
            "QPushButton:hover { color:%5; }")
            .arg(QString(active ? "600" : "500"),
                 colCss(active ? m_tk.text : m_tk.text2),
                 colCss(active ? m_tk.bg : inactiveBg),
                 colCss(m_tk.border),
                 colCss(m_tk.text)));
    };
    auto makeTab = [&](const QString& text) {
        auto* b = new QPushButton(text);
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        styleTab(b, false);
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

    // ===== Stacked pages inside a rounded panel =====
    m_stack = new QStackedWidget;
    m_stack->setObjectName("pagePane");
    m_stack->setStyleSheet(QStringLiteral(
        "#pagePane { background-color:%1; border:1px solid %2; border-radius:10px; }")
        .arg(colCss(m_tk.bg), colCss(m_tk.border)));
    m_stack->addWidget(buildGeneralTab());
    m_stack->addWidget(buildFeaturesTab());

    auto applyActiveTab = [this, styleTab](int id) {
        styleTab(m_tabGeneral,  id == 0);
        styleTab(m_tabFeatures, id == 1);
    };
    connect(tabGroup, &QButtonGroup::idClicked, this, [this, applyActiveTab](int id) {
        m_currentTab = id;
        m_stack->setCurrentIndex(id);
        applyActiveTab(id);
    });

    // Restore the previously selected tab across rebuilds (explicit style + state).
    m_currentTab = (m_currentTab == 1) ? 1 : 0;
    (m_currentTab == 1 ? m_tabFeatures : m_tabGeneral)->setChecked(true);
    m_stack->setCurrentIndex(m_currentTab);
    applyActiveTab(m_currentTab);

    // The tab row overlaps the panel by 1px so the active tab merges into it.
    auto* body = new QWidget;
    auto* bodyCol = new QVBoxLayout(body);
    bodyCol->setContentsMargins(12, 12, 12, 12);
    bodyCol->setSpacing(0);
    bodyCol->addWidget(tabBar, 0, Qt::AlignHCenter);
    bodyCol->addSpacing(-1);
    bodyCol->addWidget(m_stack, 1);

    // Wrap the body in a scroll area so that when the window height was clamped
    // below the design's 840 (small screens), the overflowing rows stay reachable
    // by scrolling instead of being clipped off the bottom. The title bar stays
    // pinned above it, always draggable/closable. On full-height screens the
    // content fits and no scrollbar shows.
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAttribute(Qt::WA_TranslucentBackground, true);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(body);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_titleBar);
    root->addWidget(scroll, 1);
}

// ---------------------------------------------------------------------------
// Custom title bar — 46px chrome: 3 traffic-light dots + centered title
// ---------------------------------------------------------------------------
QWidget* SettingsWindow::buildTitleBar() {
    auto* bar = new QWidget;
    bar->setObjectName("titleBar");
    bar->setFixedHeight(46);
    const QColor chrome = m_tk.dark ? QColor("#28282b") : QColor("#f5f5f7");
    // The chrome strip carries the window's top rounded corners; only round the
    // top so the bottom edge sits flush against the body.
    bar->setStyleSheet(QStringLiteral(
        "#titleBar { background-color:%1; border:1px solid %2;"
        " border-bottom:1px solid %2;"
        " border-top-left-radius:12px; border-top-right-radius:12px;"
        " border-bottom-left-radius:0; border-bottom-right-radius:0; }")
        .arg(colCss(chrome), colCss(m_tk.border)));

    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(16, 0, 16, 0);
    h->setSpacing(8);

    // Single "close" control. On macOS it sits on the LEFT (traffic-light
    // convention); on Windows/Linux it sits on the RIGHT to match those
    // platforms' window conventions. The yellow (minimize) and green (zoom) dots
    // are dropped — this is a fixed-size accessory panel, so they'd be inert.
    auto addCloseDot = [this, h]() {
        auto* dot = new QPushButton;
        dot->setFixedSize(12, 12);
        dot->setCursor(Qt::PointingHandCursor);
        dot->setFocusPolicy(Qt::NoFocus);
        dot->setStyleSheet(QStringLiteral(
            "QPushButton { background-color:#ff5f57; border:none; border-radius:6px; }"
            "QPushButton:hover { background-color:#ff4438; }"));
        connect(dot, &QPushButton::clicked, this, &QWidget::close);
        h->addWidget(dot, 0, Qt::AlignVCenter);
    };

#if defined(Q_OS_MACOS)
    addCloseDot();
    h->addStretch(1);
#else
    h->addStretch(1);
    addCloseDot();
#endif

    // Centered title overlaid across the full bar width (so the dots don't push
    // it off-center). Use a child label positioned to span the bar.
    auto* title = new QLabel(Loc::t("settings.title"), bar);
    title->setAlignment(Qt::AlignCenter);
    title->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    title->setStyleSheet(QStringLiteral(
        "color:%1; font-size:14px; font-weight:600; background:transparent;")
        .arg(colCss(m_tk.text)));
    // Span the whole bar; centered text stays centered regardless of the dots.
    title->setGeometry(0, 0, 500, 46);
    title->lower();   // keep it behind the dots for click-through safety

    return bar;
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
    auto* langCombo = new LanguageCombo(m_tk);
    for (const auto& l : kLanguages) langCombo->addItem(QString::fromUtf8(l.title));
    int langIdx = 0;
    for (size_t i = 0; i < kLanguages.size(); ++i)
        if (s.language() == QString::fromUtf8(kLanguages[i].code)) { langIdx = int(i); break; }
    langCombo->setCurrentIndex(langIdx);
    langCombo->setMinimumWidth(150);
    langCombo->setFixedHeight(30);
    langCombo->setCursor(Qt::PointingHandCursor);
    // Native single-triangle arrow hidden (LanguageCombo paints the design's
    // up/down double-chevron); rounded control + a clean, padded popup with an
    // accent-weak selection, matching the design.
    langCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background-color:%1; color:%2; border:1px solid %3;"
        " border-radius:8px; padding:0 28px 0 12px; font-size:13px; }"
        "QComboBox:hover { border-color:%4; }"
        "QComboBox:focus { border-color:%4; }"
        "QComboBox::drop-down { border:none; width:26px; }"
        "QComboBox::down-arrow { image:none; width:0; height:0; }"
        "QComboBox QAbstractItemView {"
        " background-color:%1; color:%2; border:1px solid %3; border-radius:8px;"
        " padding:4px; outline:none;"
        " selection-background-color:%5; selection-color:%4; }"
        "QComboBox QAbstractItemView::item {"
        " min-height:26px; padding:2px 8px; border-radius:6px; }")
        .arg(colCss(m_tk.control), colCss(m_tk.text), colCss(m_tk.border),
             colCss(m_tk.accent), colCss(m_tk.accentWeak)));
    connect(langCombo, QOverload<int>::of(&QComboBox::activated),
            this, &SettingsWindow::onLanguageChanged);
    addRowWidget(makeRow(Loc::t("settings.language.plain"), langCombo, nullptr));

    // --- Appearance: NEW 3-way segmented switch (Auto / Light / Dark) ---
    {
        const QString mode = s.appearance();
        const int idx = (mode == QStringLiteral("light")) ? 1
                      : (mode == QStringLiteral("dark"))  ? 2 : 0;
        auto* seg = new AppearanceSegment(
            m_tk, idx,
            Loc::t("settings.appearance.auto"),
            Loc::t("settings.appearance.light"),
            Loc::t("settings.appearance.dark"));
        connect(seg, &AppearanceSegment::selected, this, [this](int i) {
            const QString m = (i == 1) ? QStringLiteral("light")
                            : (i == 2) ? QStringLiteral("dark")
                                       : QStringLiteral("auto");
            Settings::instance().setAppearance(m);
            applyAppearance(m);   // live-apply the color scheme
        });
        addRowWidget(makeRow(Loc::t("settings.appearance"), seg, nullptr));
    }

    // --- Menu-bar icon: clipped segmented preset chooser + custom-image button ---
    {
        auto* iconWrap = new QWidget;
        auto* h = new QHBoxLayout(iconWrap);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);

        // Custom image active -> no preset is highlighted; else highlight the
        // stored preset (default "feather" = index 0).
        const bool usingCustom = s.barIconCustomPath().has_value();
        int selectedSeg = -1;
        if (!usingCustom) {
            selectedSeg = kBarIcons.indexOf(s.barIcon());
            if (selectedSeg < 0) selectedSeg = 0;
        }

        // One rounded, bordered strip with the selection wash CLIPPED to the
        // rounded corners (IconSegment). Click -> set preset + reload.
        auto* strip = new IconSegment(
            m_tk, kBarIcons, selectedSeg,
            [this](int i) { onBarIconSegment(i); });
        h->addWidget(strip, 0, Qt::AlignVCenter);

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
        custom->setIcon(presetGlyphIcon("photo", customTint, 17));
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

        addRowWidget(makeRow(Loc::t("settings.barIcon.plain"), iconWrap, nullptr));
    }

    // --- Hotkey recorder + reset ---
    m_recorder->applyTokens(m_tk);
    m_recorder->setText(s.hotKeyDisplay());
    m_recorder->setFixedSize(kFieldWidth, 32);
    addRowWidget(makeRow(Loc::t("settings.hotkey.plain"), m_recorder,
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
        addRowWidget(makeRow(Loc::t("settings.saveFolder.plain"), folderBtn,
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
        addRowWidget(makeRow(Loc::t("settings.dim.plain"), slider,
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

    // Preview-chip builder for the trailing column on every Features row. Each
    // chip is a fixed-height (26) box whose width hugs its content, then is LOCKED
    // to that exact size with setFixedSize(). Locking matters: a QSS-styled
    // QWidget's sizeHint() can disagree with its laid-out width, and CheckRow
    // positions the trailing chip from its sizeHint — that mismatch is what pushed
    // the wider colour-dots chip out of the shared right column. With a fixed
    // size, sizeHint() == painted size, so every chip's RIGHT EDGE aligns to the
    // same column and each is vertically centered in its row (see CheckRow).
    auto previewChip = [&](QWidget* inner) -> QWidget* {
        auto* chip = new QWidget;
        chip->setObjectName("chip");
        chip->setStyleSheet(QStringLiteral(
            "#chip { background-color:%1; border:1px solid %2; border-radius:7px; }")
            .arg(colCss(m_tk.controlFill), colCss(m_tk.border)));
        auto* ch = new QHBoxLayout(chip);
        ch->setContentsMargins(11, 0, 11, 0);
        ch->setSpacing(5);
        ch->addWidget(inner, 0, Qt::AlignCenter);   // center content H+V in the chip
        // Resolve the content-driven width now, clamp to a shared minimum so the
        // narrow chips (line / dot) don't look tiny, then LOCK it so sizeHint()
        // and the actual width can never diverge.
        ch->activate();
        constexpr int kChipMinW = 56;   // shared minimum chip-column width
        const int w = qMax(kChipMinW, chip->sizeHint().width());
        chip->setFixedSize(w, 26);
        return chip;
    };

    // ===== Tools in the toolbar =====
    {
        std::vector<std::function<CheckRow*()>> makers;
        for (Tool t : kFeatureTools) {
            makers.push_back([this, t, &s, &previewChip]() {
                const QString key = QStringLiteral("tool.%1")
                                        .arg(QString::fromUtf8(toolKey(t)));
                // Right-side preview chip showing the tool's red glyph; "Text"
                // uses a red "Abc" label per the design.
                QWidget* inner = nullptr;
                if (t == Tool::Text) {
                    auto* abc = new QLabel(QStringLiteral("Abc"));
                    abc->setStyleSheet(QStringLiteral(
                        "color:#ff453a; font-weight:700; font-size:14px;"
                        " letter-spacing:0.2px;"));
                    inner = abc;
                } else {
                    inner = new ToolGlyph(t);
                }
                auto* chip = previewChip(inner);
                auto* row = new CheckRow(m_tk, Loc::t(key), s.isToolEnabled(t), chip);
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
        // Show color palette + a custom-painted 5-dot preview as the trailing chip.
        // Using a fixed-size painted PaletteGlyph (exactly like the tool chips use
        // ToolGlyph) keeps the dots centered in the chip and the chip the same
        // compact width as the tool chips.
        makers.push_back([this, &s, &previewChip]() {
            auto* glyph = new PaletteGlyph(m_tk.border);
            auto* chip = previewChip(glyph);
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
    // Order mirrors the design: Font / Font size / Bold / Italic / Underline /
    // Text alignment / Text background color. The first five back NEW Settings
    // flags; the last two keep their existing settings/handlers.
    {
        // Generic builder: a CheckRow with a preview chip, wired to a getter/setter.
        auto makeTextRow = [this, &previewChip](
                const QString& labelKey, bool checkedState, QWidget* chipInner,
                std::function<void(bool)> apply) -> CheckRow* {
            auto* chip = previewChip(chipInner);
            auto* row = new CheckRow(m_tk, Loc::t(labelKey), checkedState, chip);
            connect(row, &QAbstractButton::toggled, this,
                    [apply](bool on) { apply(on); });
            return row;
        };

        std::vector<std::function<CheckRow*()>> makers;

        // Font — "Aa" upright + "Aa" italic serif.
        makers.push_back([this, &s, makeTextRow]() {
            auto* wrap = new QWidget;
            auto* h = new QHBoxLayout(wrap);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(8);
            auto* upright = new QLabel(QStringLiteral("Aa"));
            upright->setStyleSheet(QStringLiteral(
                "color:%1; font-weight:600; font-size:15px;").arg(colCss(m_tk.text)));
            auto* italic = new QLabel(QStringLiteral("Aa"));
            italic->setStyleSheet(QStringLiteral(
                "color:%1; font-style:italic; font-weight:500; font-size:15px;"
                " font-family:Georgia,'Times New Roman',serif;").arg(colCss(m_tk.text)));
            h->addWidget(upright);
            h->addWidget(italic);
            return makeTextRow("features.textFont", s.textFontEnabled(), wrap,
                               [](bool on){ Settings::instance().setTextFontEnabled(on); });
        });

        // Font size — small "A" + large "A".
        makers.push_back([this, &s, makeTextRow]() {
            auto* wrap = new QWidget;
            auto* h = new QHBoxLayout(wrap);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(5);
            auto* small = new QLabel(QStringLiteral("A"));
            small->setStyleSheet(QStringLiteral(
                "color:%1; font-weight:700; font-size:11px;").arg(colCss(m_tk.text)));
            auto* big = new QLabel(QStringLiteral("A"));
            big->setStyleSheet(QStringLiteral(
                "color:%1; font-weight:700; font-size:17px;").arg(colCss(m_tk.text)));
            h->addWidget(small, 0, Qt::AlignBottom);
            h->addWidget(big, 0, Qt::AlignBottom);
            return makeTextRow("features.textFontSize", s.textFontSizeEnabled(), wrap,
                               [](bool on){ Settings::instance().setTextFontSizeEnabled(on); });
        });

        // Bold — "Bold" in heavy weight.
        makers.push_back([this, &s, makeTextRow]() {
            auto* lbl = new QLabel(QStringLiteral("Bold"));
            lbl->setStyleSheet(QStringLiteral(
                "color:%1; font-weight:800; font-size:14px;").arg(colCss(m_tk.text)));
            return makeTextRow("features.textBold", s.textBoldEnabled(), lbl,
                               [](bool on){ Settings::instance().setTextBoldEnabled(on); });
        });

        // Italic — "Italic" slanted.
        makers.push_back([this, &s, makeTextRow]() {
            auto* lbl = new QLabel(QStringLiteral("Italic"));
            lbl->setStyleSheet(QStringLiteral(
                "color:%1; font-style:italic; font-weight:500; font-size:14px;")
                .arg(colCss(m_tk.text)));
            return makeTextRow("features.textItalic", s.textItalicEnabled(), lbl,
                               [](bool on){ Settings::instance().setTextItalicEnabled(on); });
        });

        // Underline — "Under" underlined.
        makers.push_back([this, &s, makeTextRow]() {
            auto* lbl = new QLabel(QStringLiteral("Under"));
            lbl->setStyleSheet(QStringLiteral(
                "color:%1; text-decoration:underline; font-weight:500; font-size:14px;")
                .arg(colCss(m_tk.text)));
            return makeTextRow("features.textUnderline", s.textUnderlineEnabled(), lbl,
                               [](bool on){ Settings::instance().setTextUnderlineEnabled(on); });
        });

        // Text alignment — left-align glyph (3 lines), painted.
        makers.push_back([this, &s, makeTextRow]() {
            auto* glyph = new AlignGlyph(m_tk.text);
            return makeTextRow("features.textAlignment", s.textAlignmentEnabled(), glyph,
                               [](bool on){ Settings::instance().setTextAlignmentEnabled(on); });
        });

        // Text background color — highlighted "Aa" chip.
        makers.push_back([this, &s, makeTextRow]() {
            auto* hl = new QLabel(QStringLiteral("Aa"));
            hl->setStyleSheet(QStringLiteral(
                "background-color:#ffd60a; color:#1c1c1e; padding:2px 6px;"
                " border-radius:4px; font-weight:600; font-size:13px;"));
            return makeTextRow("features.textBackground", s.textBackgroundEnabled(), hl,
                               [](bool on){ Settings::instance().setTextBackgroundEnabled(on); });
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
    // Footer copyright/version: design size is 11.5px; use 12px so it reads
    // clearly (the old 9pt name + 10px version were noticeably too small).
    const int fs = 12;
    auto* pre = grayText(QStringLiteral("© "), fs);
    auto* nameLink = new InlineLinkLabel(QStringLiteral("Sergey Emelyanov"),
                                         QStringLiteral("https://github.com/VeDono"),
                                         fs, m_tk.text3, m_tk.link);
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
    version->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                               .arg(colCss(m_tk.text3)));
    generalCol->addSpacing(4);
    generalCol->addWidget(version);
}

// ---------------------------------------------------------------------------
// Reset button + handler
// ---------------------------------------------------------------------------
QPushButton* SettingsWindow::makeResetButton(ResetTarget target) {
    // Painted 28px circular reset (ResetButton): crisp reload glyph, accent hover
    // wash + border, and a -360° spin on click — no asset dependency, so the icon
    // can never come up missing/blurry. The spin is handled inside ResetButton;
    // we only wire the actual reset action here.
    auto* b = new ResetButton(m_tk);
    b->setToolTip(Loc::t("reset.tooltip"));
    connect(b, &QPushButton::clicked, this,
            [this, target]() { resetTapped(target); });
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
        // Quote + escape the Exec value per the Desktop Entry spec: an install
        // path with spaces (e.g. /home/u/My Apps/LightGet) parsed unquoted splits
        // into program "/home/u/My" + arg "Apps/LightGet", so autostart silently
        // fails while the toggle reports success (the Windows branch quotes too).
        QString exe = QApplication::applicationFilePath();
        exe.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        exe.replace(QLatin1Char('"'),  QLatin1String("\\\""));
        exe.replace(QLatin1Char('`'),  QLatin1String("\\`"));
        exe.replace(QLatin1Char('$'),  QLatin1String("\\$"));
        const QByteArray data =
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=LightGet\n"
            "Exec=\"" + exe.toUtf8() + "\"\n"
            "X-GNOME-Autostart-enabled=true\n";
        const bool ok = f.write(data) == data.size();
        f.close();
        return ok;
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
