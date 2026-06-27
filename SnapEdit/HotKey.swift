import AppKit
import Carbon.HIToolbox

// Тонкая обёртка над Carbon API глобальных горячих клавиш.
// Глобальный хоткей работает, даже когда наше приложение неактивно —
// именно это нужно для «нажал из любого места и сделал скриншот».
final class HotKey {

    private var hotKeyRef: EventHotKeyRef?
    private var eventHandler: EventHandlerRef?
    private var callback: (() -> Void)?

    // Carbon-колбэк — это C-функция, поэтому общаемся с экземплярами
    // через статический реестр по числовому id.
    private let id: UInt32
    private static var registry: [UInt32: HotKey] = [:]
    private static var counter: UInt32 = 0

    init() {
        HotKey.counter += 1
        self.id = HotKey.counter
    }

    func register(keyCode: UInt32, modifiers: UInt32, callback: @escaping () -> Void) {
        self.callback = callback
        HotKey.registry[id] = self
        installHandlerIfNeeded()
        reregister(keyCode: keyCode, modifiers: modifiers)
    }

    // Сменить комбинацию на лету (вызывается из настроек).
    func reregister(keyCode: UInt32, modifiers: UInt32) {
        if let hotKeyRef {
            UnregisterEventHotKey(hotKeyRef)
            self.hotKeyRef = nil
        }
        // Сигнатура 'SNAP' — произвольный 4-байтовый идентификатор владельца.
        let signature: OSType = 0x534E4150
        var hotKeyID = EventHotKeyID(signature: signature, id: id)
        RegisterEventHotKey(keyCode, modifiers, hotKeyID,
                            GetApplicationEventTarget(), 0, &hotKeyRef)
    }

    private func installHandlerIfNeeded() {
        guard eventHandler == nil else { return }
        var eventType = EventTypeSpec(eventClass: OSType(kEventClassKeyboard),
                                      eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, _ -> OSStatus in
            var hkID = EventHotKeyID()
            GetEventParameter(event,
                              EventParamName(kEventParamDirectObject),
                              EventParamType(typeEventHotKeyID),
                              nil,
                              MemoryLayout<EventHotKeyID>.size,
                              nil,
                              &hkID)
            HotKey.registry[hkID.id]?.callback?()
            return noErr
        }, 1, &eventType, nil, &eventHandler)
    }

    deinit {
        if let hotKeyRef { UnregisterEventHotKey(hotKeyRef) }
        if let eventHandler { RemoveEventHandler(eventHandler) }
        HotKey.registry[id] = nil
    }
}
