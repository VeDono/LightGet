#pragma once

// Updater.h — in-app update check against the project's GitHub Releases.
//
// The app ships as a tray utility people install once, so it asks GitHub for the
// latest published release, compares it with the version compiled into this build
// (LIGHTGET_VERSION) and offers to fetch + install the newer one. There is no
// telemetry: a single unauthenticated GET to the public releases API, nothing is
// sent about the machine.
//
// Install strategy per platform (see Updater.cpp for the details):
//   - Windows : download the Inno Setup installer and run it. The .iss carries a
//               stable AppId and CloseApplications=yes, so it upgrades in place.
//   - macOS   : download the .zip, expand it, and hand a detached helper script the
//               job of swapping /Applications/LightGet.app once we have exited,
//               then relaunching.
//   - Linux   : too many install shapes (AppImage / tarball / distro) to replace
//               safely, so open the release page instead.
//
// Checking is opt-out (Settings::updateCheckOnLaunch) and can also be triggered
// manually from the tray menu.

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QWidget;

class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QObject* parent = nullptr);

    // Ask GitHub for the newest release. `silent` = only surface something when an
    // update actually exists (used for the automatic check on launch); otherwise
    // "you're up to date" / network errors are reported too.
    void check(bool silent, QWidget* dialogParent = nullptr);

    // "1.0.7" > "1.0.6". Tolerates a leading "v" and differing segment counts;
    // returns false for anything it cannot parse, so a malformed tag never
    // nags the user.
    static bool isNewerVersion(const QString& remote, const QString& local);

    // The version this binary was built as (LIGHTGET_VERSION).
    static QString currentVersion();

private:
    void onReplyFinished(class QNetworkReply* reply, bool silent, QWidget* parent);
    void promptAndInstall(const QString& version, const QString& assetUrl,
                          const QString& assetName, const QString& pageUrl,
                          QWidget* parent);
    void downloadAndInstall(const QString& url, const QString& assetName,
                            QWidget* parent);
    bool installDownloaded(const QString& filePath, QWidget* parent);

    QString m_pendingVersion;   // shown in the download window
    QNetworkAccessManager* m_net = nullptr;
    bool m_busy = false;
};
