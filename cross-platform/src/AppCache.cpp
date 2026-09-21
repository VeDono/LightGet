#include "AppCache.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace AppCache {
namespace {

// MATCHED BY PREFIX, not by an enumerated list of asset names.
//
// The obvious implementation asks the updater which asset THIS build downloads
// and deletes that. It leaves things behind in two cases that both happen in
// practice: a user who moved between the installer and the portable zip has the
// other kind sitting there too, and a release-workflow rename silently turns the
// button into a no-op for every file published before the change. Everything the
// app writes into the temp directory starts with "LightGet" — the release assets,
// the LightGetUpdate staging folder and lightget-trace.log alike — and nothing
// else does, so matching the prefix covers all of it and keeps working when the
// asset names change.
//
// The match is case-insensitive on Windows and case-sensitive elsewhere, which is
// exactly how the filesystem behaves; the trace log is lowercase, so on Linux and
// macOS it is matched by the second pattern rather than the first.
const QStringList& patterns() {
    static const QStringList p{QStringLiteral("LightGet*"), QStringLiteral("lightget*")};
    return p;
}

QString tempDir() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
}

qint64 sizeOf(const QString& path) {
    const QFileInfo fi(path);
    if (fi.isFile()) return fi.size();
    if (!fi.isDir()) return 0;
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// The most recent modification anywhere at `path`. For a directory that means the
// newest file inside it, not the directory's own stamp: on Windows a folder's
// mtime does not follow edits made deeper in the tree, so trusting it would let
// the sweep delete a staging folder an update is still filling.
QDateTime newestStamp(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.isDir()) return fi.lastModified();

    QDateTime newest = fi.lastModified();
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QDateTime stamp = it.fileInfo().lastModified();
        if (stamp > newest) newest = stamp;
    }
    return newest;
}

}  // namespace

QStringList existingPaths() {
    QDir tmp(tempDir());
    if (!tmp.exists()) return {};

    QStringList out;
    const QFileInfoList entries = tmp.entryInfoList(
        patterns(),
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& fi : entries) {
        // The two patterns overlap on a case-insensitive filesystem, so the same
        // entry can be listed twice.
        const QString path = fi.absoluteFilePath();
        if (!out.contains(path)) out << path;
    }
    return out;
}

QStringList pathsOlderThan(int days) {
    if (days <= 0) return {};          // 0 = the sweep is off; never match anything
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);

    QStringList out;
    for (const QString& p : existingPaths()) {
        const QDateTime stamp = newestStamp(p);
        // An unreadable stamp is treated as "recent" so a file we cannot judge is
        // kept rather than deleted on a guess.
        if (stamp.isValid() && stamp < cutoff) out << p;
    }
    return out;
}

qint64 totalBytes(const QStringList& paths) {
    qint64 total = 0;
    for (const QString& p : paths) total += sizeOf(p);
    return total;
}

qint64 totalBytes() {
    return totalBytes(existingPaths());
}

QString humanSize(qint64 bytes) {
    constexpr qint64 kKB = 1024;
    constexpr qint64 kMB = 1024 * 1024;
    if (bytes >= kMB)
        return QStringLiteral("%1 MB").arg(double(bytes) / double(kMB), 0, 'f', 1);
    // Anything under a megabyte reads better whole than as "0.1 MB".
    return QStringLiteral("%1 KB").arg((bytes + kKB - 1) / kKB);
}

bool remove(const QStringList& paths) {
    bool ok = true;
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (fi.isDir()) {
            if (!QDir(p).removeRecursively()) ok = false;
        } else if (!QFile::remove(p)) {
            ok = false;
        }
    }
    return ok;
}

bool clear() {
    return remove(existingPaths());
}

}  // namespace AppCache
