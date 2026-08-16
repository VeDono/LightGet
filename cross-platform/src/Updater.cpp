#include "Updater.h"

#include "Localization.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QAbstractButton>
#include <QProcess>
#include <QPushButton>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#ifndef LIGHTGET_VERSION
#define LIGHTGET_VERSION "0.0.0"
#endif

#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
// Implemented in mac/MacNative.mm. MUST be declared at file scope: inside the
// anonymous namespace below it would get internal linkage and fail to link.
extern void MacNative_activateApp();
#endif

namespace {

// Public, unauthenticated endpoint — the repo is public, so no token is involved.
constexpr char kLatestReleaseApi[] =
    "https://api.github.com/repos/VeDono/LightGet/releases/latest";

// Which release asset this build should download.
QString wantedAssetPattern() {
#if defined(Q_OS_WIN)
    return QStringLiteral("LightGet-Setup-Windows");   // Inno installer (.exe)
#elif defined(Q_OS_MACOS)
    return QStringLiteral("LightGet-macOS");           // .app in a .zip
#else
    return QStringLiteral("LightGet-x86_64.AppImage");
#endif
}

QList<int> versionParts(QString v) {
    v = v.trimmed();
    if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) v.remove(0, 1);
    QList<int> parts;
    const QStringList chunks = v.split(QLatin1Char('.'));
    for (const QString& c : chunks) {
        bool ok = false;
        // Tolerate suffixes such as "1.0.7-beta2" by taking the leading digits.
        QString digits;
        for (QChar ch : c) { if (!ch.isDigit()) break; digits += ch; }
        const int n = digits.toInt(&ok);
        if (!ok) return {};
        parts << n;
    }
    return parts;
}

// LightGet is an accessory/tray app, so a dialog it opens can end up BEHIND the
// window the user is looking at (the same class of bug as the tray menu's first
// click). Bring the app forward and raise the box once the modal loop starts.
void presentDialog(QDialog& box) {
#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)
    MacNative_activateApp();
#endif
    QTimer::singleShot(0, &box, [&box]() {
        box.raise();
        box.activateWindow();
    });
}

} // namespace

Updater::Updater(QObject* parent) : QObject(parent) {
    m_net = new QNetworkAccessManager(this);
}

QString Updater::currentVersion() {
    return QString::fromUtf8(LIGHTGET_VERSION);
}

bool Updater::isNewerVersion(const QString& remote, const QString& local) {
    const QList<int> r = versionParts(remote);
    const QList<int> l = versionParts(local);
    if (r.isEmpty() || l.isEmpty()) return false;   // unparseable -> never nag
    const int n = std::max(r.size(), l.size());
    for (int i = 0; i < n; ++i) {
        const int rv = i < r.size() ? r[i] : 0;
        const int lv = i < l.size() ? l[i] : 0;
        if (rv != lv) return rv > lv;
    }
    return false;
}

void Updater::check(bool silent, QWidget* dialogParent) {
    if (m_busy) return;
    m_busy = true;

    QNetworkRequest req((QUrl(QString::fromLatin1(kLatestReleaseApi))));
    req.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub rejects API requests without a User-Agent.
    req.setRawHeader("User-Agent",
                     QStringLiteral("LightGet/%1").arg(currentVersion()).toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, silent, dialogParent]() {
        onReplyFinished(reply, silent, dialogParent);
    });
}

void Updater::onReplyFinished(QNetworkReply* reply, bool silent, QWidget* parent) {
    m_busy = false;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        if (!silent) {
            QMessageBox b(QMessageBox::Warning, Loc::t(QStringLiteral("update.title")),
                          Loc::t(QStringLiteral("update.failed")).arg(reply->errorString()),
                          QMessageBox::Ok, parent);
            presentDialog(b);
            b.exec();
        }
        return;
    }

    const QJsonObject root =
        QJsonDocument::fromJson(reply->readAll()).object();
    const QString tag = root.value(QStringLiteral("tag_name")).toString();
    const QString pageUrl = root.value(QStringLiteral("html_url")).toString();
    if (tag.isEmpty()) {
        if (!silent)
            QMessageBox::warning(parent, Loc::t(QStringLiteral("update.title")),
                                 Loc::t(QStringLiteral("update.failed"))
                                     .arg(QStringLiteral("malformed response")));
        return;
    }

    if (!isNewerVersion(tag, currentVersion())) {
        if (!silent) {
            QMessageBox b(QMessageBox::Information, Loc::t(QStringLiteral("update.title")),
                          Loc::t(QStringLiteral("update.upToDate")).arg(currentVersion()),
                          QMessageBox::Ok, parent);
            presentDialog(b);
            b.exec();
        }
        return;
    }

    // Pick this platform's asset.
    QString assetUrl, assetName;
    const QString want = wantedAssetPattern();
    for (const QJsonValue& v : root.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value(QStringLiteral("name")).toString();
        if (name.contains(want, Qt::CaseInsensitive)) {
            assetName = name;
            assetUrl = a.value(QStringLiteral("browser_download_url")).toString();
            break;
        }
    }

    promptAndInstall(tag, assetUrl, assetName, pageUrl, parent);
}

void Updater::promptAndInstall(const QString& version, const QString& assetUrl,
                               const QString& assetName, const QString& pageUrl,
                               QWidget* parent) {
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(Loc::t(QStringLiteral("update.title")));
    box.setText(Loc::t(QStringLiteral("update.available")).arg(version, currentVersion()));

#if defined(Q_OS_LINUX)
    // No safe in-place replacement for the various Linux install shapes.
    const bool canSelfInstall = false;
#else
    const bool canSelfInstall = !assetUrl.isEmpty();
#endif

    QPushButton* primary = box.addButton(
        canSelfInstall ? Loc::t(QStringLiteral("update.install"))
                       : Loc::t(QStringLiteral("update.openPage")),
        QMessageBox::AcceptRole);
    box.addButton(Loc::t(QStringLiteral("update.later")), QMessageBox::RejectRole);
    box.setDefaultButton(primary);
    presentDialog(box);
    box.exec();
    if (box.clickedButton() != static_cast<QAbstractButton*>(primary)) return;

    if (!canSelfInstall) {
        QDesktopServices::openUrl(QUrl(pageUrl.isEmpty()
            ? QStringLiteral("https://github.com/VeDono/LightGet/releases/latest")
            : pageUrl));
        return;
    }
    downloadAndInstall(assetUrl, assetName, parent);
}

void Updater::downloadAndInstall(const QString& url, const QString& assetName,
                                 QWidget* parent) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString target = QDir(dir).filePath(assetName);
    QFile::remove(target);

    QNetworkRequest req((QUrl(url)));
    req.setRawHeader("User-Agent",
                     QStringLiteral("LightGet/%1").arg(currentVersion()).toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_net->get(req);

    QProgressDialog progress(Loc::t(QStringLiteral("update.downloading")),
                             Loc::t(QStringLiteral("error.close")), 0, 100, parent);
    progress.setWindowTitle(Loc::t(QStringLiteral("update.title")));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setValue(0);

    connect(reply, &QNetworkReply::downloadProgress, &progress,
            [&progress](qint64 got, qint64 total) {
                if (total > 0) progress.setValue(int(got * 100 / total));
            });
    connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);

    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    progress.close();

    const bool ok = (reply->error() == QNetworkReply::NoError);
    QByteArray payload = ok ? reply->readAll() : QByteArray();
    reply->deleteLater();

    if (!ok || payload.isEmpty()) {
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            QMessageBox::warning(parent, Loc::t(QStringLiteral("update.title")),
                                 Loc::t(QStringLiteral("update.failed"))
                                     .arg(QStringLiteral("download failed")));
        }
        return;
    }

    QFile f(target);
    if (!f.open(QIODevice::WriteOnly) || f.write(payload) != payload.size()) {
        f.close();
        QMessageBox::warning(parent, Loc::t(QStringLiteral("update.title")),
                             Loc::t(QStringLiteral("update.failed")).arg(target));
        return;
    }
    f.close();

    if (!installDownloaded(target, parent)) {
        QMessageBox::warning(parent, Loc::t(QStringLiteral("update.title")),
                             Loc::t(QStringLiteral("update.failed")).arg(target));
    }
}

bool Updater::installDownloaded(const QString& filePath, QWidget* parent) {
#if defined(Q_OS_WIN)
    Q_UNUSED(parent);
    // The Inno installer closes the running tray app itself (CloseApplications=yes)
    // and upgrades in place thanks to the stable AppId, then relaunches it.
    if (!QProcess::startDetached(filePath, {})) return false;
    QTimer::singleShot(0, qApp, &QApplication::quit);
    return true;

#elif defined(Q_OS_MACOS)
    // Expand the zip, then hand the swap to a detached script: the bundle cannot
    // replace itself while it is running, so the script waits for this process to
    // exit first, then relaunches the new build.
    const QString tmp = QFileInfo(filePath).absolutePath()
                      + QStringLiteral("/LightGetUpdate");
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp);
    if (QProcess::execute(QStringLiteral("/usr/bin/ditto"),
                          {QStringLiteral("-x"), QStringLiteral("-k"),
                           filePath, tmp}) != 0)
        return false;

    // Locate the extracted .app.
    QString newApp;
    for (const QFileInfo& fi : QDir(tmp).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.fileName().endsWith(QStringLiteral(".app"))) { newApp = fi.absoluteFilePath(); break; }
    }
    if (newApp.isEmpty()) return false;

    const QString current = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../.."));
    if (!current.endsWith(QStringLiteral(".app"))) return false;

    const QString script = tmp + QStringLiteral("/apply.sh");
    QFile s(script);
    if (!s.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    // Wait for our PID to exit, swap the bundle, clear the download quarantine so
    // the replacement opens without a Gatekeeper prompt, then relaunch.
    s.write(QStringLiteral(
        "#!/bin/sh\n"
        "for i in $(seq 1 100); do kill -0 %1 2>/dev/null || break; sleep 0.1; done\n"
        "/usr/bin/ditto \"%2\" \"%3.new\" || exit 1\n"
        "/bin/rm -rf \"%3\" && /bin/mv \"%3.new\" \"%3\" || exit 1\n"
        "/usr/bin/xattr -dr com.apple.quarantine \"%3\" 2>/dev/null\n"
        "/usr/bin/open \"%3\"\n")
        .arg(QCoreApplication::applicationPid())
        .arg(newApp, current).toUtf8());
    s.close();
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), {script})) return false;
    QTimer::singleShot(0, qApp, &QApplication::quit);
    return true;

#else
    Q_UNUSED(filePath); Q_UNUSED(parent);
    return false;
#endif
}
