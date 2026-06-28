// Toolbar.cpp — floating toolbar + text inspector (port of ToolbarView.swift).
//
// Two frameless overlay panels:
//   ToolbarView       : tools + optional color palette + actions.
//   TextInspectorView : text color row + optional background row.
//
// Qt is top-left / +Y down already, so the Swift `isFlipped` views need no
// conversion (Spec 4). The Swift closures become Qt signals. Buttons never take
// keyboard focus (Qt::NoFocus) so overlay shortcuts keep flowing.
//
// SF Symbols are unavailable cross-platform, so each glyph is drawn with vector
// QPainter paths inside a QIcon (semantically equal to the macOS system symbol).

#include "Toolbar.h"
#include "Settings.h"
#include "Localization.h"

#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QBrush>
#include <QIcon>
#include <QPixmap>
#include <QFont>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QTransform>
#include <QStyle>
#include <QStyleOptionButton>
#include <QEnterEvent>
#include <QEvent>
#include <QPolygonF>
#include <QtMath>
#include <cmath>
#include <cstdlib>   // std::abs(int) in Palette::sameColor
#include <cstring>   // std::strcmp (close-action detection)

// ---------------------------------------------------------------------------
// Palette namespace
// ---------------------------------------------------------------------------
namespace Palette {

// 0 red, 1 green, 2 blue, 3 yellow, 4 black, 5 white.
// Values mirror AppKit's .systemRed/.systemGreen/.systemBlue/.systemYellow.
const QVector<QColor>& colors() {
    // Design palette (toolbar HTML §4): brighter "dark-mode" hues + near-black,
    // used by both the toolbar wells and the text colour/background popups.
    static const QVector<QColor> c = {
        QColor(0xff, 0x45, 0x3a),   // red    #ff453a
        QColor(0x32, 0xd7, 0x4b),   // green  #32d74b
        QColor(0x0a, 0x84, 0xff),   // blue   #0a84ff
        QColor(0xff, 0xd6, 0x0a),   // yellow #ffd60a
        QColor(0x1c, 0x1c, 0x1e),   // near-black #1c1c1e
        QColor(255, 255, 255),      // white  #ffffff
    };
    return c;
}

// ITU-R BT.601 luma; light if brightness > 0.6 (sRGB 0..1, alpha ignored).
bool isLight(const QColor& c) {
    const double brightness = 0.299 * (c.red()   / 255.0)
                            + 0.587 * (c.green() / 255.0)
                            + 0.114 * (c.blue()  / 255.0);
    return brightness > 0.6;
}

// Per-channel sRGB equality within 0.02 (alpha ignored).
bool sameColor(const QColor& a, const QColor& b) {
    return std::abs(a.red()   - b.red())   / 255.0 < 0.02
        && std::abs(a.green() - b.green()) / 255.0 < 0.02
        && std::abs(a.blue()  - b.blue())  / 255.0 < 0.02;
}

} // namespace Palette

// ---------------------------------------------------------------------------
// Local glyph + helper utilities (file-private)
// ---------------------------------------------------------------------------
namespace {

// Design tokens (toolbar_canvas.html): surface #242429, accent #0A84FF, border
// white@9%. The toolbar panel uses radius 16; the inspector keeps the tighter 8.
const QColor kPanelSurface(0x24, 0x24, 0x29);     // #242429
const QColor kPanelBorder(255, 255, 255, 23);     // rgba(255,255,255,0.09)
constexpr double kPanelRadiusToolbar   = 8.0;   // native cornerRadius
constexpr double kPanelRadiusInspector = 8.0;

const QColor kSelectedBlue(0x0a, 0x84, 0xff);     // accent / active — #0A84FF
const QColor kGlyphIdle(0xd4, 0xd5, 0xda);        // resting glyph color — #D4D5DA
const QColor kGlyphSelected(255, 255, 255);       // glyph on the blue fill
const QColor kCloseGlyph(0xff, 0x69, 0x61);       // close action glyph — #FF6961
const QColor kHoverFill(255, 255, 255, 23);       // hover bg — rgba(255,255,255,0.09)
const QColor kCloseHoverFill(0xff, 0x69, 0x61, 41);// close hover — rgba(255,105,97,0.16)
const QColor kSeparator(255, 255, 255, 26);       // 1px divider — rgba(255,255,255,0.1)

// Render a toolbar glyph centered into a transparent pixmap of `size`, stroked /
// filled in `tint`. Each glyph is drawn 1:1 from the design's 24x24 SVG viewBox
// (see the per-glyph comments below), scaled into a centered side*0.525 box.
QIcon makeGlyph(const QString& symbol, const QColor& tint, int size = 30) {
    const int side = size;
    const qreal dpr = 2.0;
    QPixmap pm(int(side * dpr), int(side * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Each glyph is drawn 1:1 from the toolbar design SVGs (24x24 viewBox), scaled
    // into a centered glyph box of side*0.525 (≈21px in a 40px button — the design
    // ratio). PT() maps design coords into that box; default stroke = the design's
    // 1.8/24 line weight, round caps/joins.
    const qreal glyphBox = side * 0.525;
    const qreal gx = (side - glyphBox) / 2.0, gy = (side - glyphBox) / 2.0;
    const qreal sc = glyphBox / 24.0;
    auto PT = [&](qreal x, qreal y) { return QPointF(gx + x * sc, gy + y * sc); };
    const qreal SW = 1.8 * sc;
    QPen stroke(tint, SW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(stroke);
    p.setBrush(Qt::NoBrush);

    if (symbol == "cursorarrow") {
        // Design cursor: M5 3 l5.4 14.6 l2.2 -6.1 l6.1 -2.2 z (filled).
        QPainterPath path;
        path.moveTo(PT(5, 3));
        path.lineTo(PT(10.4, 17.6));
        path.lineTo(PT(12.6, 11.5));
        path.lineTo(PT(18.7, 9.3));
        path.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(tint);
        p.drawPath(path);
    } else if (symbol == "arrow.up.right") {
        p.drawLine(PT(6, 18), PT(17.5, 6.5));
        QPainterPath head;
        head.moveTo(PT(10, 6.5));
        head.lineTo(PT(17.5, 6.5));
        head.lineTo(PT(17.5, 14));
        p.drawPath(head);
    } else if (symbol == "line.diagonal") {
        p.drawLine(PT(5, 19), PT(19, 5));
    } else if (symbol == "rectangle") {
        p.drawRoundedRect(QRectF(PT(4, 6.5), PT(20, 17.5)), 2.5 * sc, 2.5 * sc);
    } else if (symbol == "rectangle.fill") {
        p.setBrush(tint);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(PT(4, 6.5), PT(20, 17.5)), 2.5 * sc, 2.5 * sc);
    } else if (symbol == "pencil") {
        // Design pen: M5 19 l1.4 -4 L16 5.4 l2.6 2.6 L9 17.6 z + ferrule M14 7.4 l2.6 2.6.
        QPainterPath body;
        body.moveTo(PT(5, 19));
        body.lineTo(PT(6.4, 15));
        body.lineTo(PT(16, 5.4));
        body.lineTo(PT(18.6, 8));
        body.lineTo(PT(9, 17.6));
        body.closeSubpath();
        p.drawPath(body);
        p.drawLine(PT(14, 7.4), PT(16.6, 10));
    } else if (symbol == "textformat") {
        p.setPen(tint);
        QFont f = p.font();
        f.setPixelSize(int(glyphBox * 0.82));
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.drawText(QRectF(gx, gy, glyphBox, glyphBox), Qt::AlignCenter, QStringLiteral("Aa"));
    } else if (symbol == "arrow.uturn.backward") {
        // Undo: shaft + right semicircle to (16,18) + open arrowhead at the start.
        QPainterPath h;
        h.moveTo(PT(8.5, 9));
        h.lineTo(PT(16, 9));
        h.cubicTo(PT(18.485, 9), PT(20.5, 11.015), PT(20.5, 13.5));
        h.cubicTo(PT(20.5, 15.985), PT(18.485, 18), PT(16, 18));
        h.lineTo(PT(11, 18));
        p.drawPath(h);
        p.drawLine(PT(8.5, 9), PT(11.7, 6));
        p.drawLine(PT(8.5, 9), PT(11.7, 12));
    } else if (symbol == "arrow.uturn.forward") {
        // Redo: mirror of undo (left semicircle to (8,18)).
        QPainterPath h;
        h.moveTo(PT(15.5, 9));
        h.lineTo(PT(8, 9));
        h.cubicTo(PT(5.515, 9), PT(3.5, 11.015), PT(3.5, 13.5));
        h.cubicTo(PT(3.5, 15.985), PT(5.515, 18), PT(8, 18));
        h.lineTo(PT(13, 18));
        p.drawPath(h);
        p.drawLine(PT(15.5, 9), PT(12.3, 6));
        p.drawLine(PT(15.5, 9), PT(12.3, 12));
    } else if (symbol == "square.on.square") {
        // Copy: front rounded square + back "L" (design rect 8.5,8.5 10.5 r2.5 + M5.5 14.5 V6.5 a2 .. h8).
        p.drawRoundedRect(QRectF(PT(8.5, 8.5), PT(19, 19)), 2.5 * sc, 2.5 * sc);
        QPainterPath back;
        back.moveTo(PT(5.5, 14.5));
        back.lineTo(PT(5.5, 6.5));
        back.quadTo(PT(5.5, 4.5), PT(7.5, 4.5));
        back.lineTo(PT(15.5, 4.5));
        p.drawPath(back);
    } else if (symbol == "floppy") {
        // Save: floppy body (cut top-right) + label rect + top slot (design paths).
        QPainterPath body;
        body.moveTo(PT(19, 21));
        body.lineTo(PT(5, 21));
        body.quadTo(PT(3, 21), PT(3, 19));
        body.lineTo(PT(3, 5));
        body.quadTo(PT(3, 3), PT(5, 3));
        body.lineTo(PT(16, 3));
        body.lineTo(PT(21, 8));
        body.lineTo(PT(21, 19));
        body.quadTo(PT(21, 21), PT(19, 21));
        body.closeSubpath();
        p.drawPath(body);
        QPainterPath label;
        label.moveTo(PT(17, 21));
        label.lineTo(PT(17, 13));
        label.lineTo(PT(7, 13));
        label.lineTo(PT(7, 21));
        p.drawPath(label);
        QPainterPath slot;
        slot.moveTo(PT(7, 3));
        slot.lineTo(PT(7, 8));
        slot.lineTo(PT(15, 8));
        p.drawPath(slot);
    } else if (symbol == "xmark") {
        QPen xp(tint, 1.9 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(xp);
        p.drawLine(PT(6.5, 6.5), PT(17.5, 17.5));
        p.drawLine(PT(17.5, 6.5), PT(6.5, 17.5));
    } else if (symbol == "checkmark") {
        QPen cp(tint, 3.0 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(cp);
        QPainterPath ck;
        ck.moveTo(PT(5, 12.5));
        ck.lineTo(PT(10, 17));
        ck.lineTo(PT(19, 7));
        p.drawPath(ck);
    } else if (symbol == "nosign") {
        p.drawEllipse(PT(12, 12), 9 * sc, 9 * sc);
        const qreal d = 6.36;   // ~9/sqrt(2)
        p.drawLine(PT(12 - d, 12 - d), PT(12 + d, 12 + d));
    }

    p.end();
    return QIcon(pm);
}

// Antialiased color well for the palette, matching the design swatches
// (toolbar_canvas.html §1): a 26px filled circle. Unselected wells carry a thin
// inset contrast ring (white@18% on color, black@15% on white); the selected
// well gets the design's two-tone halo — a dark panel-colored gap ring followed
// by a bright white outer ring (box-shadow: 0 0 0 2px #242429, 0 0 0 4px white)
// — plus a centered white checkmark. The icon is drawn slightly smaller than the
// button so the halo and the 1.10x pop never reach the clip boundary.
QIcon makeSwatch(const QColor& color, bool selected, int d = 26) {
    const qreal dpr = 2.0;
    QPixmap pm(int(d * dpr), int(d * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The white outer ring lives OUTSIDE the fill in the design (0 0 0 4px), so
    // leave room for it: the colored disc is inset when selected.
    const qreal discInset = selected ? d * 0.135 : 0.5;   // ~3.5px halo room at 26
    const QRectF disc(discInset, discInset, d - 2 * discInset, d - 2 * discInset);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(disc);

    if (selected) {
        // Two-tone halo: panel-color gap, then bright white outer ring.
        const qreal whiteW = d * 0.075;                   // ~2px white ring
        const qreal gapW   = d * 0.075;                   // ~2px panel-color gap
        const qreal whiteR = (disc.width() + gapW * 2 + whiteW) / 2.0;
        const QPointF c = disc.center();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(kPanelSurface, gapW));              // gap ring (matches panel)
        p.drawEllipse(c, disc.width() / 2.0 + gapW / 2.0,
                         disc.height() / 2.0 + gapW / 2.0);
        p.setPen(QPen(QColor(255, 255, 255, 230), whiteW));
        p.drawEllipse(c, whiteR, whiteR);

        // Centered checkmark, contrast against the swatch color.
        const QColor tint = Palette::isLight(color) ? Qt::black : Qt::white;
        auto u = [&](qreal t) { return t * d; };
        QPen cm(tint, d * 0.115, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(cm);
        p.drawPolyline(QPolygonF({ QPointF(u(0.31), u(0.52)),
                                   QPointF(u(0.44), u(0.66)),
                                   QPointF(u(0.70), u(0.33)) }));
    } else {
        // Thin inset contrast ring on the fill edge (design: inset 0 0 0 1px).
        // Black (and very dark swatches) sit on the near-black panel, so they need a
        // stronger rim to read as a distinct well — matching the visible grey ring
        // around the black swatch in the design. White/light swatches use a soft
        // dark rim; mid colors a faint white rim.
        const bool light = Palette::isLight(color);
        const double lum = 0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue();
        QColor edge;
        if (light)            edge = QColor(0, 0, 0, 38);          // dark rim on white/yellow
        else if (lum < 40.0)  edge = QColor(255, 255, 255, 92);    // strong rim on black
        else                  edge = QColor(255, 255, 255, 46);    // faint rim on color
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(edge, 1.0));
        const QRectF ring = disc.adjusted(0.5, 0.5, -0.5, -0.5);
        p.drawEllipse(ring);
    }
    p.end();
    return QIcon(pm);
}

// Small stroked icons for the Text panel (design 24x24 viewBox), in `color`.
QIcon makeTextIcon(const QString& name, const QColor& color, int size) {
    const qreal dpr = 2.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal sc = size / 24.0;
    auto PT = [&](qreal x, qreal y) { return QPointF(x * sc, y * sc); };
    QPen pen(color, 1.9 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (name == "align.left") {
        p.drawLine(PT(4, 7), PT(20, 7)); p.drawLine(PT(4, 12), PT(14, 12)); p.drawLine(PT(4, 17), PT(17, 17));
    } else if (name == "align.center") {
        p.drawLine(PT(4, 7), PT(20, 7)); p.drawLine(PT(7, 12), PT(17, 12)); p.drawLine(PT(5, 17), PT(19, 17));
    } else if (name == "align.right") {
        p.drawLine(PT(4, 7), PT(20, 7)); p.drawLine(PT(10, 12), PT(20, 12)); p.drawLine(PT(6, 17), PT(20, 17));
    } else if (name == "minus") {
        QPen mp(color, 2.4 * sc, Qt::SolidLine, Qt::RoundCap); p.setPen(mp);
        p.drawLine(PT(6, 12), PT(18, 12));
    } else if (name == "plus") {
        QPen mp(color, 2.4 * sc, Qt::SolidLine, Qt::RoundCap); p.setPen(mp);
        p.drawLine(PT(12, 6), PT(12, 18)); p.drawLine(PT(6, 12), PT(18, 12));
    } else if (name == "check") {
        QPen cp(color, 2.2 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(cp);
        QPainterPath ck; ck.moveTo(PT(5, 12.5)); ck.lineTo(PT(10, 17.5)); ck.lineTo(PT(19, 6.5)); p.drawPath(ck);
    } else if (name == "chevron.down") {
        QPen cp(color, 2.0 * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(cp);
        QPainterPath ch; ch.moveTo(PT(7, 10)); ch.lineTo(PT(12, 15)); ch.lineTo(PT(17, 10)); p.drawPath(ch);
    } else if (name == "marker") {
        p.drawLine(PT(4, 20), PT(20, 20));
        QPainterPath h; h.moveTo(PT(7, 15)); h.lineTo(PT(15, 7)); h.lineTo(PT(18, 10)); h.lineTo(PT(10, 18)); h.closeSubpath();
        p.drawPath(h);
    }
    p.end();
    return QIcon(pm);
}

// "A" with a colored underline bar — the Text-panel text-color button (design).
QIcon makeColorAIcon(const QColor& underline, const QColor& glyph, int size) {
    const qreal dpr = 2.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(glyph);
    QFont f = p.font();
    f.setPixelSize(int(size * 0.62));
    f.setWeight(QFont::Bold);
    p.setFont(f);
    p.drawText(QRectF(0, -size * 0.10, size, size), Qt::AlignCenter, QStringLiteral("A"));
    p.setPen(Qt::NoPen);
    p.setBrush(underline);
    const qreal bw = size * 0.62, bh = std::max<qreal>(2.0, size * 0.13);
    p.drawRoundedRect(QRectF((size - bw) / 2.0, size * 0.80, bw, bh), bh / 2.0, bh / 2.0);
    p.end();
    return QIcon(pm);
}

// A QPushButton that pops (scales 1.0->peak->1.0 about its center) on demand and
// paints its own rounded background to match the LightGet design
// (toolbar_canvas.html §1): a resting transparent button, a hover fill
// (white@9%, or a custom tint for the close action), and a full selected fill
// (systemBlue). Replaces the Swift ActionButton + CALayer transform animation.
//
// CLIPPING NOTE: the pop scales the button content via a QTransform in
// paintEvent, which is clipped to the widget's own rect. Both the rounded
// background (drawn 0.5px inset) and the glyph icon (rendered at the button size
// but with the glyph's own transparent margin) stay clear of the clip boundary,
// so a 1.10x pop never clips. The fill is a single rounded rect at the design's
// 11px radius, scaling cleanly with the pop.
class PopButton : public QPushButton {
public:
    explicit PopButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
    }

    // Full selected fill (design: a solid systemBlue button at the 11px radius).
    // Pass a non-null color to enable; the glyph tint is the caller's concern.
    void setHighlight(const QColor& color, qreal radius = 11.0) {
        m_highlight = color;
        m_highlightRadius = radius;
        update();
    }
    void clearHighlight() {
        m_highlight = QColor();   // invalid -> no selected fill
        update();
    }

    // Per-button hover fill + the rounded-rect radius used for hover/selected.
    void setHoverFill(const QColor& color, qreal radius = 11.0) {
        m_hoverFill = color;
        m_highlightRadius = radius;
    }

    // Trigger the center-anchored pop. peak 1.10 (toolbar) / 1.18 (inspector).
    void pop(qreal peak) {
        if (m_anim) { m_anim->stop(); m_anim->deleteLater(); }
        m_anim = new QVariantAnimation(this);
        m_anim->setStartValue(0.0);
        m_anim->setEndValue(1.0);
        m_anim->setDuration(180);                       // 0.18s
        m_anim->setEasingCurve(QEasingCurve::OutQuad);  // easeOut
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this, peak](const QVariant& v) {
            const qreal phase = v.toReal();             // keyTimes 0, 0.5, 1
            qreal s;
            if (phase <= 0.5) s = 1.0 + (peak - 1.0) * (phase / 0.5);
            else              s = peak - (peak - 1.0) * ((phase - 0.5) / 0.5);
            m_scale = s;
            update();
        });
        connect(m_anim, &QVariantAnimation::finished, this, [this] {
            m_scale = 1.0;
            update();
        });
        m_anim->start();
    }

protected:
    void enterEvent(QEnterEvent* e) override {
        m_hovered = true;
        update();
        QPushButton::enterEvent(e);
    }
    void leaveEvent(QEvent* e) override {
        m_hovered = false;
        update();
        QPushButton::leaveEvent(e);
    }

    void paintEvent(QPaintEvent* e) override {
        const bool popping = !qFuzzyCompare(m_scale, qreal(1.0));
        // The background to paint: selected fill > hover fill > nothing.
        QColor bg;
        if (m_highlight.isValid())                 bg = m_highlight;
        else if (m_hovered && m_hoverFill.isValid()) bg = m_hoverFill;

        if (!popping && !bg.isValid()) {
            // Plain, unscaled, no self-drawn background -> let the style paint it.
            QPushButton::paintEvent(e);
            return;
        }

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Scale about center without shifting (matches the Swift centered() matrix).
        // The same transform covers the rounded background AND the icon, so both
        // pop together and both stay inside the widget rect (background is 0.5px
        // inset, the glyph has its own transparent margin), so neither clips.
        if (popping) {
            const QPointF c(width() / 2.0, height() / 2.0);
            QTransform t;
            t.translate(c.x(), c.y());
            t.scale(m_scale, m_scale);
            t.translate(-c.x(), -c.y());
            p.setTransform(t);
        }

        // Rounded background (selected/hover) painted by us, not via QSS.
        if (bg.isValid()) {
            const QRectF hr = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(hr, m_highlightRadius, m_highlightRadius);
        }

        // Re-render the base button content (the icon) under the transform. The
        // glyph's transparent inset keeps it clear of the clip boundary at the pop.
        QStyleOptionButton opt;
        initStyleOption(&opt);
        style()->drawControl(QStyle::CE_PushButton, &opt, &p, this);
    }

private:
    QVariantAnimation* m_anim = nullptr;
    qreal m_scale = 1.0;
    bool   m_hovered = false;
    QColor m_highlight;            // invalid = not selected
    QColor m_hoverFill;            // invalid = no hover fill
    qreal  m_highlightRadius = 11.0;
};

} // namespace

// ===========================================================================
// ToolbarView
// ===========================================================================
ToolbarView::ToolbarView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_TranslucentBackground, true);
    buildButtons();
}

void ToolbarView::rebuild() {
    buildButtons();
}

void ToolbarView::buildButtons() {
    // Clear any previous build.
    for (auto* b : m_toolButtons) b->deleteLater();
    for (const auto& pr : m_colorButtons) pr.second->deleteLater();
    m_toolButtons.clear();
    m_colorButtons.clear();
    // Action buttons aren't tracked; remove every remaining child PopButton.
    // PopButton is a .cpp-local class without Q_OBJECT, so it can't be the target
    // of qobject_cast. Every push button this widget creates is a PopButton, so
    // matching its Q_OBJECT base QPushButton selects exactly the same set.
    for (QObject* child : children()) {
        if (auto* pb = qobject_cast<QPushButton*>(child)) pb->deleteLater();
    }

    m_separatorX.clear();
    qreal x = kPadH;

    // Lay a group divider at the current cursor: design §1 is a 1px line,
    // height 26, with margin 7 on each side. Advances x past the line+margins.
    auto addSeparator = [&]() {
        x += 7;                       // left margin
        m_separatorX.append(int(x));  // centre of the 1px line (painted in paintEvent)
        x += 1 + 7;                   // line + right margin
    };

    // --- Tools (only the ones enabled in Settings; Select is always enabled) ---
    struct ToolSym { Tool tool; const char* symbol; };
    const ToolSym tools[] = {
        { Tool::Select,     "cursorarrow" },
        { Tool::Arrow,      "arrow.up.right" },
        { Tool::Line,       "line.diagonal" },
        { Tool::Rectangle,  "rectangle" },
        { Tool::FilledRect, "rectangle.fill" },
        { Tool::Pen,        "pencil" },
        { Tool::Text,       "textformat" },
    };
    for (const auto& ts : tools) {
        if (!Settings::instance().isToolEnabled(ts.tool)) continue;
        const Tool tool = ts.tool;
        auto* b = new PopButton(this);
        // Idle glyph in the design's resting color (#D4D5DA); selected/hover state
        // swaps the glyph to white (see setSelectedTool). Glyph drawn at the button
        // size with its own transparent margin so the 1.10x pop never clips.
        b->setIcon(makeGlyph(QString::fromLatin1(ts.symbol), kGlyphIdle, kIconSize));
        b->setIconSize(QSize(kIconSize, kIconSize));
        b->setHoverFill(kHoverFill, kBtnRadius);
        b->setGeometry(int(x), kPadV, kButtonSize, kButtonSize);
        b->setProperty("symbol", QString::fromLatin1(ts.symbol));
        connect(b, &QPushButton::clicked, this, [this, tool] {
            setSelectedTool(tool);
            emit selectToolRequested(tool);
            popTool(tool);
        });
        b->show();
        m_toolButtons.insert(tool, b);
        x += kButtonSize + kGap;
    }

    // --- Color palette (only if enabled in Settings) ---
    if (Settings::instance().showColorPalette()) {
        addSeparator();   // divider between tools and the swatch group
        const auto& cols = Palette::colors();
        for (int i = 0; i < cols.size(); ++i) {
            const QColor color = cols[i];
            auto* well = new PopButton(this);
            well->setProperty("fill", color);
            // 26px circle (design §1), vertically centered within the button row.
            const int wy = kPadV + (kButtonSize - kSwatch) / 2;
            well->setGeometry(int(x), wy, kSwatch, kSwatch);
            // The well is painted as an antialiased icon (makeSwatch); the button
            // itself stays borderless/transparent (no QSS ring, no hover fill —
            // the design swatches don't get a hover background).
            well->setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
            well->setIconSize(QSize(kSwatch, kSwatch));
            connect(well, &QPushButton::clicked, this, [this, color, i] {
                emit selectColorRequested(color);
                setSelectedColor(i);
                popColor(i);
            });
            well->show();
            m_colorButtons.append(qMakePair(color, static_cast<QPushButton*>(well)));
            x += kSwatch + kSwatchGap;
        }
        x -= kSwatchGap;          // trailing swatch needs no gap before the divider
        addSeparator();           // divider between the swatch group and actions
    } else {
        addSeparator();           // divider between tools and actions
    }

    // --- Actions ---
    struct ActionSym { const char* symbol; void (ToolbarView::*sig)(); };
    const ActionSym actions[] = {
        { "arrow.uturn.backward",  &ToolbarView::undoRequested },
        { "arrow.uturn.forward",   &ToolbarView::redoRequested },
        { "square.on.square",      &ToolbarView::copyRequested },
        { "floppy",                &ToolbarView::saveRequested },
        { "xmark",                 &ToolbarView::closeRequested },
    };
    for (const auto& a : actions) {
        const bool isClose = (std::strcmp(a.symbol, "xmark") == 0);
        auto* b = new PopButton(this);
        // The close action carries the design's red glyph + red hover; the rest
        // use the resting glyph color + white@9% hover.
        b->setIcon(makeGlyph(QString::fromLatin1(a.symbol),
                             isClose ? kCloseGlyph : kGlyphIdle, kIconSize));
        b->setIconSize(QSize(kIconSize, kIconSize));
        b->setHoverFill(isClose ? kCloseHoverFill : kHoverFill, kBtnRadius);
        b->setGeometry(int(x), kPadV, kButtonSize, kButtonSize);
        auto sig = a.sig;
        connect(b, &QPushButton::clicked, this, [this, sig] { (this->*sig)(); });
        b->show();
        x += kButtonSize + kGap;
    }
    x -= kGap;   // trailing button needs no gap before the panel padding

    // Re-apply the highlight (default Select) so the active tool stays lit, and
    // sync the selected swatch. Done after every button exists.
    setSelectedTool(m_selectedTool);
    if (Settings::instance().showColorPalette()) setSelectedColor(m_selectedColor);

    // Height = buttonSize(40) + vertical padding(8) * 2 = 56.
    setFixedSize(int(x + kPadH), kButtonSize + kPadV * 2);
    update();
}

void ToolbarView::setSelectedTool(Tool t) {
    m_selectedTool = t;
    for (auto it = m_toolButtons.begin(); it != m_toolButtons.end(); ++it) {
        const bool sel = (it.key() == t);
        auto* b = static_cast<PopButton*>(it.value());
        // Design §1: the selected tool is a SOLID systemBlue button (full fill at
        // radius 11) with a white glyph; idle tools have a transparent button and
        // the resting #D4D5DA glyph. Swap both the fill and the glyph tint.
        const QString symbol = b->property("symbol").toString();
        if (sel) {
            b->setHighlight(kSelectedBlue, kBtnRadius);
            b->setIcon(makeGlyph(symbol, kGlyphSelected, kIconSize));
        } else {
            b->clearHighlight();
            b->setIcon(makeGlyph(symbol, kGlyphIdle, kIconSize));
        }
        b->setIconSize(QSize(kIconSize, kIconSize));
    }
}

void ToolbarView::setSelectedColor(int paletteIndex) {
    m_selectedColor = paletteIndex;
    for (int i = 0; i < m_colorButtons.size(); ++i) {
        const QColor color = m_colorButtons[i].first;
        QPushButton* well = m_colorButtons[i].second;
        // Antialiased 26px swatch icon (filled circle + inset ring; selected adds
        // the two-tone halo + checkmark). No QSS border -> no aliased outline.
        well->setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
        well->setIcon(makeSwatch(color, i == paletteIndex, kSwatch));
        well->setIconSize(QSize(kSwatch, kSwatch));
    }
}

void ToolbarView::popTool(Tool t) {
    auto it = m_toolButtons.find(t);
    if (it == m_toolButtons.end()) return;
    if (auto* pb = static_cast<PopButton*>(it.value())) pb->pop(1.10);
}

void ToolbarView::popColor(int index) {
    if (index < 0 || index >= m_colorButtons.size()) return;  // guard, matches Swift
    if (auto* pb = static_cast<PopButton*>(m_colorButtons[index].second)) pb->pop(1.10);
}

void ToolbarView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Design §1: surface #242429, border white@9% (1px), radius 16.
    p.setPen(QPen(kPanelBorder, 1.0));
    p.setBrush(kPanelSurface);
    p.drawRoundedRect(r, kPanelRadiusToolbar, kPanelRadiusToolbar);

    // 1px group dividers (white@10%) centred on the recorded x's, scaled to the
    // compact native panel.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(kSeparator, 1.0));
    const qreal sepH = 20.0;
    const qreal y0 = (height() - sepH) / 2.0;
    for (int sx : m_separatorX) {
        const qreal xc = sx + 0.5;   // pixel centre of the 1px line
        p.drawLine(QPointF(xc, y0), QPointF(xc, y0 + sepH));
    }
}

// ===========================================================================
// TextInspectorView
// ===========================================================================
TextInspectorView::TextInspectorView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_TranslucentBackground, true);
    build();
}

void TextInspectorView::rebuild() {
    build();
}

void TextInspectorView::build() {
    // Clear previous build.
    for (const auto& pr : m_textSwatches) pr.second->deleteLater();
    for (const auto& pr : m_bgSwatches) pr.second->deleteLater();
    m_textSwatches.clear();
    m_bgSwatches.clear();
    for (QObject* child : children()) {
        if (auto* l = qobject_cast<QLabel*>(child)) l->deleteLater();
    }

    const int pad = 6, sw = 16, gap = 4, labelW = 16;
    const auto& cols = Palette::colors();

    auto makeLabel = [this](const QString& s, int x, int y) {
        auto* l = new QLabel(s, this);
        l->setStyleSheet(QStringLiteral("color:white;background:transparent;"));
        QFont f = l->font();
        f.setBold(true);
        f.setPixelSize(12);
        l->setFont(f);
        l->setGeometry(x, y, 16, 16);
        l->setAlignment(Qt::AlignCenter);
        l->show();
        return l;
    };

    auto makeSwatch = [this](const QColor& fill, int x, int y) {
        auto* b = new PopButton(this);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{border:1px solid rgba(255,255,255,128);"
            "border-radius:4px;background:%1;}").arg(fill.name()));
        b->setGeometry(x, y, 16, 16);
        b->show();
        return b;
    };

    // --- Row 1: text color ("A" label) ---
    int x1 = pad + labelW + 4;
    makeLabel(QStringLiteral("A"), pad, pad);
    for (int i = 0; i < cols.size(); ++i) {
        const QColor c = cols[i];
        auto* b = makeSwatch(c, x1, pad);
        connect(b, &QPushButton::clicked, this, [this, c, i] {
            emit textColorRequested(c);
            if (auto* pb = static_cast<PopButton*>(m_textSwatches[i].second)) pb->pop(1.18);
        });
        m_textSwatches.append(qMakePair(c, static_cast<QPushButton*>(b)));
        x1 += sw + gap;
    }

    // --- Row 2: background color (only if enabled in Settings) ---
    int x2 = pad + labelW + 4;
    int rows = 1;
    if (Settings::instance().textBackgroundEnabled()) {
        rows = 2;
        const int y2 = pad + sw + gap;
        makeLabel(QString::fromUtf8("\xE2\x96\xA2"), pad, y2);  // "▢"

        // "no background" swatch (index 0): transparent fill + nosign glyph.
        auto* none = new PopButton(this);
        none->setProperty("none", true);
        none->setStyleSheet(QStringLiteral(
            "QPushButton{border:1px solid rgba(255,255,255,128);"
            "border-radius:4px;background:transparent;}"));
        none->setIcon(makeGlyph(QStringLiteral("nosign"), Qt::white, 16));
        none->setIconSize(QSize(12, 12));
        none->setGeometry(x2, y2, sw, sw);
        none->show();
        connect(none, &QPushButton::clicked, this, [this] {
            emit bgColorRequested(std::nullopt);
            if (auto* pb = static_cast<PopButton*>(m_bgSwatches[0].second)) pb->pop(1.18);
        });
        m_bgSwatches.append(qMakePair(std::optional<QColor>(std::nullopt),
                                      static_cast<QPushButton*>(none)));
        x2 += sw + gap;

        for (int j = 0; j < cols.size(); ++j) {
            const QColor c = cols[j];
            const int idx = j + 1;   // +1: index 0 is the "no background" swatch
            auto* b = makeSwatch(c, x2, y2);
            connect(b, &QPushButton::clicked, this, [this, c, idx] {
                emit bgColorRequested(c);
                if (auto* pb = static_cast<PopButton*>(m_bgSwatches[idx].second)) pb->pop(1.18);
            });
            m_bgSwatches.append(qMakePair(std::optional<QColor>(c),
                                          static_cast<QPushButton*>(b)));
            x2 += sw + gap;
        }
    }

    // width = max(x1, x2) + pad - gap; height = pad*2 + sw*rows + gap*(rows-1).
    const int w = qMax(x1, x2) + pad - gap;
    const int h = pad * 2 + sw * rows + gap * (rows - 1);
    setFixedSize(w, h);
    update();
}

void TextInspectorView::setSelected(const QColor& textColor,
                                    const std::optional<QColor>& bgColor) {
    // Text row: mark the matching color.
    for (const auto& pr : m_textSwatches) {
        const QColor c = pr.first;
        const bool on = Palette::sameColor(c, textColor);
        QPushButton* b = pr.second;
        if (on) {
            const QColor tint = Palette::isLight(c) ? Qt::black : Qt::white;
            b->setIcon(makeGlyph(QStringLiteral("checkmark"), tint, 16));
            b->setIconSize(QSize(9, 9));   // pointSize 9, bold
        } else {
            b->setIcon(QIcon());
        }
    }
    // Background row.
    for (const auto& pr : m_bgSwatches) {
        const std::optional<QColor>& c = pr.first;
        QPushButton* b = pr.second;
        const bool on = (!c.has_value() && !bgColor.has_value())
                     || ( c.has_value() &&  bgColor.has_value()
                          && Palette::sameColor(*c, *bgColor));
        if (on) {
            // "no background" (c == nullopt) -> white checkmark (transparent swatch
            // over the dark panel). Otherwise contrast against the swatch color.
            const bool light = c.has_value() ? Palette::isLight(*c) : false;
            const QColor tint = light ? Qt::black : Qt::white;
            b->setIcon(makeGlyph(QStringLiteral("checkmark"), tint, 16));
            b->setIconSize(QSize(9, 9));
        } else if (!c.has_value()) {
            // restore the "no background" nosign glyph
            b->setIcon(makeGlyph(QStringLiteral("nosign"), Qt::white, 16));
            b->setIconSize(QSize(12, 12));
        } else {
            b->setIcon(QIcon());
        }
    }
}

void TextInspectorView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Same surface/border tokens as the toolbar (design: one dark panel for all
    // floating chrome), kept at the tighter inspector radius.
    p.setPen(QPen(kPanelBorder, 1.0));
    p.setBrush(kPanelSurface);
    p.drawRoundedRect(r, kPanelRadiusInspector, kPanelRadiusInspector);
}

// ===========================================================================
// TextPanel — unified contextual "Text" panel (design §2/§3)
// ===========================================================================
static QString cssCol(const QColor& c) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

TextPanel::TextPanel(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_TranslucentBackground, true);
    m_row = new QHBoxLayout(this);
    m_row->setContentsMargins(8, 6, 8, 6);
    m_row->setSpacing(2);
    rebuild();
}

TextPanel::~TextPanel() { closeColorPopup(); }

void TextPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(kPanelBorder, 1.0));
    p.setBrush(kPanelSurface);
    p.drawRoundedRect(r, 13, 13);   // design panel radius
}

void TextPanel::rebuild() {
    closeColorPopup();
    QLayoutItem* item;
    while ((item = m_row->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    m_fontBtn = nullptr; m_sizeLabel = nullptr;
    m_boldBtn = m_italicBtn = m_underlineBtn = nullptr;
    m_alignBtns.clear(); m_colorBtn = m_markerBtn = nullptr;

    Settings& s = Settings::instance();
    const QColor glyph(0xd4, 0xd5, 0xda);

    auto addDivider = [&]() {
        auto* d = new QFrame(this);
        d->setFixedSize(1, 22);
        d->setStyleSheet(QStringLiteral("background:%1;border:none;").arg(cssCol(kSeparator)));
        m_row->addSpacing(2);
        m_row->addWidget(d, 0, Qt::AlignVCenter);
        m_row->addSpacing(2);
    };
    auto flatBtn = [&](int w, int h) {
        auto* b = new QPushButton(this);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(w, h);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{border:none;border-radius:8px;background:transparent;}"
            "QPushButton:hover{background:%1;}").arg(cssCol(kHoverFill)));
        return b;
    };

    // --- Font dropdown ---
    if (s.textFontEnabled()) {
        m_fontBtn = new QPushButton(this);
        m_fontBtn->setFocusPolicy(Qt::NoFocus);
        m_fontBtn->setCursor(Qt::PointingHandCursor);
        m_fontBtn->setFixedHeight(30);
        m_fontBtn->setLayoutDirection(Qt::RightToLeft);   // chevron right of the label
        m_fontBtn->setIcon(makeTextIcon("chevron.down", glyph, 11));
        m_fontBtn->setIconSize(QSize(11, 11));
        m_fontBtn->setStyleSheet(QStringLiteral(
            "QPushButton{border:none;border-radius:8px;background:transparent;color:%1;"
            "padding:0 8px;font-size:13px;}"
            "QPushButton:hover{background:%2;}").arg(cssCol(glyph), cssCol(kHoverFill)));
        connect(m_fontBtn, &QPushButton::clicked, this, [this]() { openFontPopup(m_fontBtn); });
        m_row->addWidget(m_fontBtn, 0, Qt::AlignVCenter);
        addDivider();
    }

    // --- Size stepper (− 17 +) ---
    if (s.textFontSizeEnabled()) {
        auto* minus = flatBtn(26, 28);
        minus->setIcon(makeTextIcon("minus", glyph, 15));
        minus->setIconSize(QSize(15, 15));
        connect(minus, &QPushButton::clicked, this, [this]() {
            m_size = qMax(8, m_size - 1); refreshVisualState(); emit fontSizeChanged(m_size);
        });
        m_sizeLabel = new QLabel(this);
        m_sizeLabel->setAlignment(Qt::AlignCenter);
        m_sizeLabel->setMinimumWidth(20);
        m_sizeLabel->setStyleSheet(
            "color:white;background:transparent;font-family:'SF Mono',Menlo,monospace;"
            "font-weight:600;font-size:13px;");
        auto* plus = flatBtn(26, 28);
        plus->setIcon(makeTextIcon("plus", glyph, 15));
        plus->setIconSize(QSize(15, 15));
        connect(plus, &QPushButton::clicked, this, [this]() {
            m_size = qMin(400, m_size + 1); refreshVisualState(); emit fontSizeChanged(m_size);
        });
        m_row->addWidget(minus, 0, Qt::AlignVCenter);
        m_row->addWidget(m_sizeLabel, 0, Qt::AlignVCenter);
        m_row->addWidget(plus, 0, Qt::AlignVCenter);
        addDivider();
    }

    // --- B / I / U ---
    bool anyStyle = false;
    auto makeStyleBtn = [&](const QString& text, bool ital, bool under) {
        auto* b = new QPushButton(text, this);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(30, 30);
        QFont f("Georgia");
        f.setPixelSize(15);
        f.setBold(true);
        f.setItalic(ital);
        f.setUnderline(under);
        b->setFont(f);
        return b;
    };
    if (s.textBoldEnabled()) {
        m_boldBtn = makeStyleBtn("B", false, false);
        connect(m_boldBtn, &QPushButton::clicked, this, [this]() {
            m_bold = !m_bold; refreshVisualState(); emit boldChanged(m_bold);
        });
        m_row->addWidget(m_boldBtn, 0, Qt::AlignVCenter); anyStyle = true;
    }
    if (s.textItalicEnabled()) {
        m_italicBtn = makeStyleBtn("I", true, false);
        connect(m_italicBtn, &QPushButton::clicked, this, [this]() {
            m_italic = !m_italic; refreshVisualState(); emit italicChanged(m_italic);
        });
        m_row->addWidget(m_italicBtn, 0, Qt::AlignVCenter); anyStyle = true;
    }
    if (s.textUnderlineEnabled()) {
        m_underlineBtn = makeStyleBtn("U", false, true);
        connect(m_underlineBtn, &QPushButton::clicked, this, [this]() {
            m_underline = !m_underline; refreshVisualState(); emit underlineChanged(m_underline);
        });
        m_row->addWidget(m_underlineBtn, 0, Qt::AlignVCenter); anyStyle = true;
    }
    if (anyStyle) addDivider();

    // --- Alignment segment (L / C / R) ---
    if (s.textAlignmentEnabled()) {
        auto* seg = new QWidget(this);
        seg->setStyleSheet("background:rgba(255,255,255,15);border-radius:9px;");
        auto* sh = new QHBoxLayout(seg);
        sh->setContentsMargins(2, 2, 2, 2);
        sh->setSpacing(0);
        const QPair<TextAlign, const char*> aligns[] = {
            {TextAlign::Left, "align.left"}, {TextAlign::Center, "align.center"},
            {TextAlign::Right, "align.right"},
        };
        for (const auto& al : aligns) {
            auto* b = new QPushButton(seg);
            b->setFocusPolicy(Qt::NoFocus);
            b->setCursor(Qt::PointingHandCursor);
            b->setFixedSize(28, 26);
            b->setIconSize(QSize(15, 15));
            const TextAlign a = al.first;
            connect(b, &QPushButton::clicked, this, [this, a]() {
                m_align = a; refreshVisualState(); emit alignChanged(a);
            });
            sh->addWidget(b);
            m_alignBtns.append({a, b});
        }
        m_row->addWidget(seg, 0, Qt::AlignVCenter);
        addDivider();
    }

    // --- A-color (text color) ---
    m_colorBtn = flatBtn(30, 30);
    m_colorBtn->setIconSize(QSize(22, 22));
    connect(m_colorBtn, &QPushButton::clicked, this, [this]() { openColorPopup(false, m_colorBtn); });
    m_row->addWidget(m_colorBtn, 0, Qt::AlignVCenter);

    // --- Marker (background) ---
    if (s.textBackgroundEnabled()) {
        m_markerBtn = flatBtn(30, 30);
        m_markerBtn->setIcon(makeTextIcon("marker", glyph, 17));
        m_markerBtn->setIconSize(QSize(17, 17));
        connect(m_markerBtn, &QPushButton::clicked, this, [this]() { openColorPopup(true, m_markerBtn); });
        m_row->addWidget(m_markerBtn, 0, Qt::AlignVCenter);
    }
    addDivider();

    // --- Done (✓) ---
    auto* done = new QPushButton(this);
    done->setFocusPolicy(Qt::NoFocus);
    done->setCursor(Qt::PointingHandCursor);
    done->setFixedSize(34, 30);
    done->setIcon(makeTextIcon("check", Qt::white, 17));
    done->setIconSize(QSize(17, 17));
    done->setStyleSheet(QStringLiteral(
        "QPushButton{border:none;border-radius:8px;background:%1;}"
        "QPushButton:hover{background:#0b76e0;}").arg(cssCol(kSelectedBlue)));
    connect(done, &QPushButton::clicked, this, [this]() { emit doneClicked(); });
    m_row->addWidget(done, 0, Qt::AlignVCenter);

    refreshVisualState();
    adjustSize();
}

void TextPanel::refreshVisualState() {
    const QColor glyph(0xd4, 0xd5, 0xda);
    if (m_fontBtn)
        m_fontBtn->setText(m_family.isEmpty() ? QStringLiteral("System") : m_family);
    if (m_sizeLabel) m_sizeLabel->setText(QString::number(m_size));

    auto styleToggle = [&](QPushButton* b, bool on) {
        if (!b) return;
        b->setStyleSheet(QStringLiteral(
            "QPushButton{border:none;border-radius:8px;color:%1;background:%2;}"
            "QPushButton:hover{background:%3;}")
            .arg(on ? QStringLiteral("#ffffff") : cssCol(glyph),
                 on ? cssCol(QColor(255, 255, 255, 30)) : QStringLiteral("transparent"),
                 cssCol(kHoverFill)));
    };
    styleToggle(m_boldBtn, m_bold);
    styleToggle(m_italicBtn, m_italic);
    styleToggle(m_underlineBtn, m_underline);

    for (const auto& pr : m_alignBtns) {
        const bool on = (pr.first == m_align);
        const char* nm = pr.first == TextAlign::Left ? "align.left"
                       : pr.first == TextAlign::Center ? "align.center" : "align.right";
        pr.second->setIcon(makeTextIcon(nm, on ? QColor(Qt::white) : glyph, 15));
        pr.second->setStyleSheet(QStringLiteral(
            "QPushButton{border:none;border-radius:7px;background:%1;}")
            .arg(on ? cssCol(kSelectedBlue) : QStringLiteral("transparent")));
    }
    if (m_colorBtn) m_colorBtn->setIcon(makeColorAIcon(m_color, Qt::white, 22));
    update();
}

void TextPanel::setState(const QString& family, int size, bool bold, bool italic,
                         bool underline, TextAlign align, const QColor& color,
                         const std::optional<QColor>& bg) {
    m_family = family; m_size = size; m_bold = bold; m_italic = italic;
    m_underline = underline; m_align = align; m_color = color; m_bg = bg;
    refreshVisualState();
}

void TextPanel::closeColorPopup() {
    if (m_colorPopup) { m_colorPopup->deleteLater(); m_colorPopup = nullptr; }
}

bool TextPanel::closePopup() {
    const bool wasOpen = (m_colorPopup != nullptr);
    closeColorPopup();
    return wasOpen;
}

void TextPanel::openFontPopup(QWidget* anchor) {
    closeColorPopup();
    QWidget* host = parentWidget();
    if (!host || !anchor) return;
    auto* pop = new QWidget(host);
    pop->setObjectName("cpop");
    pop->setStyleSheet(QStringLiteral(
        "#cpop{background:%1;border:1px solid %2;border-radius:10px;}")
        .arg(cssCol(kPanelSurface), cssCol(kPanelBorder)));
    auto* v = new QVBoxLayout(pop);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(2);
    struct F { const char* label; const char* fam; };
    static const F fonts[] = {
        {"System", ""}, {"Helvetica Neue", "Helvetica Neue"}, {"Arial", "Arial"},
        {"Georgia", "Georgia"}, {"Times New Roman", "Times New Roman"},
        {"Courier New", "Courier New"}, {"Menlo", "Menlo"},
    };
    for (const F& f : fonts) {
        const QString fam = QString::fromLatin1(f.fam);
        auto* b = new QPushButton(QString::fromLatin1(f.label), pop);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{border:none;border-radius:6px;color:#e8e8ea;background:transparent;"
            "text-align:left;padding:5px 12px;font-size:13px;}"
            "QPushButton:hover{background:%1;}").arg(cssCol(kSelectedBlue)));
        if (!fam.isEmpty()) { QFont ff(fam); ff.setPixelSize(13); b->setFont(ff); }
        connect(b, &QPushButton::clicked, this, [this, fam]() {
            m_family = fam; refreshVisualState();
            emit fontFamilyChanged(fam); closeColorPopup();
        });
        v->addWidget(b);
    }
    pop->adjustSize();
    const QPoint a = anchor->mapTo(host, QPoint(0, anchor->height()));
    int x = qBound(0, a.x(), qMax(0, host->width() - pop->width()));
    int y = qBound(0, a.y() + 6, qMax(0, host->height() - pop->height()));
    pop->move(x, y);
    pop->show();
    pop->raise();
    m_colorPopup = pop;
}

void TextPanel::openColorPopup(bool background, QWidget* anchor) {
    closeColorPopup();
    QWidget* host = parentWidget();
    if (!host || !anchor) return;
    auto* pop = new QWidget(host);
    pop->setObjectName("cpop");
    pop->setStyleSheet(QStringLiteral(
        "#cpop{background:%1;border:1px solid %2;border-radius:14px;}")
        .arg(cssCol(kPanelSurface), cssCol(kPanelBorder)));
    auto* h = new QHBoxLayout(pop);
    h->setContentsMargins(14, 12, 14, 12);   // design palette padding 14
    h->setSpacing(9);                        // design swatch gap 9

    // Row label (design §4: "ТЕКСТ" / "ФОН"), localized to the app language.
    auto* lab = new QLabel(background ? Loc::t(QStringLiteral("palette.bg"))
                                      : Loc::t(QStringLiteral("palette.text")), pop);
    lab->setStyleSheet("color:#9a9aa0;background:transparent;font-weight:600;font-size:11px;");
    h->addWidget(lab, 0, Qt::AlignVCenter);
    h->addSpacing(3);

    auto addSwatch = [&](std::optional<QColor> c) {
        auto* b = new QPushButton(pop);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(26, 26);             // design swatch 26px
        b->setIconSize(QSize(26, 26));
        b->setStyleSheet("QPushButton{border:none;background:transparent;}");
        if (!c.has_value()) {
            b->setIcon(makeGlyph(QStringLiteral("nosign"), QColor(0xff, 0x69, 0x61), 26));
        } else {
            const bool sel = background ? (m_bg && Palette::sameColor(*m_bg, *c))
                                        : Palette::sameColor(m_color, *c);
            b->setIcon(makeSwatch(*c, sel, 26));
        }
        connect(b, &QPushButton::clicked, this, [this, background, c]() {
            if (background) { m_bg = c; emit bgColorChanged(c); }
            else if (c.has_value()) { m_color = *c; emit textColorChanged(*c); }
            refreshVisualState();
            closeColorPopup();
        });
        h->addWidget(b);
    };

    if (background) addSwatch(std::nullopt);
    for (const QColor& c : Palette::colors()) addSwatch(c);   // none + all 6 (incl. yellow)

    pop->adjustSize();
    const QPoint a = anchor->mapTo(host, QPoint(anchor->width() / 2, anchor->height()));
    int x = a.x() - pop->width() / 2;
    int y = a.y() + 6;
    x = qBound(0, x, qMax(0, host->width() - pop->width()));
    y = qBound(0, y, qMax(0, host->height() - pop->height()));
    pop->move(x, y);
    pop->show();
    pop->raise();
    m_colorPopup = pop;
}
