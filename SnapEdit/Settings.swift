import AppKit
import Carbon.HIToolbox

// Простое хранилище настроек поверх UserDefaults.
// Значения по умолчанию совпадают с прежним «зашитым» поведением.
enum Settings {
    private static let d = UserDefaults.standard

    private enum Key {
        static let keyCode   = "hotKeyCode"
        static let modifiers = "hotKeyModifiers"
        static let display   = "hotKeyDisplay"
        static let dim       = "dimOpacity"
        static let downscale = "downscaleRetina"
        static let language  = "language"
        static let saveFolder = "saveFolder"
        static let barIcon    = "barIcon"
        static let barIconCustom = "barIconCustom"
    }

    // Путь к своей картинке для значка в строке меню. nil → используется barIcon.
    static var barIconCustomPath: String? {
        get { d.string(forKey: Key.barIconCustom) }
        set {
            if let v = newValue { d.set(v, forKey: Key.barIconCustom) }
            else { d.removeObject(forKey: Key.barIconCustom) }
        }
    }

    // Папка для сохранения. nil → спрашивать каждый раз.
    static var saveFolderPath: String? {
        get { d.string(forKey: Key.saveFolder) }
        set {
            if let v = newValue { d.set(v, forKey: Key.saveFolder) }
            else { d.removeObject(forKey: Key.saveFolder) }
        }
    }

    // SF Symbol для значка в строке меню.
    static var barIcon: String {
        get { d.string(forKey: Key.barIcon) ?? "scissors" }
        set { d.set(newValue, forKey: Key.barIcon) }
    }

    // MARK: - Тумблеры функций (по умолчанию всё включено)

    // Показывать ли инструмент в панели. .select включён всегда.
    static func isToolEnabled(_ tool: Tool) -> Bool {
        if tool == .select { return true }
        return d.object(forKey: "tool_\(tool.key)") as? Bool ?? true
    }
    static func setTool(_ tool: Tool, enabled: Bool) {
        d.set(enabled, forKey: "tool_\(tool.key)")
    }

    static var showColorPalette: Bool {
        get { d.object(forKey: "showColors") as? Bool ?? true }
        set { d.set(newValue, forKey: "showColors") }
    }
    static var textAlignmentEnabled: Bool {
        get { d.object(forKey: "textAlign") as? Bool ?? true }
        set { d.set(newValue, forKey: "textAlign") }
    }
    static var textBackgroundEnabled: Bool {
        get { d.object(forKey: "textBg") as? Bool ?? true }
        set { d.set(newValue, forKey: "textBg") }
    }

    // Плавное появление/исчезновение затемнения. По умолчанию выключено
    // (затемнение появляется/исчезает мгновенно — как прежде).
    static var animatedDim: Bool {
        get { d.bool(forKey: "animatedDim") }   // по умолчанию false
        set { d.set(newValue, forKey: "animatedDim") }
    }

    // "en" / "ru" / "uk". По умолчанию — английский (для новых пользователей).
    static var language: String {
        get { d.string(forKey: Key.language) ?? "en" }
        set { d.set(newValue, forKey: Key.language) }
    }

    static var hotKeyCode: UInt32 {
        get { UInt32(d.object(forKey: Key.keyCode) as? Int ?? kVK_ANSI_2) }
        set { d.set(Int(newValue), forKey: Key.keyCode) }
    }

    static var hotKeyModifiers: UInt32 {
        get { UInt32(d.object(forKey: Key.modifiers) as? Int ?? (cmdKey | shiftKey)) }
        set { d.set(Int(newValue), forKey: Key.modifiers) }
    }

    static var hotKeyDisplay: String {
        get { d.string(forKey: Key.display) ?? "⇧⌘2" }
        set { d.set(newValue, forKey: Key.display) }
    }

    static var dimOpacity: Double {
        get { d.object(forKey: Key.dim) as? Double ?? 0.45 }
        set { d.set(newValue, forKey: Key.dim) }
    }

    // true → итоговый скриншот сохраняется в обычном (1×) размере, а не в Retina (2×).
    static var downscaleRetina: Bool {
        get { d.bool(forKey: Key.downscale) }   // по умолчанию false
        set { d.set(newValue, forKey: Key.downscale) }
    }
}
