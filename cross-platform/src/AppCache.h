#pragma once

// AppCache.h — the disposable files LightGet leaves in the temp directory.
//
// Two kinds accumulate. The updater downloads a release asset there (~50 MB for
// the Windows zip) and unpacks it into a staging folder beside it; neither is
// cleaned up afterwards, and not by oversight: by the time the update has been
// applied, the process that would have deleted them has already exited to let the
// new build replace it. Separately, the slow-capture diagnostic appends to a log
// in the same place and never truncates it.
//
// None of it is needed for the app to run — it is all re-created on demand — so
// the settings window offers a manual purge plus an optional automatic one.
//
// DELIBERATELY NOT INCLUDED: QSettings. Losing a hotkey or a save folder is not
// "clearing a cache", and no timer should ever be able to do it.
//
// WHY THE AUTOMATIC SWEEP IS AGE-BASED. The obvious design deletes everything on
// a schedule, which quietly makes the app worse at the one job the log exists for:
// a capture that took seconds cannot be reproduced on demand, so the evidence has
// to survive until somebody reads it. A sweep that only removes entries nothing
// has touched for N days never eats a log written this morning, while still
// keeping months-old archives from piling up.

#include <QString>
#include <QStringList>

namespace AppCache {

// Absolute paths of the cache entries that currently EXIST — files and
// directories both. Empty when there is nothing to clean.
QStringList existingPaths();

// The subset of existingPaths() that nothing has modified for at least `days`
// days. A directory counts as modified as recently as the newest file anywhere
// inside it, so an update being unpacked right now is never swept.
QStringList pathsOlderThan(int days);

// Combined size of `paths` in bytes; directories counted recursively.
qint64 totalBytes(const QStringList& paths);

// Combined size of everything: totalBytes(existingPaths()).
qint64 totalBytes();

// Compact size for a button label: "52.4 MB", "812 KB", "0 KB".
QString humanSize(qint64 bytes);

// Delete every listed entry. Returns false when at least one survived — an update
// in progress still holds its zip open — so the caller can say so instead of
// claiming a purge that did not happen.
bool remove(const QStringList& paths);

// Everything, now: remove(existingPaths()).
bool clear();

}  // namespace AppCache
