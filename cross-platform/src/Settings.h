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
namespace CarbonKeys {
constexpr uint32_t kVK_ANSI_2 = 0x13;   // 19
constexpr uint32_t kVK_Escape = 0x35;   // 53
constexpr uint32_t cmdKey     = 0x0100; // 256
constexpr uint32_t shiftKey   = 0x0200; // 512
constexpr uint32_t optionKey  = 0x0800; // 2048
constexpr uint32_t controlKey = 0x1000; // 4096
}

class Settings {
public:
    static Settings& instance();

    // --- Hotkey ---
    uint32_t hotKeyCode() const;             // default kVK_ANSI_2 (19)
    void setHotKeyCode(uint32_t v);
    uint32_t hotKeyModifiers() const;        // default cmdKey|shiftKey (768)
    void setHotKeyModifiers(uint32_t v);
    QString hotKeyDisplay() const;           // default "⇧⌘2"
    void setHotKeyDisplay(const QString& v);

    // --- Capture / output ---
    double dimOpacity() const;               // default 0.45 (slider 0.10..0.85)
    void setDimOpacity(double v);
    bool downscaleRetina() const;            // default false (save at 1x)
    void setDownscaleRetina(bool v);

    // --- General ---
    QString language() const;                // default "en"; one of en/ru/uk
    void setLanguage(const QString& v);

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

private:
    Settings();
    QSettings m_s;
};
