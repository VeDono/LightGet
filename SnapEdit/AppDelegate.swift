import AppKit
import Carbon.HIToolbox
import CoreGraphics

// Главный «контроллер жизни» приложения:
//  - живёт в строке меню (без иконки в Dock),
//  - регистрирует глобальный хоткей ⇧⌘2,
//  - по хоткею делает скриншот и показывает overlay поверх всего экрана.
final class AppDelegate: NSObject, NSApplicationDelegate {

    private var statusItem: NSStatusItem?
    private let hotKey = HotKey()
    private var overlay: OverlayController?   // не nil, пока overlay открыт
    private var isCapturing = false           // захват в процессе (защита от двойного вызова)
    private var settingsController: SettingsWindowController?
    private var captureMenuItem: NSMenuItem?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // .accessory = приложение без иконки в Dock, только в строке меню.
        NSApp.setActivationPolicy(.accessory)

        setupEditMenu()
        setupStatusItem()

        // Комбинацию берём из настроек (по умолчанию ⇧⌘2).
        hotKey.register(keyCode: Settings.hotKeyCode,
                        modifiers: Settings.hotKeyModifiers) { [weak self] in
            self?.startCapture()
        }
    }

    // Скрытое меню Edit. У accessory-приложения нет строки меню, поэтому стандартные
    // ⌘C/⌘V/⌘X/⌘A/⌘Z в текстовых полях не работают без него. Меню не отображается,
    // но включает диспетч этих команд редактирования по цепочке ответчика.
    private func setupEditMenu() {
        let mainMenu = NSMenu()
        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)
        let editMenu = NSMenu(title: "Edit")
        editItem.submenu = editMenu

        func add(_ title: String, _ selector: String, _ key: String,
                 _ mask: NSEvent.ModifierFlags = .command) {
            let item = NSMenuItem(title: title, action: NSSelectorFromString(selector), keyEquivalent: key)
            item.keyEquivalentModifierMask = mask
            editMenu.addItem(item)
        }
        add("Undo", "undo:", "z")
        add("Redo", "redo:", "z", [.command, .shift])
        editMenu.addItem(.separator())
        add("Cut", "cut:", "x")
        add("Copy", "copy:", "c")
        add("Paste", "paste:", "v")
        add("Select All", "selectAll:", "a")

        NSApp.mainMenu = mainMenu
    }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem = item
        applyBarIcon()
        item.menu = buildMenu()
    }

    func applyBarIcon() {
        if let path = Settings.barIconCustomPath, let img = NSImage(contentsOfFile: path) {
            img.size = NSSize(width: 18, height: 18)   // подгоняем под строку меню
            statusItem?.button?.image = img
        } else {
            statusItem?.button?.image = NSImage(systemSymbolName: Settings.barIcon,
                                                accessibilityDescription: "LightGet")
        }
    }

    private func buildMenu() -> NSMenu {
        let menu = NSMenu()

        let capture = NSMenuItem(title: "\(Loc.t("menu.capture"))  (\(Settings.hotKeyDisplay))",
                                 action: #selector(startCapture), keyEquivalent: "")
        capture.target = self
        capture.image = NSImage(systemSymbolName: "camera.viewfinder", accessibilityDescription: nil)
        menu.addItem(capture)
        captureMenuItem = capture

        // У Settings и Quit убираем keyEquivalent — эти подсказки сбивают с толку:
        // сочетания срабатывают только при открытом меню, а не глобально.
        let settings = NSMenuItem(title: Loc.t("menu.settings"),
                                  action: #selector(openSettings), keyEquivalent: "")
        settings.target = self
        settings.image = NSImage(systemSymbolName: "gearshape", accessibilityDescription: nil)
        menu.addItem(settings)

        menu.addItem(.separator())

        let quit = NSMenuItem(title: Loc.t("menu.quit"),
                              action: #selector(NSApplication.terminate(_:)), keyEquivalent: "")
        quit.image = NSImage(systemSymbolName: "power", accessibilityDescription: nil)
        menu.addItem(quit)
        return menu
    }

    @objc private func openSettings() {
        if settingsController == nil {
            let controller = SettingsWindowController()
            controller.onHotKeyChanged = { [weak self] in
                self?.hotKey.reregister(keyCode: Settings.hotKeyCode,
                                        modifiers: Settings.hotKeyModifiers)
                self?.captureMenuItem?.title = "\(Loc.t("menu.capture"))  (\(Settings.hotKeyDisplay))"
            }
            controller.onLanguageChanged = { [weak self] in
                self?.statusItem?.menu = self?.buildMenu()
            }
            controller.onBarIconChanged = { [weak self] in
                self?.applyBarIcon()
            }
            settingsController = controller
        }
        NSApp.activate(ignoringOtherApps: true)
        settingsController?.showWindow(nil)
        settingsController?.window?.makeKeyAndOrderFront(nil)
    }

    @objc private func startCapture() {
        // Уже открыт overlay ИЛИ захват уже в процессе — второй раз не запускаем.
        // isCapturing закрывает гонку: overlay присваивается только после async-захвата,
        // а флаг ставится синхронно — поэтому второй хоткей не создаст второй overlay.
        guard overlay == nil, !isCapturing else { return }

        // Нет доступа к записи экрана — показываем ТОЛЬКО системное окно macOS
        // (без нашего дублирующего сообщения) и выходим.
        guard CGPreflightScreenCaptureAccess() else {
            CGRequestScreenCaptureAccess()
            return
        }

        isCapturing = true
        Task {
            do {
                let shots = try await ScreenCapture.captureAllDisplays()
                await MainActor.run {
                    let controller = OverlayController(captures: shots) { [weak self] in
                        self?.overlay = nil       // overlay сообщает, что закрылся
                        self?.isCapturing = false
                    }
                    self.overlay = controller
                    self.isCapturing = false
                    controller.show()
                }
            } catch {
                await MainActor.run {
                    self.isCapturing = false
                    self.presentCaptureError(error)
                }
            }
        }
    }

    private func presentCaptureError(_ error: Error) {
        let alert = NSAlert()
        alert.messageText = Loc.t("error.title")
        alert.informativeText = Loc.t("error.body") + "\n\n\(error.localizedDescription)"
        alert.addButton(withTitle: Loc.t("error.openSettings"))
        alert.addButton(withTitle: Loc.t("error.close"))
        if alert.runModal() == .alertFirstButtonReturn {
            if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") {
                NSWorkspace.shared.open(url)
            }
        }
    }
}
