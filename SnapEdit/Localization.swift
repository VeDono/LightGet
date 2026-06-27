import Foundation

// Простая локализация с переключением языка в рантайме (не через .strings,
// чтобы можно было менять язык прямо в настройках без перезапуска).
enum Loc {

    static func t(_ key: String) -> String {
        let lang = Settings.language
        return table[key]?[lang] ?? table[key]?["en"] ?? key
    }

    private static let table: [String: [String: String]] = [
        "menu.capture":      ["en": "Take Screenshot",  "ru": "Сделать снимок",  "uk": "Зробити знімок"],
        "menu.settings":     ["en": "Settings…",         "ru": "Настройки…",      "uk": "Налаштування…"],
        "menu.quit":         ["en": "Quit",              "ru": "Выйти",           "uk": "Вийти"],

        "error.title":       ["en": "Couldn't capture the screen",
                              "ru": "Не удалось сделать снимок экрана",
                              "uk": "Не вдалося зробити знімок екрана"],
        "error.body":        ["en": "The app most likely doesn't have the “Screen Recording” permission.\n\nOpen: System Settings → Privacy & Security → Screen Recording → enable LightGet, then relaunch the app.",
                              "ru": "Скорее всего приложению не выдано разрешение «Запись экрана».\n\nОткрой: Системные настройки → Конфиденциальность и безопасность → Запись экрана → включи LightGet, затем перезапусти приложение.",
                              "uk": "Найімовірніше, застосунку не надано дозвіл «Запис екрана».\n\nВідкрий: Системні налаштування → Конфіденційність і безпека → Запис екрана → увімкни LightGet, потім перезапусти застосунок."],
        "error.openSettings":["en": "Open Settings",     "ru": "Открыть настройки", "uk": "Відкрити налаштування"],
        "error.close":       ["en": "Close",             "ru": "Закрыть",         "uk": "Закрити"],

        "settings.title":    ["en": "LightGet Settings", "ru": "Настройки LightGet", "uk": "Налаштування LightGet"],
        "settings.hotkey":   ["en": "Shortcut:",         "ru": "Горячая клавиша:", "uk": "Гаряча клавіша:"],
        "settings.dim":      ["en": "Dim level:",        "ru": "Затемнение:",     "uk": "Затемнення:"],
        "settings.language": ["en": "Language:",         "ru": "Язык:",           "uk": "Мова:"],
        "settings.downscale":["en": "Downscale to standard size (1×)",
                              "ru": "Уменьшать до обычного размера (1×)",
                              "uk": "Зменшувати до звичайного розміру (1×)"],
        "settings.animatedDim":["en": "Animated dimming",
                              "ru": "Плавное затемнение",
                              "uk": "Плавне затемнення"],
        "settings.hint":     ["en": "Tip: set a key together with a modifier (⌘ / ⌥ / ⌃ / ⇧).",
                              "ru": "Совет: задайте клавишу вместе с модификатором (⌘ / ⌥ / ⌃ / ⇧).",
                              "uk": "Порада: задайте клавішу разом із модифікатором (⌘ / ⌥ / ⌃ / ⇧)."],
        "settings.saveFolder":["en": "Save folder:",     "ru": "Папка сохранения:", "uk": "Папка збереження:"],
        "settings.askEachTime":["en": "Ask every time",  "ru": "Спрашивать каждый раз", "uk": "Запитувати щоразу"],
        "settings.barIcon":  ["en": "Menu bar icon:",    "ru": "Значок в меню:",  "uk": "Значок у меню:"],
        "settings.chooseFolder":["en": "Choose folder",  "ru": "Выбор папки",     "uk": "Вибір папки"],
        "settings.customIcon":["en": "Custom image for the icon",
                              "ru": "Своя картинка для значка",
                              "uk": "Власна картинка для значка"],
        "settings.chooseIcon":["en": "Choose image",     "ru": "Выбор картинки",  "uk": "Вибір картинки"],
        "settings.launchAtLogin":["en": "Launch at login",
                              "ru": "Запускать при входе в систему",
                              "uk": "Запускати під час входу в систему"],
        "settings.madeInUkraine": ["en": "Made in Ukraine 🇺🇦",
                              "ru": "Сделано в Украине 🇺🇦",
                              "uk": "Зроблено в Україні 🇺🇦"],

        "launch.errorTitle": ["en": "Couldn't change launch-at-login",
                              "ru": "Не удалось изменить автозапуск",
                              "uk": "Не вдалося змінити автозапуск"],
        "launch.errorBody":  ["en": "Launch-at-login works reliably once the app is installed in Applications. It may fail when run from the build folder.",
                              "ru": "Автозапуск надёжно работает, когда приложение установлено в «Программы». Из папки сборки он может не сработать.",
                              "uk": "Автозапуск надійно працює, коли застосунок встановлено в «Програми». З теки збірки він може не спрацювати."],

        "recorder.press":    ["en": "Press keys…",       "ru": "Нажмите клавиши…", "uk": "Натисніть клавіші…"],
        "save.filename":     ["en": "Screenshot.png",    "ru": "Снимок.png",      "uk": "Знімок.png"],

        "reset.tooltip":     ["en": "Reset to default",  "ru": "Сбросить до значения по умолчанию", "uk": "Скинути до типового значення"],
        "reset.title":       ["en": "Reset to default?", "ru": "Сбросить до значения по умолчанию?", "uk": "Скинути до типового значення?"],
        "reset.body":        ["en": "This setting will return to its default value.",
                              "ru": "Этот параметр вернётся к стандартному значению.",
                              "uk": "Цей параметр повернеться до типового значення."],
        "reset.confirm":     ["en": "Reset",             "ru": "Сбросить",        "uk": "Скинути"],
        "reset.cancel":      ["en": "Cancel",            "ru": "Отмена",          "uk": "Скасувати"],

        // Вкладки настроек
        "tab.general":       ["en": "General",           "ru": "Основные",        "uk": "Загальні"],
        "tab.features":      ["en": "Features",          "ru": "Функции",         "uk": "Функції"],
        "tab.about":         ["en": "About",             "ru": "О программе",     "uk": "Про програму"],

        // Раздел «Функции»
        "features.toolsTitle":     ["en": "Tools in the toolbar", "ru": "Инструменты в панели", "uk": "Інструменти в панелі"],
        "features.interfaceTitle": ["en": "Interface",      "ru": "Интерфейс",       "uk": "Інтерфейс"],
        "features.textTitle":      ["en": "Text options",   "ru": "Опции текста",    "uk": "Опції тексту"],
        "features.showColors":     ["en": "Show color palette", "ru": "Показывать палитру цветов", "uk": "Показувати палітру кольорів"],
        "features.textAlignment":  ["en": "Text alignment",  "ru": "Выравнивание текста", "uk": "Вирівнювання тексту"],
        "features.textBackground": ["en": "Text background color", "ru": "Цвет фона текста", "uk": "Колір фону тексту"],
        "features.hint":           ["en": "Disable what you don't use to keep the toolbar simple.",
                                    "ru": "Отключите ненужное, чтобы упростить панель инструментов.",
                                    "uk": "Вимкніть непотрібне, щоб спростити панель інструментів."],

        // Названия инструментов
        "tool.arrow":        ["en": "Arrow",             "ru": "Стрелка",         "uk": "Стрілка"],
        "tool.line":         ["en": "Line",              "ru": "Линия",           "uk": "Лінія"],
        "tool.rectangle":    ["en": "Rectangle",         "ru": "Прямоугольник",   "uk": "Прямокутник"],
        "tool.filledRect":   ["en": "Filled rectangle",  "ru": "Залитый прямоугольник", "uk": "Залитий прямокутник"],
        "tool.pen":          ["en": "Pen",               "ru": "Карандаш",        "uk": "Олівець"],
        "tool.text":         ["en": "Text",              "ru": "Текст",           "uk": "Текст"],
    ]
}
