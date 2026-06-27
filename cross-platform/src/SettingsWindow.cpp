// SettingsWindow.cpp — Tabbed settings window (General + Features) + hotkey
// recorder. Faithful Qt6 port of SettingsWindowController.swift.
//
// COORDINATE NOTE: the Swift layout used bottom-left origin with hand-computed
// y values. Here we use Qt layouts (QTabWidget/QFormLayout/QVBoxLayout) — the
// absolute y math from the spec is documentation, not a requirement. The About
// section's social card + copyright row match visually.

#include "SettingsWindow.h"

#include "Settings.h"
#include "Localization.h"
#include "Annotation.h"

#include <QApplication>
#include <QTabWidget>
#include <QWidget>
#include <QLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QGroupBox>
#include <QSlider>
#include <QFrame>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QDate>
#include <QScreen>
#include <QGuiApplication>
#include <QCursor>
#include <QSignalBlocker>
#include <QFile>
#include <QFont>
#include <QSizePolicy>
#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// Local helpers / data
// ---------------------------------------------------------------------------
namespace {

// Languages: (title, code). English first (default language).
struct LangEntry { const char* title; const char* code; };
const std::vector<LangEntry> kLanguages = {
    {"English", "en"}, {"Русский", "ru"}, {"Українська", "uk"}
};

// Preset bar icons (asset names mirroring SF Symbols), exactly 5 (Spec 5 §5.2).
const QStringList kBarIcons = {
    "scissors", "camera.viewfinder", "crop", "rectangle.dashed",
    "paintbrush.pointed.fill"
};

// Tools shown on the Features tab (Select is always-on, never listed).
const std::vector<Tool> kFeatureTools = {
    Tool::Arrow, Tool::Line, Tool::Rectangle, Tool::FilledRect,
    Tool::Pen, Tool::Text
};

// Recolor an alpha-only (white-on-transparent) glyph pixmap to a solid `tint`,
// preserving the alpha channel. The bundled assets are white-on-transparent
// (see assets.qrc), which is invisible on the LIGHT settings background — this
// stamps them in a dark, readable color instead (task 3).
QPixmap tintPixmap(const QPixmap& src, const QColor& tint) {
    if (src.isNull()) return src;
    QPixmap out(src.size());
    out.setDevicePixelRatio(src.devicePixelRatio());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPixmap(0, 0, src);                       // lay down the alpha shape
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), tint);                  // recolor opaque areas to tint
    p.end();
    return out;
}

// Resolve a preset bar-icon asset to a QIcon. SF Symbols are macOS-only, so we
// load a bundled replacement asset from the qrc (assets.qrc maps these names);
// fall back to a themed icon, then an empty icon, so the UI still builds.
QIcon barIconAsset(const QString& name) {
    QIcon icon(QStringLiteral(":/assets/%1.png").arg(name));
    if (!icon.isNull()) return icon;
    QIcon theme = QIcon::fromTheme(name);
    if (!theme.isNull()) return theme;
    return QIcon();
}

// Same as barIconAsset() but recolored to a tint visible on the light settings
// background. The qrc PNGs are white-on-transparent (invisible by default), so
// the menu-bar-icon chooser / reset glyphs MUST be tinted dark here. Renders a
// crisp 2x pixmap so the recolor stays sharp on Retina. `size` is in points.
QIcon barIconAssetTinted(const QString& name, const QColor& tint, int size = 18) {
    QIcon base = barIconAsset(name);
    if (base.isNull()) {
        QIcon theme = QIcon::fromTheme(name);
        if (!theme.isNull()) base = theme;
    }
    if (base.isNull()) return QIcon();
    const qreal dpr = 2.0;
    QPixmap pm = base.pixmap(QSize(int(size * dpr), int(size * dpr)));
    pm.setDevicePixelRatio(dpr);
    return QIcon(tintPixmap(pm, tint));
}

// Open a URL in the default browser (mirrors NSWorkspace.shared.open).
void openExternal(const QString& url) {
    QDesktopServices::openUrl(QUrl(url));
}

// ---- Social card link button (blue link text, pointing-hand cursor) --------
// Mirrors Swift LinkButton: blue text, opens URL on click.
class LinkButton : public QLabel {
public:
    LinkButton(const QString& text, const QString& url, Qt::Alignment align,
               QWidget* parent = nullptr)
        : QLabel(parent), m_url(url) {
        setText(text);
        setAlignment(align | Qt::AlignVCenter);
        setCursor(Qt::PointingHandCursor);
        setTextInteractionFlags(Qt::NoTextInteraction);
        // .linkColor ~= system link blue.
        setStyleSheet("color: palette(link);");
    }
protected:
    void mousePressEvent(QMouseEvent*) override { openExternal(m_url); }
private:
    QString m_url;
};

// ---- Inline copyright name link --------------------------------------------
// Mirrors Swift InlineLinkField: gray + underlined, blue on hover, opens URL.
class InlineLinkLabel : public QLabel {
public:
    InlineLinkLabel(const QString& text, const QString& url, int sizePt,
                    QWidget* parent = nullptr)
        : QLabel(text, parent), m_url(url) {
        QFont f = font();
        f.setPointSize(sizePt);
        f.setUnderline(true);
        setFont(f);
        setCursor(Qt::PointingHandCursor);
        applyColor(false);
    }
protected:
    void enterEvent(QEnterEvent*) override { applyColor(true); }
    void leaveEvent(QEvent*) override { applyColor(false); }
    void mousePressEvent(QMouseEvent*) override { openExternal(m_url); }
private:
    void applyColor(bool hover) {
        // base = secondaryLabelColor (gray), hover = linkColor (blue).
        setStyleSheet(hover ? "color: palette(link);"
                            : "color: palette(mid);");
    }
    QString m_url;
};

// Version + edition (set as compile definitions in CMakeLists.txt). Defaults
// keep the file self-contained if a definition is ever missing.
#ifndef LIGHTGET_VERSION
#define LIGHTGET_VERSION "1.0.0"
#endif
#ifndef LIGHTGET_EDITION
#define LIGHTGET_EDITION "Cross-platform (Qt 6)"
#endif

} // namespace

// ===========================================================================
// HotkeyRecorder
// ===========================================================================

// Idle: a plain white rounded field with a subtle border (NOT a bright-blue
// selected/checked bar). Recording: a faint blue-tinted border to signal the
// widget is listening. Styled explicitly so the recorder never renders as the
// platform's "pressed/default" highlighted push button.
static const char* kRecorderIdleStyle =
    "HotkeyRecorder {"
    " background-color: palette(base);"
    " color: palette(text);"
    " border: 1px solid palette(mid);"
    " border-radius: 6px;"
    " padding: 3px 8px;"
    " text-align: center; }"
    "HotkeyRecorder:hover {"
    " border-color: palette(dark); }";

static const char* kRecorderRecordingStyle =
    "HotkeyRecorder {"
    " background-color: palette(base);"
    " color: palette(text);"
    " border: 2px solid #007AFF;"   // systemBlue — only while listening
    " border-radius: 6px;"
    " padding: 2px 7px;"
    " text-align: center; }";

HotkeyRecorder::HotkeyRecorder(QWidget* parent) : QPushButton(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCheckable(false);
    setStyleSheet(kRecorderIdleStyle);
    connect(this, &QPushButton::clicked, this, &HotkeyRecorder::startRecording);
}

void HotkeyRecorder::startRecording() {
    m_recording = true;
    setStyleSheet(kRecorderRecordingStyle);
    setText(Loc::t("recorder.press"));
    setFocus(Qt::OtherFocusReason);
}

void HotkeyRecorder::keyPressEvent(QKeyEvent* event) {
    if (!m_recording) { QPushButton::keyPressEvent(event); return; }

    // Esc — cancel recording, restore previous display + idle style.
    if (event->key() == Qt::Key_Escape) {
        m_recording = false;
        setStyleSheet(kRecorderIdleStyle);
        setText(Settings::instance().hotKeyDisplay());
        return;
    }

    // Ignore lone modifier presses; wait for an actual key.
    const int k = event->key();
    if (k == Qt::Key_Control || k == Qt::Key_Shift || k == Qt::Key_Alt ||
        k == Qt::Key_Meta || k == Qt::Key_AltGr || k == 0 ||
        k == Qt::Key_unknown) {
        return;
    }

    const Qt::KeyboardModifiers mods = event->modifiers();
    const uint32_t carbonMods = carbonModifiers(mods);
    if (carbonMods == 0) {            // at least one modifier required
        QApplication::beep();
        return;                      // keep recording
    }

    // Key text ignoring modifiers, uppercased (charactersIgnoringModifiers).
    QString keyText = event->text();
    if (!keyText.isEmpty() && keyText.at(0).isPrint())
        keyText = keyText.toUpper();
    else
        keyText = QKeySequence(k).toString(QKeySequence::NativeText).toUpper();

    const uint32_t carbonCode = carbonKeyCode(k, event->nativeVirtualKey());
    const QString display = displayString(mods, keyText);

    m_recording = false;
    setStyleSheet(kRecorderIdleStyle);
    setText(display);
    emit captured(carbonCode, carbonMods, display);
}

uint32_t HotkeyRecorder::carbonModifiers(Qt::KeyboardModifiers mods) {
    using namespace CarbonKeys;
    uint32_t m = 0;
    // Qt::ControlModifier maps to Cmd on macOS, Ctrl elsewhere — treat it as the
    // command-key for persistence parity with the Swift app's cmdKey.
    if (mods & Qt::ControlModifier) m |= cmdKey;
    if (mods & Qt::AltModifier)     m |= optionKey;
    if (mods & Qt::MetaModifier)    m |= controlKey;
    if (mods & Qt::ShiftModifier)   m |= shiftKey;
    return m;
}

QString HotkeyRecorder::displayString(Qt::KeyboardModifiers mods,
                                      const QString& keyText) {
    // Glyph order: ⌃ ⌥ ⇧ ⌘ then the uppercased key.
    QString s;
    if (mods & Qt::MetaModifier)    s += QStringLiteral("⌃"); // ⌃ control
    if (mods & Qt::AltModifier)     s += QStringLiteral("⌥"); // ⌥ option
    if (mods & Qt::ShiftModifier)   s += QStringLiteral("⇧"); // ⇧ shift
    if (mods & Qt::ControlModifier) s += QStringLiteral("⌘"); // ⌘ command
    s += keyText.isEmpty() ? QStringLiteral("?") : keyText;
    return s;
}

// Translate a Qt key (+ native VK fallback) to a Carbon virtual-key code so the
// persisted value matches the macOS app's defaults file (kVK_* constants).
uint32_t HotkeyRecorder::carbonKeyCode(int qtKey, quint32 nativeVK) {
#ifdef Q_OS_MAC
    // On macOS, nativeVirtualKey() already IS the Carbon kVK_* code.
    if (nativeVK != 0) return nativeVK;
#else
    Q_UNUSED(nativeVK);
#endif
    // Portable Qt::Key -> Carbon kVK_ANSI_* table (ANSI US layout positions).
    switch (qtKey) {
    case Qt::Key_A: return 0x00; case Qt::Key_B: return 0x0B;
    case Qt::Key_C: return 0x08; case Qt::Key_D: return 0x02;
    case Qt::Key_E: return 0x0E; case Qt::Key_F: return 0x03;
    case Qt::Key_G: return 0x05; case Qt::Key_H: return 0x04;
    case Qt::Key_I: return 0x22; case Qt::Key_J: return 0x26;
    case Qt::Key_K: return 0x28; case Qt::Key_L: return 0x25;
    case Qt::Key_M: return 0x2E; case Qt::Key_N: return 0x2D;
    case Qt::Key_O: return 0x1F; case Qt::Key_P: return 0x23;
    case Qt::Key_Q: return 0x0C; case Qt::Key_R: return 0x0F;
    case Qt::Key_S: return 0x01; case Qt::Key_T: return 0x11;
    case Qt::Key_U: return 0x20; case Qt::Key_V: return 0x09;
    case Qt::Key_W: return 0x0D; case Qt::Key_X: return 0x07;
    case Qt::Key_Y: return 0x10; case Qt::Key_Z: return 0x06;
    case Qt::Key_1: return 0x12; case Qt::Key_2: return 0x13;
    case Qt::Key_3: return 0x14; case Qt::Key_4: return 0x15;
    case Qt::Key_5: return 0x17; case Qt::Key_6: return 0x16;
    case Qt::Key_7: return 0x1A; case Qt::Key_8: return 0x1C;
    case Qt::Key_9: return 0x19; case Qt::Key_0: return 0x1D;
    case Qt::Key_Space:  return 0x31;
    case Qt::Key_Return: return 0x24;
    case Qt::Key_Tab:    return 0x30;
    case Qt::Key_Escape: return CarbonKeys::kVK_Escape;
    case Qt::Key_Minus:  return 0x1B;
    case Qt::Key_Equal:  return 0x18;
    case Qt::Key_BracketLeft:  return 0x21;
    case Qt::Key_BracketRight: return 0x1E;
    case Qt::Key_Backslash:    return 0x2A;
    case Qt::Key_Semicolon:    return 0x29;
    case Qt::Key_Apostrophe:   return 0x27;
    case Qt::Key_Comma:        return 0x2B;
    case Qt::Key_Period:       return 0x2F;
    case Qt::Key_Slash:        return 0x2C;
    case Qt::Key_QuoteLeft:    return 0x32;
    case Qt::Key_F1: return 0x7A; case Qt::Key_F2: return 0x78;
    case Qt::Key_F3: return 0x63; case Qt::Key_F4: return 0x76;
    case Qt::Key_F5: return 0x60; case Qt::Key_F6: return 0x61;
    case Qt::Key_F7: return 0x62; case Qt::Key_F8: return 0x64;
    case Qt::Key_F9: return 0x65; case Qt::Key_F10: return 0x6D;
    case Qt::Key_F11: return 0x67; case Qt::Key_F12: return 0x6F;
    default: break;
    }
    // Unknown key: persist the native VK if any, else 0.
    return static_cast<uint32_t>(nativeVK);
}

// ===========================================================================
// SettingsWindow
// ===========================================================================

SettingsWindow::SettingsWindow(QWidget* parent) : QDialog(parent) {
    // Fixed-size window, not resizable, persists across open/close (the owner
    // keeps the instance alive). Title bar with close only. Size bumped a little
    // (was 440x580) to give the grouped sections breathing room so nothing is
    // cramped or clipped; a minimum width keeps labels/controls comfortable.
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setFixedSize(460, 620);

    m_recorder = new HotkeyRecorder(this);   // reused member across rebuilds
    connect(m_recorder, &HotkeyRecorder::captured, this,
            [this](uint32_t code, uint32_t mods, const QString& display) {
        Settings& s = Settings::instance();
        s.setHotKeyCode(code);
        s.setHotKeyModifiers(mods);
        s.setHotKeyDisplay(display);
        emit hotKeyChanged();
    });

    buildUI();
}

void SettingsWindow::showCentered() {
    // Center on the screen under the cursor (accessory-app focus dance).
    if (QScreen* scr = QGuiApplication::screenAt(QCursor::pos())) {
        const QRect g = scr->availableGeometry();
        move(g.center() - rect().center());
    }
    show();
    raise();
    activateWindow();
}

void SettingsWindow::reloadUI() {
    // Full teardown + rebuild of both tabs. The recorder is a reused member
    // (reparented into the rebuilt tab), so detach it before tearing down so it
    // survives deletion of the old tab widget.
    if (m_recorder) m_recorder->setParent(this);
    if (m_tabs) {
        m_tabs->hide();
        m_tabs->setParent(nullptr);   // remove from the old layout immediately
        m_tabs->deleteLater();
        m_tabs = nullptr;
    }
    if (QLayout* old = layout()) {
        delete old;                   // detaches; child widgets reparented above
    }
    buildUI();
}

void SettingsWindow::buildUI() {
    setWindowTitle(Loc::t("settings.title"));

    m_tabs = new QTabWidget(this);
    // Rounded top corners + comfortable padding on the tab buttons to match the
    // native look (task 4). The selected tab reads as a continuous surface with
    // the pane below; idle tabs are slightly recessed.
    m_tabs->setStyleSheet(
        "QTabWidget::pane {"
        " border: 1px solid palette(mid); border-radius: 6px; top: -1px; }"
        "QTabBar::tab {"
        " padding: 6px 18px; margin-right: 4px;"
        " border: 1px solid palette(mid); border-bottom: none;"
        " border-top-left-radius: 7px; border-top-right-radius: 7px;"
        " background: palette(window); color: palette(text); }"
        "QTabBar::tab:selected {"
        " background: palette(base); }"
        "QTabBar::tab:!selected {"
        " margin-top: 2px; background: palette(window); color: palette(mid); }"
        "QTabBar::tab:hover:!selected { color: palette(text); }");
    m_tabs->addTab(buildGeneralTab(),  Loc::t("tab.general"));
    m_tabs->addTab(buildFeaturesTab(), Loc::t("tab.features"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(7, 7, 7, 7);
    root->addWidget(m_tabs);
}

// ---------------------------------------------------------------------------
// General tab
// ---------------------------------------------------------------------------
QWidget* SettingsWindow::buildGeneralTab() {
    Settings& s = Settings::instance();

    auto* tab = new QWidget;
    auto* outer = new QVBoxLayout(tab);
    outer->setContentsMargins(20, 18, 20, 16);
    outer->setSpacing(12);

    // Rows laid out plainly on the tab background — NO surrounding titled group
    // box (matches the native General tab's clean light look). A QFormLayout
    // keeps the labels right-aligned in a column with the controls beside them.
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);

    // Small helper: place [control][reset] in one row.
    auto controlWithReset = [&](QWidget* control, ResetTarget target) -> QWidget* {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        h->addWidget(control, 1);
        h->addWidget(makeResetButton(target), 0);
        return w;
    };

    // --- Language ---
    auto* langCombo = new QComboBox;
    for (const auto& l : kLanguages) langCombo->addItem(QString::fromUtf8(l.title));
    int langIdx = 0;
    for (size_t i = 0; i < kLanguages.size(); ++i)
        if (s.language() == QString::fromUtf8(kLanguages[i].code)) { langIdx = int(i); break; }
    langCombo->setCurrentIndex(langIdx);
    connect(langCombo, QOverload<int>::of(&QComboBox::activated),
            this, &SettingsWindow::onLanguageChanged);
    form->addRow(Loc::t("settings.language"), langCombo);

    // --- Menu-bar icon: clean segmented preset chooser + custom-image button ---
    {
        auto* iconRow = new QWidget;
        auto* h = new QHBoxLayout(iconRow);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);   // small gaps so the icons read as a tidy, uncramped row

        // Dark tint for the glyphs: the bundled assets are white-on-transparent
        // (invisible on this light tab), so we recolor them to the text color.
        // A faintly-blue selected highlight (NOT a full systemBlue fill) keeps the
        // dark icon legible while still clearly marking the active segment.
        const QColor iconTint = palette().color(QPalette::Active, QPalette::Text);
        static const char* kSegStyle =
            "QToolButton { border: 1px solid transparent; border-radius: 6px;"
            " background: transparent; }"
            "QToolButton:hover { background-color: rgba(0,0,0,18); }"
            "QToolButton:checked {"
            " background-color: rgba(0,122,255,40);"          // soft systemBlue wash
            " border: 1px solid rgba(0,122,255,140); }";

        auto* group = new QButtonGroup(iconRow);
        group->setExclusive(true);
        const bool usingCustom = s.barIconCustomPath().has_value();
        int selectedSeg = -1;
        if (!usingCustom) {
            selectedSeg = kBarIcons.indexOf(s.barIcon());
            if (selectedSeg < 0) selectedSeg = 0;
        }
        for (int i = 0; i < kBarIcons.size(); ++i) {
            auto* b = new QToolButton(iconRow);
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setFixedSize(32, 28);
            b->setIconSize(QSize(18, 18));
            // Tinted dark so the glyph is clearly visible on the light background.
            b->setIcon(barIconAssetTinted(kBarIcons.at(i), iconTint, 18));
            b->setToolTip(kBarIcons.at(i));
            b->setStyleSheet(kSegStyle);
            if (i == selectedSeg) b->setChecked(true);
            group->addButton(b, i);
            h->addWidget(b);
        }
        connect(group, &QButtonGroup::idClicked, this, &SettingsWindow::onBarIconSegment);

        // A thin separator before the custom-image button so it reads as a
        // distinct affordance, not a sixth preset.
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::VLine);
        sep->setFrameShadow(QFrame::Sunken);
        h->addSpacing(4);
        h->addWidget(sep);
        h->addSpacing(4);

        // Custom image button (SF Symbol "photo"). Tinted dark like the presets
        // so it is visible on the light background; reuses the segment style so
        // the whole row reads consistently. If a custom image is active, mark it
        // checked so the user sees which mode is selected.
        auto* custom = new QToolButton(iconRow);
        custom->setCheckable(true);
        custom->setAutoRaise(true);
        custom->setFixedSize(32, 28);
        custom->setIconSize(QSize(18, 18));
        custom->setIcon(barIconAssetTinted("photo", iconTint, 18));
        custom->setStyleSheet(kSegStyle);
        custom->setChecked(usingCustom);
        custom->setToolTip(Loc::t("settings.customIcon"));
        connect(custom, &QToolButton::clicked, this, &SettingsWindow::chooseCustomIcon);
        h->addWidget(custom);
        h->addStretch(1);

        form->addRow(Loc::t("settings.barIcon"), iconRow);
    }

    // --- Hotkey recorder + reset ---
    m_recorder->setText(s.hotKeyDisplay());
    form->addRow(Loc::t("settings.hotkey"), controlWithReset(m_recorder, ResetTarget::Hotkey));

    // Hint under the hotkey row (small secondary text, wraps to 2 lines).
    {
        auto* hint = new QLabel(Loc::t("settings.hint"));
        QFont f = hint->font(); f.setPointSize(11); hint->setFont(f);
        hint->setStyleSheet("color: palette(mid);");
        hint->setWordWrap(true);
        form->addRow(QString(), hint);
    }

    // --- Dim slider (0.10 .. 0.85) + reset ---
    {
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setMinimum(10);
        slider->setMaximum(85);
        slider->setValue(int(s.dimOpacity() * 100.0 + 0.5));
        connect(slider, &QSlider::valueChanged, this,
                [this](int v) { onDimChanged(double(v) / 100.0); });
        form->addRow(Loc::t("settings.dim"), controlWithReset(slider, ResetTarget::Dim));
    }

    // --- Save folder picker + reset ---
    {
        auto* folderBtn = new QPushButton(saveFolderTitle());
        folderBtn->setToolTip(saveFolderTitle());
        connect(folderBtn, &QPushButton::clicked, this, &SettingsWindow::chooseFolder);
        form->addRow(Loc::t("settings.saveFolder"),
                     controlWithReset(folderBtn, ResetTarget::SaveFolder));
    }

    // --- Downscale checkbox + reset ---
    {
        auto* downscale = new QCheckBox(Loc::t("settings.downscale"));
        downscale->setChecked(s.downscaleRetina());
        connect(downscale, &QCheckBox::toggled, this, &SettingsWindow::onDownscaleToggled);
        form->addRow(QString(), controlWithReset(downscale, ResetTarget::Downscale));
    }

    // --- Launch at login (no reset button) ---
    {
        auto* launch = new QCheckBox(Loc::t("settings.launchAtLogin"));
        launch->setChecked(isLaunchAtLoginEnabled());
        connect(launch, &QCheckBox::toggled, this, &SettingsWindow::onLaunchAtLoginToggled);
        form->addRow(QString(), launch);
    }

    outer->addLayout(form);
    outer->addStretch(1);

    // About section pinned at the bottom of the General tab.
    addAboutSection(tab);
    outer->addWidget(m_aboutContainer);

    return tab;
}

// ---------------------------------------------------------------------------
// Features tab
// ---------------------------------------------------------------------------
QWidget* SettingsWindow::buildFeaturesTab() {
    Settings& s = Settings::instance();

    auto* tab = new QWidget;
    auto* v = new QVBoxLayout(tab);
    v->setContentsMargins(20, 20, 20, 20);
    v->setSpacing(6);

    // Bold text section headers + plain checkboxes (matching the native Features
    // tab) — NOT boxed group boxes. Each header gets a little top margin so the
    // sections read as distinct blocks.
    auto addHeader = [&](const QString& title, bool first) {
        if (!first) v->addSpacing(12);
        auto* h = new QLabel(title);
        QFont hf = h->font();
        hf.setBold(true);
        h->setFont(hf);
        v->addWidget(h);
    };
    auto check = [&](const QString& title, bool on,
                     std::function<void(bool)> handler) {
        auto* b = new QCheckBox(title);
        b->setChecked(on);
        // Indent checkboxes slightly under their bold header.
        b->setStyleSheet("QCheckBox { margin-left: 2px; }");
        connect(b, &QCheckBox::toggled, this, [handler](bool val){ handler(val); });
        v->addWidget(b);
    };

    // Tools.
    addHeader(Loc::t("features.toolsTitle"), /*first=*/true);
    for (Tool t : kFeatureTools) {
        const QString key = QStringLiteral("tool.%1").arg(QString::fromUtf8(toolKey(t)));
        check(Loc::t(key), s.isToolEnabled(t), [t](bool on) {
            Settings::instance().setToolEnabled(t, on);
        });
    }

    // Interface.
    addHeader(Loc::t("features.interfaceTitle"), /*first=*/false);
    check(Loc::t("features.showColors"), s.showColorPalette(), [](bool on) {
        Settings::instance().setShowColorPalette(on);
    });
    // Animated dimming — directly under "Show color palette" (default OFF).
    check(Loc::t("features.animatedDim"), s.animatedDim(), [](bool on) {
        Settings::instance().setAnimatedDim(on);
    });

    // Text.
    addHeader(Loc::t("features.textTitle"), /*first=*/false);
    check(Loc::t("features.textAlignment"), s.textAlignmentEnabled(), [](bool on) {
        Settings::instance().setTextAlignmentEnabled(on);
    });
    check(Loc::t("features.textBackground"), s.textBackgroundEnabled(), [](bool on) {
        Settings::instance().setTextBackgroundEnabled(on);
    });

    v->addStretch(1);

    // Footer hint at the very bottom (gray, small).
    auto* hint = new QLabel(Loc::t("features.hint"));
    QFont f = hint->font(); f.setPointSize(11); hint->setFont(f);
    hint->setStyleSheet("color: palette(mid);");
    hint->setWordWrap(true);
    v->addWidget(hint);

    return tab;
}

// ---------------------------------------------------------------------------
// About section (bottom of General tab)
// ---------------------------------------------------------------------------
void SettingsWindow::addAboutSection(QWidget* generalTab) {
    Q_UNUSED(generalTab);

    m_aboutContainer = new QWidget;
    auto* col = new QVBoxLayout(m_aboutContainer);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(10);

    // Divider above the block.
    auto* divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    col->addWidget(divider);

    // Social card.
    struct Social { const char* title; const char* handle; const char* url; };
    const std::vector<Social> socials = {
        {"GitHub",      "@VeDono",          "https://github.com/VeDono"},
        {"LinkedIn",    "Sergey Emelyanov", "https://www.linkedin.com/in/sergey-emelyanov-18082b27a/"},
        {"X / Twitter", "@SergeyEDev",      "https://x.com/SergeyEDev"},
    };

    auto* card = new QFrame;
    card->setObjectName("aboutCard");
    card->setStyleSheet(
        "#aboutCard { background-color: palette(base);"
        " border: 1px solid palette(mid); border-radius: 8px; }");
    auto* cardCol = new QVBoxLayout(card);
    cardCol->setContentsMargins(16, 0, 16, 0);
    cardCol->setSpacing(0);

    for (size_t i = 0; i < socials.size(); ++i) {
        const Social& sc = socials[i];
        auto* rowW = new QWidget;
        rowW->setFixedHeight(30);
        auto* h = new QHBoxLayout(rowW);
        h->setContentsMargins(0, 0, 0, 0);
        auto* title = new QLabel(QString::fromUtf8(sc.title));
        auto* link = new LinkButton(QString::fromUtf8(sc.handle),
                                    QString::fromUtf8(sc.url), Qt::AlignRight);
        h->addWidget(title, 0, Qt::AlignLeft);
        h->addStretch(1);
        h->addWidget(link, 0, Qt::AlignRight);
        cardCol->addWidget(rowW);

        if (i < socials.size() - 1) {
            auto* sep = new QFrame;
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Sunken);
            cardCol->addWidget(sep);
        }
    }
    col->addWidget(card);

    // Copyright: "© Sergey Emelyanov YYYY · Made in Ukraine 🇺🇦", where the name
    // is a GitHub link (underlined, blue on hover).
    const int year = QDate::currentDate().year();
    auto* copyRow = new QWidget;
    auto* ch = new QHBoxLayout(copyRow);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(0);

    auto grayText = [](const QString& text, int sizePt) {
        auto* l = new QLabel(text);
        QFont f = l->font(); f.setPointSize(sizePt); l->setFont(f);
        l->setStyleSheet("color: palette(mid);");
        return l;
    };
    const int fs = 11;
    auto* pre = grayText(QStringLiteral("© "), fs);
    auto* nameLink = new InlineLinkLabel(QStringLiteral("Sergey Emelyanov"),
                                         QStringLiteral("https://github.com/VeDono"), fs);
    auto* post = grayText(QStringLiteral(" %1 · %2")
                              .arg(year).arg(Loc::t("settings.madeInUkraine")), fs);

    ch->addStretch(1);
    ch->addWidget(pre);
    ch->addWidget(nameLink);
    ch->addWidget(post);
    ch->addStretch(1);

    col->addWidget(copyRow);

    // Version + edition line (small gray, centered) so this build is easy to tell
    // apart from the native macOS one, e.g. "LightGet 1.0.0 · Cross-platform (Qt 6)".
    // Uses the fixed product name "LightGet" (not the configurable APP_NAME) so
    // the edition tag, not a duplicated "Qt", carries the distinction.
    auto* version = grayText(
        QStringLiteral("LightGet %1 · %2")
            .arg(QString::fromUtf8(LIGHTGET_VERSION),
                 QString::fromUtf8(LIGHTGET_EDITION)),
        10);
    version->setAlignment(Qt::AlignHCenter);
    col->addWidget(version);
}

// ---------------------------------------------------------------------------
// Reset button + handler
// ---------------------------------------------------------------------------
QPushButton* SettingsWindow::makeResetButton(ResetTarget target) {
    // Borderless 22x22 reset button with a "counterclockwise arrow" glyph,
    // tinted secondary. Header asks for QPushButton*, so use a flat QPushButton.
    auto* b = new QPushButton;
    b->setFlat(true);
    b->setFixedSize(22, 22);
    // Tinted to the secondary text color so the white-on-transparent asset is
    // visible on the light tab (same root cause as the menu-bar-icon row, task 3).
    const QColor resetTint = palette().color(QPalette::Active, QPalette::Mid);
    QIcon icon = barIconAssetTinted("arrow.counterclockwise", resetTint, 14);
    if (icon.isNull()) icon = QIcon::fromTheme("edit-undo");
    b->setIcon(icon);
    b->setIconSize(QSize(14, 14));
    if (icon.isNull()) b->setText(QStringLiteral("↺"));  // ↺ fallback glyph
    b->setToolTip(Loc::t("reset.tooltip"));
    connect(b, &QPushButton::clicked, this, [this, target]() { resetTapped(target); });
    return b;
}

void SettingsWindow::resetTapped(ResetTarget target) {
    // Confirmation dialog (Confirm / Cancel).
    QMessageBox box(this);
    box.setIcon(QMessageBox::NoIcon);
    box.setText(Loc::t("reset.title"));
    box.setInformativeText(Loc::t("reset.body"));
    QPushButton* confirm = box.addButton(Loc::t("reset.confirm"), QMessageBox::AcceptRole);
    box.addButton(Loc::t("reset.cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(confirm);
    box.exec();
    if (box.clickedButton() != confirm) return;

    Settings& s = Settings::instance();
    switch (target) {
    case ResetTarget::Language:
        s.setLanguage(QStringLiteral("en"));
        emit languageChanged();
        break;
    case ResetTarget::Hotkey:
        s.setHotKeyCode(CarbonKeys::kVK_ANSI_2);
        s.setHotKeyModifiers(CarbonKeys::cmdKey | CarbonKeys::shiftKey);
        s.setHotKeyDisplay(QStringLiteral("⇧⌘2"));   // ⇧⌘2
        emit hotKeyChanged();
        break;
    case ResetTarget::Dim:
        s.setDimOpacity(0.45);
        break;
    case ResetTarget::Downscale:
        s.setDownscaleRetina(false);
        break;
    case ResetTarget::SaveFolder:
        s.setSaveFolderPath(std::nullopt);
        break;
    }
    reloadUI();
}

// ---------------------------------------------------------------------------
// General-tab action handlers
// ---------------------------------------------------------------------------
void SettingsWindow::chooseFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, Loc::t("settings.chooseFolder"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        Settings::instance().setSaveFolderPath(dir);
        reloadUI();
    }
}

void SettingsWindow::chooseCustomIcon() {
    const QString file = QFileDialog::getOpenFileName(
        this, Loc::t("settings.chooseIcon"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tiff *.webp)"));
    if (!file.isEmpty()) {
        Settings::instance().setBarIconCustomPath(file);
        emit barIconChanged();
        reloadUI();
    }
}

void SettingsWindow::onBarIconSegment(int index) {
    if (index < 0 || index >= kBarIcons.size()) return;
    Settings& s = Settings::instance();
    s.setBarIconCustomPath(std::nullopt);     // choosing a preset clears custom image
    s.setBarIcon(kBarIcons.at(index));
    emit barIconChanged();
}

void SettingsWindow::onLanguageChanged(int index) {
    if (index >= 0 && index < int(kLanguages.size())) {
        const QString code = QString::fromUtf8(kLanguages[size_t(index)].code);
        Settings::instance().setLanguage(code);
        // Persist AppleLanguages-equivalent so native dialogs follow next launch.
        // main.cpp reads this key before constructing QApplication.
        QSettings().setValue(QStringLiteral("AppleLanguages"), QStringList{code});
    }
    reloadUI();
    emit languageChanged();
}

void SettingsWindow::onDimChanged(double value) {
    Settings::instance().setDimOpacity(value);
}

void SettingsWindow::onDownscaleToggled(bool on) {
    Settings::instance().setDownscaleRetina(on);
}

void SettingsWindow::onLaunchAtLoginToggled(bool on) {
    if (setLaunchAtLogin(on)) return;

    // Failure: revert the checkbox and explain (often the cause is running from
    // a build folder). Find the sender checkbox and flip it back silently.
    if (auto* cb = qobject_cast<QCheckBox*>(sender())) {
        QSignalBlocker block(cb);
        cb->setChecked(!on);
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::NoIcon);
    box.setText(Loc::t("launch.errorTitle"));
    box.setInformativeText(Loc::t("launch.errorBody"));
    box.exec();
}

QString SettingsWindow::saveFolderTitle() const {
    const auto path = Settings::instance().saveFolderPath();
    if (!path.has_value()) return Loc::t("settings.askEachTime");
    return QFileInfo(*path).fileName();
}

// ---------------------------------------------------------------------------
// Launch-at-login platform shim (Spec 6 §7: registry / autostart / SMAppService)
// ---------------------------------------------------------------------------
bool SettingsWindow::isLaunchAtLoginEnabled() const {
#if defined(Q_OS_WIN)
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    return run.contains(QStringLiteral("LightGet"));
#elif defined(Q_OS_LINUX)
    const QString dir = QDir::homePath() + "/.config/autostart";
    return QFileInfo::exists(dir + "/LightGet.desktop");
#else
    // macOS: SMAppService status would be queried in an .mm shim; default false.
    return false;
#endif
}

bool SettingsWindow::setLaunchAtLogin(bool enabled) {
#if defined(Q_OS_WIN)
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled) {
        const QString exe = QDir::toNativeSeparators(QApplication::applicationFilePath());
        run.setValue(QStringLiteral("LightGet"), QStringLiteral("\"%1\"").arg(exe));
    } else {
        run.remove(QStringLiteral("LightGet"));
    }
    run.sync();
    return run.status() == QSettings::NoError;
#elif defined(Q_OS_LINUX)
    const QString dir = QDir::homePath() + "/.config/autostart";
    const QString file = dir + "/LightGet.desktop";
    if (enabled) {
        if (!QDir().mkpath(dir)) return false;
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        const QString exe = QApplication::applicationFilePath();
        QByteArray data =
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=LightGet\n"
            "Exec=" + exe.toUtf8() + "\n"
            "X-GNOME-Autostart-enabled=true\n";
        f.write(data);
        f.close();
        return true;
    } else {
        if (!QFileInfo::exists(file)) return true;
        return QFile::remove(file);
    }
#else
    // macOS: real impl lives in an SMAppService .mm shim (register/unregister).
    // TODO(platform): wire SMAppService.mainApp.register()/unregister(). For now
    // report failure so the UI surfaces the explanatory alert as in the source.
    Q_UNUSED(enabled);
    return false;
#endif
}
