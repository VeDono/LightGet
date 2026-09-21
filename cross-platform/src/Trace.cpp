#include "Trace.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace Trace {
namespace {

// Milliseconds since the first call — i.e. roughly since process start, since
// enabled() is consulted before anything else does work.
QElapsedTimer& clock() {
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t;
}

// Nesting depth, so a capture's sub-phases read as a tree.
int& depth() {
    static int d = 0;
    return d;
}

}  // namespace

bool enabled() {
    static const bool on = qgetenv("LIGHTGET_TRACE").trimmed() == "1";
    return on;
}

QString logPath() {
    static const QString p =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("lightget-trace.log"));
    return p;
}

// Shared writer. `force` is what lets note() through while tracing is off.
static void write(const QString& line, bool force);

void log(const QString& line) { write(line, false); }

void note(const QString& line) { write(line, true); }

static void write(const QString& line, bool force) {
    if (!force && !enabled()) return;

    // Opened per line and closed again: tracing is rare, and a crash mid-capture
    // must not cost us the lines written before it.
    QFile f(logPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&f);

    static bool header = false;
    if (!header) {
        header = true;
        out << "\n=== LightGet trace, session started "
            << QDateTime::currentDateTime().toString(Qt::ISODate) << " ===\n";
    }
    out << QStringLiteral("%1 ms  %2%3\n")
               .arg(clock().elapsed(), 7)
               .arg(QString(depth() * 2, QLatin1Char(' ')), line);
}

Scope::Scope(const char* name) : m_name(name), m_on(name && enabled()) {
    if (!m_on) return;
    log(QStringLiteral("> %1").arg(QLatin1String(name)));
    ++depth();
    m_timer.start();
}

void Scope::mark(const QString& what) {
    if (!m_on) return;
    log(QStringLiteral(". %1 (+%2 ms)").arg(what).arg(m_timer.elapsed()));
}

Scope::~Scope() {
    if (!m_on) return;
    const qint64 ms = m_timer.elapsed();
    --depth();
    log(QStringLiteral("< %1: %2 ms").arg(QLatin1String(m_name)).arg(ms));
}

}  // namespace Trace
