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
#include "SettingsWindow.h"
#include "Toolbar.h"
#include "Localization.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QList>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QStyleHints>
#include <QString>
#include <QStringList>

// Settings scope (overridable at build time so an alternate/parallel install
// keeps its OWN QSettings store instead of sharing the native app's).
#ifndef LIGHTGET_APP_NAME
#define LIGHTGET_APP_NAME "LightGet"
#endif

namespace {

// ---------------------------------------------------------------------------
// Hidden debug harness: --render-dump <dir>
//
// Renders the REAL SettingsWindow (same construction path the tray app uses) to
// PNGs so we can eyeball what it currently draws vs. the design. Produces four
// images — {General, Features} x {light, dark}. This is NOT part of normal
// startup: it is reached only when the flag is present, and main() exit(0)s
// afterwards so the tray app never starts. No design/behaviour is altered.
//
// Theme switch: the window resolves light vs. dark purely from
// palette().color(QPalette::Window).lightness() (see SettingsWindow::resolveTokens),
// so we install a light or dark QApplication palette (and the matching
// Qt::ColorScheme) BEFORE building the window, exactly mirroring an OS theme.
// ---------------------------------------------------------------------------

QPalette makeLightPalette() {
    QPalette p;
    p.setColor(QPalette::Window,          QColor("#f3f3f5"));
    p.setColor(QPalette::WindowText,      QColor("#1d1d1f"));
    p.setColor(QPalette::Base,            QColor("#ffffff"));
    p.setColor(QPalette::AlternateBase,   QColor("#eef0f2"));
    p.setColor(QPalette::Text,            QColor("#1d1d1f"));
    p.setColor(QPalette::Button,          QColor("#ffffff"));
    p.setColor(QPalette::ButtonText,      QColor("#1d1d1f"));
    p.setColor(QPalette::Highlight,       QColor("#007aff"));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipBase,     QColor("#ffffff"));
    p.setColor(QPalette::ToolTipText,     QColor("#1d1d1f"));
    return p;
}

QPalette makeDarkPalette() {
    QPalette p;
    p.setColor(QPalette::Window,          QColor("#1a1a1c"));
    p.setColor(QPalette::WindowText,      QColor("#f2f2f4"));
    p.setColor(QPalette::Base,            QColor("#262629"));
    p.setColor(QPalette::AlternateBase,   QColor("#37373b"));
    p.setColor(QPalette::Text,            QColor("#f2f2f4"));
    p.setColor(QPalette::Button,          QColor("#2f2f33"));
    p.setColor(QPalette::ButtonText,      QColor("#f2f2f4"));
    p.setColor(QPalette::Highlight,       QColor("#0a84ff"));
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipBase,     QColor("#262629"));
    p.setColor(QPalette::ToolTipText,     QColor("#f2f2f4"));
    return p;
}

// Grab one tab of a freshly-built SettingsWindow under the given palette/theme
// and save it. Building a fresh window per (theme, tab) keeps each capture clean
// and avoids relying on changeEvent timing. Returns true on a successful save.
bool dumpOne(const QString& dir, const QString& theme, bool dark, int tabIndex) {
    QApplication::setPalette(dark ? makeDarkPalette() : makeLightPalette());
    if (auto* hints = QGuiApplication::styleHints())
        hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);

    SettingsWindow w;
    w.setPalette(QApplication::palette());   // ensure the window resolves the theme
    w.ensurePolished();

    // Switch the active tab via the real tab button (checkable, in the exclusive
    // button group wired to the stacked pages) — no private API, exercises the
    // actual design path. Tab 0 (General) is the default, so only act for tab 1.
    if (tabIndex == 1) {
        const QString featuresLabel = Loc::t(QStringLiteral("tab.features"));
        const QList<QPushButton*> buttons = w.findChildren<QPushButton*>();
        for (QPushButton* b : buttons) {
            if (b->isCheckable() && b->text() == featuresLabel) {
                b->click();   // switches the stacked page to Features (idClicked)
                break;
            }
        }
    }

    // Let layout/styling settle before grabbing.
    QApplication::processEvents();

    const QString tabName = (tabIndex == 1) ? QStringLiteral("features")
                                            : QStringLiteral("general");
    const QString path =
        QDir(dir).filePath(QStringLiteral("current_%1_%2.png").arg(tabName, theme));

    const QPixmap pm = w.grab();
    const QImage img = pm.toImage();
    return img.save(path, "PNG");
}

// Grab the REAL ToolbarView (same construction path the overlay uses) under the
// given palette/theme and save it. The toolbar always paints its own dark panel
// surface regardless of palette, but we still render under each palette so the
// dump mirrors the settings dump (light + dark) and would catch any palette-
// dependent drift. We select a tool and (when the palette is enabled in Settings)
// the toolbar already shows the color row, exercising the full design path.
bool dumpToolbar(const QString& dir, const QString& theme, bool dark) {
    QApplication::setPalette(dark ? makeDarkPalette() : makeLightPalette());
    if (auto* hints = QGuiApplication::styleHints())
        hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);

    ToolbarView tb;
    tb.setPalette(QApplication::palette());
    tb.rebuild();                      // lay out tools + palette + actions
    tb.setSelectedTool(Tool::Select);  // show the active-tool (blue) state
    tb.setSelectedColor(0);            // show the selected color well (red)
    tb.ensurePolished();
    tb.resize(tb.sizeHint());          // ToolbarView is fixed-size; honour it
    QApplication::processEvents();     // let layout/styling settle before grabbing

    const QString path =
        QDir(dir).filePath(QStringLiteral("current_toolbar_%1.png").arg(theme));
    const QPixmap pm = tb.grab();      // QWidget::grab works fully offscreen
    return pm.toImage().save(path, "PNG");
}

// Drive all dumps; returns process exit code (0 = all saved).
int runRenderDump(const QString& dir) {
    QDir().mkpath(dir);
    bool ok = true;
    ok &= dumpOne(dir, QStringLiteral("light"), /*dark=*/false, /*tab=*/0);
    ok &= dumpOne(dir, QStringLiteral("dark"),  /*dark=*/true,  /*tab=*/0);
    ok &= dumpOne(dir, QStringLiteral("light"), /*dark=*/false, /*tab=*/1);
    ok &= dumpOne(dir, QStringLiteral("dark"),  /*dark=*/true,  /*tab=*/1);
    ok &= dumpToolbar(dir, QStringLiteral("light"), /*dark=*/false);
    ok &= dumpToolbar(dir, QStringLiteral("dark"),  /*dark=*/true);
    return ok ? 0 : 1;
}

} // namespace

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

    // Apply the saved Appearance preference to the app color scheme (Qt 6.8):
    // "light"/"dark" force it; "auto" follows the OS. Mirrors what the Settings
    // window's Appearance segment does live. Skipped under --render-dump below,
    // where each capture forces its own scheme.
    {
        const QStringList ddArgs = QCoreApplication::arguments();
        if (!ddArgs.contains(QStringLiteral("--render-dump"))) {
            if (auto* hints = QGuiApplication::styleHints()) {
                const QString appr = Settings::instance().appearance();
                if (appr == QStringLiteral("light"))
                    hints->setColorScheme(Qt::ColorScheme::Light);
                else if (appr == QStringLiteral("dark"))
                    hints->setColorScheme(Qt::ColorScheme::Dark);
                else
                    hints->setColorScheme(Qt::ColorScheme::Unknown);
            }
        }
    }

    // Hidden debug harness: "--render-dump <dir>" renders the real SettingsWindow
    // to PNGs (General/Features x light/dark) and exits WITHOUT starting the tray.
    // Behind the flag so normal startup is completely unaffected.
    {
        const QStringList args = QCoreApplication::arguments();
        const int idx = args.indexOf(QStringLiteral("--render-dump"));
        if (idx >= 0 && idx + 1 < args.size()) {
            const int code = runRenderDump(args.at(idx + 1));
            return code;   // never reach TrayApp / app.exec()
        }
    }

    TrayApp tray;
    tray.start();   // build tray, register hotkey (applicationDidFinishLaunching)

    return app.exec();
}
