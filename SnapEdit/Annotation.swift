import AppKit

// Инструменты на панели.
enum Tool {
    case select      // курсор: выделять / двигать / менять размер области
    case arrow       // стрелка
    case line        // прямая линия (например, подчеркнуть)
    case rectangle   // рамка-прямоугольник
    case filledRect  // прямоугольник, залитый цветом (для цензуры)
    case pen         // свободное рисование
    case text        // текст

    // Стабильный ключ для хранения настроек.
    var key: String {
        switch self {
        case .select:     return "select"
        case .arrow:      return "arrow"
        case .line:       return "line"
        case .rectangle:  return "rectangle"
        case .filledRect: return "filledRect"
        case .pen:        return "pen"
        case .text:       return "text"
        }
    }
}

// Одна нарисованная фигура. Все координаты — в точках вью (origin сверху-слева).
struct Annotation {
    var tool: Tool
    var color: CGColor
    var lineWidth: CGFloat
    var start: CGPoint
    var end: CGPoint
    var points: [CGPoint]   // используется только для pen
    var text: String = ""                       // только для .text
    var fontSize: CGFloat = 18                  // только для .text
    var rotation: CGFloat = 0                   // наклон текста, радианы
    var bgColor: CGColor? = nil                 // фон под текстом (nil = без фона)
    var alignment: NSTextAlignment = .left      // выравнивание текста
}
