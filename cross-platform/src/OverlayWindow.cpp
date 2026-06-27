// OverlayWindow.cpp — THE HEART. Faithful Qt6 port of OverlayView.swift
// (+ OverlayWindow window config from OverlayController.swift).
//
// Coordinate system: Qt is top-left / +Y-down, matching the flipped AppKit
// view. The two Swift "un-flip" blocks (drawImageUpright, per-line CTLine) are
// NOT ported — QPainter::drawImage and drawText are already upright.
//
// Annotation.rotation is RADIANS; QPainter::rotate takes DEGREES -> * 180/M_PI.

#include "OverlayWindow.h"

#include "Toolbar.h"
#include "Settings.h"
#include "Localization.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QFont>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QPushButton>
#include <QPixmap>
#include <QIcon>
#include <QPen>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QStringList>
#include <QTimer>

#include <cmath>
#include <algorithm>

// ============================================================================
// Construction / lifecycle
// ============================================================================

OverlayWindow::OverlayWindow(const QImage& screenshot, QScreen* screen, QWidget* parent)
    : QWidget(parent)
    , m_screenshot(screenshot)
    , m_screen(screen)
    , m_color(QColor(Qt::red))   // NSColor.systemRed default
{
    // Frameless, translucent, always-on-top overlay (DESIGN.md §2).
    // macOS: it MUST be a real Window, not Qt::Tool. A Tool maps to an NSPanel
    // that an accessory (LSUIElement) app cannot make the key window, so the
    // shield-level overlay would receive NO keyboard (Esc dead) and no reliable
    // mouse — the dimmed screen gets trapped until reboot. A frameless Qt::Window
    // can become key (made key by OverlayWindow_applyShieldLevel). Elsewhere keep
    // Qt::Tool so the transient overlay stays out of the taskbar.
#if defined(Q_OS_MACOS)
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Window);
#else
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
#endif
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    // PERF: skip Qt's automatic per-frame background erase. Our paintEvent ALWAYS
    // blits the opaque cached screenshot backdrop across the entire damaged
    // region as its first op, so every damaged pixel is overwritten with an opaque
    // value — the erase-to-transparent would be immediately clobbered and is pure
    // wasted work (a full-screen clear on every drag frame). WA_OpaquePaintEvent
    // tells Qt the handler covers all its pixels, which is exactly true here.
    //
    // This coexists with WA_TranslucentBackground: that flag controls the NSWindow
    // surface compositing (needed for the shield), while WA_OpaquePaintEvent only
    // governs the pre-paint erase. Coverage is guaranteed by the backdrop blit, so
    // no transparency/black leaks through. If the integrator's macOS build ever
    // shows a black flash or stale-pixel artifact at the overlay edges, removing
    // THIS ONE LINE restores the previous (correct but slower) erase-every-frame
    // behavior without touching anything else.
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    // Default active color matches systemRed (palette index 0).
    m_color = Palette::colors().isEmpty() ? QColor(Qt::red) : Palette::colors().at(0);

    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);   // crosshair until a selection exists

    if (m_screen)
        setGeometry(m_screen->geometry());

    // macOS: kill the default window-appear animation BEFORE the overlay is ever
    // shown, so the screen does NOT visibly "zoom out and back" on capture. We do
    // this here (in the constructor, well before show()) rather than in
    // applyShieldLevel() — that runs after show(), too late to suppress the appear
    // animation. winId() forces native NSWindow creation so the hook can reach it.
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    extern void OverlayWindow_disableShowAnimation(WId win);  // implemented in MacNative.mm
    OverlayWindow_disableShowAnimation(winId());
#endif
}

OverlayWindow::~OverlayWindow() = default;

// Apply native shield level after show(): macOS-only (CGShieldingWindowLevel via
// NSWindow). No-op here; the faithful implementation lives in a platform .mm file
// and is linked only when HAVE_MAC_NATIVE is defined. The pure-Qt overlay (a
// topmost Qt::Tool window) is fully functional without it, so the default build
// keeps this a no-op on every platform including macOS.
void OverlayWindow::applyShieldLevel() {
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    extern void OverlayWindow_applyShieldLevel(WId win);  // implemented in OverlayWindow_mac.mm
    OverlayWindow_applyShieldLevel(winId());
#endif
    // No-op otherwise (off macOS, or macOS without HAVE_MAC_NATIVE).
}

// Reset this monitor to a clean dimmed state — called on OTHER monitors when a
// new selection begins on one of them. (Spec 3 §12.6)
void OverlayWindow::clearSelectionState() {
    if (m_textEditor) { m_textEditor->removeEventFilter(this); m_textEditor->deleteLater(); m_textEditor = nullptr; }
    m_editingIndex = -1;
    if (m_editControls) { m_editControls->deleteLater(); m_editControls = nullptr; }
    if (m_alignControls) { m_alignControls->deleteLater(); m_alignControls = nullptr; }
    m_alignButtons.clear();
    if (m_textInspector) { m_textInspector->deleteLater(); m_textInspector = nullptr; }
    m_selection.reset();
    m_annotations.clear();
    m_current.reset();
    m_activeTextIndex.reset();
    m_dragMode = DragMode::None;
    hideToolbar();
    setCursor(Qt::CrossCursor);
    update();
}

// ============================================================================
// Scale (Spec 3 §1)
// ============================================================================

qreal OverlayWindow::scale() const {
    // screenshot px per view point (~ device pixel ratio).
    const qreal w = static_cast<qreal>(width());
    if (w <= 0.0) return 1.0;
    return static_cast<qreal>(m_screenshot.width()) / w;
}

qreal OverlayWindow::outputScale() const {
    return Settings::instance().downscaleRetina() ? 1.0 : scale();
}

// ============================================================================
// Geometry helpers (Spec 3 §6)
// ============================================================================

QRectF OverlayWindow::rectFrom(const QPointF& a, const QPointF& b) {
    return QRectF(std::min(a.x(), b.x()), std::min(a.y(), b.y()),
                  std::abs(a.x() - b.x()), std::abs(a.y() - b.y()));
}

QVector<QPair<OverlayWindow::Handle, QRectF>>
OverlayWindow::handleRects(const QRectF& sel) const {
    const qreal s = kHandleSize;
    auto r = [s](qreal cx, qreal cy) {
        return QRectF(cx - s / 2.0, cy - s / 2.0, s, s);
    };
    // Order mirrors the Swift dictionary's enumerated draw set (order is
    // visually irrelevant for filled white squares).
    QVector<QPair<Handle, QRectF>> v;
    v.append({Handle::TL, r(sel.left(),    sel.top())});
    v.append({Handle::T,  r(sel.center().x(), sel.top())});
    v.append({Handle::TR, r(sel.right(),   sel.top())});
    v.append({Handle::R,  r(sel.right(),   sel.center().y())});
    v.append({Handle::BR, r(sel.right(),   sel.bottom())});
    v.append({Handle::B,  r(sel.center().x(), sel.bottom())});
    v.append({Handle::BL, r(sel.left(),    sel.bottom())});
    v.append({Handle::L,  r(sel.left(),    sel.center().y())});
    return v;
}

std::optional<OverlayWindow::Handle>
OverlayWindow::handleHit(const QPointF& p, const QRectF& sel) const {
    for (const auto& pr : handleRects(sel)) {
        QRectF hit = pr.second.adjusted(-4, -4, 4, 4);   // insetBy(dx:-4,dy:-4)
        if (hit.contains(p)) return pr.first;
    }
    return std::nullopt;
}

QRectF OverlayWindow::resized(const QRectF& start, Handle h, const QPointF& p) const {
    qreal minX = start.left(), minY = start.top();
    qreal maxX = start.right(), maxY = start.bottom();
    switch (h) {
    case Handle::TL: minX = p.x(); minY = p.y(); break;
    case Handle::T:  minY = p.y(); break;
    case Handle::TR: maxX = p.x(); minY = p.y(); break;
    case Handle::R:  maxX = p.x(); break;
    case Handle::BR: maxX = p.x(); maxY = p.y(); break;
    case Handle::B:  maxY = p.y(); break;
    case Handle::BL: minX = p.x(); maxY = p.y(); break;
    case Handle::L:  minX = p.x(); break;
    }
    return rectFrom(QPointF(minX, minY), QPointF(maxX, maxY));
}

// ============================================================================
// Text geometry helpers (Spec 3 §4.4) — all in local (unrotated) coords.
// ============================================================================

QSizeF OverlayWindow::textSize(const Annotation& a) const {
    QFont font;
    font.setPointSizeF(a.fontSize);
    // Placeholder string used only for measuring an empty annotation (matches the
    // Swift hardcoded "Текст" literal — same glyph width for the min-size clamp).
    const QString s = a.text.isEmpty() ? QString::fromUtf8("Текст") : a.text;
    // Multi-line bounding rect, word-wrapped at width 1000 (matches Swift).
    QFontMetricsF fm(font);
    QRectF r = fm.boundingRect(QRectF(0, 0, 1000, 1e7),
                               Qt::AlignLeft | Qt::TextWordWrap, s);
    const qreal w = std::max(std::ceil(r.width()), qreal(10));
    // Height = line count x the render pitch (font height x 1.15, see
    // drawTextAnnotation / applyAlignmentToEditor) so the selection box and
    // handles hug the tightened multi-line text. The render is '\n'-split
    // (NoWrap), so count newlines rather than using the word-wrapped height.
    const int lineCount = static_cast<int>(s.count('\n')) + 1;
    const qreal h = std::max(std::ceil(lineCount * fm.height() * 1.15),
                             qreal(font.pointSizeF()));
    return QSizeF(w, h);
}

QRectF OverlayWindow::textLocalRect(const Annotation& a) const {
    return QRectF(a.start, textSize(a));
}

QPointF OverlayWindow::textCenter(const Annotation& a) const {
    const QSizeF s = textSize(a);
    return QPointF(a.start.x() + s.width() / 2.0, a.start.y() + s.height() / 2.0);
}

QRectF OverlayWindow::resizeHandleLocal(const Annotation& a) const {
    QRectF r = textLocalRect(a).adjusted(-3, -3, 3, 3);   // insetBy(dx:-3,dy:-3)
    return QRectF(r.right() - 9, r.bottom() - 9, 18, 18); // bottom-right
}

QRectF OverlayWindow::rotateHandleLocal(const Annotation& a) const {
    QRectF r = textLocalRect(a).adjusted(-3, -3, 3, 3);
    return QRectF(r.right() - 9, r.top() - 9, 18, 18);    // top-right
}

QPointF OverlayWindow::localPoint(const QPointF& p, const Annotation& a) const {
    const QPointF c = textCenter(a);
    const qreal dx = p.x() - c.x(), dy = p.y() - c.y();
    const qreal ca = std::cos(-a.rotation), sa = std::sin(-a.rotation);
    return QPointF(c.x() + dx * ca - dy * sa, c.y() + dx * sa + dy * ca);
}

QPointF OverlayWindow::screenPoint(const QPointF& lp, const Annotation& a) const {
    const QPointF c = textCenter(a);
    const qreal dx = lp.x() - c.x(), dy = lp.y() - c.y();
    const qreal ca = std::cos(a.rotation), sa = std::sin(a.rotation);
    return QPointF(c.x() + dx * ca - dy * sa, c.y() + dx * sa + dy * ca);
}

QRectF OverlayWindow::screenRectAround(const QRectF& localRect, const Annotation& a) const {
    const QPointF sc = screenPoint(QPointF(localRect.center().x(), localRect.center().y()), a);
    return QRectF(sc.x() - 11, sc.y() - 11, 22, 22);
}

std::optional<int> OverlayWindow::textAnnotationIndex(const QPointF& p) const {
    for (int i = m_annotations.size() - 1; i >= 0; --i) {
        if (m_annotations[i].tool != Tool::Text) continue;
        const Annotation& a = m_annotations[i];
        if (textLocalRect(a).adjusted(-3, -3, 3, 3).contains(localPoint(p, a)))
            return i;
    }
    return std::nullopt;
}

// ============================================================================
// Rendering (Spec 3 §4) — strict paint order
// ============================================================================

void OverlayWindow::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Respect the damaged region. During a drag the dirty rect is a small band
    // around the selection/annotation, so clip every draw op to it and — crucially
    // — blit only the matching sub-rectangle of the cached Retina backdrop instead
    // of the whole screenshot. This is what makes the per-move repaint cost scale
    // with the gesture size, not the screen size. On a full repaint (press /
    // release / tool change) the damage is the whole widget, so nothing shrinks.
    const QRect damage = event->rect();
    p.setClipRect(damage);

    // Blit the cached upright backdrop instead of re-rendering the full QImage
    // (with its orientation transform) every frame — pixmap blits are GPU-fast.
    // Map the logical damage rect into backdrop device pixels (the pixmap carries
    // the dpr) and blit just that sub-rect to its matching destination.
    ensureBackdrop();
    if (!m_backdrop.isNull()) {
        const qreal dpr = m_backdrop.devicePixelRatio();
        QRectF srcPx(damage.x() * dpr, damage.y() * dpr,
                     damage.width() * dpr, damage.height() * dpr);
        p.drawPixmap(QRectF(damage), m_backdrop, srcPx);
    }

    // drawDim already builds an even-odd path covering rect() with the selection
    // hole; the clip rect restricts the fill to the damaged band, so re-dimming a
    // small vacated area is cheap (no full-screen alpha fill).
    drawDim(p);
    for (int i = 0; i < m_annotations.size(); ++i) {
        if (i == m_editingIndex) continue;   // hide the one being edited inline
        drawAnnotation(p, m_annotations[i]);
    }
    if (m_current) drawAnnotation(p, *m_current);
    drawSelectionChrome(p);
    drawActiveTextChrome(p);
    drawSnapGuides(p);
}

void OverlayWindow::ensureBackdrop() {
    // (Re)build the cached upright screenshot pixmap when it is missing or no
    // longer matches the widget's logical size. The cache carries the device
    // pixel ratio so the backdrop stays sharp on Retina displays.
    const qreal dpr = devicePixelRatioF();
    const QSize logical = size();
    const QSize wantPx(static_cast<int>(std::ceil(logical.width()  * dpr)),
                       static_cast<int>(std::ceil(logical.height() * dpr)));
    if (!m_backdrop.isNull() &&
        m_backdrop.size() == wantPx &&
        qFuzzyCompare(m_backdrop.devicePixelRatio(), dpr)) {
        return;
    }
    if (logical.isEmpty()) return;

    QPixmap pm(wantPx);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter pp(&pm);
    pp.setRenderHint(QPainter::SmoothPixmapTransform, true);
    drawImageUpright(pp, m_screenshot, QRectF(QPointF(0, 0), QSizeF(logical)));
    pp.end();
    m_backdrop = pm;
}

// ----- Dirty-rect drag optimization ----------------------------------------
//
// currentDragBounds() returns the logical-pixel bounding rect of EVERYTHING the
// active gesture currently paints (geometry + its chrome), padded so handles,
// borders, the size label and the dashed text frame are all covered. The caller
// (updateDragRegion) unions this with the previous frame's bounds so the band
// the gesture vacated is re-dimmed too. Returns a null QRect when no rect/text
// gesture is in flight (caller then does a full update()).
QRect OverlayWindow::currentDragBounds() const {
    switch (m_dragMode) {
    case DragMode::NewSelection:
    case DragMode::MoveSelection:
    case DragMode::Resize: {
        if (!m_selection) return QRect();
        QRectF r = *m_selection;
        // Pad for the 1px border + the 9px handles centered on the edges (~5px
        // overhang). The "W × H" size label sits ~22px above the top-left and is
        // left-anchored, so for a narrow selection it extends well past the right
        // edge — reserve a worst-case label band on the top and right. When the
        // label has no room above it is drawn just BELOW the top edge instead
        // (drawSelectionChrome), so reserve the band on the top INSIDE too via the
        // top pad covering both placements. All cheap vs. a full-screen repaint.
        const qreal padSide  = kHandleSize;     // handle overhang + border
        const qreal padTop   = 28;              // size-label band above the top
        const qreal labelMax = 160;             // worst-case "9999 × 9999" + bg pad
        r = r.adjusted(-padSide, -padTop,
                       std::max(padSide, labelMax - r.width()), padSide);
        return r.toAlignedRect();
    }
    case DragMode::Draw: {
        if (!m_current) return QRect();
        const Annotation& a = *m_current;
        QRectF r;
        if (a.tool == Tool::Pen && !a.points.isEmpty()) {
            r = QRectF(a.points.first(), a.points.first());
            for (const QPointF& pt : a.points) {
                r.setLeft(std::min(r.left(), pt.x()));
                r.setTop(std::min(r.top(), pt.y()));
                r.setRight(std::max(r.right(), pt.x()));
                r.setBottom(std::max(r.bottom(), pt.y()));
            }
        } else {
            r = rectFrom(a.start, a.end);
        }
        // Pad for stroke half-width and (for arrows) the arrowhead wings, which
        // overshoot the end point by up to max(12, width*4).
        const qreal head = std::max<qreal>(12, a.lineWidth * 4);
        const qreal pad = std::max(a.lineWidth, head) + 2;
        return r.adjusted(-pad, -pad, pad, pad).toAlignedRect();
    }
    case DragMode::MoveText:
    case DragMode::ResizeText:
    case DragMode::RotateText: {
        if (!m_activeTextIndex || *m_activeTextIndex >= m_annotations.size())
            return QRect();
        const Annotation& a = m_annotations[*m_activeTextIndex];
        // Local (unrotated) rect padded for the dashed frame (3px) + the 18px
        // resize/rotate handle circles that hang off the top-right/bottom-right.
        const QRectF local = textLocalRect(a).adjusted(-12, -12, 12, 12);
        // Transform all four corners through the rotation so a rotated text's
        // axis-aligned screen bounds are captured.
        const QPointF c[4] = {
            screenPoint(local.topLeft(),     a),
            screenPoint(local.topRight(),    a),
            screenPoint(local.bottomRight(), a),
            screenPoint(local.bottomLeft(),  a),
        };
        qreal minX = c[0].x(), minY = c[0].y(), maxX = c[0].x(), maxY = c[0].y();
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, c[i].x()); maxX = std::max(maxX, c[i].x());
            minY = std::min(minY, c[i].y()); maxY = std::max(maxY, c[i].y());
        }
        QRect r = QRectF(minX, minY, maxX - minX, maxY - minY).toAlignedRect();
        // Snap guides (drawn while moving text) span the WHOLE selection rect, so
        // when one is active include the selection so the guide line is painted /
        // erased in the same damaged band.
        if ((m_snapGuideV || m_snapGuideH) && m_selection)
            r = r.united(m_selection->toAlignedRect().adjusted(-1, -1, 1, 1));
        return r;
    }
    default:
        return QRect();
    }
}

void OverlayWindow::updateDragRegion() {
    const QRect now = currentDragBounds();
    if (now.isNull() || !now.isValid()) {
        // No tight bounds available for this gesture -> repaint everything.
        m_lastPaintedDirty = QRect();
        update();
        return;
    }
    // Damage the union of where the gesture was last frame and where it is now,
    // so the band it just vacated gets re-dimmed. Both rects already carry their
    // chrome padding. Intersect with the widget so we never enqueue off-screen
    // damage. +1/+1 guards against fractional-edge antialiasing seams.
    QRect dirty = now;
    if (!m_lastPaintedDirty.isNull())
        dirty = dirty.united(m_lastPaintedDirty);
    dirty.adjust(-1, -1, 1, 1);
    dirty &= rect();
    m_lastPaintedDirty = now;
    if (!dirty.isEmpty())
        update(dirty);
}

void OverlayWindow::drawImageUpright(QPainter& p, const QImage& img, const QRectF& rect) {
    // Qt draws images upright already — NO Y-flip. Scale the full screenshot
    // into the view rect (logical points).
    p.drawImage(rect, img, QRectF(0, 0, img.width(), img.height()));
}

void OverlayWindow::drawDim(QPainter& p) {
    // Black @ dimOpacity over everything, with an even-odd hole over selection.
    // m_dimProgress (1.0 unless an animated fade is running) scales the alpha so
    // the dim layer can ease in/out when Settings::animatedDim() is on.
    p.save();
    QColor dim(0, 0, 0);
    dim.setAlphaF(static_cast<float>(Settings::instance().dimOpacity() * m_dimProgress));
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    path.addRect(QRectF(rect()));
    if (m_selection) path.addRect(*m_selection);
    p.fillPath(path, dim);
    p.restore();
}

void OverlayWindow::startDimFadeIn() {
    // 0 -> 1 over ~0.18s with an ease-out curve. Only ever called when animated
    // dimming is enabled; restarts cleanly if invoked again.
    if (m_dimAnim) { m_dimAnim->stop(); m_dimAnim->deleteLater(); m_dimAnim = nullptr; }
    m_dimProgress = 0.0;
    update();
    auto* anim = new QVariantAnimation(this);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setDuration(180);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_dimProgress = v.toReal();
        update();
    });
    connect(anim, &QVariantAnimation::finished, this, [this, anim]() {
        m_dimProgress = 1.0;
        if (m_dimAnim == anim) m_dimAnim = nullptr;
        anim->deleteLater();
        update();
    });
    m_dimAnim = anim;
    anim->start();
}

void OverlayWindow::startDimFadeOut() {
    // current -> 0 over ~0.16s. The controller defers the actual hide()/delete
    // until this finishes so the fade is visible; we only animate the alpha here.
    if (m_dimAnim) { m_dimAnim->stop(); m_dimAnim->deleteLater(); m_dimAnim = nullptr; }
    auto* anim = new QVariantAnimation(this);
    anim->setStartValue(m_dimProgress);
    anim->setEndValue(0.0);
    anim->setDuration(160);
    anim->setEasingCurve(QEasingCurve::InCubic);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_dimProgress = v.toReal();
        update();
    });
    connect(anim, &QVariantAnimation::finished, this, [this, anim]() {
        m_dimProgress = 0.0;
        if (m_dimAnim == anim) m_dimAnim = nullptr;
        anim->deleteLater();
        update();
    });
    m_dimAnim = anim;
    anim->start();
}

void OverlayWindow::drawAnnotation(QPainter& p, const Annotation& a) {
    p.save();
    QPen pen(a.color);
    pen.setWidthF(a.lineWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    switch (a.tool) {
    case Tool::Rectangle:
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(rectFrom(a.start, a.end));
        break;
    case Tool::FilledRect:
        p.setPen(Qt::NoPen);
        p.setBrush(a.color);
        p.drawRect(rectFrom(a.start, a.end));
        break;
    case Tool::Pen: {
        if (a.points.isEmpty()) break;
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath poly(a.points.first());
        for (int i = 1; i < a.points.size(); ++i) poly.lineTo(a.points[i]);
        p.drawPath(poly);
        break;
    }
    case Tool::Arrow:
        drawArrow(p, a.start, a.end, a.lineWidth, a.color);
        break;
    case Tool::Line:
        p.setPen(pen);
        p.drawLine(a.start, a.end);
        break;
    case Tool::Text:
        drawTextAnnotation(p, a);
        break;
    case Tool::Select:
        break;
    }
    p.restore();
}

void OverlayWindow::drawArrow(QPainter& p, const QPointF& from, const QPointF& to,
                              qreal width, const QColor& color) {
    QPen pen(color);
    pen.setWidthF(width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawLine(from, to);

    const qreal angle = std::atan2(to.y() - from.y(), to.x() - from.x());
    const qreal head = std::max<qreal>(12, width * 4);   // wing length
    const qreal spread = M_PI / 7.0;                     // opening angle
    const QPointF p1(to.x() - head * std::cos(angle - spread),
                     to.y() - head * std::sin(angle - spread));
    const QPointF p2(to.x() - head * std::cos(angle + spread),
                     to.y() - head * std::sin(angle + spread));
    QPainterPath head_path(to);
    head_path.lineTo(p1);
    head_path.lineTo(p2);
    head_path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.fillPath(head_path, color);
}

// Multi-line text with per-line alignment offset, optional background rect,
// rotation about center. NO Y-flip (Qt drawText is upright).
void OverlayWindow::drawTextAnnotation(QPainter& p, const Annotation& a) {
    if (a.text.isEmpty()) return;
    QFont font;
    font.setPointSizeF(a.fontSize);
    QFontMetricsF fm(font);

    const QStringList lines = a.text.split('\n');
    // Compact, chat-like line pitch — matches the inline editor's FixedHeight
    // (applyAlignmentToEditor) so committed text keeps the same spacing it had
    // while typing. font height x 1.15 (small gap, no leading bloat).
    const qreal lineHeight = fm.height() * 1.15;
    const QPointF c = textCenter(a);

    p.save();
    // Rotate about center: translate to center, rotate (radians -> degrees), back.
    p.translate(c);
    p.rotate(a.rotation * 180.0 / M_PI);
    p.translate(-c);

    if (a.bgColor) {
        p.fillRect(textLocalRect(a).adjusted(-3, -2, 3, 2), *a.bgColor);
    }

    // Lay every line out inside the SAME box width so left/center/right are
    // visibly different. The box width is the widest line's advance (textSize),
    // clamped to be >= each line so no line is ever clipped. We align each line
    // explicitly via QTextOption + a per-line rect spanning the full box width;
    // this is the canonical path (the previous manual dx math silently produced
    // no visible offset whenever boxW happened to equal the measured line width).
    qreal boxW = textSize(a).width();
    for (const QString& str : lines)
        boxW = std::max(boxW, fm.horizontalAdvance(str));

    const Qt::Alignment hAlign = toQtAlignment(a.alignment);
    QTextOption opt(hAlign | Qt::AlignTop);
    opt.setWrapMode(QTextOption::NoWrap);

    p.setFont(font);
    p.setPen(a.color);
    for (int i = 0; i < lines.size(); ++i) {
        // Each line occupies a full-box-width rect; QPainter aligns the glyphs
        // within it according to hAlign (left/center/right).
        const qreal top = a.start.y() + i * lineHeight;
        const QRectF lineRect(a.start.x(), top, boxW, lineHeight);
        p.drawText(lineRect, lines[i], opt);
    }
    p.restore();
}

// White 1px border + 8 handles (white fill / blue 1px border) + WxH size label.
void OverlayWindow::drawSelectionChrome(QPainter& p) {
    if (!m_selection) return;
    const QRectF sel = *m_selection;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen white(Qt::white);
    white.setWidthF(1);
    p.setPen(white);
    p.setBrush(Qt::NoBrush);
    p.drawRect(sel);

    for (const auto& pr : handleRects(sel)) {
        const QRectF& r = pr.second;
        p.fillRect(r, Qt::white);
        QPen blue(QColor("#007AFF"));   // systemBlue
        blue.setWidthF(1);
        p.setPen(blue);
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
    }
    p.restore();

    // Size label: pixels at output scale.
    const int wPx = static_cast<int>(sel.width() * outputScale());
    const int hPx = static_cast<int>(sel.height() * outputScale());
    const QString label = QString("%1 × %2").arg(wPx).arg(hPx);  // "W × H"

    QFont font;
    font.setPointSizeF(12);
    font.setWeight(QFont::Medium);
    QFontMetricsF fm(font);
    const QSizeF size(fm.horizontalAdvance(label), fm.height());

    QPointF origin(sel.left(), sel.top() - size.height() - 6);
    if (origin.y() < 2) origin.setY(sel.top() + 4);   // keep below top edge
    const QRectF bg(origin.x() - 4, origin.y() - 2, size.width() + 8, size.height() + 4);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QColor bgc(0, 0, 0);
    bgc.setAlphaF(0.6f);
    p.fillRect(bg, bgc);
    p.setFont(font);
    p.setPen(Qt::white);
    // Swift draws text at top-left origin; QPainter::drawText(point) uses the
    // baseline, so offset by ascent.
    p.drawText(QPointF(origin.x(), origin.y() + fm.ascent()), label);
    p.restore();
}

// Dashed frame + blue resize handle (diagonal-arrows) + blue rotate handle
// (reload icon), rotated about center.
void OverlayWindow::drawActiveTextChrome(QPainter& p) {
    if (m_tool != Tool::Text || m_editingIndex != -1) return;
    if (!m_activeTextIndex) return;
    const int idx = *m_activeTextIndex;
    if (idx >= m_annotations.size() || m_annotations[idx].tool != Tool::Text) return;
    const Annotation& a = m_annotations[idx];
    const QPointF c = textCenter(a);

    p.save();
    p.translate(c);
    p.rotate(a.rotation * 180.0 / M_PI);
    p.translate(-c);

    // Dashed white@90% frame.
    QColor dashWhite(255, 255, 255);
    dashWhite.setAlphaF(0.9f);
    QPen dash(dashWhite);
    dash.setWidthF(1);
    dash.setStyle(Qt::CustomDashLine);
    dash.setDashPattern({4, 3});
    p.setPen(dash);
    p.setBrush(Qt::NoBrush);
    p.drawRect(textLocalRect(a).adjusted(-3, -3, 3, 3));

    const QColor blue("#007AFF");

    // Resize handle (bottom-right): blue circle + diagonal-arrows glyph.
    const QRectF rh = resizeHandleLocal(a);
    p.setPen(Qt::NoPen);
    p.setBrush(blue);
    p.drawEllipse(rh);
    {
        // "arrow.up.left.and.arrow.down.right" — two opposing diagonal arrows.
        QRectF g = rh.adjusted(3, 3, -3, -3);
        QPen wp(Qt::white);
        wp.setWidthF(1.6);
        wp.setCapStyle(Qt::RoundCap);
        wp.setJoinStyle(Qt::RoundJoin);
        p.setPen(wp);
        p.setBrush(Qt::NoBrush);
        const QPointF tl = g.topLeft();
        const QPointF br = g.bottomRight();
        p.drawLine(tl, br);
        const qreal aw = g.width() * 0.42;   // arrowhead wing length
        // top-left head
        p.drawLine(tl, tl + QPointF(aw, 0));
        p.drawLine(tl, tl + QPointF(0, aw));
        // bottom-right head
        p.drawLine(br, br - QPointF(aw, 0));
        p.drawLine(br, br - QPointF(0, aw));
    }

    // Rotate handle (top-right): blue circle + circular-reload glyph.
    const QRectF roth = rotateHandleLocal(a);
    p.setPen(Qt::NoPen);
    p.setBrush(blue);
    p.drawEllipse(roth);
    {
        // "arrow.triangle.2.circlepath" — a ~300° arc with an arrowhead.
        QRectF g = roth.adjusted(3, 3, -3, -3);
        QPen wp(Qt::white);
        wp.setWidthF(1.6);
        wp.setCapStyle(Qt::RoundCap);
        p.setPen(wp);
        p.setBrush(Qt::NoBrush);
        p.drawArc(g, 40 * 16, 280 * 16);   // arc spanning 280°, start 40°
        // Arrowhead at the arc end (~40°, Qt +CCW from 3 o'clock; y is down).
        const QPointF ctr = g.center();
        const qreal rad = g.width() / 2.0;
        const qreal a0 = -40.0 * M_PI / 180.0;   // Qt angle -> screen (y down)
        const QPointF tip(ctr.x() + rad * std::cos(a0), ctr.y() + rad * std::sin(a0));
        const qreal hw = g.width() * 0.30;
        p.drawLine(tip, tip + QPointF(-hw, -hw * 0.2));
        p.drawLine(tip, tip + QPointF(-hw * 0.2, hw));
    }

    p.restore();
}

// Pink dashed center guides when snapping text center to selection center.
void OverlayWindow::drawSnapGuides(QPainter& p) {
    if (!m_selection || (!m_snapGuideV && !m_snapGuideH)) return;
    const QRectF sel = *m_selection;
    p.save();
    QPen pink(QColor("#FF2D55"));   // systemPink
    pink.setWidthF(1);
    pink.setStyle(Qt::CustomDashLine);
    pink.setDashPattern({5, 4});
    p.setPen(pink);
    if (m_snapGuideV)
        p.drawLine(QPointF(sel.center().x(), sel.top()),
                   QPointF(sel.center().x(), sel.bottom()));
    if (m_snapGuideH)
        p.drawLine(QPointF(sel.left(), sel.center().y()),
                   QPointF(sel.right(), sel.center().y()));
    p.restore();
}

// ============================================================================
// Cursors (Spec 3 §5)
// ============================================================================

Qt::CursorShape OverlayWindow::resizeCursorFor(Handle h) const {
    switch (h) {
    case Handle::L: case Handle::R:   return Qt::SizeHorCursor;
    case Handle::T: case Handle::B:   return Qt::SizeVerCursor;
    case Handle::TL: case Handle::BR: return Qt::SizeFDiagCursor;
    case Handle::TR: case Handle::BL: return Qt::SizeBDiagCursor;
    }
    return Qt::SizeAllCursor;
}

void OverlayWindow::updateHoverCursor(const QPointF& p) {
    if (m_cursorRectsDisabled) return;   // suppressed during a custom-cursor drag

    if (!m_selection) {
        setCursor(Qt::CrossCursor);   // no selection yet -> crosshair everywhere
        return;
    }
    const QRectF sel = *m_selection;
    switch (m_tool) {
    case Tool::Select:
        // Over a handle -> resize arrow; over body -> open hand.
        if (auto h = handleHit(p, sel)) { setCursor(resizeCursorFor(*h)); return; }
        if (sel.contains(p)) { setCursor(Qt::OpenHandCursor); return; }
        setCursor(Qt::CrossCursor);
        break;
    case Tool::Text:
        if (m_editingIndex == -1 && m_activeTextIndex) {
            const int idx = *m_activeTextIndex;
            if (idx < m_annotations.size() && m_annotations[idx].tool == Tool::Text) {
                const Annotation& a = m_annotations[idx];
                if (screenRectAround(resizeHandleLocal(a), a).contains(p)) {
                    setCursor(Qt::CrossCursor); return;
                }
                if (screenRectAround(rotateHandleLocal(a), a).contains(p)) {
                    setCursor(Qt::OpenHandCursor); return;
                }
            }
        }
        if (sel.contains(p)) { setCursor(Qt::IBeamCursor); return; }
        setCursor(Qt::CrossCursor);
        break;
    default:
        setCursor(Qt::CrossCursor);
        break;
    }
}

void OverlayWindow::beginCustomCursorDrag() {
    m_cursorRectsDisabled = true;
}
void OverlayWindow::endCustomCursorDrag() {
    m_cursorRectsDisabled = false;
}

// Clamp QCursor::pos to this screen during selection so the cursor cannot fly
// to an adjacent monitor (Qt top-left virtual-desktop coords).
void OverlayWindow::confineCursorToScreen() {
    if (!m_screen) return;
    const QRect b = m_screen->geometry();
    QPoint cur = QCursor::pos();
    QPoint p = cur;
    p.setX(std::min(std::max(b.left() + 1, p.x()), b.right() - 2));
    p.setY(std::min(std::max(b.top() + 1, p.y()), b.bottom() - 2));
    if (p != cur) QCursor::setPos(p);
}

// ============================================================================
// Mouse (Spec 3 §2.4) — mousePress
// ============================================================================

void OverlayWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    const QPointF p = e->position();
    m_dragStart = p;
    // Start a fresh dirty-rect cache for this gesture. Where a drag begins from
    // existing geometry (resize / move selection / move existing text) we seed it
    // below with the pre-drag bounds so the FIRST move erases the old position.
    m_lastPaintedDirty = QRect();

    // A press that REACHES the overlay while editing text commits that text and is
    // CONSUMED — we do NOT also start a new text / selection in the same press.
    // That double-action (commit + immediately spawn a new text) is exactly what
    // looked like "clicking the align option closes the text and creates a new
    // one": the align / ✓ / ✗ buttons are child widgets, so a real click on them
    // never reaches here — only a genuine "click elsewhere" does, and that should
    // simply finish the current text.
    if (m_textEditor) { commitTextEditing(); update(); return; }

    if (m_selection) {
        const QRectF sel = *m_selection;
        if (m_tool == Tool::Select) {
            if (auto h = handleHit(p, sel)) {
                m_dragMode = DragMode::Resize;
                m_dragHandle = *h;
                m_selectionAtDragStart = sel;
                setCursor(resizeCursorFor(*h));
                beginCustomCursorDrag();
                m_lastPaintedDirty = currentDragBounds();   // seed: erase old pos on 1st move
                return;
            }
            if (sel.contains(p)) {
                m_dragMode = DragMode::MoveSelection;
                m_selectionAtDragStart = sel;
                setCursor(Qt::ClosedHandCursor);   // grabbed the selection
                beginCustomCursorDrag();
                m_lastPaintedDirty = currentDragBounds();   // seed: erase old pos on 1st move
                return;
            }
            // Click outside -> begin a new selection.
            m_selection.reset();
            hideToolbar();
            m_dragMode = DragMode::NewSelection;
            emit beganSelection();
        } else if (m_tool == Tool::Text) {
            // ----- text mouse-down (mirrors handleTextMouseDown) -----
            bool handled = false;
            if (m_activeTextIndex) {
                const int idx = *m_activeTextIndex;
                if (idx < m_annotations.size() && m_annotations[idx].tool == Tool::Text) {
                    const Annotation& a = m_annotations[idx];
                    const QPointF lp = localPoint(p, a);
                    if (rotateHandleLocal(a).adjusted(-4, -4, 4, 4).contains(lp)) {
                        m_dragMode = DragMode::RotateText;
                        const QPointF c = textCenter(a);
                        m_textRotateStartAngle = a.rotation;
                        m_textRotateStartPointerAngle = std::atan2(p.y() - c.y(), p.x() - c.x());
                        setCursor(Qt::ClosedHandCursor);
                        beginCustomCursorDrag();
                        handled = true;
                    } else if (resizeHandleLocal(a).adjusted(-4, -4, 4, 4).contains(lp)) {
                        m_dragMode = DragMode::ResizeText;
                        m_textResizeStartSize = a.fontSize;
                        m_textResizeStartPoint = p;
                        setCursor(Qt::CrossCursor);
                        beginCustomCursorDrag();
                        handled = true;
                    } else if (textLocalRect(a).adjusted(-3, -3, 3, 3).contains(lp)) {
                        m_dragMode = DragMode::MoveText;   // body -> move
                        m_textMoveStartOrigin = a.start;
                        m_dragStart = p;
                        setCursor(Qt::ClosedHandCursor);
                        beginCustomCursorDrag();
                        handled = true;
                    }
                }
            }
            if (!handled) {
                if (auto idx = textAnnotationIndex(p)) {
                    // Click on another text -> select + prepare move.
                    m_activeTextIndex = *idx;
                    m_dragMode = DragMode::MoveText;
                    m_textMoveStartOrigin = m_annotations[*idx].start;
                    m_dragStart = p;
                    setCursor(Qt::ClosedHandCursor);
                    beginCustomCursorDrag();
                    updateTextInspector();
                    m_lastPaintedDirty = currentDragBounds();   // seed for 1st move
                    update();
                    return;
                }
                // Empty spot -> new text.
                if (sel.contains(p)) {
                    m_activeTextIndex.reset();
                    beginTextEditingAt(p);
                }
            }
        } else {
            // Drawing tool active -> draw only inside the selection.
            if (sel.contains(p)) {
                Annotation a;
                a.tool = m_tool;
                a.color = m_color;
                a.lineWidth = m_lineWidth;
                a.start = p;
                a.end = p;
                a.points = {p};
                m_current = a;
                m_dragMode = DragMode::Draw;
            }
        }
    } else {
        m_dragMode = DragMode::NewSelection;
        emit beganSelection();
    }
    // Seed the dirty-rect cache with the gesture's pre-move bounds (null for a
    // brand-new selection that has no rect yet) so the first mouseMove erases the
    // gesture's prior on-screen position. The full update() below then paints the
    // whole widget once; subsequent moves shrink to the dirty band.
    m_lastPaintedDirty = currentDragBounds();
    update();
}

// ============================================================================
// Mouse — mouseMove (drag)
// ============================================================================

void OverlayWindow::mouseMoveEvent(QMouseEvent* e) {
    const QPointF p = e->position();

    // Hover cursor when not dragging.
    if (m_dragMode == DragMode::None) {
        updateHoverCursor(p);
        QWidget::mouseMoveEvent(e);
        return;
    }

    switch (m_dragMode) {
    case DragMode::NewSelection:
        m_selection = rectFrom(m_dragStart, p);
        break;
    case DragMode::MoveSelection: {
        setCursor(Qt::ClosedHandCursor);
        const qreal dx = p.x() - m_dragStart.x(), dy = p.y() - m_dragStart.y();
        QRectF moved = m_selectionAtDragStart.translated(dx, dy);
        qreal ox = std::min(std::max<qreal>(0, moved.left()), width() - moved.width());
        qreal oy = std::min(std::max<qreal>(0, moved.top()), height() - moved.height());
        moved.moveTo(ox, oy);
        m_selection = moved;
        break;
    }
    case DragMode::Resize:
        setCursor(resizeCursorFor(m_dragHandle));
        m_selection = resized(m_selectionAtDragStart, m_dragHandle, p);
        break;
    case DragMode::Draw:
        if (m_current) {
            m_current->end = p;
            m_current->points.append(p);
        }
        break;
    case DragMode::ResizeText:
        if (m_activeTextIndex && *m_activeTextIndex < m_annotations.size()) {
            setCursor(Qt::CrossCursor);
            const qreal dy = p.y() - m_textResizeStartPoint.y();   // drag down = larger
            m_annotations[*m_activeTextIndex].fontSize = std::max<qreal>(8, m_textResizeStartSize + dy);
            updateTextInspector();
        }
        break;
    case DragMode::MoveText:
        if (m_activeTextIndex && *m_activeTextIndex < m_annotations.size()) {
            const int idx = *m_activeTextIndex;
            setCursor(Qt::ClosedHandCursor);
            const qreal dx = p.x() - m_dragStart.x(), dy = p.y() - m_dragStart.y();
            QPointF newStart(m_textMoveStartOrigin.x() + dx, m_textMoveStartOrigin.y() + dy);
            // Snap text center to selection center (threshold 6).
            m_snapGuideV = false; m_snapGuideH = false;
            if (m_selection) {
                const QRectF sel = *m_selection;
                const QSizeF size = textSize(m_annotations[idx]);
                const qreal threshold = 6;
                if (std::abs(newStart.x() + size.width() / 2.0 - sel.center().x()) < threshold) {
                    newStart.setX(sel.center().x() - size.width() / 2.0);
                    m_snapGuideV = true;
                }
                if (std::abs(newStart.y() + size.height() / 2.0 - sel.center().y()) < threshold) {
                    newStart.setY(sel.center().y() - size.height() / 2.0);
                    m_snapGuideH = true;
                }
            }
            m_annotations[idx].start = newStart;
            updateTextInspector();
        }
        break;
    case DragMode::RotateText:
        if (m_activeTextIndex && *m_activeTextIndex < m_annotations.size()) {
            const int idx = *m_activeTextIndex;
            const QPointF c = textCenter(m_annotations[idx]);
            const qreal ang = std::atan2(p.y() - c.y(), p.x() - c.x());
            m_annotations[idx].rotation = m_textRotateStartAngle + (ang - m_textRotateStartPointerAngle);
        }
        break;
    case DragMode::None:
        break;
    }

    // Confine cursor during selection-area operations.
    if (m_dragMode == DragMode::NewSelection ||
        m_dragMode == DragMode::MoveSelection ||
        m_dragMode == DragMode::Resize) {
        confineCursorToScreen();
    }

    // Toolbar follows the selection while it moves/resizes. The toolbar is a
    // child widget, so moving it schedules its OWN repaint independently of the
    // overlay's damaged region — we do not need to include it in the dirty rect.
    if (m_toolbar && m_selection) positionToolbar(*m_selection);

    // Per-move repaint: damage only the union of the gesture's previous and
    // current bounds instead of the whole Retina screen. Falls back to a full
    // update() when the gesture has no tight bounds (handled inside).
    updateDragRegion();
}

// ============================================================================
// Mouse — mouseRelease
// ============================================================================

void OverlayWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    const QPointF up = e->position();

    switch (m_dragMode) {
    case DragMode::Draw:
        if (m_current) { m_annotations.append(*m_current); m_redoStack.clear(); }
        m_current.reset();
        break;
    case DragMode::MoveText:
        // Click without drag (< 4px) -> edit the text.
        if (std::hypot(up.x() - m_dragStart.x(), up.y() - m_dragStart.y()) < 4 && m_activeTextIndex) {
            beginTextEditingIndex(*m_activeTextIndex);
        }
        break;
    default:
        break;
    }
    m_dragMode = DragMode::None;
    m_snapGuideV = false; m_snapGuideH = false;
    m_lastPaintedDirty = QRect();   // gesture over: drop the dirty-rect cache

    if (m_selection && m_selection->width() > 4 && m_selection->height() > 4) {
        showToolbar(*m_selection);
    } else {
        m_selection.reset();
        hideToolbar();
    }
    endCustomCursorDrag();

    // After release, restore the open hand if hovering over the selection.
    if (m_tool == Tool::Select && m_selection && m_selection->contains(up)) {
        setCursor(Qt::OpenHandCursor);
    } else {
        updateHoverCursor(up);
    }
    updateTextInspector();
    update();
}

// ============================================================================
// Keyboard (Spec 3) — matched by Qt key constants (layout-independent)
// ============================================================================

void OverlayWindow::keyPressEvent(QKeyEvent* e) {
    // While editing text, all keys go to the editor (handled in eventFilter).
    if (m_textEditor) { QWidget::keyPressEvent(e); return; }

    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);   // Cmd on macOS
    const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);

    switch (e->key()) {
    case Qt::Key_Escape:
        finish();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        copyToClipboard();
        return;
    case Qt::Key_C:
    case Qt::Key_X:
        if (ctrl) { copyToClipboard(); return; }
        break;
    case Qt::Key_S:
        if (ctrl) { saveToFile(); return; }
        break;
    case Qt::Key_Z:
        if (ctrl) {
            if (shift) redoLast(); else undoLast();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(e);
}

// ============================================================================
// Toolbar (Spec 3 §9)
// ============================================================================

void OverlayWindow::showToolbar(const QRectF& sel) {
    const bool isNew = (m_toolbar == nullptr);
    if (isNew) {
        m_toolbar = new ToolbarView(this);
        connect(m_toolbar, &ToolbarView::selectToolRequested, this, &OverlayWindow::onSelectTool);
        connect(m_toolbar, &ToolbarView::selectColorRequested, this, &OverlayWindow::onSelectColor);
        connect(m_toolbar, &ToolbarView::copyRequested, this, [this]{ copyToClipboard(); });
        connect(m_toolbar, &ToolbarView::saveRequested, this, [this]{ saveToFile(); });
        connect(m_toolbar, &ToolbarView::closeRequested, this, [this]{ finish(); });
        connect(m_toolbar, &ToolbarView::undoRequested, this, [this]{ undoLast(); });
        connect(m_toolbar, &ToolbarView::redoRequested, this, [this]{ redoLast(); });
        m_toolbar->rebuild();
        m_toolbar->setSelectedTool(m_tool);
        m_toolbar->show();
    }
    positionToolbar(sel);
    if (isNew) animateToolbarIn();
}

void OverlayWindow::hideToolbar() {
    if (m_toolbar) { m_toolbar->deleteLater(); m_toolbar = nullptr; }
}

void OverlayWindow::positionToolbar(const QRectF& sel) {
    if (!m_toolbar) return;
    const QSizeF barSize = m_toolbar->size();
    const qreal gap = 8;
    QPointF origin(sel.left(), sel.bottom() + gap);   // default: below the area

    const bool fitsBelow = origin.y() + barSize.height() <= height();
    if (!fitsBelow) {
        const qreal above = sel.top() - barSize.height() - gap;
        if (above >= 0) {
            origin.setY(above);                       // above the area
        } else {
            origin.setY(sel.bottom() - barSize.height() - gap);  // inside, near bottom
        }
    }
    origin.setX(std::min(std::max<qreal>(0, origin.x()), width() - barSize.width()));
    origin.setY(std::min(std::max<qreal>(0, origin.y()), height() - barSize.height()));
    m_toolbar->move(static_cast<int>(origin.x()), static_cast<int>(origin.y()));
}

void OverlayWindow::animateToolbarIn() {
    if (!m_toolbar) return;
    const QPoint finalPos = m_toolbar->pos();

    auto* effect = new QGraphicsOpacityEffect(m_toolbar);
    m_toolbar->setGraphicsEffect(effect);
    effect->setOpacity(0.0);
    m_toolbar->move(finalPos.x(), finalPos.y() + 6);

    auto* fade = new QPropertyAnimation(effect, "opacity", m_toolbar);
    fade->setDuration(160);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);

    auto* slide = new QPropertyAnimation(m_toolbar, "pos", m_toolbar);
    slide->setDuration(160);
    slide->setStartValue(QPoint(finalPos.x(), finalPos.y() + 6));
    slide->setEndValue(finalPos);
    slide->setEasingCurve(QEasingCurve::OutCubic);

    fade->start(QAbstractAnimation::DeleteWhenStopped);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void OverlayWindow::onSelectTool(Tool t) {
    if (m_textEditor) commitTextEditing();
    m_tool = t;
    if (t != Tool::Text) m_activeTextIndex.reset();   // drop text frame
    updateTextInspector();
    update();
}

void OverlayWindow::onSelectColor(const QColor& c) {
    m_color = c;
    // Apply to the live editor and the selected text immediately.
    if (m_textEditor) {
        QString style = QString("color: %1; background-color: rgba(0,0,0,71);")
                            .arg(c.name(QColor::HexRgb));
        m_textEditor->setStyleSheet(style);
    }
    if (m_activeTextIndex && m_editingIndex == -1) {
        const int i = *m_activeTextIndex;
        if (i < m_annotations.size() && m_annotations[i].tool == Tool::Text) {
            m_annotations[i].color = c;
            update();
        }
    }
}

// ============================================================================
// Undo / redo (Spec 3 §10) — add/remove only
// ============================================================================

void OverlayWindow::undoLast() {
    if (m_annotations.isEmpty()) return;
    m_redoStack.append(m_annotations.takeLast());   // remember for redo
    if (m_activeTextIndex) m_activeTextIndex.reset();
    updateTextInspector();
    update();
}

void OverlayWindow::redoLast() {
    if (m_redoStack.isEmpty()) return;
    m_annotations.append(m_redoStack.takeLast());
    update();
}

// ============================================================================
// Text editing (Spec 3 §11)
// ============================================================================

void OverlayWindow::beginTextEditingAt(const QPointF& point) {
    m_editingIndex = -1;
    presentEditor(point, 18, m_color, QString(), m_currentTextAlignment);
    updateTextInspector();   // hide inspector while editing
}

void OverlayWindow::beginTextEditingIndex(int index) {
    const Annotation a = m_annotations[index];
    m_editingIndex = index;
    m_currentTextAlignment = a.alignment;
    presentEditor(a.start, a.fontSize, a.color, a.text, a.alignment);
    updateTextInspector();
    update();   // hide the painted version while editing
}

void OverlayWindow::presentEditor(const QPointF& origin, qreal fontSize,
                                  const QColor& col, const QString& text, TextAlign align) {
    auto* editor = new QTextEdit(this);
    editor->setFrameStyle(QFrame::NoFrame);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setLineWrapMode(QTextEdit::NoWrap);
    editor->setAcceptRichText(false);

    QFont font;
    font.setPointSizeF(fontSize);
    editor->setFont(font);

    // background rgba(black, 0.28) ≈ alpha 71/255; text color = col.
    editor->setStyleSheet(QString("color: %1; background-color: rgba(0,0,0,71);")
                              .arg(col.name(QColor::HexRgb)));
    editor->setAlignment(toQtAlignment(align));
    editor->setPlainText(text);
    editor->setGeometry(static_cast<int>(origin.x()), static_cast<int>(origin.y()),
                        44, static_cast<int>(fontSize + 8));

    m_textEditor = editor;
    m_currentTextAlignment = align;

    editor->installEventFilter(this);
    connect(editor, &QTextEdit::textChanged, this, [this]{
        sizeFieldToFit();
        positionEditControls();
        positionAlignmentControls();
        update();
    });

    editor->show();
    sizeFieldToFit();

    showEditControls();
    if (Settings::instance().textAlignmentEnabled()) showAlignmentControls();
    applyAlignmentToEditor(align);

    editor->setFocus(Qt::OtherFocusReason);
    QTextCursor tc = editor->textCursor();
    tc.movePosition(QTextCursor::End);
    editor->setTextCursor(tc);
}

void OverlayWindow::sizeFieldToFit() {
    if (!m_textEditor) return;
    QFont font = m_textEditor->font();
    QFontMetricsF fm(font);
    QString s = m_textEditor->toPlainText();
    if (s.isEmpty()) s = " ";
    QRectF bounding = fm.boundingRect(QRectF(0, 0, 600, 1e7),
                                      Qt::AlignLeft | Qt::TextWordWrap, s);
    const qreal w = std::max<qreal>(40, std::ceil(bounding.width()) + 14);
    const qreal h = std::max<qreal>(font.pointSizeF() + 8, std::ceil(bounding.height()) + 8);
    m_textEditor->resize(static_cast<int>(w), static_cast<int>(h));

    // With NoWrap the document otherwise has no reference width, so per-block
    // center/right alignment is invisible while editing. Give the document a text
    // width equal to the editor's inner content width so shorter lines have room
    // to shift. Derive it from the just-computed widget width `w` (not
    // viewport()->width(), which may lag a frame after resize()): subtract the
    // document margin on both sides. The frame is NoFrame and scrollbars are off,
    // so the viewport width ~= `w`. Runs on every textChanged via sizeFieldToFit,
    // keeping the width in step with the widest line. Single-line text is
    // unaffected (a lone full-width line aligns identically left/center/right).
    QTextDocument* doc = m_textEditor->document();
    const qreal contentWidth = std::max<qreal>(1.0, w - 2.0 * doc->documentMargin());
    doc->setTextWidth(contentWidth);
}

void OverlayWindow::showEditControls() {
    if (!m_textEditor) return;
    auto* container = new QWidget(this);
    container->resize(58, 30);
    container->setStyleSheet("background-color: rgba(31,31,31,242); border-radius: 7px;");
    // white 0.12 alpha 0.95 -> ~ rgba(31,31,31,242)

    auto* confirm = new QPushButton(QString::fromUtf8("✓"), container);  // ✓
    confirm->setFocusPolicy(Qt::NoFocus);
    confirm->setStyleSheet("color: #34C759; border: none; background: transparent; font-weight: bold;");
    confirm->setGeometry(5, 4, 22, 22);
    confirm->setCursor(Qt::PointingHandCursor);
    connect(confirm, &QPushButton::clicked, this, [this]{ commitTextEditing(); });

    auto* cancel = new QPushButton(QString::fromUtf8("✗"), container);   // ✗
    cancel->setFocusPolicy(Qt::NoFocus);
    cancel->setStyleSheet("color: #FF3B30; border: none; background: transparent; font-weight: bold;");
    cancel->setGeometry(31, 4, 22, 22);
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, [this]{ cancelTextEditing(); });

    m_editControls = container;
    positionEditControls();
    container->show();
    container->raise();   // keep the controls above the editor so clicks land on them
}

void OverlayWindow::positionEditControls() {
    if (!m_editControls || !m_textEditor) return;
    const QRect f = m_textEditor->geometry();
    qreal x = f.left();
    qreal y = f.bottom() + 6;   // (maxY + 6) — below the field, +Y down
    x = std::min<qreal>(std::max<qreal>(0, x), width() - m_editControls->width());
    y = std::min<qreal>(std::max<qreal>(0, y), height() - m_editControls->height());
    m_editControls->move(static_cast<int>(x), static_cast<int>(y));
}

void OverlayWindow::showAlignmentControls() {
    if (!m_textEditor) return;
    auto* container = new QWidget(this);
    container->resize(84, 30);
    container->setStyleSheet("background-color: rgba(31,31,31,242); border-radius: 7px;");

    m_alignButtons.clear();
    // Three painted three-line icons, each justified left / center / right so the
    // affordance is unambiguous (a single-glyph "☰/≡/☰" looked identical for
    // left and right). Drawn into a QIcon rather than relying on font glyphs.
    auto makeAlignIcon = [](TextAlign al) -> QIcon {
        const qreal dpr = 2.0;
        const int side = 22;
        QPixmap pm(int(side * dpr), int(side * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(Qt::white);
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // Four lines; long lines span the full width, short lines (every 2nd) are
        // half width and shifted to match the justification.
        const qreal left = 5.0, full = side - 2 * 5.0, halfW = full * 0.55;
        const qreal ys[] = { 6.0, 10.0, 14.0, 18.0 };
        for (int i = 0; i < 4; ++i) {
            const bool shortLine = (i % 2 == 1);
            const qreal w = shortLine ? halfW : full;
            qreal x0 = left;
            if (shortLine) {
                if (al == TextAlign::Center) x0 = left + (full - w) / 2.0;
                else if (al == TextAlign::Right) x0 = left + (full - w);
            }
            p.drawLine(QPointF(x0, ys[i]), QPointF(x0 + w, ys[i]));
        }
        p.end();
        return QIcon(pm);
    };

    struct Opt { TextAlign align; };
    const Opt options[] = {
        { TextAlign::Left },
        { TextAlign::Center },
        { TextAlign::Right },
    };
    int x = 5;
    for (const Opt& o : options) {
        auto* b = new QPushButton(container);
        b->setIcon(makeAlignIcon(o.align));
        b->setIconSize(QSize(22, 22));
        b->setFocusPolicy(Qt::NoFocus);
        b->setGeometry(x, 4, 22, 22);
        b->setCursor(Qt::PointingHandCursor);
        TextAlign al = o.align;
        connect(b, &QPushButton::clicked, this, [this, al]{ setTextAlignment(al); });
        m_alignButtons.append({al, b});
        x += 26;
    }
    m_alignControls = container;
    updateAlignmentHighlight();
    positionAlignmentControls();
    container->show();
    container->raise();   // above the editor so the align buttons get the clicks
}

void OverlayWindow::positionAlignmentControls() {
    if (!m_alignControls || !m_textEditor) return;
    const QRect f = m_textEditor->geometry();
    qreal x = f.left();
    qreal y = f.top() - m_alignControls->height() - 6;   // above the field
    x = std::min<qreal>(std::max<qreal>(0, x), width() - m_alignControls->width());
    y = std::max<qreal>(0, y);
    m_alignControls->move(static_cast<int>(x), static_cast<int>(y));
}

void OverlayWindow::updateAlignmentHighlight() {
    for (const auto& pr : m_alignButtons) {
        const bool sel = (pr.first == m_currentTextAlignment);
        pr.second->setStyleSheet(
            QString("color: white; border: none; border-radius: 5px; background: %1;")
                .arg(sel ? "#007AFF" : "transparent"));
    }
}

void OverlayWindow::setTextAlignment(TextAlign a) {
    m_currentTextAlignment = a;
    updateAlignmentHighlight();
    if (m_textEditor) {
        // Live editing: push the alignment into the inline editor.
        applyAlignmentToEditor(a);
        update();
        return;
    }
    // Not editing but a committed text is selected -> apply directly to it so
    // the alignment control is not a no-op outside edit mode.
    if (m_activeTextIndex && *m_activeTextIndex < m_annotations.size() &&
        m_annotations[*m_activeTextIndex].tool == Tool::Text) {
        m_annotations[*m_activeTextIndex].alignment = a;
        update();
    }
}

void OverlayWindow::applyAlignmentToEditor(TextAlign a) {
    if (!m_textEditor) return;
    // Apply alignment to the whole document AND a tight, fixed line height with
    // zero paragraph margins, so multi-line text uses a compact, chat-like line
    // spacing instead of the loose default (the font's natural leading made the
    // gap look too big while typing). New blocks (Enter) inherit this format.
    // kTextLinePitch = font height x 1.15 (a small gap, never clips descenders).
    m_textEditor->setFocus(Qt::OtherFocusReason);
    const qreal pitch = QFontMetricsF(m_textEditor->font()).height() * 1.15;
    QTextCursor tc = m_textEditor->textCursor();
    const int savedPos = tc.position();
    {
        // Block signals so this format merge doesn't re-enter the textChanged path.
        const QSignalBlocker blocker(m_textEditor);
        tc.select(QTextCursor::Document);
        QTextBlockFormat fmt;
        fmt.setAlignment(toQtAlignment(a));
        fmt.setTopMargin(0);
        fmt.setBottomMargin(0);
        fmt.setLineHeight(pitch, QTextBlockFormat::FixedHeight);
        tc.mergeBlockFormat(fmt);
    }
    QTextCursor restore = m_textEditor->textCursor();
    restore.setPosition(std::min<int>(savedPos, static_cast<int>(m_textEditor->toPlainText().length())));
    m_textEditor->setTextCursor(restore);
    m_textEditor->setAlignment(toQtAlignment(a));
}

void OverlayWindow::commitTextEditing() { endTextEditing(true); }
void OverlayWindow::cancelTextEditing() { endTextEditing(false); }

void OverlayWindow::endTextEditing(bool commit) {
    if (!m_textEditor || m_endingTextEditing) return;
    m_endingTextEditing = true;

    QTextEdit* field = m_textEditor;
    m_textEditor = nullptr;   // null first to avoid re-entry (focus-out commit)

    const QString text = field->toPlainText();
    const QString trimmed = text.trimmed();
    const QPointF origin(field->geometry().left(), field->geometry().top());
    const qreal size = field->font().pointSizeF();
    // Foreground color: stored alignment is the reliable source (field.alignment
    // can lag during editing); color stays whatever was set on the field.
    QColor col = m_color;
    const TextAlign align = m_currentTextAlignment;
    const int idx = m_editingIndex;
    m_editingIndex = -1;

    field->removeEventFilter(this);
    field->deleteLater();
    if (m_editControls) { m_editControls->deleteLater(); m_editControls = nullptr; }
    if (m_alignControls) { m_alignControls->deleteLater(); m_alignControls = nullptr; }
    m_alignButtons.clear();

    if (idx >= 0 && idx < m_annotations.size()) {
        // Editing an existing text.
        if (commit) {
            if (trimmed.isEmpty()) {
                m_annotations.remove(idx);
                m_activeTextIndex.reset();
            } else {
                m_annotations[idx].text = text;         // keep rotation + bg
                m_annotations[idx].fontSize = size;
                m_annotations[idx].color = col;
                m_annotations[idx].start = origin;
                m_annotations[idx].alignment = align;
                m_activeTextIndex = idx;
            }
        } else {
            m_activeTextIndex = idx;                     // cancel -> leave as was
        }
    } else if (commit && !trimmed.isEmpty()) {
        // New text.
        Annotation a;
        a.tool = Tool::Text;
        a.color = col;
        a.lineWidth = m_lineWidth;
        a.start = origin;
        a.end = origin;
        a.text = text;
        a.fontSize = size;
        a.alignment = align;
        m_annotations.append(a);
        m_redoStack.clear();
        m_activeTextIndex = m_annotations.size() - 1;
    } else {
        m_activeTextIndex.reset();
    }

    setFocus(Qt::OtherFocusReason);
    updateTextInspector();
    update();
    m_endingTextEditing = false;
}

// Intercept the inline editor's keys/focus (Enter / Shift+Enter / Esc / focus-out).
bool OverlayWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (m_textEditor && obj == m_textEditor) {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (ke->modifiers().testFlag(Qt::ShiftModifier)) {
                    return false;   // Shift+Enter -> newline (let editor insert it)
                }
                commitTextEditing();   // Enter -> confirm
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                cancelTextEditing();   // Esc -> cancel
                return true;
            }
        } else if (ev->type() == QEvent::FocusOut) {
            // Intentionally do NOT auto-commit on focus-out. On macOS, clicking our
            // own Qt::NoFocus controls (the align buttons / ✓ / ✗ / toolbar) can
            // deliver a transient FocusOut to the editor; auto-committing here made
            // the alignment buttons feel like they "closed" the text. Commit now
            // happens only on explicit actions: a genuine click elsewhere on the
            // overlay (mousePressEvent), Enter, or the ✓ button.
        }
    }
    return QWidget::eventFilter(obj, ev);
}

// ============================================================================
// Text inspector (Spec 3 §11.11)
// ============================================================================

void OverlayWindow::updateTextInspector() {
    const bool show = (m_tool == Tool::Text && m_editingIndex == -1 && m_activeTextIndex &&
                       *m_activeTextIndex < m_annotations.size() &&
                       m_annotations[*m_activeTextIndex].tool == Tool::Text);
    if (!show) {
        if (m_textInspector) { m_textInspector->deleteLater(); m_textInspector = nullptr; }
        return;
    }
    const int idx = *m_activeTextIndex;
    if (!m_textInspector) {
        m_textInspector = new TextInspectorView(this);
        connect(m_textInspector, &TextInspectorView::textColorRequested, this, [this](const QColor& c){
            if (m_activeTextIndex && *m_activeTextIndex < m_annotations.size()) {
                m_annotations[*m_activeTextIndex].color = c;
                m_color = c;
                update();
                updateTextInspector();
            }
        });
        connect(m_textInspector, &TextInspectorView::bgColorRequested, this, [this](const std::optional<QColor>& c){
            if (m_activeTextIndex && *m_activeTextIndex < m_annotations.size()) {
                m_annotations[*m_activeTextIndex].bgColor = c;
                update();
                updateTextInspector();
            }
        });
        m_textInspector->rebuild();
        m_textInspector->show();
    }
    positionTextInspector(m_annotations[idx]);
    m_textInspector->setSelected(m_annotations[idx].color, m_annotations[idx].bgColor);
}

void OverlayWindow::positionTextInspector(const Annotation& a) {
    if (!m_textInspector) return;
    const QRectF box = textLocalRect(a);
    const QSize size = m_textInspector->size();
    qreal x = box.center().x() - size.width() / 2.0;
    qreal y = box.top() - size.height() - 10;     // above the text
    if (y < 0) y = box.bottom() + 10;             // no room above -> below
    x = std::min<qreal>(std::max<qreal>(0, x), width() - size.width());
    y = std::min<qreal>(std::max<qreal>(0, y), height() - size.height());
    m_textInspector->move(static_cast<int>(x), static_cast<int>(y));
}

// ============================================================================
// Output (Spec 3 §12)
// ============================================================================

QImage OverlayWindow::renderOutput() const {
    if (!m_selection || m_selection->width() <= 1 || m_selection->height() <= 1)
        return QImage();
    const QRectF sel = *m_selection;
    const qreal outScale = outputScale();
    const int pxW = static_cast<int>(sel.width() * outScale);
    const int pxH = static_cast<int>(sel.height() * outScale);
    if (pxW <= 0 || pxH <= 0) return QImage();

    QImage out(pxW, pxH, QImage::Format_RGBA8888_Premultiplied);
    out.fill(Qt::transparent);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Map view-point coords (top-left, +Y down) into the crop. NO Y-flip: scale
    // by outScale, then translate so the selection's top-left is the origin.
    p.scale(outScale, outScale);
    p.translate(-sel.left(), -sel.top());

    // drawImageUpright would use rect()=view bounds; mirror that mapping by
    // drawing the full screenshot into the view bounds rect.
    const_cast<OverlayWindow*>(this)->drawImageUpright(p, m_screenshot, QRectF(rect()));
    for (const Annotation& a : m_annotations)
        const_cast<OverlayWindow*>(this)->drawAnnotation(p, a);

    p.end();
    return out;
}

void OverlayWindow::writeToClipboard(const QImage& img) {
    QApplication::clipboard()->setImage(img);
}

void OverlayWindow::copyToClipboard() {
    if (m_textEditor) commitTextEditing();
    const QImage img = renderOutput();
    if (img.isNull()) return;
    writeToClipboard(img);
    finish();
}

void OverlayWindow::saveToFile() {
    if (m_textEditor) commitTextEditing();
    const QImage img = renderOutput();
    if (img.isNull()) return;

    // Silent save to a configured folder, plus clipboard.
    const auto folder = Settings::instance().saveFolderPath();
    if (folder) {
        const QString url = makeSaveUrl(*folder);
        img.save(url, "PNG");
        writeToClipboard(img);
        finish();
        return;
    }

    // No folder set -> file dialog with the default macOS-style name.
    const QString suggested = QDir(QStandardPaths::writableLocation(
                                      QStandardPaths::PicturesLocation))
                                  .filePath(defaultScreenshotName());

    // Remember the capture screen BEFORE tearing down the overlay, so we can put
    // the save dialog on the SAME monitor the selection was made on. Falls back
    // to this widget's current screen, then the primary screen.
    QScreen* captureScreen = m_screen;
    if (!captureScreen) captureScreen = screen();
    if (!captureScreen) captureScreen = QGuiApplication::primaryScreen();

    // CRITICAL: tear down the shield overlay first so the dialog is visible.
    finish();

    // Build an explicit dialog (not the static helper) so we can place it on the
    // capture screen before showing it. Keeps the PNG filter + default name.
    QFileDialog dialog(nullptr, Loc::t("save.title"), suggested, "PNG Image (*.png)");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("png");
    if (captureScreen) {
        // Anchor the dialog to the target screen, then re-center it there once it
        // has a real size. setScreen alone is not enough on macOS (the native
        // panel still opens on the screen with the key window), so we also move
        // the dialog's frame to that screen's center.
        dialog.setScreen(captureScreen);
        dialog.ensurePolished();
        QSize sz = dialog.sizeHint();
        if (sz.isEmpty()) sz = QSize(700, 500);
        const QRect g = captureScreen->geometry();
        dialog.move(g.x() + (g.width() - sz.width()) / 2,
                    g.y() + (g.height() - sz.height()) / 2);
    }

    if (dialog.exec() == QDialog::Accepted) {
        const QStringList files = dialog.selectedFiles();
        if (!files.isEmpty() && !files.first().isEmpty()) {
            img.save(files.first(), "PNG");
            writeToClipboard(img);
        }
    }
}

QString OverlayWindow::defaultScreenshotName() const {
    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd 'at' HH.mm.ss");
    return QString("Screenshot %1.png").arg(stamp);
}

QString OverlayWindow::makeSaveUrl(const QString& folder) const {
    QDir dir(folder);
    QString base = QFileInfo(defaultScreenshotName()).completeBaseName();
    QString path = dir.filePath(base + ".png");
    int i = 2;
    while (QFileInfo::exists(path)) {
        path = dir.filePath(QString("%1 (%2).png").arg(base).arg(i));
        ++i;
    }
    return path;
}

void OverlayWindow::finish() {
    emit finished();
}
