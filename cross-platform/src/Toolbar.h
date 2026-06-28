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
// LAYOUT CONSTANTS — restyled to the LightGet "Main toolbar" design
// (/private/tmp/lightget_design/toolbar_canvas.html, section 1):
//   Toolbar: buttonSize=40, radius 11, gap=3, panel padding 8 (v) / 10 (h);
//            tool/action glyph color #d4d5da, hover bg white@9%;
//            selected tool = full systemBlue(#0A84FF) fill + white glyph;
//            color wells 26px circles, gap 4, vertically centered;
//            selected well = white outer ring + checkmark; unselected = inset ring;
//            1px vertical separators (white@10%, height 26, margin 7) bracket
//            the palette group; close action uses red(#FF6961) glyph + red hover.
//   Inspector: swatch 16x16 (rounded square radius 4), gap 4, label 16;
//              column start x=26, advance +20; height 28 (1 row) / 48 (2 rows).
//   Pop animation: 180ms easeOut, scale 1.0->peak->1.0 about center,
//                  peak 1.10 (toolbar) / 1.18 (inspector), auto-reset.
//   Panel surface #242429; panel border white@9%; radius 16 (toolbar) / 8 (inspector);
//   swatch border white@50% 1px; checkmark black on light swatch / white on dark.

#include "Annotation.h"

#include <QWidget>
#include <QColor>
#include <QVector>
#include <QHash>
#include <QPointer>
#include <optional>

class QPushButton;
class QLabel;
class QHBoxLayout;

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

    // Design (toolbar_canvas.html §1): 40px buttons, radius 11, 3px gap, panel
    // padding 8 (vertical) / 10 (horizontal). The glyph icon canvas matches the
    // button so the procedural glyphs (tuned to the native SF symbols) keep their
    // relative footprint; the button margin + each glyph's transparent inset keep
    // the click pop (1.10x) from clipping, and the hover/selected fills are drawn
    // inset in PopButton::paintEvent so they scale cleanly with the pop.
    // NATIVE sizes (SnapEdit/ToolbarView.swift): button 30, pad 6, gap 2, panel
    // radius 8, button radius 5, colour well 18 (gap 4) -> 42px tall panel. The
    // design HTML draws a bigger 40px toolbar, but the user wants the compact
    // native size, so these mirror the Swift constants 1:1.
    static constexpr int kButtonSize = 30;
    static constexpr int kIconSize   = 30;
    static constexpr int kPadV       = 6;    // panel vertical padding
    static constexpr int kPadH       = 6;    // panel horizontal padding
    static constexpr int kGap        = 2;    // gap between adjacent buttons
    static constexpr int kBtnRadius  = 5;    // button corner radius
    static constexpr int kSwatch     = 18;   // color well diameter
    static constexpr int kSwatchGap  = 4;    // gap between color wells

    QHash<Tool, QPushButton*> m_toolButtons;
    QVector<QPair<QColor, QPushButton*>> m_colorButtons;
    QVector<int> m_separatorX;   // x-centres of the 1px group dividers (design §1)
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

// ---------------------------------------------------------------------------
// TextPanel — the unified contextual "Text" panel from the design: ONE dark
// floating pill above the text block with font ▾ · size −/+ · B I U · alignment ·
// A-color · marker(bg) · ✓ Done. Each group appears only if its Settings toggle
// is enabled (Features tab). A-color / marker open a small swatch popup. Emits a
// high-level signal per change; the owner (OverlayWindow) applies it live to the
// inline editor / selected annotation. Replaces the old ✓/✗ + alignment controls
// + TextInspectorView.
// ---------------------------------------------------------------------------
class TextPanel : public QWidget {
    Q_OBJECT
public:
    explicit TextPanel(QWidget* parent = nullptr);
    ~TextPanel() override;

    // Rebuild the controls honoring the Features-tab text-option toggles, then
    // reflect the given state. Call when (re)showing the panel.
    void setState(const QString& family, int size, bool bold, bool italic,
                  bool underline, TextAlign align, const QColor& color,
                  const std::optional<QColor>& bg);

signals:
    void fontFamilyChanged(const QString& family);   // "" = system default
    void fontSizeChanged(int pt);
    void boldChanged(bool on);
    void italicChanged(bool on);
    void underlineChanged(bool on);
    void alignChanged(TextAlign a);
    void textColorChanged(const QColor& c);
    void bgColorChanged(const std::optional<QColor>& c);   // nullopt = no background
    void doneClicked();

protected:
    void paintEvent(QPaintEvent*) override;          // rounded dark pill bg

private:
    void rebuild();                                  // (re)build per Settings toggles
    void refreshVisualState();                       // sync button looks to m_*
    void openColorPopup(bool background, QWidget* anchor);
    void openFontPopup(QWidget* anchor);             // in-overlay font list (no QMenu)
    void closeColorPopup();

    // Current state (mirrors the edited annotation).
    QString m_family;
    int     m_size = 18;
    bool    m_bold = false, m_italic = false, m_underline = false;
    TextAlign m_align = TextAlign::Left;
    QColor  m_color = QColor(255, 59, 48);
    std::optional<QColor> m_bg;

    // Control handles kept for restyling on state change.
    QHBoxLayout* m_row = nullptr;
    QPushButton* m_fontBtn = nullptr;
    QLabel*      m_sizeLabel = nullptr;
    QPushButton* m_boldBtn = nullptr;
    QPushButton* m_italicBtn = nullptr;
    QPushButton* m_underlineBtn = nullptr;
    QVector<QPair<TextAlign, QPushButton*>> m_alignBtns;
    QPushButton* m_colorBtn = nullptr;
    QPushButton* m_markerBtn = nullptr;
    QPointer<QWidget> m_colorPopup;   // child of the overlay; auto-nulls on delete
};
