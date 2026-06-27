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
#include <QLocale>

int main(int argc, char** argv) {
    // Identify the QSettings store BEFORE anything reads Settings. These static
    // setters are valid without a QApplication and give QSettings a real backing
    // store. On macOS this yields the com.mrleondono.LightGet domain, matching the
    // native app for settings parity.
    QCoreApplication::setOrganizationName(QStringLiteral("LightGet"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("mrleondono.com"));
    QCoreApplication::setApplicationName(QStringLiteral("LightGet"));

    // Language preference BEFORE QApplication (mirrors AppleLanguages-before-AppKit).
    // Settings is a plain QSettings wrapper that does not require a QApplication,
    // so it is safe to read here. Installing the default locale up-front makes
    // Qt's own locale-aware machinery (and native dialogs) honour the choice.
    const QString lang = Settings::instance().language();   // "en" / "ru" / "uk"
    QLocale::setDefault(QLocale(lang));

    QApplication app(argc, argv);

    // Tray-only app: never quit just because a transient window closed.
    QApplication::setQuitOnLastWindowClosed(false);

    TrayApp tray;
    tray.start();   // build tray, register hotkey (applicationDidFinishLaunching)

    return app.exec();
}
