#pragma once

// Settings.h — Persistent settings + feature toggles, backed by QSettings.
//
// Source: Settings.swift (Spec 5 §1). Singleton wrapper over QSettings.
//
// KEY SEMANTICS to preserve:
//  - Most booleans default to TRUE when the key is ABSENT (tools, showColors,
//    textAlign, textBg). downscaleRetina defaults FALSE.
//  - saveFolderPath / barIconCustomPath: setting nullopt REMOVES the key
//    (do not store an empty string). Reading an absent key -> nullopt.
//  - Persist the same string keys as the macOS app for migration compatibility.
//  - Hotkey code/modifiers are stored as Carbon virtual-key / modifier ints
//    (kVK_ANSI_2 = 19, cmdKey|shiftKey = 768). The GlobalHotkey backend
//    translates these per-OS; persist the raw ints for default-file parity.

#include "Annotation.h"

#include <QString>
#include <QSettings>
#include <cstdint>
#include <optional>

// Carbon virtual-key / modifier constants (hardcoded; no Carbon off-macOS).
//
// MODIFIER SEMANTICS across platforms (what the recorder stores, what each
// GlobalHotkey backend registers):
//   cmdKey     -> macOS ⌘        | Windows Ctrl   | X11 Control
//   controlKey -> macOS ⌃        | Windows Win    | X11 Super (Mod4)
//   optionKey  -> macOS ⌥        | Windows Alt    | X11 Alt (Mod1)
//   shiftKey   -> macOS/Win/X11 Shift
// (Qt reports ⌘ as ControlModifier and ⌃ as MetaModifier on macOS, so the
// recorder's Control->cmdKey / Meta->controlKey mapping lines the two up.)
namespace CarbonKeys {
constexpr uint32_t kVK_ANSI_2 = 0x13;   // 19
constexpr uint32_t kVK_Escape = 0x35;   // 53
constexpr uint32_t cmdKey     = 0x0100; // 256
constexpr uint32_t shiftKey   = 0x0200; // 512
constexpr uint32_t optionKey  = 0x0800; // 2048
constexpr uint32_t controlKey = 0x1000; // 4096
}

// Persisted hotkey KEY code space.
//
// The base representation is a macOS Carbon virtual-key code (kVK_*) — the format
// the native app writes, kept for compatibility. Carbon cannot name every key a PC
// keyboard has (PrintScreen, Pause, numpad, F13-F24, OEM punctuation), and on
// Windows the CHARACTER a key produces depends on the layout and on AltGr — which
// is why combos like Ctrl+Alt+2 or a bare PrintScreen could not be assigned at all.
// Two tagged spaces extend the format so any key can be stored losslessly:
//
//   <no flag>     payload = Carbon kVK_* code (legacy + macOS recordings)
//   kWinVkFlag    payload = Windows virtual-key code (VK_*), layout-independent
//   kQtKeyFlag    payload = Qt::Key value (portable fallback, used on X11)
//
// Each backend decodes the tag at its edge; unmappable combinations fail cleanly
// through registerHotkey()'s bool (TrayApp surfaces + rolls those back).
namespace HotKeyCode {
constexpr uint32_t kWinVkFlag   = 0x40000000u;
constexpr uint32_t kQtKeyFlag   = 0x20000000u;
constexpr uint32_t kPayloadMask = 0x01FFFFFFu;   // fits every Qt::Key value
inline bool isWinVk(uint32_t code) { return (code & kWinVkFlag) != 0; }
inline bool isQtKey(uint32_t code) { return (code & kQtKeyFlag) != 0; }
inline uint32_t payload(uint32_t code) { return code & kPayloadMask; }
}

class Settings {
public:
    static Settings& instance();

    // --- Hotkey ---
    uint32_t hotKeyCode() const;             // default kVK_ANSI_2 (19)
    void setHotKeyCode(uint32_t v);
    uint32_t hotKeyModifiers() const;        // default cmdKey|shiftKey (768)
    void setHotKeyModifiers(uint32_t v);
    // Default display is platform-correct: "Ctrl+Shift+2" (Win/Linux) / "⇧⌘2" (mac).
    QString hotKeyDisplay() const;
    void setHotKeyDisplay(const QString& v);

    // Format a human-readable shortcut string from a Carbon modifier mask + key
    // text, rendered per-platform so users see the keys they actually press:
    //   - Windows: Ctrl / Alt / Shift / Win  (cmdKey & controlKey both -> Ctrl)
    //   - Linux:   Ctrl / Alt / Shift / Super
    //   - macOS:   ⌃ ⌥ ⇧ ⌘  glyphs
    // The underlying registered combo (Carbon code/mods) is unchanged; this is
    // display only. Shared by the recorder, the reset action, and the default.
    static QString hotKeyDisplayString(uint32_t carbonModifiers,
                                       const QString& keyText);

    // --- Capture / output ---
    double dimOpacity() const;               // default 0.45 (slider 0.10..0.85)
    void setDimOpacity(double v);
    bool downscaleRetina() const;            // default false (save at 1x)
    void setDownscaleRetina(bool v);
    bool animatedDim() const;                // default false (instant dim/teardown)
    void setAnimatedDim(bool v);
    // Ask GitHub for a newer release when the app starts (default true). A single
    // unauthenticated request to the public releases API; no telemetry.
    bool updateCheckOnLaunch() const;
    void setUpdateCheckOnLaunch(bool v);

    // --- General ---
    QString language() const;                // default "en"; one of en/ru/uk
    void setLanguage(const QString& v);

    // Appearance / color scheme: "auto" (follow OS), "light", or "dark".
    // Default "auto". Applied to QGuiApplication::styleHints()->setColorScheme()
    // at startup (main.cpp) and on toggle (SettingsWindow).
    QString appearance() const;              // default "auto"
    void setAppearance(const QString& v);

    // A release the user chose to skip. While the newest release on GitHub is not
    // newer than this, the automatic check stays quiet; an explicit "Check for
    // Updates" still offers it, so the choice is never a dead end.
    QString skippedUpdateVersion() const;            // empty = nothing skipped
    void setSkippedUpdateVersion(const QString& v);

    std::optional<QString> saveFolderPath() const;   // nullopt = ask every time
    void setSaveFolderPath(const std::optional<QString>& v); // nullopt removes key

    QString barIcon() const;                 // default "scissors" (icon asset name)
    void setBarIcon(const QString& v);

    std::optional<QString> barIconCustomPath() const; // nullopt = use barIcon
    void setBarIconCustomPath(const std::optional<QString>& v); // nullopt removes key

    // --- Per-tool enable (default true; Select is ALWAYS true, never stored) ---
    bool isToolEnabled(Tool t) const;
    void setToolEnabled(Tool t, bool enabled);

    // --- Feature flags (default true when absent) ---
    bool showColorPalette() const;           // key "showColors"
    void setShowColorPalette(bool v);
    bool textAlignmentEnabled() const;       // key "textAlign"
    void setTextAlignmentEnabled(bool v);
    bool textBackgroundEnabled() const;      // key "textBg"
    void setTextBackgroundEnabled(bool v);

    // --- Text style flags (default true when absent) ---
    // Stored toggles that gate future text-tool typography controls. They are
    // surfaced as checkbox rows on the Features tab ("Text options"); not yet
    // consumed by the toolbar/text editor, but persisted so the UI is stateful.
    bool textFontEnabled() const;            // key "textFont"
    void setTextFontEnabled(bool v);
    bool textFontSizeEnabled() const;        // key "textFontSize"
    void setTextFontSizeEnabled(bool v);
    bool textBoldEnabled() const;            // key "textBold"
    void setTextBoldEnabled(bool v);
    bool textItalicEnabled() const;          // key "textItalic"
    void setTextItalicEnabled(bool v);
    bool textUnderlineEnabled() const;       // key "textUnderline"
    void setTextUnderlineEnabled(bool v);

private:
    Settings();
    QSettings m_s;
};
