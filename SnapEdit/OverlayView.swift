import AppKit
import Carbon.HIToolbox

// Сердце приложения. Один вью на весь экран, в котором происходит ВСЁ:
// затемнение, выделение области, изменение её размера, рисование стрелок,
// плавающая панель кнопок, копирование и сохранение.
final class OverlayView: NSView {

    // MARK: - Данные

    private let screenshot: CGImage
    var onFinish: (() -> Void)?
    var onBeganSelection: (() -> Void)?   // начали выделять — очистить другие экраны

    // Выделение в точках вью. Вью flipped → origin сверху-слева (как у скриншота).
    private var selection: CGRect?

    private var annotations: [Annotation] = []
    private var redoStack: [Annotation] = []   // отменённые (для кнопки «вернуть»)
    private var currentAnnotation: Annotation?

    private var tool: Tool = .select
    private var color: CGColor = NSColor.systemRed.cgColor
    private let lineWidth: CGFloat = 3

    private var toolbar: ToolbarView?

    // MARK: - Состояние перетаскивания

    private enum DragMode {
        case none
        case newSelection
        case moveSelection
        case resize(Handle)
        case draw
        case resizeText
        case moveText
        case rotateText
    }
    private enum Handle { case tl, t, tr, r, br, b, bl, l }

    private var dragMode: DragMode = .none
    private var dragStart: CGPoint = .zero
    private var selectionAtDragStart: CGRect = .zero

    private let handleSize: CGFloat = 9

    // MARK: - Состояние текста

    private var activeTextIndex: Int?            // выбранный текст (рамка + ручки)
    private var editingIndex: Int?               // индекс редактируемого текста (nil = новый)
    private weak var textEditor: NSTextField?    // живое поле ввода
    private weak var textInspector: TextInspectorView?  // плавающая панель цветов
    private weak var editControls: NSView?              // ✓/✗ под полем ввода
    private weak var alignControls: NSView?             // выбор выравнивания над полем
    private var alignButtons: [(NSTextAlignment, NSButton)] = []
    private var currentTextAlignment: NSTextAlignment = .left
    private var textResizeStartSize: CGFloat = 18
    private var textResizeStartPoint: CGPoint = .zero
    private var textMoveStartOrigin: CGPoint = .zero
    private var textRotateStartAngle: CGFloat = 0
    private var textRotateStartPointerAngle: CGFloat = 0
    private var snapGuideV = false   // вертикальная направляющая (центр по X совпал)
    private var snapGuideH = false   // горизонтальная направляющая (центр по Y совпал)
    private var cursorRectsDisabled = false   // чтобы курсор не мерцал при перетаскивании

    // MARK: - Инициализация

    init(frame: NSRect, screenshot: CGImage) {
        self.screenshot = screenshot
        super.init(frame: frame)
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) не используется") }

    override var isFlipped: Bool { true }            // origin сверху-слева
    override var acceptsFirstResponder: Bool { true }

    // Реагировать на ПЕРВЫЙ клик, даже если окно ещё не активно — иначе при переходе
    // на другой монитор первый клик «съедается» на активацию окна.
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    // Масштаб: сколько пикселей скриншота приходится на 1 точку вью (≈ Retina factor).
    private var scale: CGFloat { CGFloat(screenshot.width) / bounds.width }

    // Масштаб итоговой картинки: 1× если включён даунскейл, иначе полный (Retina).
    private var outputScale: CGFloat { Settings.downscaleRetina ? 1 : scale }

    // MARK: - Курсор

    override func resetCursorRects() {
        // Кнопки ✓/✗ под полем ввода — указатель.
        if let ec = editControls { addCursorRect(ec.frame, cursor: .pointingHand) }

        guard let sel = selection else {
            // Область ещё не выделена — крестик по всему экрану.
            addCursorRect(bounds, cursor: .crosshair)
            return
        }
        switch tool {
        case .select:
            // Над выделенной областью — «рука» (можно схватить и двигать)…
            addCursorRect(sel, cursor: .openHand)
            // …а над ручками по краям/углам — стрелки изменения размера.
            for (h, r) in handleRects(for: sel) {
                addCursorRect(r.insetBy(dx: -3, dy: -3), cursor: resizeCursor(for: h))
            }
        case .text:
            addCursorRect(sel, cursor: .iBeam)
            if editingIndex == nil, let idx = activeTextIndex, idx < annotations.count,
               annotations[idx].tool == .text {
                let a = annotations[idx]
                addCursorRect(screenRectAround(resizeHandleLocal(a), a), cursor: .crosshair)
                addCursorRect(screenRectAround(rotateHandleLocal(a), a), cursor: .openHand)
            }
        default:
            // Активен инструмент рисования — крестик внутри области.
            addCursorRect(sel, cursor: .crosshair)
        }
    }

    // На время перетаскивания глушим cursor-rects, иначе система переустанавливает
    // курсор из них поверх нашего → мерцание «рука ↔ сжатая рука».
    // Во время выделения/изменения области держим курсор в пределах текущего экрана,
    // чтобы он не «улетал» на соседний монитор. Всё считаем в координатах дисплея (CG).
    private func confineCursorToScreen() {
        guard let id = window?.screen?.displayID,
              let current = CGEvent(source: nil)?.location else { return }
        let b = CGDisplayBounds(id)
        var p = current
        p.x = min(max(b.minX + 1, p.x), b.maxX - 2)
        p.y = min(max(b.minY + 1, p.y), b.maxY - 2)
        if p != current {
            CGWarpMouseCursorPosition(p)
            CGAssociateMouseAndMouseCursorPosition(1)   // вернуть синхронность курсора
        }
    }

    private func beginCustomCursorDrag() {
        if !cursorRectsDisabled { window?.disableCursorRects(); cursorRectsDisabled = true }
    }
    private func endCustomCursorDrag() {
        if cursorRectsDisabled { window?.enableCursorRects(); cursorRectsDisabled = false }
    }

    private func resizeCursor(for handle: Handle) -> NSCursor {
        switch handle {
        case .l, .r:   return .resizeLeftRight
        case .t, .b:   return .resizeUpDown
        case .tl, .br: return OverlayView.diagonalCursor("_windowResizeNorthWestSouthEastCursor") ?? .resizeUpDown
        case .tr, .bl: return OverlayView.diagonalCursor("_windowResizeNorthEastSouthWestCursor") ?? .resizeUpDown
        }
    }

    // Диагональных курсоров нет в публичном API NSCursor — берём системные
    // «оконные» через приватный селектор, с безопасным запасным вариантом.
    private static func diagonalCursor(_ name: String) -> NSCursor? {
        let sel = NSSelectorFromString(name)
        let cls: AnyObject = NSCursor.self
        guard cls.responds(to: sel) else { return nil }
        return cls.perform(sel)?.takeUnretainedValue() as? NSCursor
    }

    // MARK: - Отрисовка

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }

        drawImageUpright(screenshot, in: bounds, ctx: ctx)
        drawDim(in: ctx)
        for (i, a) in annotations.enumerated() where i != editingIndex {
            drawAnnotation(a, in: ctx)
        }
        if let cur = currentAnnotation { drawAnnotation(cur, in: ctx) }
        drawSelectionChrome(in: ctx)
        drawActiveTextChrome(in: ctx)
        drawSnapGuides(in: ctx)
    }

    // Розовые пунктирные направляющие при привязке центра текста к центру области.
    private func drawSnapGuides(in ctx: CGContext) {
        guard let sel = selection, snapGuideV || snapGuideH else { return }
        ctx.saveGState()
        ctx.setStrokeColor(NSColor.systemPink.cgColor)
        ctx.setLineWidth(1)
        ctx.setLineDash(phase: 0, lengths: [5, 4])
        if snapGuideV {
            ctx.move(to: CGPoint(x: sel.midX, y: sel.minY))
            ctx.addLine(to: CGPoint(x: sel.midX, y: sel.maxY))
        }
        if snapGuideH {
            ctx.move(to: CGPoint(x: sel.minX, y: sel.midY))
            ctx.addLine(to: CGPoint(x: sel.maxX, y: sel.midY))
        }
        ctx.strokePath()
        ctx.restoreGState()
    }

    // Затемняем весь экран, кроме выделенной области (правило even-odd «вырезает дырку»).
    private func drawDim(in ctx: CGContext) {
        ctx.saveGState()
        ctx.setFillColor(NSColor.black.withAlphaComponent(CGFloat(Settings.dimOpacity)).cgColor)
        ctx.addRect(bounds)
        if let sel = selection { ctx.addRect(sel) }
        ctx.fillPath(using: .evenOdd)
        ctx.restoreGState()
    }

    // Белая рамка вокруг выделения, 8 «ручек» и подпись с размером.
    private func drawSelectionChrome(in ctx: CGContext) {
        guard let sel = selection else { return }

        ctx.saveGState()
        ctx.setStrokeColor(NSColor.white.cgColor)
        ctx.setLineWidth(1)
        ctx.stroke(sel)

        for rect in handleRects(for: sel).values {
            ctx.setFillColor(NSColor.white.cgColor)
            ctx.fill(rect)
            ctx.setStrokeColor(NSColor.systemBlue.cgColor)
            ctx.setLineWidth(1)
            ctx.stroke(rect)
        }
        ctx.restoreGState()

        // Размер выделения в пикселях итоговой картинки.
        let wPx = Int(sel.width * outputScale)
        let hPx = Int(sel.height * outputScale)
        let label = "\(wPx) × \(hPx)"
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 12, weight: .medium),
            .foregroundColor: NSColor.white
        ]
        let size = (label as NSString).size(withAttributes: attrs)
        var origin = CGPoint(x: sel.minX, y: sel.minY - size.height - 6)
        if origin.y < 2 { origin.y = sel.minY + 4 }   // не выходить за верх экрана
        let bg = CGRect(x: origin.x - 4, y: origin.y - 2,
                        width: size.width + 8, height: size.height + 4)
        ctx.setFillColor(NSColor.black.withAlphaComponent(0.6).cgColor)
        ctx.fill(bg)
        (label as NSString).draw(at: origin, withAttributes: attrs)
    }

    // Рисуем CGImage «правильной стороной вверх» в flipped-контексте.
    private func drawImageUpright(_ image: CGImage, in rect: CGRect, ctx: CGContext) {
        ctx.saveGState()
        ctx.translateBy(x: rect.minX, y: rect.maxY)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: rect.width, height: rect.height))
        ctx.restoreGState()
    }

    private func drawAnnotation(_ a: Annotation, in ctx: CGContext) {
        ctx.saveGState()
        ctx.setStrokeColor(a.color)
        ctx.setFillColor(a.color)
        ctx.setLineWidth(a.lineWidth)
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)

        switch a.tool {
        case .rectangle:
            ctx.stroke(rect(from: a.start, to: a.end))

        case .filledRect:
            ctx.fill(rect(from: a.start, to: a.end))

        case .pen:
            guard let first = a.points.first else { break }
            ctx.move(to: first)
            for p in a.points.dropFirst() { ctx.addLine(to: p) }
            ctx.strokePath()

        case .arrow:
            drawArrow(from: a.start, to: a.end, width: a.lineWidth, in: ctx)

        case .line:
            ctx.move(to: a.start)
            ctx.addLine(to: a.end)
            ctx.strokePath()

        case .text:
            drawTextAnnotation(a, in: ctx)

        case .select:
            break
        }
        ctx.restoreGState()
    }

    // --- Геометрия текста (всё в локальных, неповёрнутых координатах) ---

    private func textSize(_ a: Annotation) -> CGSize {
        let font = NSFont.systemFont(ofSize: a.fontSize)
        let s = a.text.isEmpty ? "Текст" : a.text
        let r = (s as NSString).boundingRect(
            with: NSSize(width: 1000, height: CGFloat.greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading],
            attributes: [.font: font])
        return CGSize(width: max(ceil(r.width), 10), height: max(ceil(r.height), font.pointSize))
    }
    private func textLocalRect(_ a: Annotation) -> CGRect { CGRect(origin: a.start, size: textSize(a)) }
    private func textCenter(_ a: Annotation) -> CGPoint {
        let s = textSize(a); return CGPoint(x: a.start.x + s.width / 2, y: a.start.y + s.height / 2)
    }
    private func resizeHandleLocal(_ a: Annotation) -> CGRect {
        let r = textLocalRect(a).insetBy(dx: -3, dy: -3)
        return CGRect(x: r.maxX - 9, y: r.maxY - 9, width: 18, height: 18)   // правый-нижний
    }
    private func rotateHandleLocal(_ a: Annotation) -> CGRect {
        let r = textLocalRect(a).insetBy(dx: -3, dy: -3)
        return CGRect(x: r.maxX - 9, y: r.minY - 9, width: 18, height: 18)   // правый-верхний
    }
    // Переводим точку из экрана в локальную (неповёрнутую) систему текста.
    private func localPoint(_ p: CGPoint, _ a: Annotation) -> CGPoint {
        let c = textCenter(a)
        let dx = p.x - c.x, dy = p.y - c.y
        let ca = cos(-a.rotation), sa = sin(-a.rotation)
        return CGPoint(x: c.x + dx * ca - dy * sa, y: c.y + dx * sa + dy * ca)
    }
    // Обратное: локальная точка → экран (с учётом поворота).
    private func screenPoint(_ lp: CGPoint, _ a: Annotation) -> CGPoint {
        let c = textCenter(a)
        let dx = lp.x - c.x, dy = lp.y - c.y
        let ca = cos(a.rotation), sa = sin(a.rotation)
        return CGPoint(x: c.x + dx * ca - dy * sa, y: c.y + dx * sa + dy * ca)
    }
    private func screenRectAround(_ localRect: CGRect, _ a: Annotation) -> CGRect {
        let sc = screenPoint(CGPoint(x: localRect.midX, y: localRect.midY), a)
        return CGRect(x: sc.x - 11, y: sc.y - 11, width: 22, height: 22)
    }

    // Текст рисуем через Core Text — одинаково на экране и при рендере картинки.
    // Поддержка нескольких строк (Shift+Enter).
    private func drawTextAnnotation(_ a: Annotation, in ctx: CGContext) {
        guard !a.text.isEmpty else { return }
        let font = NSFont.systemFont(ofSize: a.fontSize)
        let attrs: [NSAttributedString.Key: Any] = [
            .font: font, .foregroundColor: NSColor(cgColor: a.color) ?? .systemRed]
        let lines = a.text.components(separatedBy: "\n")
        let lineHeight = font.ascender - font.descender + font.leading
        let c = textCenter(a)

        ctx.saveGState()
        ctx.translateBy(x: c.x, y: c.y); ctx.rotate(by: a.rotation); ctx.translateBy(x: -c.x, y: -c.y)

        if let bg = a.bgColor {
            ctx.setFillColor(bg)
            ctx.fill(textLocalRect(a).insetBy(dx: -3, dy: -2))
        }
        let boxW = textSize(a).width
        for (i, str) in lines.enumerated() {
            // Горизонтальный сдвиг строки по выравниванию относительно ширины блока.
            let lineW = (str as NSString).size(withAttributes: [.font: font]).width
            let dx: CGFloat
            switch a.alignment {
            case .center: dx = (boxW - lineW) / 2
            case .right:  dx = boxW - lineW
            default:      dx = 0
            }
            let line = CTLineCreateWithAttributedString(NSAttributedString(string: str, attributes: attrs))
            ctx.saveGState()
            ctx.textMatrix = .identity
            ctx.translateBy(x: a.start.x + dx, y: a.start.y + font.ascender + CGFloat(i) * lineHeight)
            ctx.scaleBy(x: 1, y: -1)
            ctx.textPosition = .zero
            CTLineDraw(line, ctx)
            ctx.restoreGState()
        }
        ctx.restoreGState()
    }

    // Рамка + ручки (ресайз — квадрат снизу-справа, поворот — кружок сверху-справа).
    private func drawActiveTextChrome(in ctx: CGContext) {
        guard tool == .text, editingIndex == nil,
              let idx = activeTextIndex, idx < annotations.count,
              annotations[idx].tool == .text else { return }
        let a = annotations[idx]
        let c = textCenter(a)
        ctx.saveGState()
        ctx.translateBy(x: c.x, y: c.y); ctx.rotate(by: a.rotation); ctx.translateBy(x: -c.x, y: -c.y)

        ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.9).cgColor)
        ctx.setLineWidth(1)
        ctx.setLineDash(phase: 0, lengths: [4, 3])
        ctx.stroke(textLocalRect(a).insetBy(dx: -3, dy: -3))
        ctx.setLineDash(phase: 0, lengths: [])

        let rh = resizeHandleLocal(a)
        ctx.setFillColor(NSColor.systemBlue.cgColor); ctx.fillEllipse(in: rh)
        OverlayView.resizeIcon?.draw(in: rh.insetBy(dx: 3, dy: 3))

        let roth = rotateHandleLocal(a)
        ctx.setFillColor(NSColor.systemBlue.cgColor); ctx.fillEllipse(in: roth)
        OverlayView.rotateIcon?.draw(in: roth.insetBy(dx: 3, dy: 3))
        ctx.restoreGState()
    }

    // Иконки ручек (рисуются только на экране, поэтому можно через NSImage).
    private static let resizeIcon = tintedSymbol("arrow.up.left.and.arrow.down.right", color: .white)
    private static let rotateIcon = tintedSymbol("arrow.triangle.2.circlepath", color: .white)

    private static func tintedSymbol(_ name: String, color: NSColor) -> NSImage? {
        let cfg = NSImage.SymbolConfiguration(pointSize: 14, weight: .bold)
        guard let base = NSImage(systemSymbolName: name, accessibilityDescription: nil)?
                .withSymbolConfiguration(cfg) else { return nil }
        let img = NSImage(size: base.size)
        img.lockFocus()
        color.set()
        let r = NSRect(origin: .zero, size: base.size)
        base.draw(in: r)
        r.fill(using: .sourceAtop)
        img.unlockFocus()
        img.isTemplate = false
        return img
    }

    private func textAnnotationIndex(at p: CGPoint) -> Int? {
        for i in annotations.indices.reversed() where annotations[i].tool == .text {
            let a = annotations[i]
            if textLocalRect(a).insetBy(dx: -3, dy: -3).contains(localPoint(p, a)) { return i }
        }
        return nil
    }

    // MARK: - Панель цветов текста

    private func updateTextInspector() {
        guard tool == .text, editingIndex == nil,
              let idx = activeTextIndex, idx < annotations.count,
              annotations[idx].tool == .text else {
            textInspector?.removeFromSuperview(); textInspector = nil; return
        }
        let panel = textInspector ?? makeTextInspector()
        positionTextInspector(panel, for: annotations[idx])
        panel.setSelected(textColor: annotations[idx].color, bgColor: annotations[idx].bgColor)
    }

    private func makeTextInspector() -> TextInspectorView {
        let panel = TextInspectorView()
        panel.onTextColor = { [weak self] c in
            guard let self, let i = self.activeTextIndex, i < self.annotations.count else { return }
            self.annotations[i].color = c; self.color = c
            self.needsDisplay = true; self.updateTextInspector()
        }
        panel.onBgColor = { [weak self] c in
            guard let self, let i = self.activeTextIndex, i < self.annotations.count else { return }
            self.annotations[i].bgColor = c
            self.needsDisplay = true; self.updateTextInspector()
        }
        addSubview(panel)
        textInspector = panel
        return panel
    }

    private func positionTextInspector(_ panel: TextInspectorView, for a: Annotation) {
        let box = textLocalRect(a)
        let size = panel.frame.size
        var x = box.midX - size.width / 2
        var y = box.minY - size.height - 10            // над текстом
        if y < 0 { y = box.maxY + 10 }                 // нет места сверху → снизу
        x = min(max(0, x), bounds.width - size.width)
        y = min(max(0, y), bounds.height - size.height)
        panel.frame = CGRect(x: x, y: y, width: size.width, height: size.height)
    }

    private func drawArrow(from start: CGPoint, to end: CGPoint,
                           width: CGFloat, in ctx: CGContext) {
        ctx.move(to: start)
        ctx.addLine(to: end)
        ctx.strokePath()

        let angle = atan2(end.y - start.y, end.x - start.x)
        let head = max(12, width * 4)            // длина «крыльев»
        let spread = CGFloat.pi / 7              // угол раскрытия
        let p1 = CGPoint(x: end.x - head * cos(angle - spread),
                         y: end.y - head * sin(angle - spread))
        let p2 = CGPoint(x: end.x - head * cos(angle + spread),
                         y: end.y - head * sin(angle + spread))
        ctx.move(to: end)
        ctx.addLine(to: p1)
        ctx.addLine(to: p2)
        ctx.closePath()
        ctx.fillPath()
    }

    // MARK: - Геометрия

    private func rect(from a: CGPoint, to b: CGPoint) -> CGRect {
        CGRect(x: min(a.x, b.x), y: min(a.y, b.y),
               width: abs(a.x - b.x), height: abs(a.y - b.y))
    }

    private func handleRects(for sel: CGRect) -> [Handle: CGRect] {
        let s = handleSize
        func r(_ cx: CGFloat, _ cy: CGFloat) -> CGRect {
            CGRect(x: cx - s/2, y: cy - s/2, width: s, height: s)
        }
        return [
            .tl: r(sel.minX, sel.minY),  .t: r(sel.midX, sel.minY),  .tr: r(sel.maxX, sel.minY),
            .l:  r(sel.minX, sel.midY),                               .r:  r(sel.maxX, sel.midY),
            .bl: r(sel.minX, sel.maxY),  .b: r(sel.midX, sel.maxY),  .br: r(sel.maxX, sel.maxY),
        ]
    }

    private func handleHit(_ p: CGPoint, _ sel: CGRect) -> Handle? {
        for (h, rect) in handleRects(for: sel) where rect.insetBy(dx: -4, dy: -4).contains(p) {
            return h
        }
        return nil
    }

    private func resized(_ start: CGRect, handle: Handle, to p: CGPoint) -> CGRect {
        var minX = start.minX, minY = start.minY, maxX = start.maxX, maxY = start.maxY
        switch handle {
        case .tl: minX = p.x; minY = p.y
        case .t:  minY = p.y
        case .tr: maxX = p.x; minY = p.y
        case .r:  maxX = p.x
        case .br: maxX = p.x; maxY = p.y
        case .b:  maxY = p.y
        case .bl: minX = p.x; maxY = p.y
        case .l:  minX = p.x
        }
        return rect(from: CGPoint(x: minX, y: minY), to: CGPoint(x: maxX, y: maxY))
    }

    // MARK: - Мышь

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        dragStart = p

        // Клик в другом месте завершает текущий ввод текста.
        if textEditor != nil { commitTextEditing() }

        if let sel = selection {
            if tool == .select {
                if let h = handleHit(p, sel) {
                    dragMode = .resize(h); selectionAtDragStart = sel
                    resizeCursor(for: h).set(); beginCustomCursorDrag()
                    return
                }
                if sel.contains(p) {
                    dragMode = .moveSelection; selectionAtDragStart = sel
                    NSCursor.closedHand.set(); beginCustomCursorDrag()   // схватил область
                    return
                }
                // Клик мимо — начинаем новое выделение.
                selection = nil; hideToolbar(); dragMode = .newSelection
                onBeganSelection?()
            } else if tool == .text {
                handleTextMouseDown(at: p, in: sel)
            } else {
                // Активен инструмент рисования — рисуем только внутри области.
                if sel.contains(p) {
                    currentAnnotation = Annotation(tool: tool, color: color,
                                                   lineWidth: lineWidth,
                                                   start: p, end: p, points: [p])
                    dragMode = .draw
                }
            }
        } else {
            dragMode = .newSelection
            onBeganSelection?()
        }
        needsDisplay = true
    }

    // Сброс состояния (вызывается на других экранах, когда начали выделять на этом).
    func clearSelectionState() {
        textEditor?.removeFromSuperview(); textEditor = nil; editingIndex = nil
        removeTextEditControls()
        textInspector?.removeFromSuperview(); textInspector = nil
        selection = nil
        annotations.removeAll()
        currentAnnotation = nil
        activeTextIndex = nil
        dragMode = .none
        hideToolbar()
        needsDisplay = true
    }

    private func handleTextMouseDown(at p: CGPoint, in sel: CGRect) {
        // Если есть активный текст — сперва проверяем его ручки (в локальных координатах).
        if let idx = activeTextIndex, idx < annotations.count, annotations[idx].tool == .text {
            let a = annotations[idx]
            let lp = localPoint(p, a)
            if rotateHandleLocal(a).insetBy(dx: -4, dy: -4).contains(lp) {
                dragMode = .rotateText
                let c = textCenter(a)
                textRotateStartAngle = a.rotation
                textRotateStartPointerAngle = atan2(p.y - c.y, p.x - c.x)
                NSCursor.closedHand.set(); beginCustomCursorDrag()
                return
            }
            if resizeHandleLocal(a).insetBy(dx: -4, dy: -4).contains(lp) {
                dragMode = .resizeText
                textResizeStartSize = a.fontSize
                textResizeStartPoint = p
                NSCursor.crosshair.set(); beginCustomCursorDrag()
                return
            }
            if textLocalRect(a).insetBy(dx: -3, dy: -3).contains(lp) {
                dragMode = .moveText             // клик по телу → перемещение
                textMoveStartOrigin = a.start
                dragStart = p
                NSCursor.closedHand.set(); beginCustomCursorDrag()
                return
            }
        }
        // Клик по другому тексту → выбираем и готовим перемещение.
        if let idx = textAnnotationIndex(at: p) {
            activeTextIndex = idx
            dragMode = .moveText
            textMoveStartOrigin = annotations[idx].start
            dragStart = p
            NSCursor.closedHand.set(); beginCustomCursorDrag()
            updateTextInspector()
            needsDisplay = true
            return
        }
        // Пустое место → новый текст.
        if sel.contains(p) {
            activeTextIndex = nil
            beginTextEditing(at: p)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        switch dragMode {
        case .newSelection:
            selection = rect(from: dragStart, to: p)
        case .moveSelection:
            NSCursor.closedHand.set()   // держим «сжатую руку», пока тащим
            let dx = p.x - dragStart.x, dy = p.y - dragStart.y
            var moved = selectionAtDragStart.offsetBy(dx: dx, dy: dy)
            moved.origin.x = min(max(0, moved.origin.x), bounds.width - moved.width)
            moved.origin.y = min(max(0, moved.origin.y), bounds.height - moved.height)
            selection = moved
        case .resize(let h):
            resizeCursor(for: h).set()
            selection = resized(selectionAtDragStart, handle: h, to: p)
        case .draw:
            currentAnnotation?.end = p
            currentAnnotation?.points.append(p)
        case .resizeText:
            if let idx = activeTextIndex, idx < annotations.count {
                NSCursor.crosshair.set()
                let dy = p.y - textResizeStartPoint.y   // тянем вниз — крупнее
                annotations[idx].fontSize = max(8, textResizeStartSize + dy)
                updateTextInspector()
            }
        case .moveText:
            if let idx = activeTextIndex, idx < annotations.count {
                NSCursor.closedHand.set()
                let dx = p.x - dragStart.x, dy = p.y - dragStart.y
                var newStart = CGPoint(x: textMoveStartOrigin.x + dx,
                                       y: textMoveStartOrigin.y + dy)
                // Привязка центра текста к центру выделенной области (как в Photoshop).
                snapGuideV = false; snapGuideH = false
                if let sel = selection {
                    let size = textSize(annotations[idx])
                    let threshold: CGFloat = 6
                    if abs(newStart.x + size.width / 2 - sel.midX) < threshold {
                        newStart.x = sel.midX - size.width / 2
                        snapGuideV = true
                    }
                    if abs(newStart.y + size.height / 2 - sel.midY) < threshold {
                        newStart.y = sel.midY - size.height / 2
                        snapGuideH = true
                    }
                }
                annotations[idx].start = newStart
                updateTextInspector()
            }
        case .rotateText:
            if let idx = activeTextIndex, idx < annotations.count {
                let c = textCenter(annotations[idx])
                let ang = atan2(p.y - c.y, p.x - c.x)
                annotations[idx].rotation = textRotateStartAngle + (ang - textRotateStartPointerAngle)
            }
        case .none:
            break
        }
        // Не выпускаем курсор за пределы экрана при работе с областью.
        switch dragMode {
        case .newSelection, .moveSelection, .resize:
            confineCursorToScreen()
        default:
            break
        }
        // Панель «прилипает» к области и едет вместе с ней при изменении размера/перемещении.
        if let bar = toolbar, let sel = selection {
            positionToolbar(bar, near: sel)
        }
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        switch dragMode {
        case .draw:
            if let a = currentAnnotation { annotations.append(a); redoStack.removeAll() }
            currentAnnotation = nil
        case .moveText:
            // Клик без перетаскивания (мышь почти не сдвинулась) → редактировать текст.
            let up = convert(event.locationInWindow, from: nil)
            if hypot(up.x - dragStart.x, up.y - dragStart.y) < 4, let idx = activeTextIndex {
                beginTextEditing(index: idx)
            }
        default:
            break
        }
        dragMode = .none
        snapGuideV = false; snapGuideH = false   // убрать направляющие после отпускания

        if let sel = selection, sel.width > 4, sel.height > 4 {
            showToolbar(near: sel)
        } else {
            selection = nil
            hideToolbar()
        }
        endCustomCursorDrag()
        window?.invalidateCursorRects(for: self)
        // Фокус возвращаем overlay'ю ТОЛЬКО если не идёт ввод текста,
        // иначе поле ввода тут же теряет фокус и исчезает.
        if textEditor == nil { window?.makeFirstResponder(self) }
        // После отпускания мыши вернуть «руку», если курсор над областью.
        if tool == .select, let sel = selection,
           sel.contains(convert(event.locationInWindow, from: nil)) {
            NSCursor.openHand.set()
        }
        updateTextInspector()
        needsDisplay = true
    }

    // MARK: - Клавиатура

    override func keyDown(with event: NSEvent) {
        if handleKey(event) { return }
        super.keyDown(with: event)
    }

    // Комбинации с ⌘ доходят сюда, а НЕ в keyDown.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if handleKey(event) { return true }
        return super.performKeyEquivalent(with: event)
    }

    // Сравниваем по КОДАМ клавиш, а не по символам — иначе ломается на
    // нелатинских раскладках (на русской ⌘C отдаёт символ «с», а не «c»).
    private func handleKey(_ event: NSEvent) -> Bool {
        // Во время ввода текста все клавиши отдаём полю (Esc/Enter/⌘C там свои).
        if textEditor != nil { return false }
        let cmd = event.modifierFlags.contains(.command)
        let shift = event.modifierFlags.contains(.shift)
        switch Int(event.keyCode) {
        case kVK_Escape:
            finish(); return true
        case kVK_Return, kVK_ANSI_KeypadEnter:
            copyToClipboard(); return true
        case kVK_ANSI_C where cmd, kVK_ANSI_X where cmd:
            copyToClipboard(); return true
        case kVK_ANSI_S where cmd:
            saveToFile(); return true
        case kVK_ANSI_Z where cmd:
            if shift { redoLast() } else { undoLast() }   // ⌘Z / ⇧⌘Z
            return true
        default:
            return false
        }
    }

    // MARK: - Панель инструментов

    private func showToolbar(near sel: CGRect) {
        let isNew = (toolbar == nil)
        let bar = toolbar ?? makeToolbar()
        positionToolbar(bar, near: sel)
        if isNew { animateToolbarIn(bar) }
    }

    // Плавное появление: панель чуть выезжает снизу и проявляется.
    private func animateToolbarIn(_ bar: ToolbarView) {
        let finalOrigin = bar.frame.origin
        bar.alphaValue = 0
        bar.setFrameOrigin(CGPoint(x: finalOrigin.x, y: finalOrigin.y + 6))
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.16
            ctx.timingFunction = CAMediaTimingFunction(name: .easeOut)
            bar.animator().alphaValue = 1
            bar.animator().setFrameOrigin(finalOrigin)
        }
    }

    private func positionToolbar(_ bar: ToolbarView, near sel: CGRect) {
        // Размер панели уже задан в buildButtons(); fittingSize тут вернул бы 0,
        // потому что панель свёрстана вручную, без Auto Layout.
        let barSize = bar.frame.size
        let gap: CGFloat = 8
        var origin = CGPoint(x: sel.minX, y: sel.maxY + gap)   // по умолчанию — под областью

        let fitsBelow = origin.y + barSize.height <= bounds.height
        if !fitsBelow {
            let above = sel.minY - barSize.height - gap
            if above >= 0 {
                origin.y = above                                // не влезло снизу — над областью
            } else {
                // Область занимает почти весь экран — кладём панель ВНУТРЬ, у нижнего края.
                origin.y = sel.maxY - barSize.height - gap
            }
        }
        // Финальная страховка: панель всегда в пределах экрана.
        origin.x = min(max(0, origin.x), bounds.width - barSize.width)
        origin.y = min(max(0, origin.y), bounds.height - barSize.height)
        bar.frame = CGRect(origin: origin, size: barSize)
    }

    private func makeToolbar() -> ToolbarView {
        let bar = ToolbarView()
        bar.onSelectTool = { [weak self] t in
            guard let self else { return }
            if self.textEditor != nil { self.commitTextEditing() }
            self.tool = t
            if t != .text { self.activeTextIndex = nil }   // убрать рамку текста
            self.updateTextInspector()
            self.window?.invalidateCursorRects(for: self)
            self.needsDisplay = true
        }
        bar.onSelectColor = { [weak self] c in
            guard let self else { return }
            self.color = c
            // Цвет применяется и к редактируемому, и к выбранному тексту — сразу.
            if let field = self.textEditor { field.textColor = NSColor(cgColor: c) }
            if let i = self.activeTextIndex, self.editingIndex == nil,
               i < self.annotations.count, self.annotations[i].tool == .text {
                self.annotations[i].color = c
                self.needsDisplay = true
            }
        }
        bar.onCopy  = { [weak self] in self?.copyToClipboard() }
        bar.onSave  = { [weak self] in self?.saveToFile() }
        bar.onClose = { [weak self] in self?.finish() }
        bar.onUndo  = { [weak self] in self?.undoLast() }
        bar.onRedo  = { [weak self] in self?.redoLast() }
        addSubview(bar)
        toolbar = bar
        return bar
    }

    private func hideToolbar() {
        toolbar?.removeFromSuperview()
        toolbar = nil
    }

    private func undoLast() {
        guard !annotations.isEmpty else { return }
        redoStack.append(annotations.removeLast())   // запоминаем для «вернуть»
        if activeTextIndex != nil { activeTextIndex = nil }
        needsDisplay = true
    }

    private func redoLast() {
        guard !redoStack.isEmpty else { return }
        annotations.append(redoStack.removeLast())
        needsDisplay = true
    }

    // MARK: - Текст (ввод)

    private func beginTextEditing(at point: CGPoint) {
        editingIndex = nil
        presentEditor(makeTextField(origin: point, size: 18, color: color, text: "",
                                    alignment: currentTextAlignment))
        updateTextInspector()   // спрятать панель на время ввода
    }

    private func beginTextEditing(index: Int) {
        let a = annotations[index]
        editingIndex = index
        currentTextAlignment = a.alignment
        presentEditor(makeTextField(origin: a.start, size: a.fontSize, color: a.color,
                                    text: a.text, alignment: a.alignment))
        updateTextInspector()
        needsDisplay = true   // спрятать нарисованную версию на время правки
    }

    private func makeTextField(origin: CGPoint, size: CGFloat, color: CGColor,
                               text: String, alignment: NSTextAlignment) -> NSTextField {
        let field = NSTextField(frame: NSRect(x: origin.x, y: origin.y, width: 44, height: size + 8))
        field.stringValue = text
        field.font = .systemFont(ofSize: size)
        field.textColor = NSColor(cgColor: color) ?? .systemRed
        field.alignment = alignment
        field.backgroundColor = NSColor.black.withAlphaComponent(0.28)
        field.drawsBackground = true
        field.isBordered = false
        field.focusRingType = .none
        field.usesSingleLineMode = false       // многострочность (Shift+Enter)
        field.cell?.wraps = true
        field.cell?.isScrollable = false
        field.lineBreakMode = .byWordWrapping
        field.delegate = self
        sizeFieldToFit(field)
        return field
    }

    // Поле подстраивается под содержимое (маленькое по умолчанию, растёт по мере ввода).
    private func sizeFieldToFit(_ field: NSTextField) {
        let font = field.font ?? .systemFont(ofSize: 18)
        let s = field.stringValue.isEmpty ? " " : field.stringValue
        let bounding = (s as NSString).boundingRect(
            with: NSSize(width: 600, height: CGFloat.greatestFiniteMagnitude),
            options: [.usesLineFragmentOrigin, .usesFontLeading],
            attributes: [.font: font])
        let w = max(40, ceil(bounding.width) + 14)
        let h = max(font.pointSize + 8, ceil(bounding.height) + 8)
        field.frame.size = NSSize(width: w, height: h)
    }

    private func presentEditor(_ field: NSTextField) {
        textEditor = field
        addSubview(field)
        showEditControls(under: field)
        if Settings.textAlignmentEnabled { showAlignmentControls(over: field) }
        window?.makeFirstResponder(field)
    }

    // Блок выбора выравнивания (слева / по центру / справа) над полем ввода.
    private func showAlignmentControls(over field: NSView) {
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 84, height: 30))
        container.wantsLayer = true
        container.layer?.backgroundColor = NSColor(white: 0.12, alpha: 0.95).cgColor
        container.layer?.cornerRadius = 7

        alignButtons.removeAll()
        let options: [(NSTextAlignment, String)] = [
            (.left, "text.alignleft"), (.center, "text.aligncenter"), (.right, "text.alignright"),
        ]
        var x: CGFloat = 5
        for (align, symbol) in options {
            let b = makeGlyphButton(symbol, tint: .white) { [weak self] in
                self?.setTextAlignment(align)
            }
            b.frame = NSRect(x: x, y: 4, width: 22, height: 22)
            b.wantsLayer = true
            b.layer?.cornerRadius = 5
            container.addSubview(b)
            alignButtons.append((align, b))
            x += 26
        }
        addSubview(container)
        alignControls = container
        updateAlignmentHighlight()
        positionAlignmentControls(over: field)
        window?.invalidateCursorRects(for: self)
    }

    private func updateAlignmentHighlight() {
        for (align, b) in alignButtons {
            b.layer?.backgroundColor = (align == currentTextAlignment)
                ? NSColor.systemBlue.cgColor : NSColor.clear.cgColor
        }
    }

    private func setTextAlignment(_ align: NSTextAlignment) {
        currentTextAlignment = align
        updateAlignmentHighlight()           // подсветка сразу
        guard let field = textEditor else { return }
        field.alignment = align              // для сохранения в аннотацию при коммите
        applyAlignmentToEditor(align)        // применяем к редактору сразу…
        // …и на следующем тике (на первый клик фокус/редактор могут быть не готовы).
        DispatchQueue.main.async { [weak self] in self?.applyAlignmentToEditor(align) }
        needsDisplay = true
    }

    // Применить выравнивание ко всему тексту, ГАРАНТИРУЯ фокус на поле.
    private func applyAlignmentToEditor(_ align: NSTextAlignment) {
        guard let field = textEditor else { return }
        // Кнопка выравнивания могла «снять» фокус с поля — возвращаем его,
        // иначе field editor недоступен и текст не переравнивается.
        if field.currentEditor() == nil {
            window?.makeFirstResponder(field)
        }
        guard let tv = field.currentEditor() as? NSTextView else { return }
        let sel = tv.selectedRange()
        let len = (tv.string as NSString).length
        tv.setAlignment(align, range: NSRange(location: 0, length: len))
        tv.alignment = align
        tv.setSelectedRange(sel)
        tv.needsDisplay = true
    }

    private func positionAlignmentControls(over field: NSView) {
        guard let c = alignControls else { return }
        var x = field.frame.minX
        var y = field.frame.minY - c.frame.height - 6   // НАД полем (flipped: меньше y = выше)
        x = min(max(0, x), bounds.width - c.frame.width)
        y = max(0, y)
        c.setFrameOrigin(CGPoint(x: x, y: y))
    }

    // ✓ (подтвердить) и ✗ (отменить) под полем ввода.
    private func showEditControls(under field: NSView) {
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 58, height: 30))
        container.wantsLayer = true
        container.layer?.backgroundColor = NSColor(white: 0.12, alpha: 0.95).cgColor
        container.layer?.cornerRadius = 7
        let confirm = makeGlyphButton("checkmark", tint: .systemGreen) { [weak self] in
            self?.commitTextEditing()
        }
        let cancel = makeGlyphButton("xmark", tint: .systemRed) { [weak self] in
            self?.cancelTextEditing()
        }
        confirm.frame = NSRect(x: 5, y: 4, width: 22, height: 22)
        cancel.frame = NSRect(x: 31, y: 4, width: 22, height: 22)
        container.addSubview(confirm)
        container.addSubview(cancel)
        addSubview(container)
        editControls = container
        positionEditControls(under: field)
        window?.invalidateCursorRects(for: self)
    }

    private func positionEditControls(under field: NSView) {
        guard let c = editControls else { return }
        var x = field.frame.minX
        var y = field.frame.maxY + 6
        x = min(max(0, x), bounds.width - c.frame.width)
        y = min(max(0, y), bounds.height - c.frame.height)
        c.setFrameOrigin(CGPoint(x: x, y: y))
    }

    private func removeTextEditControls() {
        editControls?.removeFromSuperview()
        editControls = nil
        alignControls?.removeFromSuperview()
        alignControls = nil
        alignButtons.removeAll()
        window?.invalidateCursorRects(for: self)
    }

    private func makeGlyphButton(_ symbol: String, tint: NSColor,
                                 action: @escaping () -> Void) -> NSButton {
        final class B: NSButton { var handler: (() -> Void)?; @objc func fire() { handler?() } }
        let b = B()
        b.handler = action; b.target = b; b.action = #selector(B.fire)
        b.isBordered = false; b.imagePosition = .imageOnly
        b.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
        b.contentTintColor = tint
        b.refusesFirstResponder = true
        return b
    }

    func commitTextEditing() { endTextEditing(commit: true) }
    private func cancelTextEditing() { endTextEditing(commit: false) }

    private func endTextEditing(commit: Bool) {
        guard let field = textEditor else { return }
        textEditor = nil   // первым делом — чтобы избежать повторного входа
        let text = field.stringValue
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let origin = field.frame.origin
        let size = field.font?.pointSize ?? 18
        let col = field.textColor?.cgColor ?? color
        let align = currentTextAlignment   // надёжнее, чем field.alignment во время правки
        let idx = editingIndex
        editingIndex = nil
        field.removeFromSuperview()
        removeTextEditControls()

        if let i = idx, i < annotations.count {
            // Редактировали существующий текст.
            if commit {
                if trimmed.isEmpty {
                    annotations.remove(at: i); activeTextIndex = nil
                } else {
                    annotations[i].text = text          // сохраняем поворот и фон
                    annotations[i].fontSize = size
                    annotations[i].color = col
                    annotations[i].start = origin
                    annotations[i].alignment = align
                    activeTextIndex = i
                }
            } else {
                activeTextIndex = i                      // отмена — оставляем как было
            }
        } else if commit && !trimmed.isEmpty {
            // Новый текст.
            var a = Annotation(tool: .text, color: col, lineWidth: lineWidth,
                               start: origin, end: origin, points: [])
            a.text = text
            a.fontSize = size
            a.alignment = align
            annotations.append(a)
            redoStack.removeAll()
            activeTextIndex = annotations.count - 1
        } else {
            activeTextIndex = nil
        }

        window?.makeFirstResponder(self)
        updateTextInspector()
        needsDisplay = true
    }

    // MARK: - Итоговая картинка

    // Кропаем скриншот по выделению и поверх дорисовываем аннотации —
    // в полном пиксельном разрешении.
    private func renderOutput() -> NSImage? {
        guard let sel = selection, sel.width > 1, sel.height > 1 else { return nil }
        let outScale = outputScale
        let pxW = Int(sel.width * outScale)
        let pxH = Int(sel.height * outScale)
        guard pxW > 0, pxH > 0,
              let cs = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: pxW, height: pxH,
                                  bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }

        // Переводим систему координат в «точки вью, origin сверху-слева».
        ctx.scaleBy(x: outScale, y: outScale)
        ctx.translateBy(x: 0, y: sel.height)
        ctx.scaleBy(x: 1, y: -1)
        ctx.translateBy(x: -sel.minX, y: -sel.minY)

        drawImageUpright(screenshot, in: bounds, ctx: ctx)
        for a in annotations { drawAnnotation(a, in: ctx) }

        guard let cgImage = ctx.makeImage() else { return nil }
        return NSImage(cgImage: cgImage, size: sel.size)
    }

    private func writeToPasteboard(_ image: NSImage) {
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.writeObjects([image])
    }

    private func copyToClipboard() {
        if textEditor != nil { commitTextEditing() }
        guard let image = renderOutput() else { return }
        writeToPasteboard(image)
        finish()
    }

    private func saveToFile() {
        if textEditor != nil { commitTextEditing() }
        guard let image = renderOutput(),
              let tiff = image.tiffRepresentation,
              let rep = NSBitmapImageRep(data: tiff),
              let png = rep.representation(using: .png, properties: [:]) else { return }

        // Если задана папка по умолчанию — сохраняем сразу, без диалога,
        // и дополнительно кладём снимок в буфер обмена.
        if let folder = Settings.saveFolderPath {
            let url = makeSaveURL(inFolder: folder)
            try? png.write(to: url)
            writeToPasteboard(image)
            finish()
            return
        }

        // Экран, где была выделена область — чтобы окно сохранения появилось на нём же.
        let targetScreen = window?.screen ?? NSScreen.main

        // КРИТИЧНО: сначала убираем overlay (он на уровне «щита», перекрывающего
        // весь экран). Иначе панель сохранения зависает невидимой поверх щита.
        finish()

        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = defaultScreenshotName()   // как в macOS: с датой и временем
        NSApp.activate(ignoringOtherApps: true)

        guard let screen = targetScreen else {
            if panel.runModal() == .OK, let url = panel.url {
                try? png.write(to: url); writeToPasteboard(image)
            }
            return
        }

        // Прозрачное хост-окно на нужном экране: панель-лист появится именно на нём.
        let host = NSWindow(contentRect: screen.frame, styleMask: .borderless,
                            backing: .buffered, defer: false)
        host.isOpaque = false
        host.backgroundColor = .clear
        host.hasShadow = false
        host.ignoresMouseEvents = true
        host.level = .normal
        host.makeKeyAndOrderFront(nil)

        panel.beginSheetModal(for: host) { response in
            if response == .OK, let url = panel.url {
                try? png.write(to: url)
                self.writeToPasteboard(image)
            }
            host.orderOut(nil)
        }
    }

    // Имя по умолчанию в стиле macOS: «Screenshot 2026-06-01 at 19.49.10.png».
    private func defaultScreenshotName() -> String {
        let fmt = DateFormatter()
        fmt.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
        return "Screenshot \(fmt.string(from: Date())).png"
    }

    // Уникальный путь в выбранной папке (если файл с таким именем уже есть — добавляем счётчик).
    private func makeSaveURL(inFolder folder: String) -> URL {
        let dir = URL(fileURLWithPath: folder, isDirectory: true)
        let base = (defaultScreenshotName() as NSString).deletingPathExtension
        var url = dir.appendingPathComponent(base + ".png")
        var i = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = dir.appendingPathComponent("\(base) (\(i)).png")
            i += 1
        }
        return url
    }

    private func finish() {
        onFinish?()
    }
}

extension OverlayView: NSTextFieldDelegate {
    // Поле потеряло фокус (клик мимо) — фиксируем текст.
    func controlTextDidEndEditing(_ obj: Notification) {
        commitTextEditing()
    }

    // Поле растёт под содержимое по мере ввода.
    func controlTextDidChange(_ obj: Notification) {
        guard let field = textEditor else { return }
        sizeFieldToFit(field)
        positionEditControls(under: field)
        positionAlignmentControls(over: field)
        needsDisplay = true
    }

    // Перехватываем Enter / Shift+Enter / Esc прямо в поле, чтобы они не уходили
    // в overlay (иначе Esc закрывал всё окно).
    func control(_ control: NSControl, textView: NSTextView,
                 doCommandBy commandSelector: Selector) -> Bool {
        switch commandSelector {
        case #selector(NSResponder.insertNewline(_:)):
            if NSApp.currentEvent?.modifierFlags.contains(.shift) == true {
                textView.insertNewlineIgnoringFieldEditor(self)   // Shift+Enter → перенос строки
            } else {
                commitTextEditing()                                // Enter → подтвердить
            }
            return true
        case #selector(NSResponder.cancelOperation(_:)):
            cancelTextEditing()                                    // Esc → отменить ввод
            return true
        default:
            return false
        }
    }
}
