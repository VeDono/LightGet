#pragma once

// SettingsWindow.h — Tabbed settings window (General + Features) + hotkey recorder.
//
// Source: SettingsWindowController.swift (Spec 5 §4-§13).
//
// Fixed-size 440x580 QDialog, not resizable, persists across open/close (reused
// instance). Two tabs only (General, Features). NO Apply/OK — every change
// persists to Settings immediately. A full rebuild (reloadUI) happens on
// language change / any reset / choosing folder or custom icon.
//
// COORDINATE NOTE: the Swift layout used bottom-left origin with hand-computed
// y values. In Qt prefer QVBoxLayout/QFormLayout/QGridLayout — the absolute
// y math from the spec is documentation, not a layout requirement. The About
// section's social card + copyright row should still match visually.
//
// Three callbacks the owner (TrayApp) wires to:
//   hotKeyChanged  -> re-register global hotkey + update tray "Capture" text
//   languageChanged-> rebuild tray menu + retranslate
//   barIconChanged -> update tray icon

#include "Annotation.h"

#include <QDialog>
#include <QPushButton>
#include <cstdint>

class QTabWidget;
class HotkeyRecorder;

class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget* parent = nullptr);

    void showCentered();    // center + raise + activate (accessory-app focus dance)

signals:
    void hotKeyChanged();
    void languageChanged();
    void barIconChanged();

private:
    // Reset targets (raw int parity with Swift ResetTarget). language=0 is a
    // dead UI case (no button) but kept for parity.
    enum class ResetTarget { Language = 0, Hotkey = 1, Dim = 2,
                             Downscale = 3, SaveFolder = 4 };

    void reloadUI();                 // full teardown + rebuild of both tabs
    void buildUI();
    QWidget* buildGeneralTab();
    QWidget* buildFeaturesTab();
    void addAboutSection(QWidget* generalTab);

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

    QTabWidget* m_tabs = nullptr;
    HotkeyRecorder* m_recorder = nullptr;    // reused member across rebuilds
    QWidget* m_aboutContainer = nullptr;     // About section widget (rebuilt each reload)

    // Preset bar icons (asset names mirroring SF Symbols), exactly 5 (Spec 5 §5.2).
    // {"scissors","camera.viewfinder","crop","rectangle.dashed","paintbrush.pointed.fill"}
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

signals:
    void captured(uint32_t carbonKeyCode, uint32_t carbonModifiers,
                  const QString& display);

protected:
    void keyPressEvent(QKeyEvent*) override;

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
};
