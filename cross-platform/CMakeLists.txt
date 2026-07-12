cmake_minimum_required(VERSION 3.16)

project(LightGet LANGUAGES CXX)

# Configurable identity. The Qt port ships as "LightGet" (com.mrleondono.LightGet);
# the native Swift app is "LightGet SWFT" (com.mrleondono.LightGetSWFT), so the two
# can coexist in /Applications. Override at configure time only for a throwaway
# parallel build, e.g. -DAPP_NAME="LightGet Dev" -DBUNDLE_ID=com.mrleondono.LightGetDev.
# APP_NAME also becomes the in-app settings scope (own QSettings store).
set(APP_NAME "LightGet" CACHE STRING "Product / bundle display name")
set(BUNDLE_ID "com.mrleondono.LightGet" CACHE STRING "macOS bundle identifier")

# ---------------------------------------------------------------------------
# Standard / Qt setup
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)   # required for Q_OBJECT classes
set(CMAKE_AUTORCC ON)   # for the icon .qrc (assets)
set(CMAKE_AUTOUIC OFF)  # no .ui files; UI is built in code

# ---------------------------------------------------------------------------
# Workaround: AGL was removed from recent macOS SDKs, but Qt's bundled
# FindWrapOpenGL.cmake still hardcodes a literal `-framework AGL` fallback when
# find_library(AGL) fails. That bogus flag makes the final link fail with
# "framework 'AGL' not found". We can't edit the Qt SDK, so we pre-seed the
# WrapOpenGL_AGL cache entry with a real, harmless framework (OpenGL, which Qt
# links anyway). find_library inside Qt then sees the variable already set and
# skips the broken literal fallback. No-op on non-Apple platforms.
# ---------------------------------------------------------------------------
if(APPLE)
    # Resolve a real, existing framework for the "AGL slot". OpenGL still ships in
    # the SDK and Qt links it anyway, so reusing it here is harmless and avoids a
    # second copy of the same framework. FORCE overrides any stale AGL path that a
    # previous configure may have cached (e.g. a phantom /System/.../AGL.framework
    # that no longer exists in the active SDK).
    find_library(_LIGHTGET_OPENGL_FW NAMES OpenGL)
    if(_LIGHTGET_OPENGL_FW)
        set(WrapOpenGL_AGL "${_LIGHTGET_OPENGL_FW}" CACHE FILEPATH
            "AGL replacement (real OpenGL framework); AGL was removed from the SDK" FORCE)
    endif()
endif()

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
    ${LIGHTGET_RESOURCES}   # resources/assets.qrc — compiled in via AUTORCC
)

target_include_directories(LightGet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(LightGet PRIVATE
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
)

# In-app settings scope = APP_NAME (so an alternate build keeps its own QSettings).
target_compile_definitions(LightGet PRIVATE LIGHTGET_APP_NAME="${APP_NAME}")

# M_PI and friends: POSIX <math.h> (Clang/GCC, i.e. macOS/Linux) define them, but
# MSVC's <cmath> only exposes them when _USE_MATH_DEFINES is set BEFORE the include.
# Toolbar.cpp / OverlayWindow.cpp use M_PI, so the Windows build fails without this.
# Harmless on the other toolchains.
target_compile_definitions(LightGet PRIVATE _USE_MATH_DEFINES)

# Version + edition label, surfaced in the settings About/footer so the two
# builds (this Qt port vs the native macOS app) are easy to tell apart.
# NOTE: the edition string contains parentheses, which trip CMake's
# "function-style macro" heuristic and get DROPPED if written inline. Routing the
# value through a set() variable first makes CMake quote it correctly so the full
# "Cross-platform (Qt 6)" reaches the compiler (verified in the generated build).
set(LIGHTGET_VERSION "1.0.5" CACHE STRING "Product version string")
set(LIGHTGET_EDITION "Cross-platform (Qt 6)" CACHE STRING "Build edition label")
target_compile_definitions(LightGet PRIVATE
    LIGHTGET_VERSION="${LIGHTGET_VERSION}"
    LIGHTGET_EDITION="${LIGHTGET_EDITION}")

# ---------------------------------------------------------------------------
# Per-platform: global hotkey + capture + cursor/focus need native APIs.
# ---------------------------------------------------------------------------
if(WIN32)
    # GUI subsystem = no console window for the tray-only app. /ENTRY:mainCRTStartup
    # keeps our plain int main() as the entry point (the CRT startup calls it), so
    # we don't need Qt's WinMain entry-point library. Without this the .exe launches
    # an empty terminal window next to the (window-less) tray app.
    if(MSVC)
        target_link_options(LightGet PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    endif()
    # Embed the application icon into the .exe so the executable file itself
    # (Explorer / taskbar / Alt-Tab) shows the LightGet logo. app.rc references
    # AppIcon.ico (next to it in resources/); MSVC's rc.exe compiles .rc files
    # automatically when they are listed as target sources.
    target_sources(LightGet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc)

    # RegisterHotKey / WM_HOTKEY, BitBlt/DXGI capture, SetForegroundWindow.
    target_link_libraries(LightGet PRIVATE user32 gdi32)
    # NOMINMAX: windows.h's min/max macros otherwise clash with std::min/max and
    # Qt's numeric_limits use. WIN32_LEAN_AND_MEAN trims the windows.h surface.
    target_compile_definitions(LightGet PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
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

    # --- macOS .app bundle (accessory app, no Dock icon via LSUIElement) ---
    set_target_properties(LightGet PROPERTIES
        OUTPUT_NAME "${APP_NAME}"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_VERSION "6"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.5"
        MACOSX_BUNDLE_ICON_FILE "AppIcon"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
    )
    set(_LIGHTGET_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.icns")
    if(EXISTS "${_LIGHTGET_ICNS}")
        target_sources(LightGet PRIVATE "${_LIGHTGET_ICNS}")
        set_source_files_properties("${_LIGHTGET_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
    endif()

    # -----------------------------------------------------------------------
    # Native macOS pieces (ENABLED): CGShieldingWindowLevel overlay (so the
    # selection can cover the menu bar), force-cursor-visible, and frontmost-app
    # record/restore. Implemented in src/mac/MacNative.mm (Objective-C++ / ARC).
    # Enabling HAVE_MAC_NATIVE activates every guarded call site, so this .mm MUST
    # define all four externs or the link fails.
    # -----------------------------------------------------------------------
    target_sources(LightGet PRIVATE src/mac/MacNative.mm)
    set_source_files_properties(src/mac/MacNative.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(LightGet PRIVATE HAVE_MAC_NATIVE=1)
    # Cocoa (already linked above) re-exports AppKit; add AppKit explicitly for
    # clarity. CoreGraphics is already linked. ApplicationServices covers the
    # CGShieldingWindowLevel / CGDisplay* calls.
    find_library(APPKIT_LIB AppKit)
    find_library(APPSERVICES_LIB ApplicationServices)
    find_library(SERVICEMGMT_LIB ServiceManagement)   # SMAppService (launch-at-login)
    target_link_libraries(LightGet PRIVATE ${APPKIT_LIB} ${APPSERVICES_LIB} ${SERVICEMGMT_LIB})

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
    # Wayland: QScreen::grabWindow(0) returns null there, so screen capture goes
    # through the org.freedesktop.portal.Screenshot D-Bus portal (QtDBus).
    find_package(Qt6 COMPONENTS DBus)
    if(Qt6DBus_FOUND)
        target_sources(LightGet PRIVATE src/WaylandPortal.cpp)
        target_link_libraries(LightGet PRIVATE Qt6::DBus)
        target_compile_definitions(LightGet PRIVATE HAVE_QTDBUS=1)
    endif()
endif()
