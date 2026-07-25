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
