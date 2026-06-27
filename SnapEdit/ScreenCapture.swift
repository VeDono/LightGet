import AppKit
import ScreenCaptureKit

enum ScreenCaptureError: Error { case noDisplay }

// Результат захвата: пиксельная картинка экрана + сам NSScreen,
// чтобы overlay знал, где и в каком масштабе её рисовать.
struct CapturedScreen {
    let image: CGImage
    let screen: NSScreen
}

// Захват экрана через современный ScreenCaptureKit (macOS 14+).
// Снимок делается ДО показа overlay, поэтому затемнение и наши
// нарисованные стрелки в итоговую картинку не попадают.
enum ScreenCapture {

    // Снимок КАЖДОГО подключённого экрана — для интерактивного overlay на всех мониторах.
    static func captureAllDisplays() async throws -> [CapturedScreen] {
        let content = try await SCShareableContent.excludingDesktopWindows(
            false, onScreenWindowsOnly: false)
        var result: [CapturedScreen] = []
        for screen in NSScreen.screens {
            guard let id = screen.displayID,
                  let display = content.displays.first(where: { $0.displayID == id }) else { continue }
            let filter = SCContentFilter(display: display, excludingWindows: [])
            let config = SCStreamConfiguration()
            let scale = screen.backingScaleFactor
            config.width = Int(CGFloat(display.width) * scale)
            config.height = Int(CGFloat(display.height) * scale)
            config.showsCursor = false
            config.captureResolution = .best
            let cgImage = try await SCScreenshotManager.captureImage(
                contentFilter: filter, configuration: config)
            result.append(CapturedScreen(image: cgImage, screen: screen))
        }
        if result.isEmpty { throw ScreenCaptureError.noDisplay }
        return result
    }

    static func captureDisplayUnderCursor() async throws -> CapturedScreen {
        // Определяем экран, над которым сейчас курсор (для мультимонитора).
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { NSMouseInRect(mouse, $0.frame, false) }
            ?? NSScreen.main
        guard let screen,
              let displayID = screen.displayID else {
            throw ScreenCaptureError.noDisplay
        }

        let content = try await SCShareableContent.excludingDesktopWindows(
            false, onScreenWindowsOnly: false)
        guard let display = content.displays.first(where: { $0.displayID == displayID })
                ?? content.displays.first else {
            throw ScreenCaptureError.noDisplay
        }

        let filter = SCContentFilter(display: display, excludingWindows: [])

        let config = SCStreamConfiguration()
        // Снимаем в полном пиксельном разрешении (Retina): точки × масштаб.
        let scale = screen.backingScaleFactor
        config.width  = Int(CGFloat(display.width)  * scale)
        config.height = Int(CGFloat(display.height) * scale)
        config.showsCursor = false
        config.captureResolution = .best

        let cgImage = try await SCScreenshotManager.captureImage(
            contentFilter: filter, configuration: config)

        return CapturedScreen(image: cgImage, screen: screen)
    }
}

extension NSScreen {
    // У NSScreen нет прямого свойства displayID — достаём его из описания.
    var displayID: CGDirectDisplayID? {
        let key = NSDeviceDescriptionKey("NSScreenNumber")
        return deviceDescription[key] as? CGDirectDisplayID
    }
}
