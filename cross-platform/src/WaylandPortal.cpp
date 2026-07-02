// WaylandPortal.cpp — Wayland screen capture via xdg-desktop-portal.
//
// On a Wayland session QScreen::grabWindow(0) returns a null pixmap (clients may
// not read the framebuffer), so the only sanctioned way to grab the screen is the
// org.freedesktop.portal.Screenshot D-Bus portal. This file is compiled ONLY on
// Linux with QtDBus available (see CMakeLists UNIX branch); it exposes a single
// plain-C++-linkage entry point that ScreenCapture.cpp calls, mirroring the
// MacNative_* extern pattern.
//
// The Screenshot call is asynchronous: it returns a Request object path and later
// emits a Response signal carrying the URI of a written PNG. We predict the
// Request path, subscribe before calling (to avoid the fire-before-connect race),
// and spin a local event loop until the Response arrives (interactive=false, so a
// short wait; a hard timeout guards a hung/absent portal).

#include <QGuiApplication>
#include <QImage>
#include <QUrl>
#include <QFile>
#include <QEventLoop>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <QRandomGenerator>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>

namespace {

// Catches the portal Request's Response signal (a string-based D-Bus connect
// needs a real slot, hence a tiny Q_OBJECT here).
class PortalResponse : public QObject {
    Q_OBJECT
public:
    QString uri;
    uint    code = 1;      // 0 = success; non-zero = user cancelled / other
    bool    received = false;
public slots:
    void onResponse(uint response, const QVariantMap& results) {
        if (received) return;   // ignore duplicates (we may subscribe twice)
        code = response;
        if (response == 0) {
            QVariant v = results.value(QStringLiteral("uri"));
            if (v.canConvert<QDBusVariant>()) v = v.value<QDBusVariant>().variant();
            uri = v.toString();
        }
        received = true;
        emit done();
    }
signals:
    void done();
};

void subscribe(QDBusConnection& bus, const QString& path, PortalResponse* h) {
    if (path.isEmpty()) return;
    bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), path,
                QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"),
                h, SLOT(onResponse(uint,QVariantMap)));
}

} // namespace

// Returns the full desktop as a QImage (native pixels), or a null QImage on any
// failure (no portal, denied, timeout). ScreenCapture.cpp crops per QScreen.
QImage LightGet_captureDesktopViaPortal() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return QImage();

    // Unique token + predicted Request path (portal spec:
    // /org/freedesktop/portal/desktop/request/<SENDER>/<TOKEN>, SENDER = unique
    // bus name sans leading ':' with '.'→'_').
    const QString token = QStringLiteral("lightget_%1")
        .arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService();
    if (sender.startsWith(QLatin1Char(':'))) sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
            .arg(sender, token);

    PortalResponse handler;
    subscribe(bus, predictedPath, &handler);

    QVariantMap options;
    options.insert(QStringLiteral("interactive"), false);
    options.insert(QStringLiteral("handle_token"), token);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Screenshot"),
        QStringLiteral("Screenshot"));
    msg << QString() << options;   // parent_window (none), options

    QDBusReply<QDBusObjectPath> reply = bus.call(msg);
    if (!reply.isValid()) return QImage();

    // Newer portals return the real Request path; subscribe to it too if it
    // differs from our prediction.
    const QString actual = reply.value().path();
    if (actual != predictedPath) subscribe(bus, actual, &handler);

    if (!handler.received) {
        QEventLoop loop;
        QObject::connect(&handler, &PortalResponse::done, &loop, &QEventLoop::quit);
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(30000);   // hard cap so a broken/absent portal can't hang us
        loop.exec();
    }

    if (handler.code != 0 || handler.uri.isEmpty()) return QImage();

    const QString path = QUrl(handler.uri).toLocalFile();
    if (path.isEmpty()) return QImage();
    QImage img(path);
    QFile::remove(path);   // portal wrote a temp file; done with it after loading
    return img;
}

#include "WaylandPortal.moc"
