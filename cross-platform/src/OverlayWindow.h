#pragma once

// OverlayWindow.h — The per-screen interactive capture/annotation overlay.
//
// THE HEART OF THE APP. One instance per QScreen. Source: OverlayView.swift
// (Spec 3) + OverlayWindow config from OverlayController.swift (Spec 2 §3).
//
// This single frameless, translucent, always-on-top, shield-level window:
//   - paints the captured screenshot, a dark dim with a transparent selection hole,
//     all committed annotations, the in-progress annotation, selection chrome
//     (border + 8 handles + WxH size label), active-text chrome (dashed frame +
//     resize/rotate handles), and pink snap guides — IN THAT PAINT ORDER (Spec 3 §4);
//   - handles mouse: new selection / move / 8-handle resize / draw / text
//     move/resize/rotate, with cursor confinement to this screen during selection;
//   - handles keyboard: Esc=close, Enter/Cmd+C/Cmd+X=copy, Cmd+S=save,
//     Cmd+Z/Shift+Cmd+Z=undo/redo (matched by key CODE, layout-independent);
//   - inline text editing (QTextEdit) with alignment/rotation/bg/snap, ✓/✗ and
//     alignment controls, and a floating TextInspectorView;
//   - undo/redo (add/remove only; edits/moves are NOT undoable);
//   - copy to clipboard and save-to-PNG (silent folder or QFileDialog).
//
// COORDINATE SYSTEM: Qt is already top-left/+Y-down, matching the flipped
// AppKit view — store/compute everything in widget-local logical points, NO flip.
// The two Swift "un-flip" blocks (drawImageUpright, per-text-line CTLine) are
// unnecessary in Qt: QPainter::drawImage and drawText are already upright.
//
// MULTI-MONITOR: a controller (TrayApp) owns one OverlayWindow per screen and
// wires beganSelection -> clearSelectionState() on the others (single active
// selection across all monitors), and finished -> tear down all overlays.

#include "Annotation.h"

#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QColor>
#include <QRectF>
#include <QPointF>
#include <QVector>
#include <optional>

class QVariantAnimation;

class ToolbarView;
class TextInspectorView;
class QTextEdit;
class QWidget;
class QPushButton;

class OverlayWindow : public QWidget {
    Q_OBJECT
public:
    // screenshot: the captured bitmap for THIS screen (dpr set on the QImage).
    // screen: the QScreen this overlay covers (geometry placed by the controller).
    OverlayWindow(const QImage& screenshot, QScreen* screen, QWidget* parent = nullptr);
    ~OverlayWindow() override;

    // Reset this monitor to a clean dimmed state — called on OTHER monitors when
    // a new selection begins on one of them. Removes editor/controls/inspector,
    // clears selection + annotations + active text, repaints. (Spec 3 §12.6)
    void clearSelectionState();

    // Apply native shield window level / collection behavior AFTER show()
    // (macOS: CGShieldingWindowLevel via NSWindow; see DESIGN.md). No-op elsewhere.
    void applyShieldLevel();

    // Optional smooth fade of the dim layer (gated by Settings::animatedDim()).
    // fade-in:  m_dimProgress 0 -> 1 (~0.18s ease-out) after the overlay shows.
    // fade-out: m_dimProgress current -> 0 (~0.16s) before teardown.
    // When animated dimming is OFF these are never called and m_dimProgress
    // stays 1.0, so the dim renders instantly exactly as before.
    void startDimFadeIn();
    void startDimFadeOut();

signals:
    void finished();          // overlay should be torn down (Esc / copy / save / close)
    void beganSelection();    // a new selection started here -> clear the others

protected:
    // --- Rendering ---
    void paintEvent(QPaintEvent*) override;   // strict paint order (Spec 3 §4)

    // --- Mouse ---
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

    // --- Keyboard ---
    void keyPressEvent(QKeyEvent*) override;

    // --- Inline text editor event interception (Enter/Shift+Enter/Esc/focus-out) ---
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    // ----- Drag state machine (Spec 3 §2.4) -----
    enum class DragMode {
        None, NewSelection, MoveSelection, Resize,
        Draw, ResizeText, MoveText, RotateText
    };
    // 8 selection handles: 4 corners + 4 edge midpoints (Spec 3 §2.5).
    enum class Handle { TL, T, TR, R, BR, B, BL, L };

    // ----- Painting helpers (Spec 3 §4) -----
    void drawImageUpright(QPainter& p, const QImage& img, const QRectF& rect);
    void drawDim(QPainter& p);                       // black @ dimOpacity, even-odd hole
    void drawAnnotation(QPainter& p, const Annotation& a);
    void drawArrow(QPainter& p, const QPointF& from, const QPointF& to,
                   qreal width, const QColor& color);
    void drawTextAnnotation(QPainter& p, const Annotation& a);
    void drawSelectionChrome(QPainter& p);           // border + 8 handles + size label
    void drawActiveTextChrome(QPainter& p);          // dashed frame + resize/rotate handles
    void drawSnapGuides(QPainter& p);                // pink dashed center guides

    // ----- Geometry helpers (Spec 3 §6) -----
    static QRectF rectFrom(const QPointF& a, const QPointF& b);       // normalized
    QVector<QPair<Handle, QRectF>> handleRects(const QRectF& sel) const; // 9x9 handles
    std::optional<Handle> handleHit(const QPointF& p, const QRectF& sel) const; // 17x17 hit
    QRectF resized(const QRectF& start, Handle h, const QPointF& p) const;

    // ----- Text geometry helpers (Spec 3 §4.4) -----
    QSizeF  textSize(const Annotation& a) const;     // measured, clamped (min 10 x fontSize)
    QRectF  textLocalRect(const Annotation& a) const;
    QPointF textCenter(const Annotation& a) const;
    QRectF  resizeHandleLocal(const Annotation& a) const;   // 18x18 bottom-right
    QRectF  rotateHandleLocal(const Annotation& a) const;   // 18x18 top-right
    QPointF localPoint(const QPointF& p, const Annotation& a) const;  // screen->unrotated
    QPointF screenPoint(const QPointF& lp, const Annotation& a) const;// unrotated->screen
    QRectF  screenRectAround(const QRectF& localRect, const Annotation& a) const; // 22x22
    std::optional<int> textAnnotationIndex(const QPointF& p) const;   // topmost hit

    // ----- Cursors (Spec 3 §5) -----
    void updateHoverCursor(const QPointF& p);        // crosshair/openHand/iBeam/resize...
    Qt::CursorShape resizeCursorFor(Handle h) const; // SizeF/BDiag, LeftRight, UpDown
    void beginCustomCursorDrag();                    // suppress hover recompute during drag
    void endCustomCursorDrag();
    void confineCursorToScreen();                    // clamp QCursor::pos to this screen

    // ----- Toolbar (Spec 3 §9) -----
    void showToolbar(const QRectF& sel);
    void hideToolbar();
    void positionToolbar(const QRectF& sel);
    void animateToolbarIn();                         // 160ms slide-up + fade

    // ----- Tool/color selection from toolbar callbacks (Spec 3 §9) -----
    void onSelectTool(Tool t);
    void onSelectColor(const QColor& c);

    // ----- Undo/redo (Spec 3 §10) -----
    void undoLast();
    void redoLast();   // redoStack cleared on NEW annotation commit only

    // ----- Text editing (Spec 3 §11) -----
    void beginTextEditingAt(const QPointF& point);   // new text (editingIndex = -1)
    void beginTextEditingIndex(int index);           // edit existing
    void presentEditor(const QPointF& origin, qreal fontSize,
                       const QColor& col, const QString& text, TextAlign align);
    void sizeFieldToFit();
    void showEditControls();                          // ✓/✗ container below field
    void positionEditControls();
    void showAlignmentControls();                     // gated by textAlignmentEnabled
    void positionAlignmentControls();
    void updateAlignmentHighlight();
    void setTextAlignment(TextAlign a);
    void applyAlignmentToEditor(TextAlign a);
    void commitTextEditing();                         // endTextEditing(commit=true)
    void cancelTextEditing();                         // endTextEditing(commit=false)
    void endTextEditing(bool commit);

    // ----- Text inspector (Spec 3 §11.11) -----
    void updateTextInspector();
    void positionTextInspector(const Annotation& a);

    // ----- Output (Spec 3 §12) -----
    QImage renderOutput() const;                      // crop+scale; nullptr if too small
    void writeToClipboard(const QImage& img);
    void copyToClipboard();                           // commit -> render -> clipboard -> finish
    void saveToFile();                                // commit -> render -> PNG (silent or dialog)
    QString defaultScreenshotName() const;            // "Screenshot yyyy-MM-dd 'at' HH.mm.ss.png"
    QString makeSaveUrl(const QString& folder) const; // unique "base (i).png" from i=2
    void finish();                                    // emit finished()

    // ----- Scale (Spec 3 §1) -----
    qreal scale() const;        // screenshot px per view point (~dpr)
    qreal outputScale() const;  // downscaleRetina ? 1 : scale

    // ===================== State =====================
    QImage m_screenshot;
    QScreen* m_screen = nullptr;

    std::optional<QRectF> m_selection;
    QVector<Annotation> m_annotations;       // committed, draw order = array order
    QVector<Annotation> m_redoStack;
    std::optional<Annotation> m_current;     // in-progress draw

    Tool m_tool = Tool::Select;
    QColor m_color;                          // active color (default systemRed)
    qreal m_lineWidth = 3.0;                // const stroke width for shapes

    ToolbarView* m_toolbar = nullptr;

    DragMode m_dragMode = DragMode::None;
    Handle m_dragHandle = Handle::BR;
    QPointF m_dragStart;
    QRectF m_selectionAtDragStart;
    static constexpr int kHandleSize = 9;

    // Text editing state
    std::optional<int> m_activeTextIndex;    // selected text (shows frame+handles)
    int m_editingIndex = -1;                 // index being edited inline; -1 = new text
    QTextEdit* m_textEditor = nullptr;
    TextInspectorView* m_textInspector = nullptr;
    QWidget* m_editControls = nullptr;       // ✓/✗ container
    QWidget* m_alignControls = nullptr;      // alignment container
    TextAlign m_currentTextAlignment = TextAlign::Left;
    // PRIVATE additions (Qt-specific): alignment buttons for live highlight,
    // and a guard so the editor's focus-out commit does not re-enter while we
    // are tearing the editor down.
    QVector<QPair<TextAlign, QPushButton*>> m_alignButtons;
    bool m_endingTextEditing = false;

    qreal m_textResizeStartSize = 18.0;
    QPointF m_textResizeStartPoint;
    QPointF m_textMoveStartOrigin;
    qreal m_textRotateStartAngle = 0.0;
    qreal m_textRotateStartPointerAngle = 0.0;

    bool m_snapGuideV = false;
    bool m_snapGuideH = false;
    bool m_cursorRectsDisabled = false;

    // Animated dimming. m_dimProgress multiplies the dim alpha (1.0 = full dim,
    // the default static value). m_dimAnim drives it only when animatedDim is on.
    qreal m_dimProgress = 1.0;
    QVariantAnimation* m_dimAnim = nullptr;

    // Cached upright screenshot backdrop, blitted each paint instead of
    // re-rendering the full QImage (with orientation transform) every frame.
    // Built lazily on first paint / when the widget size changes; carries the
    // device pixel ratio so it stays sharp on Retina.
    QPixmap m_backdrop;
    void ensureBackdrop();   // (re)build m_backdrop if missing or stale
};
