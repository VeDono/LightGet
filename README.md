<p align="center">
  <img src="docs/logo.png" width="128" alt="LightGet logo">
</p>

<h1 align="center">LightGet</h1>

<p align="center">A fast, Lightshot-style screenshot &amp; annotation tool.</p>

Press a hotkey, the screen dims, you select an area and annotate it right there
(arrows, shapes, text), then copy to the clipboard or save to a file.

---

## Two editions

LightGet comes in **two editions** that share the same workflow but are built on
different stacks. Pick whichever matches your platform:

| Edition | Platforms | Stack | Where |
| --- | --- | --- | --- |
| **Native macOS** | macOS only | Swift + AppKit + ScreenCaptureKit (no Electron) | this branch (`main`) — `SnapEdit.xcodeproj` + `SnapEdit/` |
| **Cross-platform** | Windows / Linux / macOS | C++ + Qt 6 Widgets | branch [`cpp-qt-port`](../../tree/cpp-qt-port) — `cross-platform/` |

### Which edition should I use?

- **On a Mac?** Use the **Native macOS** edition — it's the macOS-optimized option:
  ScreenCaptureKit capture, a `CGShieldingWindowLevel` overlay that can cover the
  menu bar, cursor/focus save-restore for fullscreen games, and a settings window
  styled to match the system.
- **On Windows or Linux** (or you just want one codebase everywhere) — use the
  **Cross-platform (Qt 6)** edition. It also runs on macOS via pure Qt, which is a
  fine fallback if you'd rather not build the native app.

Both show their edition + version in the settings footer
(`LightGet <version> · Native (macOS)` vs `LightGet <version> · Cross-platform (Qt 6)`),
so you can always tell which one you're running.

---

## Features (both editions)

- **Global hotkey** (default ⇧⌘2 on macOS) — trigger capture from anywhere.
- **Live annotation** directly on the screenshot:
  - Arrow, line, rectangle (outline), filled rectangle (great for censoring), freehand pen
  - Text with color, background color, resize, and rotation
- **Copy** to clipboard (⌘C / ⌘X / Enter) or **save** as PNG.
- **Settings** — change the hotkey, language (EN/RU/UK), dim level, default save
  folder, bar/tray icon, launch at login.
- Lives in the menu bar / system tray, no Dock icon.

macOS-native extras (Native edition): multi-monitor interactive dimming,
macOS-style filenames (`Screenshot 2026-06-01 at 19.49.10.png`, never overwrites),
Retina downscaling, and forcing the cursor visible when a fullscreen game has
hidden it.

---

## Native macOS edition

Swift + AppKit build, optimized for macOS.

**Requirements:** macOS 14 (Sonoma)+, Xcode 16+ to build.

**Build & run:**

1. Open `SnapEdit.xcodeproj` in Xcode.
2. Press ⌘R.
3. On first capture, grant **Screen Recording** permission:
   System Settings → Privacy &amp; Security → Screen Recording → enable LightGet,
   then relaunch.

**Usage:**

- **⇧⌘2** — dim the screen and start a selection (or use the menu-bar item).
- Drag to select an area; drag the handles to resize, drag inside to move.
- Toolbar: cursor / arrow / rectangle / filled rectangle / pen / text, color
  picker, undo, copy, save, close.
- **⌘C**, **⌘X**, or **Enter** — copy. **⌘S** — save. **Esc** — cancel.
- Text tool: click to place, type, **Enter** to confirm, **Shift+Enter** for a new
  line, drag to move, corner handles to resize and rotate, inline panel for
  text/background color.

### Project structure (Native)

| File | Responsibility |
| --- | --- |
| `SnapEdit/main.swift` | Entry point |
| `SnapEdit/AppDelegate.swift` | Menu bar, global hotkey, capture trigger |
| `SnapEdit/HotKey.swift` | Global hotkey registration (Carbon) |
| `SnapEdit/ScreenCapture.swift` | Screen capture via ScreenCaptureKit |
| `SnapEdit/OverlayController.swift` | Transparent overlay windows on every screen |
| `SnapEdit/OverlayView.swift` | Dimming, selection, handles, drawing, rendering |
| `SnapEdit/ToolbarView.swift` | Floating toolbar and text color panel |
| `SnapEdit/Annotation.swift` | Annotation model |
| `SnapEdit/Settings.swift` | UserDefaults-backed settings |
| `SnapEdit/SettingsWindowController.swift` | Settings window |
| `SnapEdit/Localization.swift` | Runtime EN/UA/RU localization |

---

## Cross-platform edition (Windows / Linux / macOS)

C++ / Qt 6 Widgets port, on branch **`cpp-qt-port`** under **`cross-platform/`**.

**Requirements:** Qt 6 (Widgets, Gui, Core), a C++17 compiler, CMake ≥ 3.16
(Ninja recommended).

**Build (quick start):**

```sh
git checkout cpp-qt-port
cd cross-platform
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/path/to/qt6"
cmake --build build
./build/LightGet          # Linux / macOS
build\LightGet.exe        # Windows
```

For per-OS Qt install commands, the optional faithful-native macOS path
(`HAVE_MAC_NATIVE`), Wayland caveats, and full details, see
[`cross-platform/README.md`](cross-platform/README.md) and `cross-platform/DESIGN.md`.

> Pre-built binaries for all platforms are produced by CI on tagged releases
> (see [`.github/workflows/release.yml`](.github/workflows/release.yml)).

---

## Video demo

https://github.com/user-attachments/assets/ac8c00cf-3264-4689-9c85-90b17594d783

## Screenshots

1:
<img width="1534" height="853" alt="exampleOfWorkOfLightGetApp" src="https://github.com/user-attachments/assets/e780e8e8-1a9f-44c9-b2b3-a010b76c5528" />
2:
<img width="3071" height="1711" alt="image" src="https://github.com/user-attachments/assets/a1800d26-0e1d-4996-b093-60bbd35444f6" />
3:
<img width="3069" height="1717" alt="image" src="https://github.com/user-attachments/assets/2e3ad08a-2e7b-45da-9745-f1c22e0434e0" />

<!--
![Selection](docs/selection.png)
![Annotation](docs/annotation.png)
![Settings](docs/settings.png)
-->

---

## License

Licensed under the **GNU General Public License v3.0** (GPL-3.0) — see [LICENSE](LICENSE).

In short: anyone is free to use, study, modify, and share this software,
and may even sell it or offer paid support. The one condition is copyleft —
if you distribute a modified version, you must release its source code under
the same GPL-3.0 license. It cannot be turned into a closed, proprietary product.

Copyright (c) 2026 Sergey Emelyanov.
