#pragma once

// Localization.h — Runtime EN/RU/UK localization via an in-memory table.
//
// Source: Localization.swift (Spec 5 §2).
//
// DESIGN: intentionally NOT QTranslator/tr() — language switches instantly at
// runtime without relaunch, so the UI rebuild (reloadUI) re-renders immediately.
// Three-level fallback (must replicate exactly):
//     requested language -> "en" -> the raw key itself.
//
// The full string table (EN/RU/UK) lives in Localization.cpp. UTF-8 strings
// contain \n, curly quotes, arrows (->), modifier glyphs, and the 🇺🇦 emoji —
// preserve them verbatim. Current language is read from Settings::language().

#include <QString>
#include <QHash>

namespace Loc {

// Look up a key for the current Settings language with EN/key fallback.
QString t(const QString& key);

// The backing table: key -> { langCode -> translation }. Defined in .cpp.
const QHash<QString, QHash<QString, QString>>& table();

} // namespace Loc
