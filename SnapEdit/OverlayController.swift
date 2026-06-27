import AppKit

// Окно overlay: безрамочное, прозрачное, поверх всего (включая Dock и меню-бар).
final class OverlayWindow: NSWindow {
    override init(contentRect: NSRect,
                  styleMask style: NSWindow.StyleMask,
                  backing backingStoreType: NSWindow.BackingStoreType,
                  defer flag: Bool) {
        super.init(contentRect: contentRect,
                   styleMask: .borderless,
                   backing: backingStoreType,
                   defer: flag)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        // CGShieldingWindowLevel — выше строки меню и Dock.
        level = NSWindow.Level(rawValue: Int(CGShieldingWindowLevel()))
        collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]
        ignoresMouseEvents = false
        acceptsMouseMovedEvents = true
        // Без анимации появления — иначе экран на миг «отъезжает»/масштабируется.
        animationBehavior = .none
    }

    // Безрамочные окна по умолчанию не становятся key — разрешаем явно,
    // иначе не будем получать нажатия клавиш (Esc, ⌘C и т.д.).
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

// Управляет интерактивными overlay на ВСЕХ экранах. Выделять можно на любом;
// начатое выделение на одном экране сбрасывает выделения на других.
final class OverlayController {
    private var windows: [OverlayWindow] = []
    private var views: [OverlayView] = []
    private let onClose: () -> Void
    private var previousApp: NSRunningApplication?   // что было активно до скрина (игра)

    init(captures: [CapturedScreen], onClose: @escaping () -> Void) {
        self.onClose = onClose
        for cap in captures {
            let frame = cap.screen.frame
            let view = OverlayView(frame: NSRect(origin: .zero, size: frame.size),
                                   screenshot: cap.image)
            let window = OverlayWindow(contentRect: frame,
                                       styleMask: .borderless,
                                       backing: .buffered, defer: false)
            window.contentView = view
            window.initialFirstResponder = view
            view.onFinish = { [weak self] in self?.close() }
            view.onBeganSelection = { [weak self, weak view] in self?.clearOthers(except: view) }
            windows.append(window)
            views.append(view)
        }
    }

    func show() {
        previousApp = NSWorkspace.shared.frontmostApplication   // запомнить игру до активации
        NSApp.activate(ignoringOtherApps: true)
        for w in windows { w.orderFront(nil) }

        // Ключевым (для клавиатуры) делаем экран под курсором.
        let mouse = NSEvent.mouseLocation
        if let idx = windows.firstIndex(where: { NSMouseInRect(mouse, $0.frame, false) }) {
            windows[idx].makeKeyAndOrderFront(nil)
            windows[idx].makeFirstResponder(views[idx])
        } else {
            windows.first?.makeKeyAndOrderFront(nil)
        }
        forceCursorVisible()
        // Подстраховка от гонки с WindowServer/игрой: повторяем на следующем тике.
        DispatchQueue.main.async { [weak self] in self?.forceCursorVisible() }
    }

    private func clearOthers(except active: OverlayView?) {
        for v in views where v !== active { v.clearSelectionState() }
    }

    // Если курсор скрыт (например, запущена игра в UE5) — принудительно показываем.
    private func forceCursorVisible() {
        CGAssociateMouseAndMouseCursorPosition(1)   // на случай mouselook (мышь отвязана)
        NSCursor.unhide()                            // на случай NSCursor.hide()
        CGDisplayShowCursor(CGMainDisplayID())       // на случай CGDisplayHideCursor()
        for screen in NSScreen.screens {
            if let id = screen.displayID { CGDisplayShowCursor(id) }
        }
    }

    func close() {
        for w in windows { w.orderOut(nil) }
        windows.removeAll()
        views.removeAll()
        // Возвращаем фокус игре — она сама снова скроет курсор в своём обработчике фокуса.
        if let prev = previousApp,
           prev.bundleIdentifier != Bundle.main.bundleIdentifier {
            prev.activate()
        }
        previousApp = nil
        onClose()
    }
}
