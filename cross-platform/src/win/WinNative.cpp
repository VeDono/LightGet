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

#include <windows.h>

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
                g_hotkeyCb();
            }
            return 1;   // swallow, so e.g. the Snipping Tool does not also open
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
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
    g_hotkeyVk = vk;
    g_hotkeyMods = mods;
    g_hotkeyCb = cb;
    g_lastFire = 0;
    g_hotkeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, hotkeyProc,
                                     GetModuleHandleW(nullptr), 0);
    if (!g_hotkeyHook) { g_hotkeyCb = nullptr; g_hotkeyVk = 0; return false; }
    return true;
}

void WinNative_removeHotkeyHook() {
    if (g_hotkeyHook) { UnhookWindowsHookEx(g_hotkeyHook); g_hotkeyHook = nullptr; }
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
                // CAPTUREBLT also picks up layered / transparent windows.
                if (BitBlt(memDC, 0, 0, w, h, screenDC,
                           target->rc.left, target->rc.top, SRCCOPY | CAPTUREBLT)) {
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
