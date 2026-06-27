import AppKit

// Плавающая панель кнопок, которая появляется рядом с выделением.
// Сама по себе ничего не «знает» — просто дёргает замыкания наверх (в OverlayView).
final class ToolbarView: NSView {

    var onSelectTool:  ((Tool) -> Void)?
    var onSelectColor: ((CGColor) -> Void)?
    var onUndo:  (() -> Void)?
    var onRedo:  (() -> Void)?
    var onCopy:  (() -> Void)?
    var onSave:  (() -> Void)?
    var onClose: (() -> Void)?

    private let buttonSize: CGFloat = 30
    private let pad: CGFloat = 6
    private var toolButtons: [Tool: NSButton] = [:]
    private var colorButtons: [(color: NSColor, button: NSButton)] = []

    private let colors: [NSColor] = [.systemRed, .systemGreen, .systemBlue,
                                     .systemYellow, .black, .white]

    init() {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0.12, alpha: 0.95).cgColor
        layer?.cornerRadius = 8
        layer?.borderWidth = 0.5
        layer?.borderColor = NSColor(white: 1, alpha: 0.15).cgColor
        buildButtons()
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .arrow)
        for sub in subviews { addCursorRect(sub.frame, cursor: .pointingHand) }
    }

    private func buildButtons() {
        var x = pad

        // Инструменты.
        let tools: [(Tool, String)] = [
            (.select, "cursorarrow"),
            (.arrow, "arrow.up.right"),
            (.line, "line.diagonal"),
            (.rectangle, "rectangle"),
            (.filledRect, "rectangle.fill"),
            (.pen, "pencil.tip"),
            (.text, "textformat"),
        ]
        // Показываем только включённые в настройках инструменты (.select всегда).
        for (tool, symbol) in tools where Settings.isToolEnabled(tool) {
            let b = makeButton(symbol: symbol) { [weak self] in self?.selectTool(tool) }
            b.frame = CGRect(x: x, y: pad, width: buttonSize, height: buttonSize)
            addSubview(b)
            toolButtons[tool] = b
            x += buttonSize + 2
        }
        highlight(tool: .select)

        // Цвета — только если палитра включена в настройках.
        if Settings.showColorPalette {
            x += 6  // разделитель
            for (i, color) in colors.enumerated() {
                let well = makeColorWell(color) { [weak self] in
                    self?.onSelectColor?(color.cgColor)
                    self?.highlightColor(index: i)
                    self?.pop(well: i)
                }
                well.frame = CGRect(x: x, y: pad + (buttonSize - 18) / 2, width: 18, height: 18)
                addSubview(well)
                colorButtons.append((color, well))
                x += 22
            }
            highlightColor(index: 0)   // по умолчанию выбран первый цвет (красный)
        }

        x += 6  // разделитель

        // Действия.
        let actions: [(String, () -> Void)] = [
            ("arrow.uturn.backward", { [weak self] in self?.onUndo?() }),
            ("arrow.uturn.forward",  { [weak self] in self?.onRedo?() }),
            ("doc.on.doc",           { [weak self] in self?.onCopy?() }),
            ("square.and.arrow.down",{ [weak self] in self?.onSave?() }),
            ("xmark",                { [weak self] in self?.onClose?() }),
        ]
        for (symbol, action) in actions {
            let b = makeButton(symbol: symbol, action: action)
            b.frame = CGRect(x: x, y: pad, width: buttonSize, height: buttonSize)
            addSubview(b)
            x += buttonSize + 2
        }

        setFrameSize(NSSize(width: x + pad, height: buttonSize + pad * 2))
    }

    private func selectTool(_ tool: Tool) {
        highlight(tool: tool)
        onSelectTool?(tool)
        if let button = toolButtons[tool] { pop(button) }
    }

    private func highlight(tool selected: Tool) {
        for (tool, button) in toolButtons {
            button.layer?.backgroundColor = (tool == selected)
                ? NSColor.systemBlue.cgColor
                : NSColor.clear.cgColor
        }
    }

    // Выбранный цвет помечаем галочкой контрастного цвета — это видно
    // на любом фоне, включая белый и чёрный (обводка тут не работала бы).
    private func highlightColor(index selected: Int) {
        for (i, item) in colorButtons.enumerated() {
            let isSelected = (i == selected)
            if isSelected {
                let tint: NSColor = item.color.isLight ? .black : .white
                item.button.contentTintColor = tint
                item.button.image = NSImage(systemSymbolName: "checkmark",
                                            accessibilityDescription: nil)?
                    .withSymbolConfiguration(.init(pointSize: 11, weight: .bold))
            } else {
                item.button.image = nil
            }
        }
    }

    private func pop(well index: Int) {
        guard index < colorButtons.count else { return }
        pop(colorButtons[index].button)
    }

    // Короткий лёгкий «поп»: кнопка чуть увеличивается из ЦЕНТРА и возвращается.
    // Масштаб задаём матрицей вокруг центра, НЕ трогая anchorPoint слоя —
    // иначе кнопка «уезжает» и не возвращается. Анимация снимается по завершении,
    // поэтому никаких остаточных смещений.
    private func pop(_ view: NSView) {
        view.wantsLayer = true
        guard let layer = view.layer else { return }
        let b = view.bounds
        let cx = b.midX, cy = b.midY
        let ax = layer.anchorPoint.x * b.width      // позиция точки привязки в координатах слоя
        let ay = layer.anchorPoint.y * b.height

        // Масштаб s вокруг центра при заданном anchorPoint: t = (1-s)·(центр − привязка).
        func centered(_ s: CGFloat) -> NSValue {
            let tx = (1 - s) * (cx - ax)
            let ty = (1 - s) * (cy - ay)
            var t = CATransform3DTranslate(CATransform3DIdentity, tx, ty, 0)
            t = CATransform3DScale(t, s, s, 1)
            return NSValue(caTransform3D: t)
        }

        let anim = CAKeyframeAnimation(keyPath: "transform")
        anim.values = [centered(1.0), centered(1.10), centered(1.0)]
        anim.keyTimes = [0, 0.5, 1]
        anim.duration = 0.18
        anim.timingFunction = CAMediaTimingFunction(name: .easeOut)
        layer.add(anim, forKey: "pop")
    }

    // MARK: - Фабрики кнопок

    private final class ActionButton: NSButton {
        var handler: (() -> Void)?
        @objc func fire() { handler?() }
    }

    private func makeButton(symbol: String, action: @escaping () -> Void) -> NSButton {
        let b = ActionButton()
        b.handler = action
        b.target = b
        b.action = #selector(ActionButton.fire)
        b.bezelStyle = .regularSquare
        b.isBordered = false
        b.wantsLayer = true
        b.layer?.cornerRadius = 5
        b.imagePosition = .imageOnly
        b.contentTintColor = .white
        b.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
        b.refusesFirstResponder = true   // не отбирать фокус у overlay → клавиши всегда работают
        return b
    }

    private func makeColorWell(_ color: NSColor, action: @escaping () -> Void) -> NSButton {
        let b = ActionButton()
        b.handler = action
        b.target = b
        b.action = #selector(ActionButton.fire)
        b.isBordered = false
        b.title = ""
        b.wantsLayer = true
        b.layer?.backgroundColor = color.cgColor
        b.layer?.cornerRadius = 9
        b.layer?.borderWidth = 1
        b.layer?.borderColor = NSColor(white: 1, alpha: 0.5).cgColor
        b.imagePosition = .imageOnly
        b.refusesFirstResponder = true   // не отбирать фокус у overlay
        return b
    }
}

private extension NSColor {
    // Светлый ли цвет — для выбора контрастного цвета галочки.
    var isLight: Bool {
        guard let c = usingColorSpace(.sRGB) else { return false }
        let brightness = 0.299 * c.redComponent
                       + 0.587 * c.greenComponent
                       + 0.114 * c.blueComponent
        return brightness > 0.6
    }
}

// Плавающая панель цветов для выбранного текста: верхний ряд — цвет текста,
// нижний — цвет фона (с вариантом «без фона»).
final class TextInspectorView: NSView {

    var onTextColor: ((CGColor) -> Void)?
    var onBgColor: ((CGColor?) -> Void)?

    private let colors: [NSColor] = [.systemRed, .systemGreen, .systemBlue,
                                     .systemYellow, .black, .white]

    init() {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0.12, alpha: 0.96).cgColor
        layer?.cornerRadius = 8
        layer?.borderWidth = 0.5
        layer?.borderColor = NSColor(white: 1, alpha: 0.15).cgColor
        build()
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isFlipped: Bool { true }

    private final class ActionButton: NSButton {
        var handler: (() -> Void)?
        @objc func fire() { handler?() }
    }

    private var textSwatches: [(color: NSColor, button: NSButton)] = []
    private var bgSwatches: [(color: NSColor?, button: NSButton)] = []   // nil = «без фона»

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .arrow)
        for sub in subviews { addCursorRect(sub.frame, cursor: .pointingHand) }
    }

    private func build() {
        let pad: CGFloat = 6, sw: CGFloat = 16, gap: CGFloat = 4, labelW: CGFloat = 16

        var x1 = pad + labelW + 4
        addSubview(makeLabel("A", x: pad, y: pad))
        for (i, c) in colors.enumerated() {
            let b = makeSwatch(fill: c) { [weak self] in
                self?.onTextColor?(c.cgColor); self?.pop(self?.textSwatches[i].button)
            }
            b.frame = NSRect(x: x1, y: pad, width: sw, height: sw); addSubview(b)
            textSwatches.append((c, b)); x1 += sw + gap
        }

        // Строка цвета фона — только если опция включена в настройках.
        var x2 = pad + labelW + 4
        var rows: CGFloat = 1
        if Settings.textBackgroundEnabled {
            rows = 2
            let y2 = pad + sw + gap
            addSubview(makeLabel("▢", x: pad, y: y2))
            let none = makeNoneSwatch { [weak self] in self?.onBgColor?(nil) }
            none.frame = NSRect(x: x2, y: y2, width: sw, height: sw); addSubview(none)
            bgSwatches.append((nil, none)); x2 += sw + gap
            for (j, c) in colors.enumerated() {
                let idx = j + 1   // +1: индекс 0 занят значком «без фона»
                let b = makeSwatch(fill: c) { [weak self] in
                    self?.onBgColor?(c.cgColor); self?.pop(self?.bgSwatches[idx].button)
                }
                b.frame = NSRect(x: x2, y: y2, width: sw, height: sw); addSubview(b)
                bgSwatches.append((c, b)); x2 += sw + gap
            }
        }

        setFrameSize(NSSize(width: max(x1, x2) + pad - gap,
                            height: pad * 2 + sw * rows + gap * (rows - 1)))
    }

    // Подсветить, какие цвета выбраны сейчас (галочкой контрастного цвета).
    func setSelected(textColor: CGColor, bgColor: CGColor?) {
        for (c, b) in textSwatches { mark(b, on: sameColor(c.cgColor, textColor), light: c.isLight) }
        for (c, b) in bgSwatches {
            let on = (c == nil && bgColor == nil) || (c != nil && bgColor != nil && sameColor(c!.cgColor, bgColor!))
            // «без фона» (c == nil) — белая галочка: swatch прозрачный, под ним тёмная панель.
            mark(b, on: on, light: c?.isLight ?? false)
        }
    }

    private func mark(_ b: NSButton, on: Bool, light: Bool) {
        b.imagePosition = .imageOnly
        if on {
            b.contentTintColor = light ? .black : .white
            b.image = NSImage(systemSymbolName: "checkmark", accessibilityDescription: nil)?
                .withSymbolConfiguration(.init(pointSize: 9, weight: .bold))
        } else if b.toolTip == "none" {   // «без фона» — вернуть перечёркнутый значок
            b.contentTintColor = .white
            b.image = NSImage(systemSymbolName: "nosign", accessibilityDescription: nil)
        } else {
            b.image = nil
        }
    }

    private func sameColor(_ a: CGColor, _ b: CGColor) -> Bool {
        guard let ca = NSColor(cgColor: a)?.usingColorSpace(.sRGB),
              let cb = NSColor(cgColor: b)?.usingColorSpace(.sRGB) else { return false }
        return abs(ca.redComponent - cb.redComponent) < 0.02
            && abs(ca.greenComponent - cb.greenComponent) < 0.02
            && abs(ca.blueComponent - cb.blueComponent) < 0.02
    }

    private func pop(_ view: NSView?) {
        guard let view, let layer = view.layer else { return }
        let b = view.bounds, cx = b.midX, cy = b.midY
        let ax = layer.anchorPoint.x * b.width, ay = layer.anchorPoint.y * b.height
        func centered(_ s: CGFloat) -> NSValue {
            var t = CATransform3DTranslate(CATransform3DIdentity, (1 - s) * (cx - ax), (1 - s) * (cy - ay), 0)
            t = CATransform3DScale(t, s, s, 1)
            return NSValue(caTransform3D: t)
        }
        let anim = CAKeyframeAnimation(keyPath: "transform")
        anim.values = [centered(1.0), centered(1.18), centered(1.0)]
        anim.keyTimes = [0, 0.5, 1]; anim.duration = 0.18
        anim.timingFunction = CAMediaTimingFunction(name: .easeOut)
        layer.add(anim, forKey: "pop")
    }

    private func makeLabel(_ s: String, x: CGFloat, y: CGFloat) -> NSTextField {
        let l = NSTextField(labelWithString: s)
        l.textColor = .white
        l.font = .boldSystemFont(ofSize: 12)
        l.frame = NSRect(x: x, y: y, width: 16, height: 16)
        return l
    }

    private func makeSwatch(fill: NSColor, action: @escaping () -> Void) -> NSButton {
        let b = ActionButton()
        b.handler = action; b.target = b; b.action = #selector(ActionButton.fire)
        b.isBordered = false; b.title = ""
        b.wantsLayer = true
        b.layer?.backgroundColor = fill.cgColor
        b.layer?.cornerRadius = 4
        b.layer?.borderWidth = 1
        b.layer?.borderColor = NSColor(white: 1, alpha: 0.5).cgColor
        b.refusesFirstResponder = true     // не отбирать фокус
        return b
    }

    private func makeNoneSwatch(action: @escaping () -> Void) -> NSButton {
        let b = makeSwatch(fill: .clear, action: action)
        b.toolTip = "none"
        b.image = NSImage(systemSymbolName: "nosign", accessibilityDescription: nil)
        b.imagePosition = .imageOnly
        b.contentTintColor = .white
        return b
    }
}
