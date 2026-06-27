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

// Render a vector glyph centered into a transparent pixmap of `size`, stroked /
// filled in `tint`. The path is drawn in a 0..1 unit box then scaled to fit.
QIcon makeGlyph(const QString& symbol, const QColor& tint, int size = 30) {
    const int side = size;
    const qreal dpr = 2.0;
    QPixmap pm(int(side * dpr), int(side * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Glyph drawing area: a centered square inset from the icon canvas. The design
    // (toolbar_canvas.html §1) draws ~21px SVG glyphs in a 40px button — a ~0.52
    // fill ratio — with a 1.8/24 ≈ 0.075 relative stroke. We keep the procedural
    // glyph SHAPES (tuned to the native SF symbols) and only match the design's
    // footprint/weight: inset 0.24 → glyph ≈ side*0.52, stroke ≈ side*0.045 of the
    // button (≈0.075 of the glyph box), reading like the design's line icons.
    const qreal inset = side * 0.24;
    const QRectF box(inset, inset, side - 2 * inset, side - 2 * inset);
    const qreal w = box.width(), h = box.height();
    const qreal x0 = box.left(), y0 = box.top();
    auto px = [&](qreal u) { return x0 + u * w; };  // u in 0..1
    auto py = [&](qreal v) { return y0 + v * h; };  // v in 0..1 (top-down)

    QPen stroke(tint);
    stroke.setWidthF(side * 0.045);   // matches the design's ~1.8/24 line weight
    stroke.setCapStyle(Qt::RoundCap);
    stroke.setJoinStyle(Qt::RoundJoin);
    p.setPen(stroke);
    p.setBrush(Qt::NoBrush);

    // Button-centered coordinate helpers: bx/by map a fraction (0..1) of the WHOLE
    // button (side), centered. The tool/action glyphs below are redrawn to match
    // the native macOS SF-symbol toolbar 1:1; several native glyphs (rectangle,
    // doc.on.doc, uturn, save) have footprints larger than the 0.48-wide default
    // `box`, so they are laid out in these button-fraction coords instead. Each
    // glyph's footprint was measured against a render of the real SF symbols at
    // the same scale and tuned to the same width/height/center.
    const qreal cxB = side * 0.5, cyB = side * 0.5;
    auto bx = [&](qreal f) { return cxB + (f - 0.5) * side; };
    auto by = [&](qreal f) { return cyB + (f - 0.5) * side; };

    if (symbol == "cursorarrow") {
        // Native cursorarrow (design §1): a FILLED classic pointer, tilted slightly,
        // hotspot tip at the upper-left. A clean arrowhead with a notch and a short
        // tail prong angling down-right. Rounded vertices via a same-color
        // round-join stroke so the filled shape reads soft like the design.
        QPainterPath path;
        path.moveTo(bx(0.345), by(0.235));  // hotspot tip (upper-left)
        path.lineTo(bx(0.345), by(0.700));  // straight leading edge down
        path.lineTo(bx(0.455), by(0.595));  // in to the deep notch base
        path.lineTo(bx(0.560), by(0.785));  // down-right to the tail foot (outer)
        path.lineTo(bx(0.645), by(0.745));  // tail foot (outer corner)
        path.lineTo(bx(0.540), by(0.560));  // back up the inner tail edge
        path.lineTo(bx(0.700), by(0.560));  // out to the trailing shoulder
        path.closeSubpath();
        p.setPen(QPen(tint, side * 0.05, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tint);
        p.drawPath(path);
    } else if (symbol == "arrow.up.right") {
        // Native: a thin shaft from bottom-left to top-right capped with a small
        // OPEN chevron head (two strokes), NOT a filled triangle — distinct from
        // the plain Line tool. Round caps. Footprint ~0.38 square.
        const QPointF tail(bx(0.32), by(0.68));
        const QPointF tip(bx(0.68), by(0.32));
        p.drawLine(tail, tip);
        const qreal armLen = side * 0.20;
        p.drawLine(tip, QPointF(tip.x() - armLen, tip.y()));   // arm to the left
        p.drawLine(tip, QPointF(tip.x(), tip.y() + armLen));   // arm downward
    } else if (symbol == "line.diagonal") {
        // Native footprint ~0.41.
        p.drawLine(QPointF(bx(0.295), by(0.705)), QPointF(bx(0.705), by(0.295)));
    } else if (symbol == "rectangle") {
        // Native (design §1): a FLAT landscape rounded outline ~0.56w x 0.34h with a
        // small corner radius — wider and shorter than a square, sitting centered.
        const qreal rw = side * 0.56, rh = side * 0.34;
        const QRectF r(bx(0.5) - rw / 2.0, by(0.5) - rh / 2.0, rw, rh);
        const qreal rad = side * 0.06;
        p.drawRoundedRect(r, rad, rad);
    } else if (symbol == "rectangle.fill") {
        // Same flat landscape shape as `rectangle`, filled.
        const qreal rw = side * 0.56, rh = side * 0.34;
        const QRectF r(bx(0.5) - rw / 2.0, by(0.5) - rh / 2.0, rw, rh);
        const qreal rad = side * 0.06;
        p.setBrush(tint);
        p.setPen(QPen(tint, side * 0.03, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(r, rad, rad);
    } else if (symbol == "pencil") {
        // Native "pencil" (design §1): a SLANTED pencil along the up-right diagonal —
        // a sharp writing tip at the lower-left, a long thin body, a ferrule band,
        // and the flat cap at the upper-right, drawn as a single line icon ~0.60
        // diagonal span. Built in axis coords: `a` runs ALONG the pencil from the tip
        // (a=0) to the cap (a=1); `b` runs ACROSS it (-1..+1 = the two long edges).
        const qreal cx = side * 0.5, cy = side * 0.5;
        const qreal span  = side * 0.62;             // tip→cap diagonal length
        const qreal halfW = side * 0.058;            // half body width (thin)
        const qreal inv2  = 0.70710678;              // 1/sqrt(2)
        // a in 0..1 measured from the tip end; centre the whole pencil on the button.
        auto pt = [&](qreal a, qreal b) {
            const qreal along = (a - 0.5) * span;    // -span/2 .. +span/2
            return QPointF(cx + along * inv2 + b * halfW * inv2,
                           cy - along * inv2 + b * halfW * inv2);
        };
        QPen pp(tint, side * 0.05, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pp);
        p.setBrush(Qt::NoBrush);
        const qreal bodyStart = 0.20;                // where the wood tip meets the body
        const qreal cap       = 1.0;                 // cap end
        // Body outline (a thin rectangle from the collar up to the cap).
        QPainterPath body;
        body.moveTo(pt(bodyStart, -1.0));
        body.lineTo(pt(cap, -1.0));
        body.lineTo(pt(cap,  1.0));
        body.lineTo(pt(bodyStart, 1.0));
        body.closeSubpath();
        p.drawPath(body);
        // Ferrule band just below the cap (a short cross-line).
        p.drawLine(pt(cap - 0.18, -1.0), pt(cap - 0.18, 1.0));
        // Filled sharp writing tip (triangle) at the lower-left (a=0 point).
        QPainterPath tip;
        tip.moveTo(pt(0.0, 0.0));                     // sharp point
        tip.lineTo(pt(bodyStart, -1.0));
        tip.lineTo(pt(bodyStart,  1.0));
        tip.closeSubpath();
        p.setBrush(tint);
        p.setPen(QPen(tint, side * 0.03, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(tip);
        p.setBrush(Qt::NoBrush);
        p.setPen(stroke);
    } else if (symbol == "textformat") {
        // Native: "Aa" — a large cap A plus a smaller lowercase a (~0.59w x 0.38h),
        // medium-weight. On macOS the default QFont is the system font (San
        // Francisco), so this reads like the native symbol.
        p.setPen(tint);
        const qreal baseline = by(0.69);
        QFont fA = p.font();
        fA.setPixelSize(int(side * 0.56));
        fA.setWeight(QFont::Medium);
        p.setFont(fA);
        p.drawText(QPointF(bx(0.19), baseline), QStringLiteral("A"));
        QFont fa = p.font();
        fa.setPixelSize(int(side * 0.40));
        p.setFont(fa);
        p.drawText(QPointF(bx(0.58), baseline), QStringLiteral("a"));
    } else if (symbol == "arrow.uturn.backward" || symbol == "arrow.uturn.forward") {
        // Native u-turn (~0.49 square): a horizontal shaft at top with an OPEN
        // chevron head on one side, curving down the far side into a half-loop that
        // hooks back under. Build for "backward" (head left), mirror for "forward".
        const bool forward = (symbol == "arrow.uturn.forward");
        auto X = [&](qreal u) { return forward ? bx(1.0 - u) : bx(u); };
        QPainterPath hook;
        hook.moveTo(X(0.35), by(0.41));                       // shaft start (near head)
        hook.lineTo(X(0.60), by(0.41));                       // shaft to the right
        hook.cubicTo(X(0.75), by(0.41), X(0.75), by(0.70),    // round the far corner
                     X(0.60), by(0.70));
        hook.lineTo(X(0.52), by(0.70));                       // short tail under
        QPen hp(tint, side * 0.066, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(hp);
        p.drawPath(hook);
        // Open chevron head at the shaft start, pointing out (left for backward).
        const QPointF tip(X(0.27), by(0.41));
        const qreal aw = side * 0.105;   // chevron arm reach (x)
        const qreal ah = side * 0.125;   // chevron arm reach (y, taller = more open)
        p.drawLine(tip, QPointF(X(0.27) + (forward ? -aw : aw), by(0.41) - ah));
        p.drawLine(tip, QPointF(X(0.27) + (forward ? -aw : aw), by(0.41) + ah));
    } else if (symbol == "square.on.square") {
        // Native "square.on.square" (copy): two overlapping ROUNDED SQUARE outlines
        // (design §1) — a back square at the upper-right and a front square at the
        // lower-left occluding it. A transparent hole is punched behind the front
        // square so the overlap reads correctly on any panel/highlight color.
        const qreal sq = side * 0.40;                // square side
        const qreal cr = side * 0.085;              // corner radius
        auto sqRect = [&](qreal lf, qreal tf) {
            return QRectF(bx(lf), by(tf), sq, sq);
        };
        QPen dp(tint, side * 0.05, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(dp);
        p.drawRoundedRect(sqRect(0.40, 0.18), cr, cr);   // back square (upper-right)
        QPainterPath front;
        front.addRoundedRect(sqRect(0.215, 0.36), cr, cr);
        p.save();                                        // punch a hole for the front
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::transparent);
        p.drawPath(front);
        p.restore();
        p.setPen(dp);
        p.drawPath(front);                               // front square (lower-left)
    } else if (symbol == "floppy") {
        // Save = a classic FLOPPY DISK (design §1): a rounded-square body with the
        // TOP-RIGHT corner cut diagonally; a metal-shutter slot rectangle in the
        // upper portion; and a wide write-protect label rectangle at the bottom.
        QPen sp(tint, side * 0.05, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(sp);
        p.setBrush(Qt::NoBrush);
        const qreal L = bx(0.275), R = bx(0.725), T = by(0.255), B = by(0.745);
        const qreal cut = side * 0.135;             // diagonal corner cut size
        const qreal cr  = side * 0.05;              // small corner radius
        // Body outline with the cut top-right corner.
        QPainterPath body;
        body.moveTo(L + cr, T);
        body.lineTo(R - cut, T);                     // top edge to the cut start
        body.lineTo(R, T + cut);                     // diagonal cut down-right
        body.lineTo(R, B - cr);
        body.quadTo(R, B, R - cr, B);
        body.lineTo(L + cr, B);
        body.quadTo(L, B, L, B - cr);
        body.lineTo(L, T + cr);
        body.quadTo(L, T, L + cr, T);
        p.drawPath(body);
        // Shutter slot (upper portion): a small rect biased toward the left, its
        // right edge stopping short of the cut corner.
        const qreal slotL = L + (R - L) * 0.18, slotR = R - cut - side * 0.02;
        const qreal slotT = T, slotB = T + (B - T) * 0.34;
        p.drawLine(QPointF(slotL, slotT), QPointF(slotL, slotB));
        p.drawLine(QPointF(slotR, slotT), QPointF(slotR, slotB));
        p.drawLine(QPointF(slotL, slotB), QPointF(slotR, slotB));
        // Bottom label rectangle (inset from the sides, sits on the lower body).
        const qreal labL = L + (R - L) * 0.20, labR = R - (R - L) * 0.20;
        const qreal labT = T + (B - T) * 0.50, labB = B;
        p.drawLine(QPointF(labL, labT), QPointF(labR, labT));   // label top
        p.drawLine(QPointF(labL, labT), QPointF(labL, labB));   // label left
        p.drawLine(QPointF(labR, labT), QPointF(labR, labB));   // label right
    } else if (symbol == "xmark") {
        // Native footprint ~0.41 square.
        p.drawLine(QPointF(bx(0.295), by(0.295)), QPointF(bx(0.705), by(0.705)));
        p.drawLine(QPointF(bx(0.705), by(0.295)), QPointF(bx(0.295), by(0.705)));
    } else if (symbol == "checkmark") {
        p.drawPolyline(QPolygonF({ QPointF(px(0.12), py(0.55)),
                                   QPointF(px(0.42), py(0.85)),
                                   QPointF(px(0.90), py(0.18)) }));
    } else if (symbol == "nosign") {
        p.drawEllipse(QRectF(px(0.06), py(0.06), w * 0.88, h * 0.88));
        const qreal d = 0.353; // ~cos(45)/2 across the circle
        p.drawLine(QPointF(px(0.5 - d), py(0.5 - d)), QPointF(px(0.5 + d), py(0.5 + d)));
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
