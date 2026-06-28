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

#include <QPushButton>
#include <QLabel>
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
    static const QVector<QColor> c = {
        QColor(255,  59,  48),   // systemRed
        QColor( 52, 199,  89),   // systemGreen
        QColor(  0, 122, 255),   // systemBlue
        QColor(255, 204,   0),   // systemYellow
        QColor(  0,   0,   0),   // black
        QColor(255, 255, 255),   // white
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
constexpr double kPanelRadiusToolbar   = 16.0;
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

    // 1px group dividers (white@10%, height 26) centred on the recorded x's.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(kSeparator, 1.0));
    const qreal sepH = 26.0;
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
