cmake_minimum_required(VERSION 3.16)

project(LightGet LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard / Qt setup
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)   # required for Q_OBJECT classes
set(CMAKE_AUTORCC ON)   # for the icon .qrc (assets)
set(CMAKE_AUTOUIC OFF)  # no .ui files; UI is built in code

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Core)

# ---------------------------------------------------------------------------
# Sources (every file we intend to exist; some .cpp not yet written)
# ---------------------------------------------------------------------------
set(LIGHTGET_HEADERS
    src/Annotation.h
    src/Settings.h
    src/Localization.h
    src/GlobalHotkey.h
    src/ScreenCapture.h
    src/OverlayWindow.h
    src/Toolbar.h
    src/SettingsWindow.h
    src/TrayApp.h
)

set(LIGHTGET_SOURCES
    src/main.cpp
    src/Settings.cpp
    src/Localization.cpp
    src/GlobalHotkey.cpp        # platform-dispatched via #ifdef inside the file
    src/ScreenCapture.cpp       # platform-dispatched via #ifdef inside the file
    src/OverlayWindow.cpp
    src/Toolbar.cpp
    src/SettingsWindow.cpp
    src/TrayApp.cpp
)

# Optional bundled icon/glyph assets (SF Symbol replacements). Create as needed.
set(LIGHTGET_RESOURCES
    resources/assets.qrc
)

add_executable(LightGet
    ${LIGHTGET_HEADERS}
    ${LIGHTGET_SOURCES}
    # ${LIGHTGET_RESOURCES}   # uncomment once resources/assets.qrc exists
)

target_include_directories(LightGet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(LightGet PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)

# ---------------------------------------------------------------------------
# Per-platform: global hotkey + capture + cursor/focus need native APIs.
# ---------------------------------------------------------------------------
if(WIN32)
    # RegisterHotKey / WM_HOTKEY, BitBlt/DXGI capture, SetForegroundWindow.
    target_link_libraries(LightGet PRIVATE user32 gdi32)
    # For DXGI Desktop Duplication (optional, faster capture):
    # target_link_libraries(LightGet PRIVATE dxgi d3d11)

elseif(APPLE)
    # Carbon (RegisterEventHotKey), AppKit/ScreenCaptureKit, CoreGraphics.
    # NOTE: native macOS pieces (ScreenCaptureKit capture, NSWindow shield level,
    # forceCursorVisible, frontmost-app save/restore) live in .mm Objective-C++
    # translation units. Add them to LIGHTGET_SOURCES as e.g. src/mac/*.mm and
    # ensure they compile as Objective-C++ (set_source_files_properties LANGUAGE OBJCXX
    # or just name them .mm; CMake handles .mm via the OBJCXX language).
    find_library(CARBON_LIB Carbon)
    find_library(COCOA_LIB Cocoa)
    find_library(COREGRAPHICS_LIB CoreGraphics)
    find_library(SCREENCAPTUREKIT_LIB ScreenCaptureKit)
    target_link_libraries(LightGet PRIVATE
        ${CARBON_LIB}
        ${COCOA_LIB}
        ${COREGRAPHICS_LIB}
        ${SCREENCAPTUREKIT_LIB}
    )

    # -----------------------------------------------------------------------
    # Faithful native macOS pieces (OPT-IN; HAVE_MAC_NATIVE is undefined here).
    #
    # By default this is a plain pure-Qt build: the overlay is a normal topmost
    # Qt::Tool window and capture goes through QScreen, so `cmake build` links on
    # macOS TODAY without any Objective-C++ translation units. The cursor/focus
    # and CGShieldingWindowLevel hooks (TrayApp_forceCursorVisible,
    # TrayApp_recordFrontmostApp, TrayApp_restoreApp, OverlayWindow_applyShieldLevel)
    # are gated behind `#if defined(Q_OS_MAC) && defined(HAVE_MAC_NATIVE)` and
    # stay no-ops while HAVE_MAC_NATIVE is absent.
    #
    # To enable the native pieces once the .mm files exist:
    #   1. Either add them to LIGHTGET_SOURCES (the set() near the top, before
    #      add_executable), e.g. src/mac/TrayApp_mac.mm and
    #      src/mac/OverlayWindow_mac.mm, OR attach them to the target here:
    #        target_sources(LightGet PRIVATE src/mac/TrayApp_mac.mm
    #                                        src/mac/OverlayWindow_mac.mm)
    #      (CMake compiles .mm as Objective-C++ automatically.)
    #   2. Define the flag so the native branches are compiled in:
    #        target_compile_definitions(LightGet PRIVATE HAVE_MAC_NATIVE=1)
    # -----------------------------------------------------------------------
    # target_compile_definitions(LightGet PRIVATE HAVE_MAC_NATIVE=1)
    # LSUIElement (accessory / no Dock icon) must be set in the app bundle's
    # Info.plist. Provide resources/Info.plist and uncomment:
    # set_target_properties(LightGet PROPERTIES
    #     MACOSX_BUNDLE TRUE
    #     MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist)

elseif(UNIX)
    # X11: XGrabKey global hotkey, XGetImage capture, EWMH focus restore.
    find_package(X11)
    if(X11_FOUND)
        target_include_directories(LightGet PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(LightGet PRIVATE ${X11_LIBRARIES})
        target_compile_definitions(LightGet PRIVATE HAVE_X11=1)
    endif()
    # Wayland: global hotkeys / screen capture require xdg-desktop-portal
    # (GlobalShortcuts / ScreenCast portals) — see DESIGN.md for limitations.
endif()
