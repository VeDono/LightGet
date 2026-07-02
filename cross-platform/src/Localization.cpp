// Localization.cpp — Runtime EN/RU/UK localization table.
//
// Faithful port of Localization.swift (Spec 5 §2). The lookup performs the
// three-level fallback: requested language -> "en" -> the raw key.
// Language switches instantly at runtime (no QTranslator/relaunch); the current
// language is read from Settings::language().
//
// UTF-8 content (\n, curly quotes “ ”, arrows ->, modifier glyphs ⌘⌥⌃⇧, the
// 🇺🇦 emoji) is preserved verbatim from the Swift source.

#include "Localization.h"
#include "Settings.h"

#include <QtGlobal>

namespace Loc {

// Platform-correct modifier list shown in the "Tip:" hint, so the example keys
// match what the user actually presses (the Swift source hardcoded mac glyphs).
//   - Windows: Ctrl / Alt / Shift / Win
//   - Linux:   Ctrl / Alt / Shift / Super
//   - macOS:   ⌘ / ⌥ / ⌃ / ⇧
#if defined(Q_OS_MACOS)
#  define LG_MODIFIER_HINT "⌘ / ⌥ / ⌃ / ⇧"
#elif defined(Q_OS_WIN)
#  define LG_MODIFIER_HINT "Ctrl / Alt / Shift / Win"
#else
#  define LG_MODIFIER_HINT "Ctrl / Alt / Shift / Super"
#endif

const QHash<QString, QHash<QString, QString>>& table() {
    static const QHash<QString, QHash<QString, QString>> t = {
        {QStringLiteral("menu.capture"),
         {{QStringLiteral("en"), QStringLiteral("Take Screenshot")},
          {QStringLiteral("ru"), QStringLiteral("Сделать снимок")},
          {QStringLiteral("uk"), QStringLiteral("Зробити знімок")}}},
        {QStringLiteral("menu.settings"),
         {{QStringLiteral("en"), QStringLiteral("Settings…")},
          {QStringLiteral("ru"), QStringLiteral("Настройки…")},
          {QStringLiteral("uk"), QStringLiteral("Налаштування…")}}},
        {QStringLiteral("menu.quit"),
         {{QStringLiteral("en"), QStringLiteral("Quit")},
          {QStringLiteral("ru"), QStringLiteral("Выйти")},
          {QStringLiteral("uk"), QStringLiteral("Вийти")}}},

        {QStringLiteral("error.title"),
         {{QStringLiteral("en"), QStringLiteral("Couldn't capture the screen")},
          {QStringLiteral("ru"), QStringLiteral("Не удалось сделать снимок экрана")},
          {QStringLiteral("uk"), QStringLiteral("Не вдалося зробити знімок екрана")}}},
        {QStringLiteral("error.body"),
         {{QStringLiteral("en"), QStringLiteral("The app most likely doesn't have the “Screen Recording” permission.\n\nOpen: System Settings → Privacy & Security → Screen Recording → enable LightGet, then relaunch the app.")},
          {QStringLiteral("ru"), QStringLiteral("Скорее всего приложению не выдано разрешение «Запись экрана».\n\nОткрой: Системные настройки → Конфиденциальность и безопасность → Запись экрана → включи LightGet, затем перезапусти приложение.")},
          {QStringLiteral("uk"), QStringLiteral("Найімовірніше, застосунку не надано дозвіл «Запис екрана».\n\nВідкрий: Системні налаштування → Конфіденційність і безпека → Запис екрана → увімкни LightGet, потім перезапусти застосунок.")}}},
        {QStringLiteral("error.body.wayland"),
         {{QStringLiteral("en"), QStringLiteral("On Wayland, screen capture goes through the desktop portal. Make sure xdg-desktop-portal (and the backend for your desktop, e.g. -gtk or -kde) is installed and running, then try again.")},
          {QStringLiteral("ru"), QStringLiteral("На Wayland захват экрана идёт через desktop-портал. Убедись, что установлен и запущен xdg-desktop-portal (и бэкенд для твоего окружения, например -gtk или -kde), затем повтори.")},
          {QStringLiteral("uk"), QStringLiteral("На Wayland захоплення екрана відбувається через desktop-портал. Переконайся, що встановлено й запущено xdg-desktop-portal (і бекенд для твого середовища, напр. -gtk або -kde), потім спробуй ще раз.")}}},
        {QStringLiteral("error.body.generic"),
         {{QStringLiteral("en"), QStringLiteral("The screen couldn't be captured. Please try again.")},
          {QStringLiteral("ru"), QStringLiteral("Не удалось захватить экран. Попробуй ещё раз.")},
          {QStringLiteral("uk"), QStringLiteral("Не вдалося захопити екран. Спробуй ще раз.")}}},
        {QStringLiteral("error.openSettings"),
         {{QStringLiteral("en"), QStringLiteral("Open Settings")},
          {QStringLiteral("ru"), QStringLiteral("Открыть настройки")},
          {QStringLiteral("uk"), QStringLiteral("Відкрити налаштування")}}},
        {QStringLiteral("error.close"),
         {{QStringLiteral("en"), QStringLiteral("Close")},
          {QStringLiteral("ru"), QStringLiteral("Закрыть")},
          {QStringLiteral("uk"), QStringLiteral("Закрити")}}},

        {QStringLiteral("save.failed.title"),
         {{QStringLiteral("en"), QStringLiteral("Couldn't save the screenshot")},
          {QStringLiteral("ru"), QStringLiteral("Не удалось сохранить снимок")},
          {QStringLiteral("uk"), QStringLiteral("Не вдалося зберегти знімок")}}},
        {QStringLiteral("save.failed.body"),
         {{QStringLiteral("en"), QStringLiteral("Couldn't write to:\n%1\n\nThe folder may be missing, full, or read-only. The image is still on your clipboard.")},
          {QStringLiteral("ru"), QStringLiteral("Не удалось записать в:\n%1\n\nПапка может отсутствовать, быть переполнена или доступна только для чтения. Изображение осталось в буфере обмена.")},
          {QStringLiteral("uk"), QStringLiteral("Не вдалося записати в:\n%1\n\nТека може бути відсутня, переповнена або доступна лише для читання. Зображення залишилося в буфері обміну.")}}},

        {QStringLiteral("hotkey.conflict"),
         {{QStringLiteral("en"), QStringLiteral("The shortcut %1 couldn't be registered — another app is probably already using it. Pick a different combination.")},
          {QStringLiteral("ru"), QStringLiteral("Не удалось назначить сочетание %1 — вероятно, оно уже занято другим приложением. Выбери другую комбинацию.")},
          {QStringLiteral("uk"), QStringLiteral("Не вдалося призначити комбінацію %1 — імовірно, вона вже зайнята іншим застосунком. Обери іншу комбінацію.")}}},

        {QStringLiteral("settings.title"),
         {{QStringLiteral("en"), QStringLiteral("LightGet Settings")},
          {QStringLiteral("ru"), QStringLiteral("Настройки LightGet")},
          {QStringLiteral("uk"), QStringLiteral("Налаштування LightGet")}}},
        {QStringLiteral("settings.hotkey"),
         {{QStringLiteral("en"), QStringLiteral("Shortcut:")},
          {QStringLiteral("ru"), QStringLiteral("Горячая клавиша:")},
          {QStringLiteral("uk"), QStringLiteral("Гаряча клавіша:")}}},
        {QStringLiteral("settings.dim"),
         {{QStringLiteral("en"), QStringLiteral("Dim level:")},
          {QStringLiteral("ru"), QStringLiteral("Затемнение:")},
          {QStringLiteral("uk"), QStringLiteral("Затемнення:")}}},
        {QStringLiteral("settings.language"),
         {{QStringLiteral("en"), QStringLiteral("Language:")},
          {QStringLiteral("ru"), QStringLiteral("Язык:")},
          {QStringLiteral("uk"), QStringLiteral("Мова:")}}},
        {QStringLiteral("settings.downscale"),
         {{QStringLiteral("en"), QStringLiteral("Downscale to standard size (1×)")},
          {QStringLiteral("ru"), QStringLiteral("Уменьшать до обычного размера (1×)")},
          {QStringLiteral("uk"), QStringLiteral("Зменшувати до звичайного розміру (1×)")}}},
        {QStringLiteral("settings.hint"),
         {{QStringLiteral("en"), QStringLiteral("Tip: set a key together with a modifier (" LG_MODIFIER_HINT ").")},
          {QStringLiteral("ru"), QStringLiteral("Совет: задайте клавишу вместе с модификатором (" LG_MODIFIER_HINT ").")},
          {QStringLiteral("uk"), QStringLiteral("Порада: задайте клавішу разом із модифікатором (" LG_MODIFIER_HINT ").")}}},
        {QStringLiteral("settings.saveFolder"),
         {{QStringLiteral("en"), QStringLiteral("Save folder:")},
          {QStringLiteral("ru"), QStringLiteral("Папка сохранения:")},
          {QStringLiteral("uk"), QStringLiteral("Папка збереження:")}}},
        {QStringLiteral("settings.askEachTime"),
         {{QStringLiteral("en"), QStringLiteral("Ask every time")},
          {QStringLiteral("ru"), QStringLiteral("Спрашивать каждый раз")},
          {QStringLiteral("uk"), QStringLiteral("Запитувати щоразу")}}},
        {QStringLiteral("settings.barIcon"),
         {{QStringLiteral("en"), QStringLiteral("Menu bar icon:")},
          {QStringLiteral("ru"), QStringLiteral("Значок в меню:")},
          {QStringLiteral("uk"), QStringLiteral("Значок у меню:")}}},
        {QStringLiteral("settings.chooseFolder"),
         {{QStringLiteral("en"), QStringLiteral("Choose folder")},
          {QStringLiteral("ru"), QStringLiteral("Выбор папки")},
          {QStringLiteral("uk"), QStringLiteral("Вибір папки")}}},
        {QStringLiteral("settings.customIcon"),
         {{QStringLiteral("en"), QStringLiteral("Custom image for the icon")},
          {QStringLiteral("ru"), QStringLiteral("Своя картинка для значка")},
          {QStringLiteral("uk"), QStringLiteral("Власна картинка для значка")}}},
        {QStringLiteral("settings.chooseIcon"),
         {{QStringLiteral("en"), QStringLiteral("Choose image")},
          {QStringLiteral("ru"), QStringLiteral("Выбор картинки")},
          {QStringLiteral("uk"), QStringLiteral("Вибір картинки")}}},
        {QStringLiteral("settings.launchAtLogin"),
         {{QStringLiteral("en"), QStringLiteral("Launch at login")},
          {QStringLiteral("ru"), QStringLiteral("Запускать при входе в систему")},
          {QStringLiteral("uk"), QStringLiteral("Запускати під час входу в систему")}}},
        {QStringLiteral("settings.madeInUkraine"),
         {{QStringLiteral("en"), QStringLiteral("Made in Ukraine 🇺🇦")},
          {QStringLiteral("ru"), QStringLiteral("Сделано в Украине 🇺🇦")},
          {QStringLiteral("uk"), QStringLiteral("Зроблено в Україні 🇺🇦")}}},

        // Colon-free row labels for the redesigned settings card (Direction B).
        // The design shows labels WITHOUT trailing colons; these mirror the
        // existing settings.* strings minus the colon. The original colon
        // variants are kept (still referenced by tooltips / other call sites).
        {QStringLiteral("settings.language.plain"),
         {{QStringLiteral("en"), QStringLiteral("Language")},
          {QStringLiteral("ru"), QStringLiteral("Язык")},
          {QStringLiteral("uk"), QStringLiteral("Мова")}}},
        {QStringLiteral("settings.barIcon.plain"),
         {{QStringLiteral("en"), QStringLiteral("Menu bar icon")},
          {QStringLiteral("ru"), QStringLiteral("Значок в меню")},
          {QStringLiteral("uk"), QStringLiteral("Значок у меню")}}},
        {QStringLiteral("settings.hotkey.plain"),
         {{QStringLiteral("en"), QStringLiteral("Shortcut")},
          {QStringLiteral("ru"), QStringLiteral("Горячая клавиша")},
          {QStringLiteral("uk"), QStringLiteral("Гаряча клавіша")}}},
        {QStringLiteral("settings.saveFolder.plain"),
         {{QStringLiteral("en"), QStringLiteral("Save folder")},
          {QStringLiteral("ru"), QStringLiteral("Папка сохранения")},
          {QStringLiteral("uk"), QStringLiteral("Папка збереження")}}},
        {QStringLiteral("settings.dim.plain"),
         {{QStringLiteral("en"), QStringLiteral("Dim level")},
          {QStringLiteral("ru"), QStringLiteral("Затемнение")},
          {QStringLiteral("uk"), QStringLiteral("Затемнення")}}},

        // Appearance (NEW segmented switch: Auto / Light / Dark).
        {QStringLiteral("settings.appearance"),
         {{QStringLiteral("en"), QStringLiteral("Appearance")},
          {QStringLiteral("ru"), QStringLiteral("Оформление")},
          {QStringLiteral("uk"), QStringLiteral("Оформлення")}}},
        {QStringLiteral("settings.appearance.auto"),
         {{QStringLiteral("en"), QStringLiteral("Auto")},
          {QStringLiteral("ru"), QStringLiteral("Авто")},
          {QStringLiteral("uk"), QStringLiteral("Авто")}}},
        {QStringLiteral("settings.appearance.light"),
         {{QStringLiteral("en"), QStringLiteral("Light")},
          {QStringLiteral("ru"), QStringLiteral("Светлая")},
          {QStringLiteral("uk"), QStringLiteral("Світла")}}},
        {QStringLiteral("settings.appearance.dark"),
         {{QStringLiteral("en"), QStringLiteral("Dark")},
          {QStringLiteral("ru"), QStringLiteral("Тёмная")},
          {QStringLiteral("uk"), QStringLiteral("Темна")}}},

        {QStringLiteral("launch.errorTitle"),
         {{QStringLiteral("en"), QStringLiteral("Couldn't change launch-at-login")},
          {QStringLiteral("ru"), QStringLiteral("Не удалось изменить автозапуск")},
          {QStringLiteral("uk"), QStringLiteral("Не вдалося змінити автозапуск")}}},
        {QStringLiteral("launch.errorBody"),
         {{QStringLiteral("en"), QStringLiteral("Launch-at-login works reliably once the app is installed in Applications. It may fail when run from the build folder.")},
          {QStringLiteral("ru"), QStringLiteral("Автозапуск надёжно работает, когда приложение установлено в «Программы». Из папки сборки он может не сработать.")},
          {QStringLiteral("uk"), QStringLiteral("Автозапуск надійно працює, коли застосунок встановлено в «Програми». З теки збірки він може не спрацювати.")}}},

        {QStringLiteral("recorder.press"),
         {{QStringLiteral("en"), QStringLiteral("Press keys…")},
          {QStringLiteral("ru"), QStringLiteral("Нажмите клавиши…")},
          {QStringLiteral("uk"), QStringLiteral("Натисніть клавіші…")}}},
        {QStringLiteral("save.filename"),
         {{QStringLiteral("en"), QStringLiteral("Screenshot.png")},
          {QStringLiteral("ru"), QStringLiteral("Снимок.png")},
          {QStringLiteral("uk"), QStringLiteral("Знімок.png")}}},
        {QStringLiteral("save.title"),
         {{QStringLiteral("en"), QStringLiteral("Save screenshot")},
          {QStringLiteral("ru"), QStringLiteral("Сохранить снимок")},
          {QStringLiteral("uk"), QStringLiteral("Зберегти знімок")}}},

        {QStringLiteral("reset.tooltip"),
         {{QStringLiteral("en"), QStringLiteral("Reset to default")},
          {QStringLiteral("ru"), QStringLiteral("Сбросить до значения по умолчанию")},
          {QStringLiteral("uk"), QStringLiteral("Скинути до типового значення")}}},
        {QStringLiteral("reset.title"),
         {{QStringLiteral("en"), QStringLiteral("Reset to default?")},
          {QStringLiteral("ru"), QStringLiteral("Сбросить до значения по умолчанию?")},
          {QStringLiteral("uk"), QStringLiteral("Скинути до типового значення?")}}},
        {QStringLiteral("reset.body"),
         {{QStringLiteral("en"), QStringLiteral("This setting will return to its default value.")},
          {QStringLiteral("ru"), QStringLiteral("Этот параметр вернётся к стандартному значению.")},
          {QStringLiteral("uk"), QStringLiteral("Цей параметр повернеться до типового значення.")}}},
        {QStringLiteral("reset.confirm"),
         {{QStringLiteral("en"), QStringLiteral("Reset")},
          {QStringLiteral("ru"), QStringLiteral("Сбросить")},
          {QStringLiteral("uk"), QStringLiteral("Скинути")}}},
        {QStringLiteral("reset.cancel"),
         {{QStringLiteral("en"), QStringLiteral("Cancel")},
          {QStringLiteral("ru"), QStringLiteral("Отмена")},
          {QStringLiteral("uk"), QStringLiteral("Скасувати")}}},

        // Text colour / background palette row labels (design §4)
        {QStringLiteral("palette.text"),
         {{QStringLiteral("en"), QStringLiteral("TEXT")},
          {QStringLiteral("ru"), QStringLiteral("ТЕКСТ")},
          {QStringLiteral("uk"), QStringLiteral("ТЕКСТ")}}},
        {QStringLiteral("palette.bg"),
         {{QStringLiteral("en"), QStringLiteral("FILL")},
          {QStringLiteral("ru"), QStringLiteral("ФОН")},
          {QStringLiteral("uk"), QStringLiteral("ФОН")}}},

        // Settings tabs
        {QStringLiteral("tab.general"),
         {{QStringLiteral("en"), QStringLiteral("General")},
          {QStringLiteral("ru"), QStringLiteral("Основные")},
          {QStringLiteral("uk"), QStringLiteral("Загальні")}}},
        {QStringLiteral("tab.features"),
         {{QStringLiteral("en"), QStringLiteral("Features")},
          {QStringLiteral("ru"), QStringLiteral("Функции")},
          {QStringLiteral("uk"), QStringLiteral("Функції")}}},
        {QStringLiteral("tab.about"),
         {{QStringLiteral("en"), QStringLiteral("About")},
          {QStringLiteral("ru"), QStringLiteral("О программе")},
          {QStringLiteral("uk"), QStringLiteral("Про програму")}}},

        // "Features" section
        {QStringLiteral("features.toolsTitle"),
         {{QStringLiteral("en"), QStringLiteral("Tools in the toolbar")},
          {QStringLiteral("ru"), QStringLiteral("Инструменты в панели")},
          {QStringLiteral("uk"), QStringLiteral("Інструменти в панелі")}}},
        {QStringLiteral("features.interfaceTitle"),
         {{QStringLiteral("en"), QStringLiteral("Interface")},
          {QStringLiteral("ru"), QStringLiteral("Интерфейс")},
          {QStringLiteral("uk"), QStringLiteral("Інтерфейс")}}},
        {QStringLiteral("features.textTitle"),
         {{QStringLiteral("en"), QStringLiteral("Text options")},
          {QStringLiteral("ru"), QStringLiteral("Опции текста")},
          {QStringLiteral("uk"), QStringLiteral("Опції тексту")}}},
        {QStringLiteral("features.showColors"),
         {{QStringLiteral("en"), QStringLiteral("Show color palette")},
          {QStringLiteral("ru"), QStringLiteral("Показывать палитру цветов")},
          {QStringLiteral("uk"), QStringLiteral("Показувати палітру кольорів")}}},
        {QStringLiteral("features.animatedDim"),
         {{QStringLiteral("en"), QStringLiteral("Animated dimming")},
          {QStringLiteral("ru"), QStringLiteral("Плавное затемнение")},
          {QStringLiteral("uk"), QStringLiteral("Плавне затемнення")}}},
        {QStringLiteral("features.textAlignment"),
         {{QStringLiteral("en"), QStringLiteral("Text alignment")},
          {QStringLiteral("ru"), QStringLiteral("Выравнивание текста")},
          {QStringLiteral("uk"), QStringLiteral("Вирівнювання тексту")}}},
        {QStringLiteral("features.textBackground"),
         {{QStringLiteral("en"), QStringLiteral("Text background color")},
          {QStringLiteral("ru"), QStringLiteral("Цвет фона текста")},
          {QStringLiteral("uk"), QStringLiteral("Колір фону тексту")}}},
        {QStringLiteral("features.textFont"),
         {{QStringLiteral("en"), QStringLiteral("Font")},
          {QStringLiteral("ru"), QStringLiteral("Шрифт")},
          {QStringLiteral("uk"), QStringLiteral("Шрифт")}}},
        {QStringLiteral("features.textFontSize"),
         {{QStringLiteral("en"), QStringLiteral("Font size")},
          {QStringLiteral("ru"), QStringLiteral("Размер шрифта")},
          {QStringLiteral("uk"), QStringLiteral("Розмір шрифту")}}},
        {QStringLiteral("features.textBold"),
         {{QStringLiteral("en"), QStringLiteral("Bold")},
          {QStringLiteral("ru"), QStringLiteral("Жирный")},
          {QStringLiteral("uk"), QStringLiteral("Жирний")}}},
        {QStringLiteral("features.textItalic"),
         {{QStringLiteral("en"), QStringLiteral("Italic")},
          {QStringLiteral("ru"), QStringLiteral("Курсив")},
          {QStringLiteral("uk"), QStringLiteral("Курсив")}}},
        {QStringLiteral("features.textUnderline"),
         {{QStringLiteral("en"), QStringLiteral("Underline")},
          {QStringLiteral("ru"), QStringLiteral("Подчёркнутый")},
          {QStringLiteral("uk"), QStringLiteral("Підкреслений")}}},
        {QStringLiteral("features.hint"),
         {{QStringLiteral("en"), QStringLiteral("Disable what you don't use to keep the toolbar simple.")},
          {QStringLiteral("ru"), QStringLiteral("Отключите ненужное, чтобы упростить панель инструментов.")},
          {QStringLiteral("uk"), QStringLiteral("Вимкніть непотрібне, щоб спростити панель інструментів.")}}},

        // Tool names
        {QStringLiteral("tool.arrow"),
         {{QStringLiteral("en"), QStringLiteral("Arrow")},
          {QStringLiteral("ru"), QStringLiteral("Стрелка")},
          {QStringLiteral("uk"), QStringLiteral("Стрілка")}}},
        {QStringLiteral("tool.line"),
         {{QStringLiteral("en"), QStringLiteral("Line")},
          {QStringLiteral("ru"), QStringLiteral("Линия")},
          {QStringLiteral("uk"), QStringLiteral("Лінія")}}},
        {QStringLiteral("tool.rectangle"),
         {{QStringLiteral("en"), QStringLiteral("Rectangle")},
          {QStringLiteral("ru"), QStringLiteral("Прямоугольник")},
          {QStringLiteral("uk"), QStringLiteral("Прямокутник")}}},
        {QStringLiteral("tool.filledRect"),
         {{QStringLiteral("en"), QStringLiteral("Filled rectangle")},
          {QStringLiteral("ru"), QStringLiteral("Залитый прямоугольник")},
          {QStringLiteral("uk"), QStringLiteral("Залитий прямокутник")}}},
        {QStringLiteral("tool.pen"),
         {{QStringLiteral("en"), QStringLiteral("Pen")},
          {QStringLiteral("ru"), QStringLiteral("Карандаш")},
          {QStringLiteral("uk"), QStringLiteral("Олівець")}}},
        {QStringLiteral("tool.text"),
         {{QStringLiteral("en"), QStringLiteral("Text")},
          {QStringLiteral("ru"), QStringLiteral("Текст")},
          {QStringLiteral("uk"), QStringLiteral("Текст")}}},
    };
    return t;
}

#undef LG_MODIFIER_HINT

QString t(const QString& key) {
    const QString lang = Settings::instance().language();
    const auto it = table().constFind(key);
    if (it == table().constEnd())
        return key; // unknown key -> raw key

    const QHash<QString, QString>& byLang = it.value();

    // 1) requested language
    const auto langIt = byLang.constFind(lang);
    if (langIt != byLang.constEnd())
        return langIt.value();

    // 2) English fallback
    const auto enIt = byLang.constFind(QStringLiteral("en"));
    if (enIt != byLang.constEnd())
        return enIt.value();

    // 3) raw key
    return key;
}

} // namespace Loc
