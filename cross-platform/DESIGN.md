# LightGet — Cross-Platform C++/Qt6 Port: Architecture

A faithful, native, cross-platform (Windows / Linux / macOS) port of the macOS
Swift app **LightGet** — a Lightshot-style screenshot + annotation tool. This
document is the architectural contract; the header files under `src/` are the
API contract that implementation agents code against without changing interfaces.

---

## 1. Module Map (Swift file → Qt/C++ class)

| Swift source | Qt/C++ class(es) | One-line responsibility |
|---|---|---|
| `Annotation.swift` | `Tool` enum, `Annotation` struct, `TextAlign` enum (`Annotation.h`) | Value-type model of one drawn shape + the 7-tool enum; top-left coords. |
| `ScreenCapture.swift` | `namespace ScreenCapture`, `CapturedScreen`, `ScreenCaptureError` (`ScreenCapture.h`) | Per-display pixel capture before the overlay; permission gate. |
| `HotKey.swift` | `GlobalHotkey` (`GlobalHotkey.h`) | Platform-abstracted system-wide hotkey that fires while backgrounded. |
| `Settings.swift` | `Settings` singleton (`Settings.h`) | QSettings-backed persistence + feature/per-tool toggles. |
| `Localization.swift` | `namespace Loc` (`Localization.h`) | Runtime EN/RU/UK in-memory table with EN/key fallback, instant switch. |
| `OverlayView.swift` (+ `OverlayWindow` from `OverlayController.swift`) | `OverlayWindow` (`OverlayWindow.h`) | **The heart:** per-screen interactive dim/select/annotate/text/undo/copy/save window. |
| `ToolbarView.swift` | `ToolbarView`, `TextInspectorView`, `namespace Palette` (`Toolbar.h`) | Floating toolbar (tools/colors/actions) + text color/background inspector. |
| `SettingsWindowController.swift` | `SettingsWindow`, `HotkeyRecorder` (`SettingsWindow.h`) | Tabbed settings window (General/Features) + key-combo recorder. |
| `main.swift` + `AppDelegate.swift` + `OverlayController.swift` | `TrayApp` (`TrayApp.h`) + `main.cpp` | Lifecycle, tray menu, capture trigger, multi-monitor overlay coordination. |

---

## 2. Chosen Qt6 Mechanisms

- **App object:** `QApplication` (Widgets). `main.cpp` installs the language
  preference *before* constructing `QApplication` (mirrors the macOS
  `AppleLanguages`-before-AppKit ordering for native-dialog localization), then
  creates a long-lived `TrayApp` and calls `app.exec()`.
- **Menu-bar / tray:** `QSystemTrayIcon` + `QMenu`. No main window is ever shown
  (the app is "accessory"). Capture / Settings / Quit actions; the Capture action
  shows the hotkey as plain text and has **no** accelerator (matches the source).
- **Per-screen overlay:** one `OverlayWindow` (`QWidget`) per `QScreen`, flags
  `Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool`,
  `Qt::WA_TranslucentBackground`, geometry set to `screen->geometry()`. All
  painting (screenshot, dim, annotations, chrome, guides) is `QPainter` in
  `paintEvent`. On macOS, `applyShieldLevel()` reaches the underlying `NSWindow`
  to set `CGShieldingWindowLevel()` so the overlay covers the menu bar / Dock.
- **Capture:** baseline `QScreen::grabWindow(0)` per screen → `QImage`;
  per-OS faithful backends below.
- **Clipboard:** `QClipboard::setImage(QImage)`.
- **Persistence:** a single `QSettings` (native format) wrapped in `Settings`.
- **PNG save:** `QImage::save(path, "PNG")`; folder picker `QFileDialog`.
- **Pop / slide animations:** `QVariantAnimation`/`QPropertyAnimation` driving a
  scale `QTransform` about widget center (toolbar pop) and opacity+offset
  (toolbar slide-in), matching the 0.16–0.18s easeOut curves.
- **Decoupling:** Qt **signals/slots** replace the Swift closures
  (`onSelectTool` → `selectToolRequested`, `onHotKeyChanged` → `hotKeyChanged`,
  `onUndo` → `undoRequested`, `onFinish` → `finished`, `onBeganSelection` →
  `beganSelection`, etc.).

---

## 3. Platform Abstraction Layer

Two pieces are genuinely OS-specific and are isolated behind clean interfaces so
the rest of the code stays portable. Each `.cpp` dispatches internally with
`#ifdef`/pimpl; **no `#ifdef` leaks into call sites.**

### 3.1 `GlobalHotkey` (`GlobalHotkey.h`)
`registerHotkey(carbonKeyCode, carbonModifiers)` → emits `activated()`.

| OS | Mechanism | Notes |
|---|---|---|
| Windows | `RegisterHotKey(hwnd, id, MOD_*, vk)` + `WM_HOTKEY` via `QAbstractNativeEventFilter` | Translate Carbon code/mods → VK_/MOD_*. |
| Linux/X11 | `XGrabKey` on the root window + filter `XCB_KEY_PRESS` | Register every NumLock/CapsLock modifier-mask variant. |
| macOS | Carbon `RegisterEventHotKey` + `kEventHotKeyPressed` | Pass-through of the persisted Carbon codes; no Accessibility permission needed. |
| Wayland | `org.freedesktop.portal.GlobalShortcuts` where available | Often needs user setup; **document as a limitation** if absent. |

Internal representation = the persisted **Carbon** code+mask; each backend
translates at the edge. Exactly **one** process-wide event handler is installed
(the macOS source's per-instance-handler duplicate-fire bug is intentionally
*not* reproduced). Registration failure is surfaced (return value) rather than
silently swallowed.

### 3.2 `ScreenCapture` (`ScreenCapture.h`)
`captureAllDisplays()` / `captureDisplayUnderCursor()` → `CapturedScreen{image, screen}`.

| OS | Mechanism | Notes |
|---|---|---|
| Baseline (all) | `QScreen::grabWindow(0)` per screen | Simple; **includes cursor**; may be empty on Wayland or without macOS perm. |
| macOS (faithful) | ScreenCaptureKit (`SCShareableContent` + `SCScreenshotManager`) in `.mm` | Correct retina scale, cursor excluded; `CGPreflight/RequestScreenCaptureAccess` gate. |
| Windows | DXGI Desktop Duplication (or `BitBlt`/`PrintWindow` per `EnumDisplayMonitors`) | No per-app permission gate. |
| X11 | `XGetImage` / `QScreen::grabWindow` | No per-app gate. |
| Wayland | `org.freedesktop.portal.ScreenCast`/Screenshot portal | Interactive permission; no direct root grab. |

**Hard invariant:** capture **before** showing any overlay; export = original
pixels + re-rendered annotations, never a re-grab with the overlay visible.
Capture at native pixels = `points × devicePixelRatio`; set
`QImage::setDevicePixelRatio` so it paints crisp at logical size and exports at
full resolution. On denial/empty → `ScreenCaptureError::NoDisplay`.

---

## 4. Coordinate-System Note (CRITICAL)

The macOS app mixes two conventions; **Qt is uniformly top-left origin, +Y down.**

- **Annotation model + overlay view:** the AppKit view was `isFlipped == true`
  (top-left, +Y down) specifically to match Qt. → **No conversion needed.**
  Store and compute everything in widget-local logical points.
- **Two deliberate Swift "un-flip" blocks** (`drawImageUpright`, per-text-line
  `CTLine`) exist only because raw `CGImage`/Core Text draw bottom-left. In Qt,
  `QPainter::drawImage` and `QPainter::drawText` are already upright — **do not
  port the flips.**
- **macOS global screen space** (`NSEvent.mouseLocation`, `NSMouseInRect`,
  `NSScreen.frame`) is **bottom-left, +Y up**, with negative coords for
  secondary displays. Qt's `QCursor::pos()`, `QGuiApplication::screenAt()`, and
  `QScreen::geometry()` are **top-left, +Y down** virtual-desktop coords. → Place
  each overlay with `setGeometry(screen->geometry())` directly; the
  "screen-under-cursor" / "mouse-in-rect" logic uses `QCursor::pos()` with
  `geometry().contains()` and needs **no manual inversion**.
- **Settings window** (`SettingsWindowController`) used bottom-left hand-computed
  y (e.g. `size.height - …`, About card at absolute bottom-origin y). In Qt use
  layouts (`QVBoxLayout`/`QFormLayout`) — the spec's y math is reference, not a
  requirement; `qt_y = parentHeight − swift_y − elementHeight` is the conversion
  formula if any element is placed absolutely.
- **Rotation** in `Annotation` is **radians**; `QPainter::rotate`/`QTransform`
  take **degrees** → multiply by `180/π` when applying.

---

## 5. Build & Run

Requirements: CMake ≥ 3.16, a C++17 compiler, **Qt 6** (Widgets, Gui, Core).

```sh
cd /Users/user/Documents/temp/Lightshot_like_app/SnapEdit/cross-platform
cmake -S . -B build -DCMAKE_PREFIX_PATH="<path-to-Qt6>"   # e.g. ~/Qt/6.7.0/macos
cmake --build build --parallel
./build/LightGet            # Linux/macOS
# build\LightGet.exe        # Windows
```

- **macOS:** to suppress the Dock icon (accessory app), build as a bundle with
  `LSUIElement=true` in `Info.plist` (see the commented `MACOSX_BUNDLE` block in
  `CMakeLists.txt`). Native pieces (ScreenCaptureKit, shield level, cursor/focus)
  go in `.mm` Objective-C++ files added to the source list. Screen Recording
  permission is requested on first capture.
- **Windows:** links `user32`/`gdi32`; optional `dxgi`/`d3d11` for Desktop
  Duplication. No taskbar button because no top-level window is shown.
- **Linux/X11:** links `X11` (`HAVE_X11`). Wayland sessions fall back to portals.
- Bundle SF-Symbol replacement icons under `resources/assets.qrc` (currently
  commented out until the asset file exists).

---

## 6. Known Fidelity Gaps vs Native macOS

1. **Settings UI** renders with native-Qt widgets (`QTabWidget`, `QComboBox`,
   `QSlider`, `QCheckBox`) — visually close but not pixel-identical to AppKit;
   the bottom-left hand-tuned geometry is replaced by Qt layouts.
2. **SF Symbols** are macOS-only; tool/action/handle glyphs ship as bundled
   SVG/PNG assets and may differ subtly from the system symbols.
3. **Wayland:** no standard global-hotkey protocol and no direct screen grab —
   both depend on `xdg-desktop-portal` (GlobalShortcuts / ScreenCast) and may
   require per-session user approval or be unavailable on some compositors.
   Always-on-top "shield" coverage over panels is also compositor-dependent.
4. **Shield window level / cover-everything:** only macOS truly overlays the
   menu bar/Dock (`CGShieldingWindowLevel`). Windows/X11 use topmost + full
   virtual-screen sizing, which is very close but not guaranteed above every
   system surface.
5. **Force-cursor-visible** (defeating a game that hid the cursor) is
   best-effort off macOS — you cannot force another app's hidden cursor visible
   from outside on Windows/Linux as reliably as the CoreGraphics calls.
6. **Native save-dialog localization** follows the OS locale unless the Qt
   non-native dialog is forced; instant language switch applies to in-app UI.
7. **Launch-at-login** uses registry Run key (Windows) / `~/.config/autostart`
   (Linux) / `SMAppService` shim (macOS); error semantics are approximated.
