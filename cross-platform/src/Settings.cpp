// Settings.cpp — QSettings-backed persistence + feature/per-tool toggles.
//
// Faithful port of Settings.swift (Spec 5 §1). Default semantics mirror the
// macOS app's UserDefaults behavior so existing keys migrate 1:1:
//   - boolean tool/feature flags default TRUE when the key is absent;
//   - downscaleRetina defaults FALSE;
//   - saveFolderPath / barIconCustomPath store nullopt by REMOVING the key.

#include "Settings.h"

#include <QStringList>

namespace {
// Persisted key names — must match the macOS app verbatim for migration parity.
constexpr char kKeyCode[]       = "hotKeyCode";
constexpr char kModifiers[]     = "hotKeyModifiers";
constexpr char kDisplay[]       = "hotKeyDisplay";
constexpr char kDim[]           = "dimOpacity";
constexpr char kDownscale[]     = "downscaleRetina";
constexpr char kAnimatedDim[]   = "animatedDim";
constexpr char kLanguage[]      = "language";
constexpr char kSaveFolder[]    = "saveFolder";
constexpr char kBarIcon[]       = "barIcon";
constexpr char kBarIconCustom[] = "barIconCustom";
constexpr char kShowColors[]    = "showColors";
constexpr char kTextAlign[]     = "textAlign";
constexpr char kTextBg[]        = "textBg";
} // namespace

Settings& Settings::instance() {
    static Settings s;
    return s;
}

Settings::Settings() = default;

// --- Hotkey ---

uint32_t Settings::hotKeyCode() const {
    return m_s.value(kKeyCode,
                     static_cast<int>(CarbonKeys::kVK_ANSI_2)).toUInt();
}

void Settings::setHotKeyCode(uint32_t v) {
    m_s.setValue(kKeyCode, static_cast<int>(v));
}

uint32_t Settings::hotKeyModifiers() const {
    return m_s.value(kModifiers,
                     static_cast<int>(CarbonKeys::cmdKey | CarbonKeys::shiftKey))
        .toUInt();
}

void Settings::setHotKeyModifiers(uint32_t v) {
    m_s.setValue(kModifiers, static_cast<int>(v));
}

QString Settings::hotKeyDisplay() const {
    // Default (no stored value, e.g. fresh install) renders the canonical default
    // combo cmdKey|shiftKey + "2" in platform-correct form: "Ctrl+Shift+2" on
    // Windows/Linux, "⇧⌘2" on macOS. A previously recorded value is shown as-is.
    const QString def =
        hotKeyDisplayString(CarbonKeys::cmdKey | CarbonKeys::shiftKey,
                            QStringLiteral("2"));
    return m_s.value(kDisplay, def).toString();
}

void Settings::setHotKeyDisplay(const QString& v) {
    m_s.setValue(kDisplay, v);
}

QString Settings::hotKeyDisplayString(uint32_t carbonModifiers,
                                      const QString& keyText) {
    using namespace CarbonKeys;
    const QString key = keyText.isEmpty() ? QStringLiteral("?") : keyText;

#if defined(Q_OS_MACOS)
    // macOS keeps the native glyph form with no separators. Glyph order: ⌃ ⌥ ⇧ ⌘.
    QString s;
    if (carbonModifiers & controlKey) s += QStringLiteral("⌃"); // ⌃ control
    if (carbonModifiers & optionKey)  s += QStringLiteral("⌥"); // ⌥ option
    if (carbonModifiers & shiftKey)   s += QStringLiteral("⇧"); // ⇧ shift
    if (carbonModifiers & cmdKey)     s += QStringLiteral("⌘"); // ⌘ command
    s += key;
    return s;
#else
    // Windows / Linux: spell out modifier names joined with "+", matching the
    // combo that is ACTUALLY registered. The GlobalHotkey backends fold BOTH
    // Carbon cmdKey and controlKey onto the platform Ctrl (MOD_CONTROL on
    // Windows, ControlMask on X11), so both render as "Ctrl" here — that is why
    // the persisted "⇧⌘2" default reads as "Ctrl+Shift+2" on these platforms.
    // ("Win"/"Super" is reserved for a true Meta bit, which the recorder maps to
    // controlKey and is therefore not emitted today; kept for completeness.)
    QStringList parts;
    if (carbonModifiers & (cmdKey | controlKey)) parts << QStringLiteral("Ctrl");
    if (carbonModifiers & optionKey)             parts << QStringLiteral("Alt");
    if (carbonModifiers & shiftKey)              parts << QStringLiteral("Shift");
    parts << key;
    return parts.join(QLatin1Char('+'));
#endif
}

// --- Capture / output ---

double Settings::dimOpacity() const {
    return m_s.value(kDim, 0.45).toDouble();
}

void Settings::setDimOpacity(double v) {
    m_s.setValue(kDim, v);
}

bool Settings::downscaleRetina() const {
    // Mirrors UserDefaults.bool(forKey:): default false when absent.
    return m_s.value(kDownscale, false).toBool();
}

void Settings::setDownscaleRetina(bool v) {
    m_s.setValue(kDownscale, v);
}

bool Settings::animatedDim() const {
    // Default false: dim layer appears/disappears instantly (current behavior).
    return m_s.value(kAnimatedDim, false).toBool();
}

void Settings::setAnimatedDim(bool v) {
    m_s.setValue(kAnimatedDim, v);
}

// --- General ---

QString Settings::language() const {
    return m_s.value(kLanguage, QStringLiteral("en")).toString();
}

void Settings::setLanguage(const QString& v) {
    m_s.setValue(kLanguage, v);
}

std::optional<QString> Settings::saveFolderPath() const {
    if (!m_s.contains(kSaveFolder))
        return std::nullopt;
    return m_s.value(kSaveFolder).toString();
}

void Settings::setSaveFolderPath(const std::optional<QString>& v) {
    if (v.has_value())
        m_s.setValue(kSaveFolder, *v);
    else
        m_s.remove(kSaveFolder);
}

QString Settings::barIcon() const {
    return m_s.value(kBarIcon, QStringLiteral("scissors")).toString();
}

void Settings::setBarIcon(const QString& v) {
    m_s.setValue(kBarIcon, v);
}

std::optional<QString> Settings::barIconCustomPath() const {
    if (!m_s.contains(kBarIconCustom))
        return std::nullopt;
    return m_s.value(kBarIconCustom).toString();
}

void Settings::setBarIconCustomPath(const std::optional<QString>& v) {
    if (v.has_value())
        m_s.setValue(kBarIconCustom, *v);
    else
        m_s.remove(kBarIconCustom);
}

// --- Per-tool enable ---

bool Settings::isToolEnabled(Tool t) const {
    // Select is always enabled and is never persisted.
    if (t == Tool::Select)
        return true;
    const QString key = QStringLiteral("tool_") + QString::fromUtf8(toolKey(t));
    return m_s.value(key, true).toBool();
}

void Settings::setToolEnabled(Tool t, bool enabled) {
    const QString key = QStringLiteral("tool_") + QString::fromUtf8(toolKey(t));
    m_s.setValue(key, enabled);
}

// --- Feature flags (default true when absent) ---

bool Settings::showColorPalette() const {
    return m_s.value(kShowColors, true).toBool();
}

void Settings::setShowColorPalette(bool v) {
    m_s.setValue(kShowColors, v);
}

bool Settings::textAlignmentEnabled() const {
    return m_s.value(kTextAlign, true).toBool();
}

void Settings::setTextAlignmentEnabled(bool v) {
    m_s.setValue(kTextAlign, v);
}

bool Settings::textBackgroundEnabled() const {
    return m_s.value(kTextBg, true).toBool();
}

void Settings::setTextBackgroundEnabled(bool v) {
    m_s.setValue(kTextBg, v);
}
