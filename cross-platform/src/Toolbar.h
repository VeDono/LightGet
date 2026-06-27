#pragma once

// Toolbar.h — Two floating overlay panels:
//   ToolbarView        : tools + color palette + actions (undo/redo/copy/save/close)
//   TextInspectorView  : text color + optional background color, for selected text
//
// Source: ToolbarView.swift (Spec 4).
//
// Both are frameless child widgets painted by the parent OverlayWindow (or
// top-level Qt::Tool popups). Qt is already top-left/+Y-down — the Swift
// `isFlipped` views need no conversion.
//
// Closures in the Swift source map to Qt signals here (onSelectTool ->
// selectToolRequested, etc.). All buttons must NOT take keyboard focus
// (setFocusPolicy(Qt::NoFocus)) so overlay shortcuts keep flowing.
//
// LAYOUT CONSTANTS (must match for pixel-exact widths — see Spec 4 §2.4/§3.3):
//   Toolbar: buttonSize=30, pad=6, height ALWAYS 42; tool/action advance +32;
//            color wells 18x18 circles (radius 9) at y=12, advance +22;
//            +6 separator before palette and before actions.
//   Inspector: swatch 16x16 (rounded square radius 4), gap 4, label 16;
//              column start x=26, advance +20; height 28 (1 row) / 48 (2 rows).
//   Pop animation: 180ms easeOut, scale 1.0->peak->1.0 about center,
//                  peak 1.10 (toolbar) / 1.18 (inspector), auto-reset.
//   Panel bg toolbar rgba(31,31,31,242); inspector rgba(31,31,31,245);
//   panel border white@15% 0.5px; swatch border white@50% 1px;
//   selected tool bg systemBlue #007AFF; checkmark black on light swatch / white on dark.

#include "Annotation.h"

#include <QWidget>
#include <QColor>
#include <QVector>
#include <QHash>
#include <optional>

class QPushButton;
class QLabel;

// The 6 palette colors, identical order in both panels (Spec 4 §1.5).
// 0 red, 1 green, 2 blue, 3 yellow, 4 black, 5 white. Default selected = 0 (red).
namespace Palette {
const QVector<QColor>& colors();
// ITU-R BT.601 luma test: brightness = .299R+.587G+.114B (0..1), light if > 0.6.
bool isLight(const QColor& c);
// sRGB per-channel equality within 0.02 (alpha ignored).
bool sameColor(const QColor& a, const QColor& b);
}

// ---------------------------------------------------------------------------
// ToolbarView — main annotation toolbar.
// ---------------------------------------------------------------------------
class ToolbarView : public QWidget {
    Q_OBJECT
public:
    explicit ToolbarView(QWidget* parent = nullptr);

    // Rebuild buttons honoring Settings::isToolEnabled / showColorPalette.
    // Sizes the widget to its content (height 42). Call after settings change.
    void rebuild();

    // Highlight the active tool (blue rounded bg behind its icon).
    void setSelectedTool(Tool t);
    // Highlight the active palette color (centered contrast checkmark).
    void setSelectedColor(int paletteIndex);

    // Pop-bounce a tool/color button (1.10 peak) by tool / color index.
    void popTool(Tool t);
    void popColor(int index);

signals:
    void selectToolRequested(Tool t);
    void selectColorRequested(const QColor& c);
    void undoRequested();
    void redoRequested();
    void copyRequested();
    void saveRequested();
    void closeRequested();

protected:
    void paintEvent(QPaintEvent*) override;     // rounded translucent panel bg

private:
    void buildButtons();                        // layout algorithm (Spec 4 §2.4)

    static constexpr int kButtonSize = 30;
    // Glyph icon rendered SMALLER than the button so the click pop (scale 1.10)
    // stays inside the 30px bounds and never clips (task 1). 24 * 1.10 = 26.4 < 30.
    static constexpr int kIconSize = 24;
    static constexpr int kPad = 6;

    QHash<Tool, QPushButton*> m_toolButtons;
    QVector<QPair<QColor, QPushButton*>> m_colorButtons;
    Tool m_selectedTool = Tool::Select;
    int m_selectedColor = 0;
};

// ---------------------------------------------------------------------------
// TextInspectorView — text/background color panel for a selected text annotation.
// ---------------------------------------------------------------------------
class TextInspectorView : public QWidget {
    Q_OBJECT
public:
    explicit TextInspectorView(QWidget* parent = nullptr);

    void rebuild();   // row 2 (background) only if Settings::textBackgroundEnabled

    // Sync checkmarks to reflect the current annotation's colors.
    // bgColor == nullopt selects the "no background" swatch.
    void setSelected(const QColor& textColor, const std::optional<QColor>& bgColor);

signals:
    void textColorRequested(const QColor& c);
    void bgColorRequested(const std::optional<QColor>& c);  // nullopt = no background

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void build();

    QVector<QPair<QColor, QPushButton*>> m_textSwatches;
    // bgSwatches[0] is the "none" swatch (color == nullopt); palette color j -> index j+1.
    QVector<QPair<std::optional<QColor>, QPushButton*>> m_bgSwatches;
};
