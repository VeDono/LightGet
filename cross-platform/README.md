# LightGet — Cross-Platform (C++ / Qt6)

A Lightshot-style screenshot + annotation tool: capture the screen, draw arrows /
lines / rectangles / pen strokes / text on a fullscreen overlay, then copy or save
the result. This is the portable **C++ / Qt6 Widgets** port of the original macOS
Swift app. It runs on **Linux, Windows, and macOS** from a single source tree.

See `DESIGN.md` for the architecture (Swift → Qt mapping, coordinate system,
platform abstraction layer).

---

## Prerequisites

You need **Qt 6** (Widgets, Gui, Core), a **C++17 compiler**, and **CMake ≥ 3.16**.
Ninja is recommended but optional.

### Easiest Qt install per OS

| OS | Install command |
|----|-----------------|
| **Linux (Debian/Ubuntu)** | `sudo apt install qt6-base-dev cmake ninja-build build-essential` |
| **Windows** | `pip install aqtinstall` then `aqt install-qt windows desktop 6.8.1 win64_msvc2022_64` — *or* `vcpkg install qt6-base` |
| **macOS** | `brew install qt cmake ninja` |

On Linux, X11 dev headers (`libx11-dev`, usually pulled in by `qt6-base-dev`) enable
the global hotkey + screen grab; without them those features no-op (see Known gaps).

---

## Build & run

From this directory (`cross-platform/`):

```sh
# 1. Configure (point CMAKE_PREFIX_PATH at your Qt 6 if it isn't on the default path)
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="/path/to/qt6"

# 2. Build
cmake --build build
```

Run the binary:

```sh
./build/LightGet          # Linux / macOS
build\LightGet.exe        # Windows
```

Notes:
- `-G Ninja` is optional. Omit it to use the platform default generator
  (Makefiles on Unix, Visual Studio on Windows — then build with
  `cmake --build build --config Release`).
- **`CMAKE_PREFIX_PATH`** must point at the Qt 6 prefix (the dir containing
  `lib/cmake/Qt6/`). Examples:
  - Homebrew macOS: `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"`
  - aqtinstall: `-DCMAKE_PREFIX_PATH="C:/Qt/6.8.1/msvc2022_64"`
  - apt: usually unnecessary (CMake finds the system Qt 6 automatically).

The toolbar/tray/settings icons are bundled into the binary from
`resources/assets.qrc` via CMake **AUTORCC** (resolved at runtime as
`:/assets/<name>.png`) — no install step or external asset files are needed.

---

## What currently works

- **Tray app** with capture / settings / quit menu, configurable bar icon.
- **Global hotkey** to trigger capture (X11 `XGrabKey`, Windows `RegisterHotKey`,
  macOS Carbon `RegisterEventHotKey`).
- **Screen capture** via `QScreen::grabWindow` baseline on every platform; native
  paths exist for Windows/macOS (see gaps).
- **Fullscreen annotation overlay**: select region; Arrow / Line / Rectangle /
  Filled Rect / Pen / Text tools; color + text styling; undo/redo; copy to
  clipboard; save to PNG.
- **Settings window** (native Qt-styled): hotkey, save folder, language
  (EN/RU/UK), launch-at-login, bar icon, tool toggles.

---

## Known gaps / platform caveats

- **Wayland (Linux):** there is no portable global-hotkey protocol and no direct
  screen grab. Both depend on `xdg-desktop-portal` (the **GlobalShortcuts** and
  **ScreenCast/Screenshot** portals), which require interactive user permission
  and aren't wired in by default. On Wayland the hotkey and capture may be inert —
  an **X11 session** works out of the box. (X11 is gated by `HAVE_X11`, defined
  automatically when the X11 dev libs are found at configure time.)

- **macOS pure-Qt capture:** the default build is **pure Qt** — capture goes
  through `QScreen` and the overlay is a normal topmost `Qt::Tool` window. macOS
  still requires the user to grant **Screen Recording** permission
  (System Settings → Privacy & Security → Screen Recording) for `QScreen` capture
  to return real pixels; otherwise frames come back blank. The faithful native
  path (ScreenCaptureKit, `CGShieldingWindowLevel`, cursor/focus save-restore) is
  **opt-in** behind `HAVE_MAC_NATIVE` (see below).

- **Settings UI** is **native-Qt styled**, not a pixel match of the original macOS
  Swift settings window. Functionally complete; visually plain.

---

## Enabling the faithful native macOS path (`HAVE_MAC_NATIVE`)

By default `HAVE_MAC_NATIVE` is **undefined**, so the macOS-specific hooks
(`TrayApp_forceCursorVisible`, `TrayApp_recordFrontmostApp`, `TrayApp_restoreApp`,
`OverlayWindow_applyShieldLevel`) compile as no-ops and the build links with **no
Objective-C++** translation units. To enable the native ScreenCaptureKit capture +
shield-level overlay + cursor/focus handling:

1. **Add the `.mm` files** (Objective-C++; CMake compiles `.mm` as OBJCXX
   automatically). In `CMakeLists.txt`, inside the `elseif(APPLE)` branch, either
   add them to `LIGHTGET_SOURCES` before `add_executable`, or attach to the target:
   ```cmake
   target_sources(LightGet PRIVATE
       src/mac/TrayApp_mac.mm
       src/mac/OverlayWindow_mac.mm)
   ```
2. **Define the flag** so the native branches are compiled in:
   ```cmake
   target_compile_definitions(LightGet PRIVATE HAVE_MAC_NATIVE=1)
   ```
3. *(Optional)* For an accessory app with **no Dock icon** (`LSUIElement`), supply
   `resources/Info.plist` and uncomment the `MACOSX_BUNDLE` /
   `MACOSX_BUNDLE_INFO_PLIST` lines in `CMakeLists.txt`.

The required frameworks (Carbon, Cocoa, CoreGraphics, ScreenCaptureKit) are already
linked in the `elseif(APPLE)` branch.

---

## Regenerating the bundled icons (maintainers)

The white-on-transparent glyphs in `resources/assets/*.png` are produced by a
**throwaway** generator, `resources/gen_icons.cpp`, which is **not** part of the
LightGet target. To rebuild them (e.g. after adding a new icon name), compile and
run it against your Qt install — for example on macOS:

```sh
QT="/path/to/qt6"   # dir containing lib/QtCore.framework etc.
clang++ -std=c++17 -F"$QT/lib" \
    -I"$QT/lib/QtCore.framework/Headers" \
    -I"$QT/lib/QtGui.framework/Headers" \
    resources/gen_icons.cpp -o /tmp/gen_icons \
    -framework QtCore -framework QtGui
QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH="$QT/plugins" \
    DYLD_FRAMEWORK_PATH="$QT/lib" /tmp/gen_icons resources/assets
```

On Linux/Windows, link against `Qt6::Core` / `Qt6::Gui` the usual way (e.g. a tiny
standalone CMake target) and run with `QT_QPA_PLATFORM=offscreen`. After
regenerating, update `resources/assets.qrc` if any icon names changed.
