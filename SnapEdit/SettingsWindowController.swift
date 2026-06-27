import AppKit
import Carbon.HIToolbox
import ServiceManagement

// Кнопка-«ловушка»: по клику начинает слушать клавиатуру и запоминает
// нажатую комбинацию (код клавиши + модификаторы), показывая её символами.
final class HotkeyRecorder: NSButton {

    var onCapture: ((UInt32, UInt32, String) -> Void)?
    private var recording = false

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        bezelStyle = .rounded
        setButtonType(.momentaryPushIn)
        target = self
        action = #selector(startRecording)
    }
    required init?(coder: NSCoder) { fatalError() }

    override var acceptsFirstResponder: Bool { true }

    @objc private func startRecording() {
        recording = true
        title = Loc.t("recorder.press")
        window?.makeFirstResponder(self)
    }

    override func keyDown(with event: NSEvent) {
        guard recording else { super.keyDown(with: event); return }

        // Esc — отмена записи.
        if Int(event.keyCode) == kVK_Escape {
            recording = false
            title = Settings.hotKeyDisplay
            return
        }

        let mods = Self.carbonModifiers(event.modifierFlags)
        guard mods != 0 else { NSSound.beep(); return }   // нужен хотя бы один модификатор

        let keyCode = UInt32(event.keyCode)
        let display = Self.displayString(modifiers: event.modifierFlags, event: event)
        recording = false
        title = display
        onCapture?(keyCode, mods, display)
    }

    static func carbonModifiers(_ flags: NSEvent.ModifierFlags) -> UInt32 {
        var m: UInt32 = 0
        if flags.contains(.command) { m |= UInt32(cmdKey) }
        if flags.contains(.option)  { m |= UInt32(optionKey) }
        if flags.contains(.control) { m |= UInt32(controlKey) }
        if flags.contains(.shift)   { m |= UInt32(shiftKey) }
        return m
    }

    static func displayString(modifiers: NSEvent.ModifierFlags, event: NSEvent) -> String {
        var s = ""
        if modifiers.contains(.control) { s += "⌃" }
        if modifiers.contains(.option)  { s += "⌥" }
        if modifiers.contains(.shift)   { s += "⇧" }
        if modifiers.contains(.command) { s += "⌘" }
        let key = (event.charactersIgnoringModifiers ?? "").uppercased()
        return s + (key.isEmpty ? "?" : key)
    }
}

// Окно настроек. Сейчас: горячая клавиша и уровень затемнения.
// Дальше сюда легко добавлять новые опции.
final class SettingsWindowController: NSWindowController {

    var onHotKeyChanged: (() -> Void)?
    var onLanguageChanged: (() -> Void)?
    var onBarIconChanged: (() -> Void)?

    private let recorder = HotkeyRecorder(frame: .zero)

    // Варианты значка в строке меню.
    private let barIcons = ["scissors", "camera.viewfinder", "crop", "rectangle.dashed", "paintbrush.pointed.fill"]

    private let margin: CGFloat = 20

    private enum ResetTarget: Int { case language, hotkey, dim, downscale, saveFolder }

    // Языки: (название в списке, код). Английский — первым (язык по умолчанию).
    private let languages: [(title: String, code: String)] = [
        ("English", "en"), ("Русский", "ru"), ("Українська", "uk")
    ]

    convenience init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 440, height: 580),
                              styleMask: [.titled, .closable],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.center()
        self.init(window: window)
        buildUI()
    }

    // Пересобрать интерфейс (после смены языка или сброса).
    private func reloadUI() {
        window?.contentView?.subviews.forEach { $0.removeFromSuperview() }
        buildUI()
    }

    private func buildUI() {
        guard let content = window?.contentView else { return }
        window?.title = Loc.t("settings.title")

        let tabView = NSTabView(frame: content.bounds)
        tabView.autoresizingMask = [.width, .height]
        content.addSubview(tabView)
        // Размер области контента вкладки (вычитаем поля рамки и панель вкладок).
        let size = NSSize(width: content.bounds.width - 14, height: content.bounds.height - 40)

        func tab(_ title: String, _ view: NSView) {
            let item = NSTabViewItem(identifier: title)
            item.label = title
            item.view = view
            tabView.addTabViewItem(item)
        }
        tab(Loc.t("tab.general"),  buildGeneralTab(size: size))
        tab(Loc.t("tab.features"), buildFeaturesTab(size: size))
    }

    // MARK: - Вкладка «Основные»

    private func buildGeneralTab(size: NSSize) -> NSView {
        let v = NSView(frame: NSRect(origin: .zero, size: size))
        let labelX = margin, labelW: CGFloat = 130, controlX: CGFloat = 160
        let resetX = size.width - margin - 22
        let controlW = resetX - 10 - controlX
        var y = size.height - 24
        func row(_ h: CGFloat, gap: CGFloat = 14) -> CGFloat { let oy = y - h; y = oy - gap; return oy }

        // Язык
        let yLang = row(26)
        v.addSubview(makeLabel(Loc.t("settings.language"), x: labelX, y: yLang + 4, width: labelW))
        let langPopup = NSPopUpButton(frame: NSRect(x: controlX, y: yLang, width: controlW, height: 26))
        langPopup.addItems(withTitles: languages.map { $0.title })
        langPopup.selectItem(at: languages.firstIndex { $0.code == Settings.language } ?? 0)
        langPopup.target = self; langPopup.action = #selector(languageChanged(_:))
        v.addSubview(langPopup)

        // Значок в строке меню
        let yIcon = row(26)
        v.addSubview(makeLabel(Loc.t("settings.barIcon"), x: labelX, y: yIcon + 4, width: labelW))
        let seg = NSSegmentedControl()
        seg.segmentCount = barIcons.count
        seg.trackingMode = .selectOne
        for (i, name) in barIcons.enumerated() {
            seg.setImage(NSImage(systemSymbolName: name, accessibilityDescription: nil), forSegment: i)
            seg.setWidth(33, forSegment: i)
        }
        seg.selectedSegment = (Settings.barIconCustomPath == nil)
            ? (barIcons.firstIndex(of: Settings.barIcon) ?? 0) : -1
        seg.target = self; seg.action = #selector(barIconChanged(_:))
        seg.frame = NSRect(x: controlX, y: yIcon, width: 33 * CGFloat(barIcons.count), height: 26)
        v.addSubview(seg)
        let customIcon = NSButton(frame: NSRect(x: controlX + 33 * CGFloat(barIcons.count) + 6,
                                                y: yIcon, width: 30, height: 26))
        customIcon.bezelStyle = .regularSquare
        customIcon.isBordered = false
        customIcon.imagePosition = .imageOnly
        customIcon.image = NSImage(systemSymbolName: "photo", accessibilityDescription: nil)
        customIcon.toolTip = Loc.t("settings.customIcon")
        customIcon.target = self; customIcon.action = #selector(chooseCustomIcon)
        v.addSubview(customIcon)

        // Горячая клавиша + подсказка
        let yHotkey = row(28, gap: 4)
        v.addSubview(makeLabel(Loc.t("settings.hotkey"), x: labelX, y: yHotkey + 4, width: labelW))
        recorder.frame = NSRect(x: controlX, y: yHotkey, width: controlW, height: 28)
        recorder.title = Settings.hotKeyDisplay
        recorder.onCapture = { [weak self] code, mods, display in
            Settings.hotKeyCode = code
            Settings.hotKeyModifiers = mods
            Settings.hotKeyDisplay = display
            self?.onHotKeyChanged?()
        }
        v.addSubview(recorder)
        v.addSubview(makeResetButton(.hotkey, x: resetX, y: yHotkey + 3))
        let yHint = row(28)
        v.addSubview(makeLabel(Loc.t("settings.hint"), x: controlX, y: yHint, width: controlW + 30, small: true))

        // Затемнение
        let yDim = row(24)
        v.addSubview(makeLabel(Loc.t("settings.dim"), x: labelX, y: yDim + 2, width: labelW))
        let slider = NSSlider(value: Settings.dimOpacity, minValue: 0.1, maxValue: 0.85,
                              target: self, action: #selector(dimChanged(_:)))
        slider.frame = NSRect(x: controlX, y: yDim, width: controlW, height: 24)
        v.addSubview(slider)
        v.addSubview(makeResetButton(.dim, x: resetX, y: yDim + 1))

        // Папка сохранения
        let ySave = row(28)
        v.addSubview(makeLabel(Loc.t("settings.saveFolder"), x: labelX, y: ySave + 4, width: labelW))
        let folderButton = NSButton(title: saveFolderTitle(), target: self, action: #selector(chooseFolder))
        folderButton.bezelStyle = .rounded
        folderButton.frame = NSRect(x: controlX, y: ySave, width: controlW, height: 28)
        folderButton.lineBreakMode = .byTruncatingMiddle
        v.addSubview(folderButton)
        v.addSubview(makeResetButton(.saveFolder, x: resetX, y: ySave + 3))

        // Даунскейл
        let yDown = row(22)
        let downscale = NSButton(checkboxWithTitle: Loc.t("settings.downscale"),
                                 target: self, action: #selector(downscaleChanged(_:)))
        downscale.state = Settings.downscaleRetina ? .on : .off
        downscale.frame = NSRect(x: labelX, y: yDown, width: resetX - labelX - 8, height: 20)
        downscale.lineBreakMode = .byTruncatingTail
        v.addSubview(downscale)
        v.addSubview(makeResetButton(.downscale, x: resetX, y: yDown))

        // Автозапуск
        let yLaunch = row(22)
        let launch = NSButton(checkboxWithTitle: Loc.t("settings.launchAtLogin"),
                              target: self, action: #selector(launchAtLoginChanged(_:)))
        launch.state = (SMAppService.mainApp.status == .enabled) ? .on : .off
        launch.frame = NSRect(x: labelX, y: yLaunch, width: size.width - 2 * margin, height: 20)
        v.addSubview(launch)

        // Инфо об авторе — внизу вкладки «Основные».
        addAboutSection(to: v, width: size.width)
        return v
    }

    // MARK: - Вкладка «Функции»

    private func buildFeaturesTab(size: NSSize) -> NSView {
        let v = NSView(frame: NSRect(origin: .zero, size: size))
        var y = size.height - 26

        func header(_ title: String) {
            y -= 22
            let l = NSTextField(labelWithString: title)
            l.font = .boldSystemFont(ofSize: 12)
            l.frame = NSRect(x: margin, y: y, width: size.width - 2 * margin, height: 18)
            v.addSubview(l)
            y -= 6
        }
        func check(_ title: String, _ on: Bool, _ handler: @escaping (Bool) -> Void) {
            y -= 22
            let b = makeCheckbox(title, on, handler)
            b.frame = NSRect(x: margin + 10, y: y, width: size.width - 2 * margin - 10, height: 20)
            v.addSubview(b)
            y -= 4
        }

        header(Loc.t("features.toolsTitle"))
        for tool in [Tool.arrow, .line, .rectangle, .filledRect, .pen, .text] {
            check(Loc.t("tool.\(tool.key)"), Settings.isToolEnabled(tool)) { on in
                Settings.setTool(tool, enabled: on)
            }
        }

        y -= 10
        header(Loc.t("features.interfaceTitle"))
        check(Loc.t("features.showColors"), Settings.showColorPalette) { on in
            Settings.showColorPalette = on
        }
        check(Loc.t("settings.animatedDim"), Settings.animatedDim) { on in
            Settings.animatedDim = on
        }

        y -= 10
        header(Loc.t("features.textTitle"))
        check(Loc.t("features.textAlignment"), Settings.textAlignmentEnabled) { on in
            Settings.textAlignmentEnabled = on
        }
        check(Loc.t("features.textBackground"), Settings.textBackgroundEnabled) { on in
            Settings.textBackgroundEnabled = on
        }

        y -= 12
        v.addSubview(makeLabel(Loc.t("features.hint"), x: margin, y: y - 28,
                               width: size.width - 2 * margin, small: true))
        return v
    }

    private func makeCheckbox(_ title: String, _ on: Bool,
                              _ handler: @escaping (Bool) -> Void) -> ToggleButton {
        let b = ToggleButton(checkboxWithTitle: title, target: nil, action: nil)
        b.state = on ? .on : .off
        b.onToggle = handler
        b.target = b
        b.action = #selector(ToggleButton.fire)
        return b
    }

    // MARK: - Блок «Об авторе» (внизу вкладки «Основные»)

    private func addAboutSection(to v: NSView, width W: CGFloat) {
        // Разделитель над блоком.
        let divider = NSBox(frame: NSRect(x: margin, y: 150, width: W - 2 * margin, height: 1))
        divider.boxType = .separator
        v.addSubview(divider)

        let socials: [(title: String, handle: String, url: String)] = [
            ("GitHub",     "@VeDono",          "https://github.com/VeDono"),
            ("LinkedIn",   "Sergey Emelyanov", "https://www.linkedin.com/in/sergey-emelyanov-18082b27a/"),
            ("X / Twitter","@SergeyEDev",      "https://x.com/SergeyEDev"),
        ]
        let rowH: CGFloat = 30
        let cardH = rowH * CGFloat(socials.count)
        let cardY: CGFloat = 50
        let card = NSView(frame: NSRect(x: margin, y: cardY, width: W - 2 * margin, height: cardH))
        card.wantsLayer = true
        card.layer?.backgroundColor = NSColor.controlBackgroundColor.cgColor
        card.layer?.cornerRadius = 8
        card.layer?.borderWidth = 0.5
        card.layer?.borderColor = NSColor.separatorColor.cgColor

        let cardW = card.bounds.width
        for (i, s) in socials.enumerated() {
            let rowBottom = cardH - rowH * CGFloat(i + 1)
            let label = makeLabel(s.title, x: 16, y: rowBottom + (rowH - 20) / 2, width: 160)
            card.addSubview(label)
            let link = LinkButton(text: s.handle, url: s.url, align: .right)
            link.frame = NSRect(x: cardW - 16 - 240, y: rowBottom + (rowH - 18) / 2, width: 240, height: 18)
            card.addSubview(link)
            if i < socials.count - 1 {
                let sep = NSBox(frame: NSRect(x: 14, y: rowBottom, width: cardW - 28, height: 1))
                sep.boxType = .separator
                card.addSubview(sep)
            }
        }
        v.addSubview(card)

        // Копирайт: «© Sergey Emelyanov YYYY · Made in Ukraine 🇺🇦»,
        // где «Sergey Emelyanov» — ссылка на GitHub (подчёркнута, синяя при наведении).
        let year = Calendar.current.component(.year, from: Date())
        let fontSize: CGFloat = 11
        let pre = makeGrayText("© ", size: fontSize)
        let nameLink = InlineLinkField(text: "Sergey Emelyanov",
                                       url: "https://github.com/VeDono", size: fontSize)
        let post = makeGrayText(" \(year) · \(Loc.t("settings.madeInUkraine"))", size: fontSize)
        pre.sizeToFit(); nameLink.sizeToFit(); post.sizeToFit()
        let total = pre.frame.width + nameLink.frame.width + post.frame.width
        let baseY = cardY - 34
        var x = (W - total) / 2
        pre.setFrameOrigin(CGPoint(x: x, y: baseY));       x += pre.frame.width
        nameLink.setFrameOrigin(CGPoint(x: x, y: baseY));  x += nameLink.frame.width
        post.setFrameOrigin(CGPoint(x: x, y: baseY))
        v.addSubview(pre); v.addSubview(nameLink); v.addSubview(post)
    }

    private func makeGrayText(_ text: String, size: CGFloat) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.font = .systemFont(ofSize: size)
        label.textColor = .secondaryLabelColor
        return label
    }

    @objc private func launchAtLoginChanged(_ sender: NSButton) {
        do {
            if sender.state == .on {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            // Откатываем галочку и поясняем (часто причина — запуск из папки сборки).
            sender.state = (sender.state == .on) ? .off : .on
            let alert = NSAlert()
            alert.icon = NSImage()
            alert.messageText = Loc.t("launch.errorTitle")
            alert.informativeText = Loc.t("launch.errorBody")
            alert.runModal()
        }
    }

    private func saveFolderTitle() -> String {
        guard let path = Settings.saveFolderPath else { return Loc.t("settings.askEachTime") }
        return (path as NSString).lastPathComponent
    }

    @objc private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = Loc.t("settings.chooseFolder")
        if panel.runModal() == .OK, let url = panel.url {
            Settings.saveFolderPath = url.path
            reloadUI()
        }
    }

    @objc private func barIconChanged(_ sender: NSSegmentedControl) {
        guard sender.selectedSegment >= 0, sender.selectedSegment < barIcons.count else { return }
        Settings.barIconCustomPath = nil   // выбор пресета сбрасывает свою картинку
        Settings.barIcon = barIcons[sender.selectedSegment]
        onBarIconChanged?()
    }

    @objc private func chooseCustomIcon() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.png, .jpeg, .image]
        panel.prompt = Loc.t("settings.chooseIcon")
        if panel.runModal() == .OK, let url = panel.url {
            Settings.barIconCustomPath = url.path
            onBarIconChanged?()
            reloadUI()
        }
    }

    private func makeResetButton(_ target: ResetTarget, x: CGFloat, y: CGFloat) -> NSButton {
        let b = NSButton(frame: NSRect(x: x, y: y, width: 22, height: 22))
        b.bezelStyle = .regularSquare
        b.isBordered = false
        b.imagePosition = .imageOnly
        b.image = NSImage(systemSymbolName: "arrow.counterclockwise", accessibilityDescription: nil)
        b.contentTintColor = .secondaryLabelColor
        b.toolTip = Loc.t("reset.tooltip")
        b.target = self
        b.action = #selector(resetTapped(_:))
        b.tag = target.rawValue
        return b
    }

    @objc private func resetTapped(_ sender: NSButton) {
        let alert = NSAlert()
        alert.icon = NSImage()   // убрать дефолтную иконку приложения из диалога
        alert.messageText = Loc.t("reset.title")
        alert.informativeText = Loc.t("reset.body")
        alert.addButton(withTitle: Loc.t("reset.confirm"))
        alert.addButton(withTitle: Loc.t("reset.cancel"))
        guard alert.runModal() == .alertFirstButtonReturn else { return }

        switch ResetTarget(rawValue: sender.tag) {
        case .language:
            Settings.language = "en"
            onLanguageChanged?()
        case .hotkey:
            Settings.hotKeyCode = UInt32(kVK_ANSI_2)
            Settings.hotKeyModifiers = UInt32(cmdKey | shiftKey)
            Settings.hotKeyDisplay = "⇧⌘2"
            onHotKeyChanged?()
        case .dim:
            Settings.dimOpacity = 0.45
        case .downscale:
            Settings.downscaleRetina = false
        case .saveFolder:
            Settings.saveFolderPath = nil
        case .none:
            break
        }
        reloadUI()
    }

    @objc private func languageChanged(_ sender: NSPopUpButton) {
        let idx = sender.indexOfSelectedItem
        if idx >= 0, idx < languages.count {
            Settings.language = languages[idx].code
            // Чтобы системные окна (сохранение) тоже сменили язык — со следующего запуска.
            UserDefaults.standard.set([languages[idx].code], forKey: "AppleLanguages")
        }
        reloadUI()
        onLanguageChanged?()
    }

    @objc private func dimChanged(_ sender: NSSlider) {
        Settings.dimOpacity = sender.doubleValue
    }

    @objc private func downscaleChanged(_ sender: NSButton) {
        Settings.downscaleRetina = (sender.state == .on)
    }

    private func makeLabel(_ text: String, x: CGFloat, y: CGFloat,
                           width: CGFloat, small: Bool = false) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.frame = NSRect(x: x, y: y, width: width, height: small ? 32 : 20)
        if small {
            label.font = .systemFont(ofSize: 11)
            label.textColor = .secondaryLabelColor
            label.lineBreakMode = .byWordWrapping
            label.maximumNumberOfLines = 2
        }
        return label
    }
}

// Кнопка-ссылка: по клику открывает URL, курсор при наведении — указатель.
// Два стиля: обычная (синий текст) и inline (серый + подчёркивание, синий при наведении).
final class LinkButton: NSButton {
    private var urlString = ""
    private var displayText = ""
    private var baseColor: NSColor = .linkColor
    private var hoverColor: NSColor = .linkColor
    private var underline = false
    private var fontValue: NSFont = .systemFont(ofSize: 12)
    private var textAlignment: NSTextAlignment = .left
    private var tracking: NSTrackingArea?

    // Обычная синяя ссылка (для карточки соцсетей).
    convenience init(text: String, url: String, align: NSTextAlignment,
                     bold: Bool = false, size: CGFloat = 12) {
        self.init(frame: .zero)
        configure(text: text, url: url, align: align,
                  base: .linkColor, hover: .linkColor, underline: false,
                  font: bold ? .boldSystemFont(ofSize: size) : .systemFont(ofSize: size))
    }

    private func configure(text: String, url: String, align: NSTextAlignment,
                           base: NSColor, hover: NSColor, underline: Bool, font: NSFont) {
        urlString = url; displayText = text; textAlignment = align
        baseColor = base; hoverColor = hover; self.underline = underline; fontValue = font
        isBordered = false
        bezelStyle = .inline
        focusRingType = .none
        target = self
        action = #selector(openURL)
        apply(color: baseColor)
    }

    private func apply(color: NSColor) {
        let style = NSMutableParagraphStyle()
        style.alignment = textAlignment
        style.lineBreakMode = .byTruncatingTail
        var attrs: [NSAttributedString.Key: Any] = [
            .foregroundColor: color, .font: fontValue, .paragraphStyle: style,
        ]
        if underline { attrs[.underlineStyle] = NSUnderlineStyle.single.rawValue }
        attributedTitle = NSAttributedString(string: displayText, attributes: attrs)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = tracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
                               options: [.mouseEnteredAndExited, .activeInActiveApp],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }

    override func mouseEntered(with event: NSEvent) { apply(color: hoverColor) }
    override func mouseExited(with event: NSEvent)  { apply(color: baseColor) }

    @objc private func openURL() {
        if let u = URL(string: urlString) { NSWorkspace.shared.open(u) }
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .pointingHand)
    }
}

// Inline-ссылка на основе NSTextField (а не NSButton) — чтобы базовая линия текста
// совпадала с соседними обычными метками в той же строке.
// Серый текст с подчёркиванием, синий при наведении, по клику открывает URL.
final class InlineLinkField: NSTextField {
    private var urlString = ""
    private var text = ""
    private var fontSize: CGFloat = 11
    private let baseColor: NSColor = .secondaryLabelColor
    private let hoverColor: NSColor = .linkColor
    private var tracking: NSTrackingArea?

    convenience init(text: String, url: String, size: CGFloat) {
        self.init(labelWithString: text)
        self.text = text
        self.urlString = url
        self.fontSize = size
        isEditable = false
        isSelectable = false
        isBordered = false
        drawsBackground = false
        apply(color: baseColor)
        sizeToFit()
    }

    private func apply(color: NSColor) {
        attributedStringValue = NSAttributedString(string: text, attributes: [
            .foregroundColor: color,
            .font: NSFont.systemFont(ofSize: fontSize),
            .underlineStyle: NSUnderlineStyle.single.rawValue,
        ])
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = tracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds,
                               options: [.mouseEnteredAndExited, .activeInActiveApp],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }

    override func mouseEntered(with event: NSEvent) { apply(color: hoverColor) }
    override func mouseExited(with event: NSEvent)  { apply(color: baseColor) }
    override func mouseDown(with event: NSEvent) {
        if let u = URL(string: urlString) { NSWorkspace.shared.open(u) }
    }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .pointingHand) }
}

// Чекбокс с замыканием вместо target/action — для вкладки «Функции».
final class ToggleButton: NSButton {
    var onToggle: ((Bool) -> Void)?
    @objc func fire() { onToggle?(state == .on) }
}
