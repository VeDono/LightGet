// ===========================================================================
// gen_icons.cpp  --  THROWAWAY icon generator for LightGet.
//
// Draws every SF-Symbol-style glyph the app references as a clean, MONOCHROME
// WHITE icon on a TRANSPARENT background at 64x64 (antialiased, ~3px pen) and
// writes resources/assets/<name>.png. White-on-transparent lets Qt tint or
// place each glyph freely on dark toolbars / the tray.
//
// This file is NOT part of the LightGet target. Build & run it standalone:
//
//   QT="$HOME/qt6-temp/6.8.1/macos"
//   clang++ -std=c++17 -fPIC \
//       -I"$QT/lib/QtCore.framework/Headers" \
//       -I"$QT/lib/QtGui.framework/Headers" \
//       -F"$QT/lib" \
//       resources/gen_icons.cpp -o /tmp/gen_icons \
//       -framework QtCore -framework QtGui
//   /tmp/gen_icons resources/assets
//
// (On Linux/Windows swap the include/lib flags for the matching Qt install;
//  the source itself is pure Qt and portable.)
// ===========================================================================
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QDir>
#include <QFont>
#include <cstdio>

namespace {

constexpr int  kSize = 64;     // output canvas px
constexpr qreal kPen = 3.0;    // ~3px stroke

// The drawing box is inset from the canvas so strokes never clip at the edge.
// All glyph geometry below is expressed in a normalised 0..1 space mapped into
// this box (matches the proportional layout the live Toolbar uses).
QRectF glyphBox() {
    const qreal m = kSize * 0.16;                 // ~10px margin
    return QRectF(m, m, kSize - 2 * m, kSize - 2 * m);
}

// Draw one glyph by name. `p` already has antialiasing + a white round pen set;
// callers that need a fill set the brush themselves and reset it after.
void drawGlyph(QPainter& p, const QString& name) {
    const QRectF box = glyphBox();
    const qreal w = box.width(), h = box.height();
    const qreal x0 = box.left(), y0 = box.top();
    auto px = [&](qreal u) { return x0 + u * w; };   // u in 0..1 (left->right)
    auto py = [&](qreal v) { return y0 + v * h; };   // v in 0..1 (top->bottom)

    const QColor white(Qt::white);

    if (name == "cursorarrow") {
        // Filled pointer arrow with tail.
        QPainterPath path;
        path.moveTo(px(0.18), py(0.02));
        path.lineTo(px(0.18), py(0.86));
        path.lineTo(px(0.40), py(0.62));
        path.lineTo(px(0.54), py(0.98));
        path.lineTo(px(0.68), py(0.90));
        path.lineTo(px(0.54), py(0.55));
        path.lineTo(px(0.86), py(0.50));
        path.closeSubpath();
        p.setBrush(white);
        p.drawPath(path);
        p.setBrush(Qt::NoBrush);

    } else if (name == "arrow.up.right") {
        p.drawLine(QPointF(px(0.10), py(0.90)), QPointF(px(0.90), py(0.10)));
        QPainterPath head;
        head.moveTo(px(0.45), py(0.10));
        head.lineTo(px(0.90), py(0.10));
        head.lineTo(px(0.90), py(0.55));
        p.drawPath(head);

    } else if (name == "line.diagonal") {
        p.drawLine(QPointF(px(0.06), py(0.94)), QPointF(px(0.94), py(0.06)));

    } else if (name == "rectangle") {
        p.drawRoundedRect(QRectF(px(0.04), py(0.16), w * 0.92, h * 0.68),
                          kSize * 0.05, kSize * 0.05);

    } else if (name == "rectangle.fill") {
        p.setBrush(white);
        p.drawRoundedRect(QRectF(px(0.04), py(0.16), w * 0.92, h * 0.68),
                          kSize * 0.05, kSize * 0.05);
        p.setBrush(Qt::NoBrush);

    } else if (name == "rectangle.dashed") {
        QPen dashed = p.pen();
        dashed.setStyle(Qt::DashLine);
        dashed.setDashPattern({3.0, 2.2});
        p.setPen(dashed);
        p.drawRoundedRect(QRectF(px(0.04), py(0.16), w * 0.92, h * 0.68),
                          kSize * 0.05, kSize * 0.05);
        // restore solid pen for any later use
        QPen solid = p.pen();
        solid.setStyle(Qt::SolidLine);
        p.setPen(solid);

    } else if (name == "pencil.tip") {
        // Pencil body + filled nib triangle.
        p.drawLine(QPointF(px(0.28), py(0.74)), QPointF(px(0.78), py(0.24)));
        QPainterPath tip;
        tip.moveTo(px(0.08), py(0.94));
        tip.lineTo(px(0.32), py(0.84));
        tip.lineTo(px(0.18), py(0.70));
        tip.closeSubpath();
        p.setBrush(white);
        p.drawPath(tip);
        p.setBrush(Qt::NoBrush);

    } else if (name == "textformat") {
        // The letter "A".
        QFont f = p.font();
        f.setPixelSize(int(h * 1.05));
        f.setBold(true);
        p.setFont(f);
        p.drawText(box.adjusted(-w * 0.06, -h * 0.06, w * 0.06, h * 0.06),
                   Qt::AlignCenter, QStringLiteral("A"));

    } else if (name == "arrow.uturn.backward") {
        QPainterPath arc;
        arc.moveTo(px(0.22), py(0.30));
        arc.arcTo(QRectF(px(0.22), py(0.20), w * 0.62, h * 0.56), 90, -270);
        p.drawPath(arc);
        QPainterPath head;
        head.moveTo(px(0.06), py(0.30));
        head.lineTo(px(0.22), py(0.08));
        head.lineTo(px(0.38), py(0.30));
        p.drawPath(head);

    } else if (name == "arrow.uturn.forward") {
        QPainterPath arc;
        arc.moveTo(px(0.78), py(0.30));
        arc.arcTo(QRectF(px(0.16), py(0.20), w * 0.62, h * 0.56), 90, 270);
        p.drawPath(arc);
        QPainterPath head;
        head.moveTo(px(0.94), py(0.30));
        head.lineTo(px(0.78), py(0.08));
        head.lineTo(px(0.62), py(0.30));
        p.drawPath(head);

    } else if (name == "doc.on.doc") {
        // Two overlapping rounded rects (back + front document).
        p.drawRoundedRect(QRectF(px(0.06), py(0.22), w * 0.54, h * 0.70),
                          kSize * 0.04, kSize * 0.04);
        p.drawRoundedRect(QRectF(px(0.40), py(0.06), w * 0.54, h * 0.70),
                          kSize * 0.04, kSize * 0.04);

    } else if (name == "square.and.arrow.down") {
        // Down arrow into an open tray.
        p.drawLine(QPointF(px(0.50), py(0.04)), QPointF(px(0.50), py(0.58)));
        QPainterPath head;
        head.moveTo(px(0.30), py(0.40));
        head.lineTo(px(0.50), py(0.62));
        head.lineTo(px(0.70), py(0.40));
        p.drawPath(head);
        QPainterPath tray;
        tray.moveTo(px(0.10), py(0.55));
        tray.lineTo(px(0.10), py(0.94));
        tray.lineTo(px(0.90), py(0.94));
        tray.lineTo(px(0.90), py(0.55));
        p.drawPath(tray);

    } else if (name == "xmark") {
        p.drawLine(QPointF(px(0.12), py(0.12)), QPointF(px(0.88), py(0.88)));
        p.drawLine(QPointF(px(0.88), py(0.12)), QPointF(px(0.12), py(0.88)));

    } else if (name == "checkmark") {
        p.drawPolyline(QPolygonF({ QPointF(px(0.10), py(0.55)),
                                   QPointF(px(0.40), py(0.86)),
                                   QPointF(px(0.92), py(0.14)) }));

    } else if (name == "nosign") {
        p.drawEllipse(QRectF(px(0.04), py(0.04), w * 0.92, h * 0.92));
        const qreal d = 0.353;   // ~cos(45)/2 across the circle
        p.drawLine(QPointF(px(0.5 - d), py(0.5 - d)),
                   QPointF(px(0.5 + d), py(0.5 + d)));

    } else if (name == "scissors") {
        // Two crossed blade-rings with cutting edges meeting at a pivot.
        const qreal rr = w * 0.20;
        p.drawEllipse(QRectF(px(0.06), py(0.62), rr, rr));            // lower-left ring
        p.drawEllipse(QRectF(px(0.06), py(0.16), rr, rr));            // upper-left ring
        const QPointF pivot(px(0.46), py(0.50));
        p.drawLine(QPointF(px(0.20), py(0.30)), QPointF(px(0.94), py(0.78)));
        p.drawLine(QPointF(px(0.20), py(0.70)), QPointF(px(0.94), py(0.22)));
        p.drawEllipse(pivot, kSize * 0.02, kSize * 0.02);

    } else if (name == "camera.viewfinder") {
        // Square with four corner ticks + a centre dot (lens).
        // Corner ticks (L shapes) at each corner of an inner square.
        const qreal a = 0.10, b = 0.90, t = 0.22;   // corners & tick length
        // top-left
        p.drawLine(QPointF(px(a), py(a)), QPointF(px(a + t), py(a)));
        p.drawLine(QPointF(px(a), py(a)), QPointF(px(a), py(a + t)));
        // top-right
        p.drawLine(QPointF(px(b), py(a)), QPointF(px(b - t), py(a)));
        p.drawLine(QPointF(px(b), py(a)), QPointF(px(b), py(a + t)));
        // bottom-left
        p.drawLine(QPointF(px(a), py(b)), QPointF(px(a + t), py(b)));
        p.drawLine(QPointF(px(a), py(b)), QPointF(px(a), py(b - t)));
        // bottom-right
        p.drawLine(QPointF(px(b), py(b)), QPointF(px(b - t), py(b)));
        p.drawLine(QPointF(px(b), py(b)), QPointF(px(b), py(b - t)));
        // lens
        p.drawEllipse(QPointF(px(0.5), py(0.5)), w * 0.14, h * 0.14);

    } else if (name == "crop") {
        // Two interlocking L corners (top-left + bottom-right) of a crop frame.
        // Top-left L (its arms overshoot, classic crop-marks look).
        p.drawLine(QPointF(px(0.22), py(0.04)), QPointF(px(0.22), py(0.78)));
        p.drawLine(QPointF(px(0.22), py(0.78)), QPointF(px(0.96), py(0.78)));
        // Bottom-right L.
        p.drawLine(QPointF(px(0.78), py(0.96)), QPointF(px(0.78), py(0.22)));
        p.drawLine(QPointF(px(0.78), py(0.22)), QPointF(px(0.04), py(0.22)));

    } else if (name == "paintbrush.pointed.fill") {
        // Brush handle (line) + filled brush head (diamond/teardrop).
        p.drawLine(QPointF(px(0.86), py(0.10)), QPointF(px(0.46), py(0.50)));
        QPainterPath head;
        head.moveTo(px(0.46), py(0.42));
        head.lineTo(px(0.62), py(0.58));
        head.lineTo(px(0.34), py(0.92));   // tapered tip pointing down-left
        head.lineTo(px(0.12), py(0.70));
        head.closeSubpath();
        p.setBrush(white);
        p.drawPath(head);
        p.setBrush(Qt::NoBrush);

    } else if (name == "photo") {
        // Picture frame with a "mountain" + sun (classic image glyph).
        p.drawRoundedRect(QRectF(px(0.06), py(0.18), w * 0.88, h * 0.64),
                          kSize * 0.04, kSize * 0.04);
        p.drawEllipse(QPointF(px(0.30), py(0.36)), w * 0.07, h * 0.07);   // sun
        QPainterPath mtn;
        mtn.moveTo(px(0.10), py(0.78));
        mtn.lineTo(px(0.40), py(0.50));
        mtn.lineTo(px(0.58), py(0.66));
        mtn.lineTo(px(0.74), py(0.48));
        mtn.lineTo(px(0.90), py(0.70));
        p.drawPath(mtn);

    } else if (name == "arrow.up.left.and.arrow.down.right") {
        // Two opposing diagonal arrows (resize / expand glyph).
        const QPointF tl(px(0.10), py(0.10)), br(px(0.90), py(0.90));
        p.drawLine(tl, br);
        const qreal aw = w * 0.30;   // arrowhead wing length
        // top-left head
        p.drawLine(tl, tl + QPointF(aw, 0));
        p.drawLine(tl, tl + QPointF(0, aw));
        // bottom-right head
        p.drawLine(br, br - QPointF(aw, 0));
        p.drawLine(br, br - QPointF(0, aw));

    } else if (name == "arrow.counterclockwise") {
        // ~300deg arc with an arrowhead (reset / refresh glyph).
        QRectF arcBox(px(0.10), py(0.10), w * 0.80, h * 0.80);
        QPainterPath arc;
        // start near top, sweep counter-clockwise leaving a gap at top-right.
        arc.arcMoveTo(arcBox, 70);
        arc.arcTo(arcBox, 70, 300);
        p.drawPath(arc);
        // arrowhead at the arc start (~70deg, upper area).
        const QPointF tip = arc.elementAt(0);
        QPainterPath head;
        head.moveTo(tip + QPointF(-w * 0.16, -h * 0.02));
        head.lineTo(tip);
        head.lineTo(tip + QPointF(w * 0.04, -h * 0.18));
        p.drawPath(head);

    } else {
        // Unknown name: draw a visible placeholder square so the button is never
        // empty (defensive — should not happen for the curated list below).
        p.drawRect(QRectF(px(0.10), py(0.10), w * 0.80, h * 0.80));
    }
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                      : QStringLiteral("resources/assets");
    QDir().mkpath(outDir);

    // Authoritative list: every :/assets/<name>.png the app may request, plus a
    // matching PNG for every Toolbar makeGlyph symbol so no button can be empty.
    const QStringList names = {
        // Toolbar tool glyphs
        "cursorarrow", "arrow.up.right", "line.diagonal",
        "rectangle", "rectangle.fill", "pencil.tip", "textformat",
        // Toolbar action glyphs
        "arrow.uturn.backward", "arrow.uturn.forward", "doc.on.doc",
        "square.and.arrow.down", "xmark", "checkmark", "nosign",
        // Bar-icon presets (loaded as :/assets/<name>.png)
        "scissors", "camera.viewfinder", "crop", "rectangle.dashed",
        "paintbrush.pointed.fill",
        // SettingsWindow extras (loaded as :/assets/<name>.png)
        "photo", "arrow.counterclockwise",
        // Overlay resize-handle glyph
        "arrow.up.left.and.arrow.down.right",
    };

    int ok = 0;
    for (const QString& name : names) {
        QImage img(kSize, kSize, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        QPen pen(Qt::white);
        pen.setWidthF(kPen);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        drawGlyph(p, name);
        p.end();

        const QString path = outDir + QStringLiteral("/") + name + QStringLiteral(".png");
        if (img.save(path, "PNG")) {
            std::printf("  wrote %s\n", path.toLocal8Bit().constData());
            ++ok;
        } else {
            std::fprintf(stderr, "  FAILED to write %s\n",
                         path.toLocal8Bit().constData());
        }
    }

    std::printf("gen_icons: wrote %d / %d icons into %s\n",
                ok, int(names.size()), outDir.toLocal8Bit().constData());
    return ok == names.size() ? 0 : 1;
}
