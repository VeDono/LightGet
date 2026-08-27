// WinNative.cpp — Windows-only native helpers (mirrors the mac/MacNative.mm
// pattern: plain C++ linkage, called through `extern` declarations at the use
// site, compiled only on WIN32).
//
// 1) WinNative_captureScreen — per-monitor screen grab.
//
//    QScreen::grabWindow(0) goes through CreateCompatibleBitmap (a bitmap in the
//    DISPLAY's format) plus a GetDIBits round-trip, so the pixel format and the
//    row padding depend on the current display mode, and the source rectangle is
//    derived from Qt's DPI-scaled geometry. On Windows that produced a corrupted
//    band along the RIGHT edge of the capture — visibly shifted/garbage columns.
//
//    This path removes every one of those variables: the monitor rectangle comes
//    from GetMonitorInfo in real physical pixels, and the target is an explicit
//    32-bpp BI_RGB top-down DIB section whose stride is exactly width*4 (32-bpp
//    rows are inherently 4-byte aligned, so there is no padding to mis-handle).
//    The blit excludes the mouse cursor (see the SRCCOPY note at the call).
//
// 2) WinNative_keyDisplayName — label a hotkey by the physical key that will be
//    registered, using the VK code, instead of by the character the key happens to
//    produce under the current layout / AltGr.
//
// NOMINMAX and WIN32_LEAN_AND_MEAN are set project-wide (see CMakeLists).

#include <QImage>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <limits>
#include <thread>

#include <windows.h>

// Forward declarations: the install helpers below call their own teardown
// counterparts before those are defined.
void WinNative_endKeyCapture();
void WinNative_removeHotkeyHook();

namespace {

struct MonitorEntry {
    QString device;   // e.g. "\\\\.\\DISPLAY1" — matches QScreen::name() on Windows
    RECT    rc{};     // physical, virtual-desktop coordinates
};

BOOL CALLBACK collectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM userData) {
    auto* out = reinterpret_cast<QVector<MonitorEntry>*>(userData);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(mon, &info))
        out->append(MonitorEntry{QString::fromWCharArray(info.szDevice), info.rcMonitor});
    return TRUE;
}

// Modifier bitmask shared with the Qt side: 1=Ctrl 2=Alt 4=Shift 8=Win.
quint32 currentModMask() {
    quint32 m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= 0x1;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= 0x2;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= 0x4;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        m |= 0x8;
    return m;
}

bool isModifierVk(DWORD vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU:    case VK_LMENU:    case VK_RMENU:
    case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
    case VK_LWIN:    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

// --- recorder capture hook -------------------------------------------------
HHOOK g_captureHook = nullptr;
void (*g_captureCb)(quint32, quint32) = nullptr;

LRESULT CALLBACK captureProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_captureCb) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        // PrintScreen is the reason this hook exists: Windows does not deliver a
        // normal key-down for it, so accept it on whichever edge shows up.
        const bool printEdge = (kb->vkCode == VK_SNAPSHOT && wParam == WM_KEYUP);
        if ((down || printEdge) && !isModifierVk(kb->vkCode)) {
            g_captureCb(quint32(kb->vkCode), currentModMask());
            return 1;   // swallow while recording so the key can't leak elsewhere
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// --- global-hotkey fallback hook -------------------------------------------
HHOOK g_hotkeyHook = nullptr;
void (*g_hotkeyCb)() = nullptr;
quint32 g_hotkeyVk = 0;
quint32 g_hotkeyMods = 0;
DWORD   g_lastFire = 0;

// A WH_KEYBOARD_LL callback runs inside the OS input path, on the thread that
// installed it, and it MUST return within LowLevelHooksTimeout (300 ms by default,
// read from HKCU\Control Panel\Desktop). Overrun it and Windows SILENTLY DROPS
// the hook:
// no error, no callback, the key simply stops being swallowed -- and the shell's
// own Print Screen binding takes over from then on. Taking a screenshot (grabbing
// every monitor, building the overlay windows) is an order of magnitude slower
// than that budget, so the hook must never do the work itself. It posts to this
// message-only window and returns in microseconds; the capture then runs from the
// ordinary message loop, safely outside the hook.
constexpr UINT     kMsgHotkeyFired  = WM_APP + 0x51;
constexpr UINT     kRearmIntervalMs = 30000;

HWND g_dispatchWnd = nullptr;

void rearmHotkeyHook();

LRESULT CALLBACK dispatchProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == kMsgHotkeyFired) {
        if (g_hotkeyCb) g_hotkeyCb();   // the slow part, safely outside the hook
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

HWND dispatchWindow() {
    if (g_dispatchWnd) return g_dispatchWnd;
    static const wchar_t kCls[] = L"LightGetHotkeyDispatch";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = dispatchProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);   // a duplicate registration just fails, harmlessly
    g_dispatchWnd = CreateWindowExW(0, kCls, kCls, 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    return g_dispatchWnd;
}

LRESULT CALLBACK hotkeyProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_hotkeyCb && g_hotkeyVk != 0) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool printEdge = (kb->vkCode == VK_SNAPSHOT && wParam == WM_KEYUP);
        if ((down || printEdge) && quint32(kb->vkCode) == g_hotkeyVk
            && currentModMask() == g_hotkeyMods) {
            const DWORD now = GetTickCount();
            if (now - g_lastFire > 300) {   // debounce auto-repeat / both edges
                g_lastFire = now;
                PostMessageW(g_dispatchWnd, kMsgHotkeyFired, 0, 0);
            }
            return 1;   // swallow, so e.g. the Snipping Tool does not also open
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Re-install the hook in place. Windows drops low-level hooks without telling
// anybody -- on a timeout, and across some session transitions -- and there is no
// API to ask whether ours is still alive. Re-arming on a timer costs a couple of
// microseconds and turns a permanently dead shortcut into, at worst, one missed
// press. The callback and key stay put; only the OS registration is renewed.
// Runs ON the hook thread: a hook is owned by the thread that installed it.
void rearmHotkeyHook() {
    if (!g_hotkeyCb || g_hotkeyVk == 0) return;
    if (g_hotkeyHook) { UnhookWindowsHookEx(g_hotkeyHook); g_hotkeyHook = nullptr; }
    g_hotkeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, hotkeyProc,
                                     GetModuleHandleW(nullptr), 0);
}

// --- the hook's own thread --------------------------------------------------
// The hook lives on a thread of its own, which does nothing but pump messages.
// That matters because the timeout above is charged against the thread that owns
// the hook: were it the GUI thread, every moment that thread spends NOT pumping
// -- taking the screenshot, building the overlay, painting -- would count against
// the 300 ms, and a second press during a capture could kill the shortcut. This
// thread is never busy, so it always answers instantly no matter what the rest of
// the app is doing. It only posts to the GUI thread's dispatch window; all real
// work still happens over there.
std::thread* g_hookThread   = nullptr;
DWORD        g_hookThreadId = 0;
HANDLE       g_hookReady    = nullptr;
bool         g_hookOk       = false;

void hookThreadMain() {
    // Force the message queue into existence before anyone may PostThreadMessage.
    MSG probe;
    PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_hookThreadId = GetCurrentThreadId();

    // This thread only waits for a keystroke and forwards it, so it can afford the
    // top priority -- and it needs it. The hook's 300 ms budget is wall-clock, so
    // a game saturating the CPU can starve this thread until Windows gives up on
    // the hook and drops it. Costs nothing: the thread is asleep in GetMessage.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    g_hotkeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, hotkeyProc,
                                     GetModuleHandleW(nullptr), 0);
    g_hookOk = (g_hotkeyHook != nullptr);
    SetEvent(g_hookReady);              // unblock the installer either way
    if (!g_hookOk) return;

    // Thread timer (no window): WM_TIMER lands straight in this loop. A window-less
    // SetTimer ignores the id argument and RETURNS the one to kill it with.
    const UINT_PTR timer = SetTimer(nullptr, 0, kRearmIntervalMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) rearmHotkeyHook();
    }

    if (timer) KillTimer(nullptr, timer);
    if (g_hotkeyHook) { UnhookWindowsHookEx(g_hotkeyHook); g_hotkeyHook = nullptr; }
}

void stopHookThread() {
    if (!g_hookThread) return;
    if (g_hookThreadId) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        if (g_hookThread->joinable()) g_hookThread->join();
    } else if (g_hookThread->joinable()) {
        g_hookThread->detach();   // never reached its loop -- don't hang on it
    }
    delete g_hookThread;
    g_hookThread   = nullptr;
    g_hookThreadId = 0;
    if (g_hookReady) { CloseHandle(g_hookReady); g_hookReady = nullptr; }
}

// Keys whose scan code needs the "extended" lParam bit for GetKeyNameText.
bool isExtendedKey(UINT vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:
    case VK_LEFT:   case VK_RIGHT:  case VK_UP:   case VK_DOWN:
    case VK_NUMLOCK: case VK_SNAPSHOT: case VK_DIVIDE:
    case VK_RCONTROL: case VK_RMENU:
        return true;
    default:
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Hotkey RECORDING via a low-level keyboard hook.
//
// Qt key events are not enough on Windows: the OS never sends a normal key-down
// for PrintScreen, and — worse — Windows 11 binds PrintScreen to the Snipping
// Tool by default, so the key never reaches the app at all. A WH_KEYBOARD_LL
// hook sees every key before that dispatch, which is how other screenshot tools
// let you assign PrintScreen (and Win / AltGr combos) at all.
//
// The callback runs on the GUI thread (hooks fire during message dispatch).
// ---------------------------------------------------------------------------
bool WinNative_beginKeyCapture(void (*cb)(quint32 vk, quint32 mods)) {
    WinNative_endKeyCapture();
    if (!cb) return false;
    g_captureCb = cb;
    g_captureHook = SetWindowsHookExW(WH_KEYBOARD_LL, captureProc,
                                      GetModuleHandleW(nullptr), 0);
    if (!g_captureHook) { g_captureCb = nullptr; return false; }
    return true;
}

void WinNative_endKeyCapture() {
    if (g_captureHook) { UnhookWindowsHookEx(g_captureHook); g_captureHook = nullptr; }
    g_captureCb = nullptr;
}

// ---------------------------------------------------------------------------
// Global-hotkey FALLBACK via the same mechanism, for when RegisterHotKey is
// refused because another process already owns the combo (again: PrintScreen,
// which Windows 11 hands to the Snipping Tool out of the box). Swallowing the key
// here also stops that other handler from firing alongside us.
// `mods` uses the 1=Ctrl 2=Alt 4=Shift 8=Win bitmask.
// ---------------------------------------------------------------------------
bool WinNative_installHotkeyHook(quint32 vk, quint32 mods, void (*cb)()) {
    WinNative_removeHotkeyHook();
    if (vk == 0 || !cb) return false;
    if (!dispatchWindow()) return false;
    g_hotkeyVk = vk;
    g_hotkeyMods = mods;
    g_hotkeyCb = cb;
    g_lastFire = 0;

    g_hookOk    = false;
    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hookThread = new std::thread(hookThreadMain);
    if (g_hookReady) WaitForSingleObject(g_hookReady, 3000);
    if (!g_hookOk) {   // SetWindowsHookEx failed -- report it like before
        stopHookThread();
        g_hotkeyCb = nullptr;
        g_hotkeyVk = 0;
        return false;
    }
    return true;
}

void WinNative_removeHotkeyHook() {
    stopHookThread();   // unhooks on the thread that owns the hook
    g_hotkeyCb = nullptr;
    g_hotkeyVk = 0;
    g_hotkeyMods = 0;
}

// ---------------------------------------------------------------------------
// Capture the given screen at its exact physical resolution. Returns a null
// QImage on any failure so the caller can fall back to the Qt baseline.
// ---------------------------------------------------------------------------
QImage WinNative_captureScreen(QScreen* screen) {
    if (!screen) return QImage();

    QVector<MonitorEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitors.isEmpty()) return QImage();

    // Prefer an exact device-name match (Qt uses the same "\\.\DISPLAYn" names).
    const MonitorEntry* target = nullptr;
    for (const MonitorEntry& m : monitors) {
        if (m.device == screen->name()) { target = &m; break; }
    }
    // Fallback: the monitor whose physical size is closest to this screen's
    // logical geometry scaled by its device pixel ratio.
    if (!target) {
        const qreal dpr = screen->devicePixelRatio() > 0 ? screen->devicePixelRatio() : 1.0;
        const QRect g = screen->geometry();
        const int wantW = qRound(g.width() * dpr);
        const int wantH = qRound(g.height() * dpr);
        int best = std::numeric_limits<int>::max();
        for (const MonitorEntry& m : monitors) {
            const int dw = qAbs(int(m.rc.right - m.rc.left) - wantW)
                         + qAbs(int(m.rc.bottom - m.rc.top) - wantH);
            if (dw < best) { best = dw; target = &m; }
        }
    }
    if (!target) return QImage();

    const int w = int(target->rc.right - target->rc.left);
    const int h = int(target->rc.bottom - target->rc.top);
    if (w <= 0 || h <= 0) return QImage();

    HDC screenDC = GetDC(nullptr);   // whole virtual desktop, physical pixels
    if (!screenDC) return QImage();

    QImage result;
    if (HDC memDC = CreateCompatibleDC(screenDC)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;          // negative => top-down rows
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;          // stride == w * 4, never padded
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        if (HBITMAP dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
            if (bits) {
                HGDIOBJ previous = SelectObject(memDC, dib);
                // SRCCOPY only — deliberately NOT CAPTUREBLT: that flag forces a
                // full screen redraw, which bakes the MOUSE CURSOR into the grab
                // (and makes the screen flash). It exists for pre-Vista layered
                // windows; since the DWM composites those into the desktop surface
                // itself, a plain SRCCOPY already sees them. macOS never captured
                // the cursor, so this also makes the platforms behave alike.
                if (BitBlt(memDC, 0, 0, w, h, screenDC,
                           target->rc.left, target->rc.top, SRCCOPY)) {
                    GdiFlush();   // make sure the GDI writes landed in `bits`
                    // 32-bpp BI_RGB is B,G,R,unused per pixel — byte-for-byte
                    // QImage::Format_RGB32. copy() detaches from the DIB memory.
                    result = QImage(static_cast<const uchar*>(bits), w, h, w * 4,
                                    QImage::Format_RGB32).copy();
                }
                SelectObject(memDC, previous);
            }
            DeleteObject(dib);
        }
        DeleteDC(memDC);
    }
    ReleaseDC(nullptr, screenDC);

    if (!result.isNull())
        result.setDevicePixelRatio(screen->devicePixelRatio());
    return result;
}

// ---------------------------------------------------------------------------
// Human-readable name for a Windows virtual-key code, for the hotkey field.
// Short labels for the keys GetKeyNameText spells out verbosely; otherwise the
// OS's own (localized, layout-aware) name, which matches what is printed on the
// physical key that gets registered.
// ---------------------------------------------------------------------------
QString WinNative_keyDisplayName(quint32 vk) {
    if (vk == 0) return QString();

    switch (vk) {
    case VK_SNAPSHOT: return QStringLiteral("PrtSc");
    case VK_PAUSE:    return QStringLiteral("Pause");
    case VK_SCROLL:   return QStringLiteral("ScrLk");
    case VK_PRIOR:    return QStringLiteral("PgUp");
    case VK_NEXT:     return QStringLiteral("PgDn");
    case VK_INSERT:   return QStringLiteral("Ins");
    case VK_DELETE:   return QStringLiteral("Del");
    case VK_LEFT:     return QStringLiteral("←");
    case VK_RIGHT:    return QStringLiteral("→");
    case VK_UP:       return QStringLiteral("↑");
    case VK_DOWN:     return QStringLiteral("↓");
    case VK_RETURN:   return QStringLiteral("Enter");
    case VK_SPACE:    return QStringLiteral("Space");
    case VK_TAB:      return QStringLiteral("Tab");
    case VK_BACK:     return QStringLiteral("Backspace");
    case VK_ESCAPE:   return QStringLiteral("Esc");
    case VK_APPS:     return QStringLiteral("Menu");
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
        return QStringLiteral("F%1").arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return QStringLiteral("Num %1").arg(vk - VK_NUMPAD0);

    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scan != 0) {
        LONG lParam = LONG(scan << 16);
        if (isExtendedKey(vk)) lParam |= (1 << 24);
        wchar_t name[64] = {};
        constexpr int kNameCap = int(sizeof(name) / sizeof(name[0]));
        if (GetKeyNameTextW(lParam, name, kNameCap) > 0) {
            QString s = QString::fromWCharArray(name);
            if (!s.isEmpty()) return s.size() == 1 ? s.toUpper() : s;
        }
    }
    // Last resort: printable ASCII VKs are their own character ('A'..'Z', '0'..'9').
    if (vk >= 0x30 && vk <= 0x5A) return QString(QChar(ushort(vk)));
    return QString();
}

// Windows parks background processes under EcoQoS: a reduced clock, and on hybrid
// CPUs a preference for the efficiency cores. A tray app with no visible window is
// exactly the profile that gets throttled, so when a game has the foreground, our
// shortcut and overlay come back sluggish. Opting out does not ask for more CPU --
// only to be clocked normally on the occasions we do run.
void WinNative_optOutOfPowerThrottling() {
#ifdef PROCESS_POWER_THROTTLING_CURRENT_VERSION
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask   = 0;   // 0 for the masked control == throttling off
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                          &st, sizeof(st));
#endif
}

// Lift the process while a capture is actually on screen, and put it straight
// back afterwards. The grab and the overlay have to feel instant, and against a
// game's threads a normal-priority background process waits its turn. Deliberately
// ABOVE_NORMAL and not HIGH: enough to be scheduled promptly, not enough to make
// the machine stutter, and it lasts only as long as the overlay does.
void WinNative_setCaptureBoost(bool on) {
    SetPriorityClass(GetCurrentProcess(),
                     on ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
}
