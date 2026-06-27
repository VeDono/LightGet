// main.cpp — Application entry point.
//
// Source: main.swift (Spec 2). Mirrors the macOS ordering: set the language
// preference BEFORE the GUI toolkit is constructed so that native dialogs
// (e.g. the QFileDialog save panel) also pick up the chosen language, exactly
// like the Swift app set AppleLanguages before touching AppKit.
//
// The app is "accessory"/tray-only: no main window is ever shown, so we set
// QApplication::setQuitOnLastWindowClosed(false) — otherwise closing the
// settings window (the only top-level we ever show) would terminate the app.

#include "Settings.h"
#include "TrayApp.h"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QLocale>

// Settings scope (overridable at build time so an alternate/parallel install
// keeps its OWN QSettings store instead of sharing the native app's).
#ifndef LIGHTGET_APP_NAME
#define LIGHTGET_APP_NAME "LightGet"
#endif

int main(int argc, char** argv) {
    // Identify the QSettings store BEFORE anything reads Settings. These static
    // setters are valid without a QApplication and give QSettings a real backing
    // store. Default APP_NAME "LightGet" + domain mrleondono.com yields the
    // com.mrleondono.LightGet domain, matching the native app for settings parity;
    // an alternate build (different APP_NAME) gets its own separate store.
    QCoreApplication::setOrganizationName(QStringLiteral("LightGet"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("mrleondono.com"));
    QCoreApplication::setApplicationName(QStringLiteral(LIGHTGET_APP_NAME));

    // Language preference BEFORE QApplication (mirrors AppleLanguages-before-AppKit).
    // Settings is a plain QSettings wrapper that does not require a QApplication,
    // so it is safe to read here. Installing the default locale up-front makes
    // Qt's own locale-aware machinery (and native dialogs) honour the choice.
    const QString lang = Settings::instance().language();   // "en" / "ru" / "uk"
    QLocale::setDefault(QLocale(lang));

    QApplication app(argc, argv);

    // Application icon (bundled in the binary via the assets .qrc). Used for the
    // settings window title-bar / taskbar entry on Windows & Linux and as the
    // tray-icon fallback. On Windows this is what removes the blank/default
    // window & taskbar icon; the .exe file icon is handled separately via the
    // embedded Win32 resource (resources/app.rc -> AppIcon.ico).
    app.setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

    // Tray-only app: never quit just because a transient window closed.
    QApplication::setQuitOnLastWindowClosed(false);

    TrayApp tray;
    tray.start();   // build tray, register hotkey (applicationDidFinishLaunching)

    return app.exec();
}
