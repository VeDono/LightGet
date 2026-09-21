// Trace.h — opt-in phase timing, for diagnosing slowness on a machine we cannot
// attach a profiler to (chiefly Windows under game load).
//
// OFF unless the environment variable LIGHTGET_TRACE is set to 1. When off every
// entry point is a single cached bool test, so instrumentation can sit in hot
// paths permanently without costing anything.
//
// Output goes to a FILE, not stdout: a Windows GUI build has no console, so
// qDebug() would vanish. The path is reported by Trace::logPath().

#pragma once

#include <QElapsedTimer>
#include <QString>

namespace Trace {

// True when LIGHTGET_TRACE=1. Read once, cached.
bool enabled();

// Where the log is written (…/lightget-trace.log in the temp directory).
QString logPath();

// Append one line, prefixed with the milliseconds since the app started.
void log(const QString& line);

// Append one line EVEN WHEN TRACING IS OFF. Reserved for anomalies worth
// recording without the user having had to predict them -- a capture that took
// seconds is exactly that: it cannot be reproduced on demand, it happens on
// somebody else's machine under somebody else's game, and asking them to arm a
// diagnostic first and then reproduce it is asking for the thing we already know
// is hard. Writes nothing on a normal, fast capture.
void note(const QString& line);

// Timestamps a phase and writes "<name>: <ms>" when it goes out of scope.
// Nested scopes are indented, so a capture reads as a tree.
class Scope {
public:
    explicit Scope(const char* name);   // nullptr => does nothing
    ~Scope();

    // Note an intermediate step without ending the scope.
    void mark(const QString& what);

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char*   m_name;
    QElapsedTimer m_timer;
    bool          m_on;
};

}  // namespace Trace

// Times the enclosing block. Compiles to nothing observable when tracing is off.
#define LG_TRACE_CAT2(a, b) a##b
#define LG_TRACE_CAT(a, b)  LG_TRACE_CAT2(a, b)
#define LG_TRACE(name) ::Trace::Scope LG_TRACE_CAT(lgScope_, __LINE__)(name)
