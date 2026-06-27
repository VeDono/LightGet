import AppKit

// Язык приложения = выбранный в настройках. Должно стоять ДО первого обращения
// к AppKit, чтобы системные окна (например, окно сохранения NSSavePanel) тоже
// были на нужном языке, а не на языке системы.
UserDefaults.standard.set([Settings.language], forKey: "AppleLanguages")

// Точка входа. Для AppKit-приложения без storyboard/nib мы вручную
// создаём NSApplication, назначаем делегата и запускаем цикл событий.
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
