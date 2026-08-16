#include "Updater.h"

#include "Localization.h"
#include "SettingsWindow.h"   // DesignTokens + lightgetDesignTokens

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
#include <QProgressBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QGuiApplication>
#include <QScreen>
#include <QStyleHints>
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


QString colToCss(const QColor& c) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

bool appIsDarkScheme() {
    if (auto* h = QGuiApplication::styleHints())
        if (h->colorScheme() != Qt::ColorScheme::Unknown)
            return h->colorScheme() == Qt::ColorScheme::Dark;
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

QString humanSize(qint64 bytes) {
    const double mb = double(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

// Download window drawn in the app's own design language (frameless rounded card,
// design tokens, accent progress track) instead of the stock QProgressDialog.
class UpdateProgressDialog : public QDialog {
public:
    UpdateProgressDialog(const QString& version, QWidget* parent, bool dark)
        : QDialog(parent), m_tk(lightgetDesignTokens(dark)) {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setWindowModality(Qt::ApplicationModal);
        setFixedSize(380, 152);

        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(24, 22, 24, 20);
        col->setSpacing(0);

        auto* title = new QLabel(Loc::t(QStringLiteral("update.title")));
        title->setStyleSheet(QStringLiteral("color:%1; font-size:15px; font-weight:600;")
                                 .arg(colToCss(m_tk.text)));
        col->addWidget(title);
        col->addSpacing(6);

        m_status = new QLabel(Loc::t(QStringLiteral("update.downloading")));
        m_status->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                                    .arg(colToCss(m_tk.text2)));
        col->addWidget(m_status);
        col->addSpacing(16);

        m_bar = new QProgressBar;
        m_bar->setRange(0, 100);
        m_bar->setValue(0);
        m_bar->setTextVisible(false);
        m_bar->setFixedHeight(8);
        m_bar->setStyleSheet(QStringLiteral(
            "QProgressBar { background:%1; border:none; border-radius:4px; }"
            "QProgressBar::chunk { background:%2; border-radius:4px; }")
            .arg(colToCss(m_tk.dark ? m_tk.controlFill : QColor("#e6e6ea")),
                 colToCss(m_tk.accent)));
        col->addWidget(m_bar);
        col->addSpacing(6);

        m_detail = new QLabel(QStringLiteral("%1 · %2").arg(version, QStringLiteral("—")));
        m_detail->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                                    .arg(colToCss(m_tk.text3)));
        col->addWidget(m_detail);
        col->addStretch(1);

        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addStretch(1);
        m_cancel = new QPushButton(Loc::t(QStringLiteral("update.cancel")));
        m_cancel->setCursor(Qt::PointingHandCursor);
        m_cancel->setFocusPolicy(Qt::NoFocus);
        m_cancel->setStyleSheet(QStringLiteral(
            "QPushButton { color:%1; background:%2; border:1px solid %3;"
            " border-radius:7px; padding:6px 16px; font-size:12px; }"
            "QPushButton:hover { color:%4; }")
            .arg(colToCss(m_tk.text2), colToCss(m_tk.control),
                 colToCss(m_tk.border), colToCss(m_tk.text)));
        row->addWidget(m_cancel);
        col->addLayout(row);

        m_version = version;
    }

    QPushButton* cancelButton() const { return m_cancel; }

    void setProgress(qint64 got, qint64 total) {
        if (total > 0) {
            m_bar->setRange(0, 100);
            m_bar->setValue(int(got * 100 / total));
            m_detail->setText(QStringLiteral("%1 · %2 / %3")
                                  .arg(m_version, humanSize(got), humanSize(total)));
        } else {
            m_bar->setRange(0, 0);   // indeterminate until a length is known
            m_detail->setText(QStringLiteral("%1 · %2").arg(m_version, humanSize(got)));
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, 12, 12);
        p.fillPath(path, m_tk.card);
        p.setPen(QPen(m_tk.border, 1.0));
        p.drawPath(path);
    }

private:
    DesignTokens m_tk;
    QLabel* m_status = nullptr;
    QLabel* m_detail = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_cancel = nullptr;
    QString m_version;
};

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

// Render-harness export: grab the download window with sample progress so its
// design can be reviewed offscreen (see main.cpp --render-dump).
QPixmap LightGet_debugUpdateDialog(bool dark) {
    auto* hints = QGuiApplication::styleHints();
    const Qt::ColorScheme prev = hints ? hints->colorScheme() : Qt::ColorScheme::Unknown;
    if (hints) hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    UpdateProgressDialog dlg(QStringLiteral("1.0.9"), nullptr, dark);
    dlg.setProgress(8ll * 1024 * 1024, 22ll * 1024 * 1024);
    dlg.ensurePolished();
    QApplication::processEvents();
    const QPixmap pm = dlg.grab();
    if (hints) hints->setColorScheme(prev);
    return pm;
}

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
    m_pendingVersion = version;
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

    UpdateProgressDialog progress(m_pendingVersion, parent, appIsDarkScheme());
    presentDialog(progress);
    progress.show();

    connect(reply, &QNetworkReply::downloadProgress, &progress,
            [&progress](qint64 got, qint64 total) { progress.setProgress(got, total); });
    connect(progress.cancelButton(), &QPushButton::clicked, reply, &QNetworkReply::abort);
    connect(&progress, &QDialog::rejected, reply, &QNetworkReply::abort);

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
