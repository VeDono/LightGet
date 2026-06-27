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
#include <QPolygonF>
#include <QtMath>
#include <cmath>
#include <cstdlib>   // std::abs(int) in Palette::sameColor

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

constexpr int kPanelBorderToolbarR  = 31;   // rgba(31,31,31,242) — NSColor(white:0.12)
constexpr int kPanelAlphaToolbar    = 242;  // 0.95 * 255
constexpr int kPanelAlphaInspector  = 245;  // 0.96 * 255
constexpr double kPanelRadius       = 8.0;

const QColor kSelectedBlue(0, 122, 255);    // systemBlue / #007AFF

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

    // Glyph drawing area: a centered square inset from the button.
    const qreal inset = side * 0.30;
    const QRectF box(inset, inset, side - 2 * inset, side - 2 * inset);
    const qreal w = box.width(), h = box.height();
    const qreal x0 = box.left(), y0 = box.top();
    auto px = [&](qreal u) { return x0 + u * w; };  // u in 0..1
    auto py = [&](qreal v) { return y0 + v * h; };  // v in 0..1 (top-down)

    QPen stroke(tint);
    stroke.setWidthF(side * 0.072);
    stroke.setCapStyle(Qt::RoundCap);
    stroke.setJoinStyle(Qt::RoundJoin);
    p.setPen(stroke);
    p.setBrush(Qt::NoBrush);

    if (symbol == "cursorarrow") {
        // Pointer arrow (filled triangle with tail).
        QPainterPath path;
        path.moveTo(px(0.20), py(0.05));
        path.lineTo(px(0.20), py(0.85));
        path.lineTo(px(0.42), py(0.62));
        path.lineTo(px(0.55), py(0.95));
        path.lineTo(px(0.68), py(0.88));
        path.lineTo(px(0.55), py(0.56));
        path.lineTo(px(0.85), py(0.50));
        path.closeSubpath();
        p.setPen(QPen(tint, side * 0.045, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tint);
        p.drawPath(path);
    } else if (symbol == "arrow.up.right") {
        // Clean diagonal arrow: a shaft from bottom-left to a tip near top-right,
        // capped with a FILLED triangular arrowhead (so it reads as an arrow, not
        // an open bracket, and is clearly distinct from the plain Line tool).
        const QPointF tail(px(0.12), py(0.88));
        const QPointF tip(px(0.84), py(0.16));
        // Shaft stops a touch short of the tip so the filled head sits on the end.
        const qreal t = 0.74;
        const QPointF shaftEnd(tail.x() + (tip.x() - tail.x()) * t,
                               tail.y() + (tip.y() - tail.y()) * t);
        p.drawLine(tail, shaftEnd);

        const qreal ang = std::atan2(tip.y() - tail.y(), tip.x() - tail.x());
        const qreal headLen = w * 0.34;
        const qreal spread  = M_PI / 7.0;
        const QPointF h1(tip.x() - headLen * std::cos(ang - spread),
                         tip.y() - headLen * std::sin(ang - spread));
        const QPointF h2(tip.x() - headLen * std::cos(ang + spread),
                         tip.y() - headLen * std::sin(ang + spread));
        QPainterPath head(tip);
        head.lineTo(h1);
        head.lineTo(h2);
        head.closeSubpath();
        p.setPen(QPen(tint, side * 0.045, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tint);
        p.drawPath(head);
    } else if (symbol == "line.diagonal") {
        p.drawLine(QPointF(px(0.08), py(0.92)), QPointF(px(0.92), py(0.08)));
    } else if (symbol == "rectangle") {
        p.drawRoundedRect(QRectF(px(0.05), py(0.15), w * 0.90, h * 0.70), 2, 2);
    } else if (symbol == "rectangle.fill") {
        p.setBrush(tint);
        p.drawRoundedRect(QRectF(px(0.05), py(0.15), w * 0.90, h * 0.70), 2, 2);
    } else if (symbol == "pencil.tip") {
        // A recognizable PENCIL drawn diagonally (sharp tip at bottom-left, flat
        // eraser end at top-right). Built from a slanted barrel (parallelogram),
        // a filled triangular wooden tip, and a short flat cap at the eraser end.
        //
        // Barrel direction (unit) and a perpendicular for the barrel half-width.
        const QPointF tipPt(px(0.16), py(0.84));   // sharpened point
        const QPointF capPt(px(0.84), py(0.16));   // eraser end
        const qreal dx = capPt.x() - tipPt.x(), dy = capPt.y() - tipPt.y();
        const qreal len = std::hypot(dx, dy);
        const qreal ux = dx / len, uy = dy / len;          // along barrel
        const qreal nx = -uy, ny = ux;                     // perpendicular
        const qreal halfW = w * 0.16;                      // barrel half-thickness

        // Where the wooden tip ends and the painted barrel begins.
        const qreal collar = len * 0.26;
        const QPointF collarMid(tipPt.x() + ux * collar, tipPt.y() + uy * collar);
        // Eraser end shortened a hair so the barrel doesn't run to the corner.
        const QPointF endMid(capPt.x() - ux * (len * 0.04),
                             capPt.y() - uy * (len * 0.04));

        auto offset = [&](const QPointF& m, qreal s) {
            return QPointF(m.x() + nx * s, m.y() + ny * s);
        };

        // Filled wooden tip: triangle from the sharp point to the two collar edges.
        QPainterPath tip;
        tip.moveTo(tipPt);
        tip.lineTo(offset(collarMid,  halfW));
        tip.lineTo(offset(collarMid, -halfW));
        tip.closeSubpath();
        p.setPen(QPen(tint, side * 0.045, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tint);
        p.drawPath(tip);

        // Barrel: outlined parallelogram from the collar to the eraser end.
        QPainterPath barrel;
        barrel.moveTo(offset(collarMid,  halfW));
        barrel.lineTo(offset(endMid,     halfW));
        barrel.lineTo(offset(endMid,    -halfW));
        barrel.lineTo(offset(collarMid, -halfW));
        barrel.closeSubpath();
        p.setBrush(Qt::NoBrush);
        p.drawPath(barrel);

        // Eraser cap: a short stroke across the very end to suggest the ferrule.
        p.drawLine(offset(endMid, halfW), offset(endMid, -halfW));
    } else if (symbol == "textformat") {
        QFont f = p.font();
        f.setPixelSize(int(h * 0.95));
        f.setBold(true);
        p.setFont(f);
        p.setPen(tint);
        p.drawText(box.adjusted(-side * 0.05, -side * 0.05, side * 0.05, side * 0.05),
                   Qt::AlignCenter, QStringLiteral("A"));
    } else if (symbol == "arrow.uturn.backward" || symbol == "arrow.uturn.forward") {
        // Clean 3/4-circle undo (CCW) / redo (CW) arrow with a filled arrowhead
        // at the moving end, oriented along the arc's local travel direction.
        // undo and redo are exact horizontal mirrors (start 305°/+250° vs
        // 235°/-250°), so the pair reads symmetrically.
        const bool forward = (symbol == "arrow.uturn.forward");
        const qreal cx = px(0.50), cy = py(0.52);
        const qreal R  = w * 0.34;
        const QRectF aRect(cx - R, cy - R, 2 * R, 2 * R);
        const qreal startDeg = forward ? 235.0 : 305.0;
        const qreal sweepDeg = forward ? -250.0 : 250.0;
        QPainterPath arc;
        arc.arcMoveTo(aRect, startDeg);
        arc.arcTo(aRect, startDeg, sweepDeg);
        p.drawPath(arc);
        const QPointF a1 = arc.pointAtPercent(1.0);
        const QPointF a0 = arc.pointAtPercent(0.92);
        QPointF dir = a1 - a0;
        const qreal dl = std::hypot(dir.x(), dir.y());
        if (dl > 0) dir /= dl;
        auto rot = [](QPointF v, qreal ang) {
            return QPointF(v.x() * std::cos(ang) - v.y() * std::sin(ang),
                           v.x() * std::sin(ang) + v.y() * std::cos(ang));
        };
        const qreal hl = w * 0.34, spread = 0.55;
        const QPointF hb1 = a1 - rot(dir,  spread) * hl;
        const QPointF hb2 = a1 - rot(dir, -spread) * hl;
        QPainterPath head(a1);
        head.lineTo(hb1);
        head.lineTo(hb2);
        head.closeSubpath();
        p.setPen(QPen(tint, side * 0.045, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(tint);
        p.drawPath(head);
    } else if (symbol == "doc.on.doc") {
        p.drawRoundedRect(QRectF(px(0.05), py(0.20), w * 0.55, h * 0.70), 2, 2);
        p.drawRoundedRect(QRectF(px(0.40), py(0.05), w * 0.55, h * 0.70), 2, 2);
    } else if (symbol == "square.and.arrow.down") {
        p.drawLine(QPointF(px(0.50), py(0.05)), QPointF(px(0.50), py(0.60)));
        QPainterPath head;
        head.moveTo(px(0.30), py(0.40));
        head.lineTo(px(0.50), py(0.62));
        head.lineTo(px(0.70), py(0.40));
        p.drawPath(head);
        QPainterPath tray;
        tray.moveTo(px(0.10), py(0.55));
        tray.lineTo(px(0.10), py(0.92));
        tray.lineTo(px(0.90), py(0.92));
        tray.lineTo(px(0.90), py(0.55));
        p.drawPath(tray);
    } else if (symbol == "xmark") {
        p.drawLine(QPointF(px(0.12), py(0.12)), QPointF(px(0.88), py(0.88)));
        p.drawLine(QPointF(px(0.88), py(0.12)), QPointF(px(0.12), py(0.88)));
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

// Antialiased color well for the palette. Replaces the QSS `border:1px solid`
// circle, which aliased badly on a tiny 18px rounded button (the "pixelated
// outline" the user reported). Drawn into a 2x pixmap: a filled AA circle with a
// subtle contrast ring; the selected well gets a bright outer ring + checkmark.
QIcon makeSwatch(const QColor& color, bool selected, int d = 18) {
    const qreal dpr = 2.0;
    QPixmap pm(int(d * dpr), int(d * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal ringInset = selected ? 2.2 : 1.0;
    const QRectF r(ringInset, ringInset, d - 2 * ringInset, d - 2 * ringInset);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(r);

    // Contrast ring on the fill edge: dark on light swatches, light on dark.
    const QColor edge = Palette::isLight(color) ? QColor(0, 0, 0, 70)
                                                : QColor(255, 255, 255, 150);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(edge, 1.0));
    p.drawEllipse(r);

    if (selected) {
        p.setPen(QPen(QColor(255, 255, 255, 235), 1.6));
        p.drawEllipse(QRectF(1.0, 1.0, d - 2.0, d - 2.0));
        const QColor tint = Palette::isLight(color) ? Qt::black : Qt::white;
        auto u = [&](qreal t) { return t * d; };
        QPen cm(tint, d * 0.10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(cm);
        p.drawPolyline(QPolygonF({ QPointF(u(0.30), u(0.52)),
                                   QPointF(u(0.45), u(0.68)),
                                   QPointF(u(0.72), u(0.34)) }));
    }
    p.end();
    return QIcon(pm);
}

// A QPushButton that pops (scales 1.0->peak->1.0 about its center) on demand and
// optionally paints a checkmark / colored fill itself. Replaces the Swift
// ActionButton + CALayer transform animation.
//
// CLIPPING FIX (task 1): the pop scales the button content via a QTransform in
// paintEvent, which is clipped to the widget's own rect. A >1 scale therefore
// clips anything that reaches the button edge. Two things used to reach the edge:
//   - the glyph icon (drawn at full kButtonSize) — now rendered SMALLER than the
//     button via setIconSize(~24px) so a 1.10x pop stays inside the 30px bounds;
//   - the selected-tool blue highlight (was a full-bleed QSS background) — now
//     painted HERE as a rounded rect INSET from the edge, so it scales without
//     touching the clip boundary. The QSS background is dropped for tool buttons.
class PopButton : public QPushButton {
public:
    explicit PopButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
    }

    // Selected-tool highlight: an inset rounded blue rect painted by this widget
    // (NOT a full-bleed QSS background) so it stays clear of the clip boundary
    // and scales cleanly with the pop. Pass a non-null color to enable.
    void setHighlight(const QColor& color, qreal inset = 3.0, qreal radius = 6.0) {
        m_highlight = color;
        m_highlightInset = inset;
        m_highlightRadius = radius;
        update();
    }
    void clearHighlight() {
        m_highlight = QColor();   // invalid -> no highlight
        update();
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
    void paintEvent(QPaintEvent* e) override {
        const bool popping = !qFuzzyCompare(m_scale, qreal(1.0));
        if (!popping && !m_highlight.isValid()) {
            // Plain, unscaled, no self-drawn highlight -> let the style paint it.
            QPushButton::paintEvent(e);
            return;
        }

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Scale about center without shifting (matches the Swift centered() matrix).
        // The same transform covers the inset highlight AND the icon, so both pop
        // together — and both stay inside the widget rect (highlight is inset,
        // icon is sub-button-size), so neither clips at the scaled edge.
        if (popping) {
            const QPointF c(width() / 2.0, height() / 2.0);
            QTransform t;
            t.translate(c.x(), c.y());
            t.scale(m_scale, m_scale);
            t.translate(-c.x(), -c.y());
            p.setTransform(t);
        }

        // Inset rounded highlight (selected tool) painted by us, not via QSS.
        if (m_highlight.isValid()) {
            const QRectF hr = QRectF(rect()).adjusted(
                m_highlightInset, m_highlightInset, -m_highlightInset, -m_highlightInset);
            p.setPen(Qt::NoPen);
            p.setBrush(m_highlight);
            p.drawRoundedRect(hr, m_highlightRadius, m_highlightRadius);
        }

        // Re-render the base button content (the icon) under the transform. The
        // icon is sized below kButtonSize (setIconSize ~24px), so it never reaches
        // the clip boundary even at the pop peak.
        QStyleOptionButton opt;
        initStyleOption(&opt);
        style()->drawControl(QStyle::CE_PushButton, &opt, &p, this);
    }

private:
    QVariantAnimation* m_anim = nullptr;
    qreal m_scale = 1.0;
    QColor m_highlight;            // invalid = no selected-tool highlight
    qreal  m_highlightInset = 3.0;
    qreal  m_highlightRadius = 6.0;
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

    qreal x = kPad;

    // --- Tools (only the ones enabled in Settings; Select is always enabled) ---
    struct ToolSym { Tool tool; const char* symbol; };
    const ToolSym tools[] = {
        { Tool::Select,     "cursorarrow" },
        { Tool::Arrow,      "arrow.up.right" },
        { Tool::Line,       "line.diagonal" },
        { Tool::Rectangle,  "rectangle" },
        { Tool::FilledRect, "rectangle.fill" },
        { Tool::Pen,        "pencil.tip" },
        { Tool::Text,       "textformat" },
    };
    for (const auto& ts : tools) {
        if (!Settings::instance().isToolEnabled(ts.tool)) continue;
        const Tool tool = ts.tool;
        auto* b = new PopButton(this);
        // Render the glyph at kIconSize (< kButtonSize) and center it in the
        // button: the smaller icon leaves margin so the 1.10x pop never clips.
        b->setIcon(makeGlyph(QString::fromLatin1(ts.symbol), Qt::white, kIconSize));
        b->setIconSize(QSize(kIconSize, kIconSize));
        b->setGeometry(int(x), kPad, kButtonSize, kButtonSize);
        connect(b, &QPushButton::clicked, this, [this, tool] {
            setSelectedTool(tool);
            emit selectToolRequested(tool);
            popTool(tool);
        });
        b->show();
        m_toolButtons.insert(tool, b);
        x += kButtonSize + 2;
    }
    // Re-apply the highlight (default Select) so the active tool stays lit.
    setSelectedTool(m_selectedTool);

    // --- Color palette (only if enabled in Settings) ---
    if (Settings::instance().showColorPalette()) {
        x += 6;  // separator
        const auto& cols = Palette::colors();
        for (int i = 0; i < cols.size(); ++i) {
            const QColor color = cols[i];
            auto* well = new PopButton(this);
            well->setProperty("fill", color);
            // 18x18 circle, vertically centered within the 30px button row.
            const int wy = kPad + (kButtonSize - 18) / 2;
            well->setGeometry(int(x), wy, 18, 18);
            // The well is painted as an antialiased icon (makeSwatch), not a QSS
            // rounded border — the latter aliased on this tiny 18px button. Keep
            // the button itself borderless/transparent; setSelectedColor assigns
            // the swatch icon (and the selected ring + checkmark).
            well->setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
            well->setIconSize(QSize(18, 18));
            connect(well, &QPushButton::clicked, this, [this, color, i] {
                emit selectColorRequested(color);
                setSelectedColor(i);
                popColor(i);
            });
            well->show();
            m_colorButtons.append(qMakePair(color, static_cast<QPushButton*>(well)));
            x += 22;
        }
        setSelectedColor(m_selectedColor);  // default selected = 0 (red)
    }

    x += 6;  // separator

    // --- Actions ---
    struct ActionSym { const char* symbol; void (ToolbarView::*sig)(); };
    const ActionSym actions[] = {
        { "arrow.uturn.backward",  &ToolbarView::undoRequested },
        { "arrow.uturn.forward",   &ToolbarView::redoRequested },
        { "doc.on.doc",            &ToolbarView::copyRequested },
        { "square.and.arrow.down", &ToolbarView::saveRequested },
        { "xmark",                 &ToolbarView::closeRequested },
    };
    for (const auto& a : actions) {
        auto* b = new PopButton(this);
        // Same sub-button-size glyph as the tools so the action pop never clips.
        b->setIcon(makeGlyph(QString::fromLatin1(a.symbol), Qt::white, kIconSize));
        b->setIconSize(QSize(kIconSize, kIconSize));
        b->setGeometry(int(x), kPad, kButtonSize, kButtonSize);
        auto sig = a.sig;
        connect(b, &QPushButton::clicked, this, [this, sig] { (this->*sig)(); });
        b->show();
        x += kButtonSize + 2;
    }

    // Height ALWAYS 42 = buttonSize(30) + pad(6) * 2.
    setFixedSize(int(x + kPad), kButtonSize + kPad * 2);
    update();
}

void ToolbarView::setSelectedTool(Tool t) {
    m_selectedTool = t;
    for (auto it = m_toolButtons.begin(); it != m_toolButtons.end(); ++it) {
        const bool sel = (it.key() == t);
        // Highlight is painted by the PopButton as an INSET rounded rect (~3px
        // from the edge), not a full-bleed QSS background — so it never reaches
        // the clip boundary and scales cleanly with the pop (task 1). The QSS
        // stays transparent/borderless for every tool button.
        auto* b = static_cast<PopButton*>(it.value());
        if (sel) b->setHighlight(kSelectedBlue, /*inset=*/3.0, /*radius=*/6.0);
        else     b->clearHighlight();
    }
}

void ToolbarView::setSelectedColor(int paletteIndex) {
    m_selectedColor = paletteIndex;
    for (int i = 0; i < m_colorButtons.size(); ++i) {
        const QColor color = m_colorButtons[i].first;
        QPushButton* well = m_colorButtons[i].second;
        // Antialiased swatch icon (filled circle + ring; selected adds the bright
        // ring + checkmark). No QSS border -> no aliased outline.
        well->setStyleSheet(QStringLiteral("QPushButton{border:none;background:transparent;}"));
        well->setIcon(makeSwatch(color, i == paletteIndex, 18));
        well->setIconSize(QSize(18, 18));
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
    const QRectF r = QRectF(rect()).adjusted(0.25, 0.25, -0.25, -0.25);
    // Panel bg rgba(31,31,31,242); border white@15% 0.5px.
    p.setPen(QPen(QColor(255, 255, 255, 38), 0.5));   // 0.15 * 255 = 38
    p.setBrush(QColor(kPanelBorderToolbarR, kPanelBorderToolbarR,
                      kPanelBorderToolbarR, kPanelAlphaToolbar));
    p.drawRoundedRect(r, kPanelRadius, kPanelRadius);
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
    const QRectF r = QRectF(rect()).adjusted(0.25, 0.25, -0.25, -0.25);
    // Panel bg rgba(31,31,31,245); border white@15% 0.5px.
    p.setPen(QPen(QColor(255, 255, 255, 38), 0.5));
    p.setBrush(QColor(kPanelBorderToolbarR, kPanelBorderToolbarR,
                      kPanelBorderToolbarR, kPanelAlphaInspector));
    p.drawRoundedRect(r, kPanelRadius, kPanelRadius);
}
