#pragma once

// SettingsWindow.h — Tabbed settings window (General + Features) + hotkey recorder.
//
// Source: SettingsWindowController.swift (Spec 5 §4-§13).
//
// Fixed-size QDialog, not resizable, persists across open/close (reused
// instance). Two tabs only (General, Features). NO Apply/OK — every change
// persists to Settings immediately. A full rebuild (reloadUI) happens on
// language change / any reset / choosing folder or custom icon.
//
// VISUAL: restyled to the "LightGet — Direction B" design (settings_window.dc.html
// / settings_canvas.html): centered rounded tabs, grouped white "card" sections
// with flat left-aligned rows, brand accent #0A84FF, round per-setting reset
// buttons, painted toggle switches + checkboxes. Colors derive from a palette-
// based token set so both LIGHT and DARK themes look right. Layout/styling only —
// every setting, signal/slot, handler and localization stays intact.
//
// Three callbacks the owner (TrayApp) wires to:
//   hotKeyChanged  -> re-register global hotkey + update tray "Capture" text
//   languageChanged-> rebuild tray menu + retranslate
//   barIconChanged -> update tray icon

#include "Annotation.h"

#include <QDialog>
#include <QPushButton>
#include <QAbstractButton>
#include <QColor>
#include <QPoint>
#include <cstdint>
#include <functional>

class QStackedWidget;
class HotkeyRecorder;
class QFrame;
class QVBoxLayout;
class QLabel;
class QMouseEvent;
class AppearanceSegment;

// ---------------------------------------------------------------------------
// Design tokens — resolved per theme (light / dark) from the window palette.
// Mirrors the --c-* variables in settings_window.dc.html / settings_canvas.html.
// ---------------------------------------------------------------------------
struct DesignTokens {
    bool   dark = false;
    QColor bg, card, control, controlFill, border, separator;
    QColor text, text2, text3;
    QColor accent, accentWeak, link;
    QColor toggleOff, toggleOn, knob;
    QColor resetFg, icon, checkOn;
};

class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget* parent = nullptr);

    void showCentered();    // center + raise + activate (accessory-app focus dance)
    // Re-sync the shortcut recorder button to Settings::hotKeyDisplay() — called
    // after TrayApp rolls a failed hotkey change back to the previous combo.
    void refreshHotKeyDisplay();

signals:
    void hotKeyChanged();
    void languageChanged();
    void barIconChanged();

protected:
    void changeEvent(QEvent* e) override;   // rebuild on palette/theme switch
    void paintEvent(QPaintEvent* e) override;       // frameless rounded background
    void mousePressEvent(QMouseEvent* e) override;  // start title-bar drag
    void mouseMoveEvent(QMouseEvent* e) override;   // perform title-bar drag
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    // Reset targets (raw int parity with Swift ResetTarget). language=0 is a
    // dead UI case (no button) but kept for parity.
    enum class ResetTarget { Language = 0, Hotkey = 1, Dim = 2,
                             Downscale = 3, SaveFolder = 4 };

    void reloadUI();                 // full teardown + rebuild of both tabs
    void buildUI();
    QWidget* buildTitleBar();        // 46px chrome: traffic lights + centered title
    QWidget* buildGeneralTab();
    QWidget* buildFeaturesTab();
    void addAboutSection(QVBoxLayout* generalCol);

    // Apply the chosen appearance ("auto"/"light"/"dark") to the app color scheme
    // via QGuiApplication::styleHints()->setColorScheme(). Called when the
    // Appearance segment changes.
    void applyAppearance(const QString& mode);

    QPushButton* makeResetButton(ResetTarget target);
    void resetTapped(ResetTarget target);   // confirm dialog -> reset -> reloadUI

    // General-tab action handlers (Spec 5 §12).
    void chooseFolder();
    void chooseCustomIcon();
    void onBarIconSegment(int index);        // clears custom path, sets preset
    void onLanguageChanged(int index);
    void onDimChanged(double value);         // slider 0.10..0.85
    void onDownscaleToggled(bool on);
    void onLaunchAtLoginToggled(bool on);    // platform autostart; revert+alert on fail
    QString saveFolderTitle() const;

    // Launch-at-login platform shim (registry / autostart / SMAppService).
    bool isLaunchAtLoginEnabled() const;
    bool setLaunchAtLogin(bool enabled);     // returns false on failure (alert+revert)

    // --- Styling helpers (Direction B) ---
    DesignTokens m_tk;                       // current theme tokens
    void resolveTokens();                    // (re)compute tokens from palette
    QFrame* makeCard();                      // empty rounded card container
    QFrame* makeSeparator();                 // 1px horizontal row separator
    // A standard card row: label on the left, control right-aligned, optional
    // round reset button in a fixed-width trailing slot. Pass nullptr reset to
    // leave the slot empty (keeps rows visually aligned).
    QWidget* makeRow(const QString& label, QWidget* control, QPushButton* reset);
    // A full-width toggle row (label stretches, switch trails, optional reset).
    QWidget* makeToggleRow(const QString& label, QWidget* toggle, QPushButton* reset);

    QStackedWidget* m_stack = nullptr;       // General / Features pages
    QPushButton* m_tabGeneral = nullptr;     // centered rounded folder tab buttons
    QPushButton* m_tabFeatures = nullptr;
    int m_currentTab = 0;                    // 0 = general, 1 = features
    HotkeyRecorder* m_recorder = nullptr;    // reused member across rebuilds

    // Title-bar drag state (frameless window dragged by the custom chrome).
    QWidget* m_titleBar = nullptr;           // the draggable 46px chrome strip
    bool     m_dragging = false;
    QPoint   m_dragOffset;                   // cursor -> window top-left at press

    // Preset bar icons (asset names mirroring SF Symbols), exactly 5 (Spec 5 §5.2).
    // {"scissors","camera.viewfinder","crop","rectangle.dashed","paintbrush.pointed.fill"}
};

// ---------------------------------------------------------------------------
// ToggleSwitch — painted on/off pill switch (QAbstractButton), matching the
// design's 38x22 track + 18px knob, animated slide on toggle. Colors come from
// DesignTokens so both themes read correctly. Used for the boolean settings
// (Downscale, Launch at login) that the native UI rendered as checkboxes — the
// underlying signal is the same toggled(bool).
// ---------------------------------------------------------------------------
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal knobPos READ knobPos WRITE setKnobPos)
public:
    explicit ToggleSwitch(const DesignTokens& tk, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    qreal knobPos() const { return m_knobPos; }
    void setKnobPos(qreal v);

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    // Mark the window in which the state flips as a real user click, so the knob
    // animates ONLY then. Programmatic setChecked() (build / reloadUI / theme
    // switch) snaps the knob into place with no slide — otherwise every unrelated
    // settings change visibly re-animates every ON toggle ("twitch").
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    DesignTokens m_tk;
    qreal m_knobPos = 0.0;   // 0 = off (left), 1 = on (right)
    bool  m_hover = false;
    bool  m_inUserClick = false;   // true only during a user-initiated toggle
};

// ---------------------------------------------------------------------------
// CheckRow — a full-width clickable row with a painted rounded checkbox (blue
// square + checkmark when on) and a label, plus an optional trailing preview
// widget. Matches the Features-tab rows in the design. Emits toggled(bool); the
// caller wires the persistence handler exactly as the old QCheckBox did.
// ---------------------------------------------------------------------------
class CheckRow : public QAbstractButton {
    Q_OBJECT
public:
    CheckRow(const DesignTokens& tk, const QString& label, bool checkedState,
             QWidget* trailing = nullptr, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    // Position the trailing preview chip: right-aligned to a shared column and
    // vertically centered on the SAME height() used to paint the checkbox, so
    // the chip's center always matches the checkbox's center (height()/2).
    void positionTrailing();

    DesignTokens m_tk;
    QString  m_label;
    QWidget* m_trailing = nullptr;
    bool     m_hover = false;
};

// ---------------------------------------------------------------------------
// AppearanceSegment — 3-way segmented switch (Auto / Light / Dark) matching the
// design: a 240x32 rounded, bordered control with an accent-weak "pill" that
// slides under the active segment, each segment showing a small glyph + label.
// The active segment's text/icon paints in the accent color, the rest in the
// dim (text2) color. Emits selected(int) where 0=Auto, 1=Light, 2=Dark.
// Index ↔ Settings::appearance: 0="auto", 1="light", 2="dark".
// ---------------------------------------------------------------------------
class AppearanceSegment : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal pillPos READ pillPos WRITE setPillPos)
public:
    AppearanceSegment(const DesignTokens& tk, int initial,
                      const QString& autoText, const QString& lightText,
                      const QString& darkText, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    int currentIndex() const { return m_index; }
    void setIndex(int i, bool animate = true);
    qreal pillPos() const { return m_pillPos; }
    void setPillPos(qreal v);

signals:
    void selected(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    int hitTest(const QPoint& p) const;   // which segment (0..2) is under p, or -1

    DesignTokens m_tk;
    QString m_labels[3];
    int   m_index = 0;
    int   m_hover = -1;
    qreal m_pillPos = 0.0;   // animated segment index position (0..2)
};

// ---------------------------------------------------------------------------
// HotkeyRecorder — push button that captures the next key combo.
// Source: SettingsWindowController.swift HotkeyRecorder (Spec 5 §7).
//
// Click -> "Press keys…"; next keyDown captures. Esc cancels (restores previous
// display). At least ONE modifier required, else beep and keep recording.
// The captured display is platform-correct (mac glyphs ⌃⌥⇧⌘ vs. Windows/Linux
// "Ctrl+Shift+<KEY>"); see Settings::hotKeyDisplayString.
// Emits captured(carbonKeyCode, carbonModifiers, display).
// ---------------------------------------------------------------------------
class HotkeyRecorder : public QPushButton {
    Q_OBJECT
public:
    explicit HotkeyRecorder(QWidget* parent = nullptr);

    // Apply theme-token styling (idle look). Called on (re)build so the field
    // matches the surrounding card in both light and dark themes.
    void applyTokens(const DesignTokens& tk);

signals:
    void captured(uint32_t carbonKeyCode, uint32_t carbonModifiers,
                  const QString& display);

protected:
    void keyPressEvent(QKeyEvent*) override;
    // Cancel recording if focus leaves the field (click elsewhere / a rebuild):
    // otherwise it stayed armed showing "Press keys…" forever with no key able to
    // reach it.
    void focusOutEvent(QFocusEvent*) override;

private:
    void startRecording();
    // Build a Carbon modifier mask from Qt::KeyboardModifiers.
    static uint32_t carbonModifiers(Qt::KeyboardModifiers mods);
    // Build the platform-correct display string (glyphs on macOS, spelled-out
    // "Ctrl+Shift+<KEY>" on Windows/Linux) via Settings::hotKeyDisplayString.
    static QString displayString(Qt::KeyboardModifiers mods, const QString& keyText);
    // Translate a Qt::Key (+ native VK fallback) to a Carbon virtual-key code,
    // so the persisted hotKeyCode matches the macOS defaults file (kVK_*).
    static uint32_t carbonKeyCode(int qtKey, quint32 nativeVK);

    bool m_recording = false;
    QString m_idleStyle;       // theme-derived idle stylesheet
    QString m_recordingStyle;  // theme-derived recording stylesheet
};
