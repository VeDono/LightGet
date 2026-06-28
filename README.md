<p align="center">
  <img src="docs/logo.png" width="128" alt="LightGet logo">
</p>

<h1 align="center">LightGet</h1>

<p align="center">
  A fast, cross-platform screenshot &amp; annotation tool — Lightshot-style.<br>
  Press a hotkey, the screen dims, select an area and annotate it right there
  (arrows, shapes, pen, text), then copy or save.
</p>

<p align="center">
  <b>Linux · Windows · macOS</b> — built with C++ / Qt 6, no Electron.
</p>

---

## Editions

LightGet comes in two editions:

| Edition | Platforms | Status |
| --- | --- | --- |
| **LightGet** (this project, C++ / Qt 6) | Linux · Windows · macOS | ✅ **Active** — the current, recommended version |
| **LightGet SWFT** (native macOS, Swift + AppKit) | macOS only | ⚠️ **Deprecated** — the original Swift app, kept for reference; no longer developed |

> **LightGet SWFT is deprecated.** It was the original native-macOS build (Swift + AppKit +
> ScreenCaptureKit). All new work happens in the cross-platform Qt edition above. The two
> install side by side (`LightGet.app` and `LightGet SWFT.app`) and keep separate settings.

---

## Features

- **Global hotkey** (default <kbd>⇧⌘2</kbd> / <kbd>Ctrl+Shift+2</kbd>) — works from anywhere, even over fullscreen apps.
- **Multi-monitor** — every display dims and is interactive; select on any screen.
- **Live annotation** right on the screenshot:
  - Arrow, line, rectangle (outline), filled rectangle (great for censoring), freehand pen.
  - **Text** with a contextual panel — font, size, **B / I / U**, alignment, text colour,
    background/marker colour, plus corner handles to **resize** and **rotate**.
- **Colour palette** with 6 colours; toggle any tool, the palette, or text options on/off in Settings.
- **Copy** to clipboard (<kbd>⌘C</kbd> / <kbd>⌘X</kbd> / <kbd>Enter</kbd>) or **save** as PNG
  (silent folder or a save dialog), with non-overwriting `Screenshot 2026-06-01 at 19.49.10.png` names.
- **Settings** — hotkey, language (EN / RU / UK), light / dark / auto appearance, dim level
  + optional animated dimming, default save folder, menu-bar icon (presets or a custom image),
  Retina downscaling, launch at login.
- **Game-friendly** — forces the cursor visible when a fullscreen game hid it, and restores focus afterwards.
- Lives in the menu bar / tray, no Dock or taskbar window.

---

## Download &amp; install

Grab the latest build for your OS from the [**Releases**](https://github.com/VeDono/LightGet/releases) page:

| OS | Artifact | Install |
| --- | --- | --- |
| **Windows** | `LightGet-Setup-Windows-x64.exe` | Run the installer (upgrades in place). |
| **Linux** | `LightGet-x86_64.AppImage` | `chmod +x` it and run. |
| **macOS** | `LightGet-macOS.zip` | Unzip and move `LightGet.app` to `/Applications`. |

On first capture, grant **Screen Recording** permission to LightGet
(macOS: System Settings → Privacy &amp; Security → Screen Recording → enable LightGet, then relaunch).

---

## Build from source

### LightGet (Qt 6, cross-platform) — recommended

Needs **Qt 6** (Widgets, Gui, Core), a **C++17** compiler, and **CMake ≥ 3.16**.

```sh
cd cross-platform
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="/path/to/qt6"
cmake --build build
```

See [`cross-platform/README.md`](cross-platform/README.md) for per-OS Qt install commands and
[`cross-platform/DESIGN.md`](cross-platform/DESIGN.md) for the architecture.

### LightGet SWFT (native macOS, deprecated)

Open `SnapEdit.xcodeproj` in Xcode 16+ and press <kbd>⌘R</kbd> (macOS 14 Sonoma or later).
This edition is no longer actively developed.

---

## Usage

- <kbd>⇧⌘2</kbd> — dim the screen and start a selection (or click the menu-bar / tray icon).
- Drag to select; drag the handles to resize, drag inside to move.
- Toolbar: cursor / arrow / line / rectangle / filled rectangle / pen / text, colour palette,
  undo / redo, copy, save, close.
- <kbd>⌘C</kbd> / <kbd>⌘X</kbd> / <kbd>Enter</kbd> — copy. <kbd>⌘S</kbd> — save. <kbd>Esc</kbd> — cancel.
- Text tool: click to place and type; a panel appears above the block for font / size / style /
  alignment / colour / background; corner handles resize and rotate; <kbd>Enter</kbd> confirms,
  <kbd>Shift+Enter</kbd> adds a line.

---

## Demo
Video usage:
https://github.com/user-attachments/assets/ac8c00cf-3264-4689-9c85-90b17594d783

General setting page:
<img width="1919" height="1072" alt="image" src="https://github.com/user-attachments/assets/4535c5ba-e493-433e-9425-ae1f9db05920" />

Features setting page:
<img width="1919" height="1072" alt="image" src="https://github.com/user-attachments/assets/43c8a554-0b8e-41da-8b40-dbbb437d7a10" />

Additional example:
<img width="1534" alt="LightGet in action" src="https://github.com/user-attachments/assets/e780e8e8-1a9f-44c9-b2b3-a010b76c5528" />

---

## License

Licensed under the **GNU General Public License v3.0** (GPL-3.0) — see [LICENSE](LICENSE).

In short: anyone is free to use, study, modify, and share this software, and may even sell it or
offer paid support. The one condition is copyleft — if you distribute a modified version you must
release its source under the same GPL-3.0 license; it cannot become a closed, proprietary product.

Copyright © 2026 Sergey Emelyanov.
