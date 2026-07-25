// GlobalHotkey.cpp — Platform-abstracted system-wide hotkey.
//
// Port of HotKey.swift (Carbon RegisterEventHotKey). The internal/persisted
// representation is always the macOS *Carbon* virtual-key code + Carbon
// modifier mask (Settings::hotKeyCode / hotKeyModifiers; default Shift+Cmd+2 =
// keyCode 19 / mods 768). Each backend translates Carbon -> native at the edge.
//
// Exactly ONE process-wide event handler / native event filter is installed,
// regardless of how many GlobalHotkey instances exist. The macOS Swift source
// installed a handler per-instance (a latent duplicate-fire bug) — here a single
// shared registry maps the OS-delivered id back to the owning instance and emits
// its activated() signal.
//
// Backends (selected via #ifdef):
//   - Windows : RegisterHotKey to a process-wide message-only window + WM_HOTKEY
//               handled by that window's WNDPROC
//   - Linux/X11 : XGrabKey on the root window + filter XCB_KEY_PRESS
//                 (every NumLock/CapsLock lock-mask variant is grabbed)
//   - macOS : Carbon RegisterEventHotKey + kEventHotKeyPressed (pass-through)
//
// Registration failure is surfaced via the return value of registerHotkey()
// (and registered()), not silently swallowed as in the original.

#include "GlobalHotkey.h"
#include "Settings.h" // CarbonKeys::*

#include <QHash>
#include <QtGlobal>
#include <QtCore/qnamespace.h>   // Qt::Key (kQtKeyFlag payloads)

// Carbon modifier constants (mirrored from CarbonKeys so each backend can mask).
namespace {
constexpr uint32_t kCmd  = CarbonKeys::cmdKey;     // 0x0100
constexpr uint32_t kShft = CarbonKeys::shiftKey;   // 0x0200
constexpr uint32_t kOpt  = CarbonKeys::optionKey;  // 0x0800
constexpr uint32_t kCtrl = CarbonKeys::controlKey; // 0x1000

// 'SNAP' — the arbitrary 4-byte owner signature from the Swift source.
constexpr uint32_t kSignature = 0x534E4150;
} // namespace

// ===========================================================================
//  Windows
// ===========================================================================
#if defined(Q_OS_WIN)

#include <windows.h>

namespace {

// Carbon virtual-key -> Windows VK_. Covers the keys the recorder can produce.
// Returns 0 (unmappable) for anything we don't know how to translate.
UINT carbonKeyToVk(uint32_t kc) {
    switch (kc) {
    // Letters (Carbon ANSI codes) -> VK is just the uppercase ASCII.
    case 0x00: return 'A'; case 0x0B: return 'B'; case 0x08: return 'C';
    case 0x02: return 'D'; case 0x0E: return 'E'; case 0x03: return 'F';
    case 0x05: return 'G'; case 0x04: return 'H'; case 0x22: return 'I';
    case 0x26: return 'J'; case 0x28: return 'K'; case 0x25: return 'L';
    case 0x2E: return 'M'; case 0x2D: return 'N'; case 0x1F: return 'O';
    case 0x23: return 'P'; case 0x0C: return 'Q'; case 0x0F: return 'R';
    case 0x01: return 'S'; case 0x11: return 'T'; case 0x20: return 'U';
    case 0x09: return 'V'; case 0x0D: return 'W'; case 0x07: return 'X';
    case 0x10: return 'Y'; case 0x06: return 'Z';
    // Number row (kVK_ANSI_0..9). kVK_ANSI_2 = 0x13 -> '2'.
    case 0x1D: return '0'; case 0x12: return '1'; case 0x13: return '2';
    case 0x14: return '3'; case 0x15: return '4'; case 0x17: return '5';
    case 0x16: return '6'; case 0x1A: return '7'; case 0x1C: return '8';
    case 0x19: return '9';
    // Function keys.
    case 0x7A: return VK_F1;  case 0x78: return VK_F2;  case 0x63: return VK_F3;
    case 0x76: return VK_F4;  case 0x60: return VK_F5;  case 0x61: return VK_F6;
    case 0x62: return VK_F7;  case 0x64: return VK_F8;  case 0x65: return VK_F9;
    case 0x6D: return VK_F10; case 0x67: return VK_F11; case 0x6F: return VK_F12;
    // Common specials.
    case CarbonKeys::kVK_Escape: return VK_ESCAPE; // 0x35
    case 0x24: return VK_RETURN;  // kVK_Return
    case 0x30: return VK_TAB;     // kVK_Tab
    case 0x31: return VK_SPACE;   // kVK_Space
    case 0x33: return VK_BACK;    // kVK_Delete (backspace)
    // Carbon codes for keys a Mac keyboard has and Windows shares.
    case 0x75: return VK_DELETE;  // kVK_ForwardDelete
    case 0x73: return VK_HOME;    // kVK_Home
    case 0x77: return VK_END;     // kVK_End
    case 0x74: return VK_PRIOR;   // kVK_PageUp
    case 0x79: return VK_NEXT;    // kVK_PageDown
    case 0x7B: return VK_LEFT;    // kVK_LeftArrow
    case 0x7C: return VK_RIGHT;   // kVK_RightArrow
    case 0x7D: return VK_DOWN;    // kVK_DownArrow
    case 0x7E: return VK_UP;      // kVK_UpArrow
    case 0x69: return VK_F13;     // kVK_F13
    case 0x6B: return VK_F14;     // kVK_F14
    case 0x71: return VK_F15;     // kVK_F15
    case 0x6A: return VK_F16;     // kVK_F16
    case 0x40: return VK_F17;     // kVK_F17
    case 0x4F: return VK_F18;     // kVK_F18
    case 0x50: return VK_F19;     // kVK_F19
    case 0x5A: return VK_F20;     // kVK_F20
    default:   return 0;
    }
}

// Qt::Key -> Windows VK, for keys stored in the portable kQtKeyFlag space (and as
// the fallback when a recording could not read a native VK).
UINT qtKeyToVk(int k) {
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return static_cast<UINT>(k);           // 'A'..'Z'
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return static_cast<UINT>(k);           // '0'..'9'
    if (k >= Qt::Key_F1 && k <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (k - Qt::Key_F1));
    switch (k) {
    case Qt::Key_Escape:      return VK_ESCAPE;
    case Qt::Key_Return:
    case Qt::Key_Enter:       return VK_RETURN;
    case Qt::Key_Tab:         return VK_TAB;
    case Qt::Key_Space:       return VK_SPACE;
    case Qt::Key_Backspace:   return VK_BACK;
    case Qt::Key_Delete:      return VK_DELETE;
    case Qt::Key_Insert:      return VK_INSERT;
    case Qt::Key_Home:        return VK_HOME;
    case Qt::Key_End:         return VK_END;
    case Qt::Key_PageUp:      return VK_PRIOR;
    case Qt::Key_PageDown:    return VK_NEXT;
    case Qt::Key_Left:        return VK_LEFT;
    case Qt::Key_Right:       return VK_RIGHT;
    case Qt::Key_Up:          return VK_UP;
    case Qt::Key_Down:        return VK_DOWN;
    case Qt::Key_Print:       return VK_SNAPSHOT;   // PrintScreen
    case Qt::Key_Pause:       return VK_PAUSE;
    case Qt::Key_ScrollLock:  return VK_SCROLL;
    case Qt::Key_NumLock:     return VK_NUMLOCK;
    case Qt::Key_Menu:        return VK_APPS;
    case Qt::Key_Help:        return VK_HELP;
    case Qt::Key_Clear:       return VK_CLEAR;
    // Punctuation (US positions; RegisterHotKey is by position, not by character).
    case Qt::Key_Minus:        return VK_OEM_MINUS;
    case Qt::Key_Equal:        return VK_OEM_PLUS;
    case Qt::Key_BracketLeft:  return VK_OEM_4;
    case Qt::Key_BracketRight: return VK_OEM_6;
    case Qt::Key_Backslash:    return VK_OEM_5;
    case Qt::Key_Semicolon:    return VK_OEM_1;
    case Qt::Key_Apostrophe:   return VK_OEM_7;
    case Qt::Key_Comma:        return VK_OEM_COMMA;
    case Qt::Key_Period:       return VK_OEM_PERIOD;
    case Qt::Key_Slash:        return VK_OEM_2;
    case Qt::Key_QuoteLeft:    return VK_OEM_3;
    // Media / browser keys (handy as bare hotkeys).
    case Qt::Key_MediaPlay:     return VK_MEDIA_PLAY_PAUSE;
    case Qt::Key_MediaStop:     return VK_MEDIA_STOP;
    case Qt::Key_MediaNext:     return VK_MEDIA_NEXT_TRACK;
    case Qt::Key_MediaPrevious: return VK_MEDIA_PREV_TRACK;
    case Qt::Key_VolumeUp:      return VK_VOLUME_UP;
    case Qt::Key_VolumeDown:    return VK_VOLUME_DOWN;
    case Qt::Key_VolumeMute:    return VK_VOLUME_MUTE;
    default: return 0;
    }
}

// Persisted key code (any of the three tagged spaces) -> Windows VK.
UINT persistedKeyToVk(uint32_t code) {
    if (HotKeyCode::isWinVk(code))
        return static_cast<UINT>(HotKeyCode::payload(code));   // recorded natively
    if (HotKeyCode::isQtKey(code))
        return qtKeyToVk(static_cast<int>(HotKeyCode::payload(code)));
    return carbonKeyToVk(code);                                 // legacy Carbon
}

// Carbon modifier mask -> Win32 MOD_* mask (see Settings.h for the semantics).
// cmdKey -> Ctrl (the persisted "⌘" combo is Ctrl here), controlKey -> Win, so a
// Win-key hotkey is assignable (it used to collapse onto Ctrl, silently changing
// the combo the user picked).
UINT carbonModsToWin(uint32_t mods) {
    UINT m = 0;
    if (mods & kCmd)  m |= MOD_CONTROL;
    if (mods & kCtrl) m |= MOD_WIN;
    if (mods & kShft) m |= MOD_SHIFT;
    if (mods & kOpt)  m |= MOD_ALT;
    m |= MOD_NOREPEAT; // single fire while held
    return m;
}

} // namespace

// Process-wide registry: RegisterHotKey id (== WM_HOTKEY wParam) -> instance.
namespace {
QHash<int, GlobalHotkey*>& winRegistry() {
    static QHash<int, GlobalHotkey*> reg;
    return reg;
}
} // namespace

// Routes WM_HOTKEY (wParam == our id) to the owning instance.
static GlobalHotkey* winLookupHotkey(int id) {
    auto& reg = winRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? it.value() : nullptr;
}

namespace {

// WNDPROC for the message-only window: dispatch WM_HOTKEY to the owner.
LRESULT CALLBACK hotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        if (GlobalHotkey* owner = winLookupHotkey(static_cast<int>(wParam)))
            owner->emitActivated();
        return 0;
    }
    return ::DefWindowProc(hwnd, msg, wParam, lParam);
}

// One process-wide hidden message-only window. Created lazily, lives for the
// process. RegisterHotKey targets THIS hwnd, so WM_HOTKEY is always delivered
// here (thread-targeted, hwnd==NULL hotkeys are not reliably routed to Qt's
// native event filter — hence this dedicated window + WNDPROC).
HWND hotkeyMessageWindow() {
    static HWND s_hwnd = nullptr;
    if (s_hwnd) return s_hwnd;

    static const wchar_t* kClassName = L"SnapEditHotkeyMsgWindow";
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = hotkeyWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClassName;
    // Register once; ignore "class already exists" so re-entry is harmless.
    ::RegisterClassExW(&wc);

    s_hwnd = ::CreateWindowExW(0, kClassName, L"SnapEditHotkey", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, hinst, nullptr);
    return s_hwnd;
}

int nextWinHotkeyId() {
    // RegisterHotKey ids for an app must be in 0x0000..0xBFFF.
    static int counter = 0;
    return (counter++ % 0xB000);
}

} // namespace

struct GlobalHotkey::Impl {
    int id = 0;       // RegisterHotKey id (== WM_HOTKEY wParam)
    bool active = false;
};

GlobalHotkey::GlobalHotkey(QObject* parent) : QObject(parent) {
    d = new Impl();
    d->id = nextWinHotkeyId();
}

GlobalHotkey::~GlobalHotkey() {
    unregisterHotkey();
    winRegistry().remove(d->id);
    delete d;
}

bool GlobalHotkey::registerHotkey(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    unregisterHotkey();

    UINT vk = persistedKeyToVk(carbonKeyCode);
    if (vk == 0) { m_registered = false; return false; }
    UINT mods = carbonModsToWin(carbonModifiers);

    HWND hwnd = hotkeyMessageWindow();
    if (!hwnd) { m_registered = false; return false; }

    winRegistry().insert(d->id, this);
    if (!::RegisterHotKey(hwnd, d->id, mods, vk)) {
        winRegistry().remove(d->id);
        m_registered = false;
        return false;
    }
    d->active = true;
    m_registered = true;
    return true;
}

bool GlobalHotkey::reregister(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    return registerHotkey(carbonKeyCode, carbonModifiers);
}

void GlobalHotkey::unregisterHotkey() {
    if (d && d->active) {
        ::UnregisterHotKey(hotkeyMessageWindow(), d->id);
        d->active = false;
    }
    if (d) winRegistry().remove(d->id);
    m_registered = false;
}

// ===========================================================================
//  Linux / X11
// ===========================================================================
#elif defined(HAVE_X11) || defined(Q_OS_LINUX)

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
// QNativeInterface::QX11Application (Qt's X11 Display/xcb_connection accessors)
// lives in this opt-in platform header; <QGuiApplication> does NOT pull it in.
#include <QtGui/qguiapplication_platform.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

// X11 #defines these as preprocessor macros, which collide with Qt enum values
// and identifiers used in this file (and any Qt headers pulled in afterwards).
// Undef them here, locally to this translation unit, before any such use.
#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif
#ifdef Bool
#undef Bool
#endif
#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif
#ifdef FocusIn
#undef FocusIn
#endif
#ifdef FocusOut
#undef FocusOut
#endif
#ifdef CursorShape
#undef CursorShape
#endif
#ifdef Expose
#undef Expose
#endif
#ifdef Always
#undef Always
#endif

namespace {

// Qt's OWN X11 Display. CRITICAL: a passive XGrabKey delivers the triggered
// KeyPress only to the GRABBING client. The old code grabbed on a private
// XOpenDisplay() connection whose event queue was never read, while the filter
// below watches Qt's xcb connection — so the hotkey could NEVER fire on X11 (and
// the combo was swallowed system-wide). Grabbing on Qt's display means the
// KeyPress arrives on Qt's xcb event stream and reaches HotkeyNativeFilter.
Display* x11Display() {
    if (auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>())
        return x11->display();
    return nullptr;
}

// Temporary X error handler for the XGrabKey block: a combo already owned by
// another client raises BadAccess ASYNCHRONOUSLY, and with no handler installed
// Xlib's default handler prints and calls exit() — so a taken hotkey killed the
// app at startup. Record it and swallow it instead; the caller then fails
// cleanly and surfaces the conflict.
bool s_x11GrabFailed = false;
int (*s_prevX11Handler)(Display*, XErrorEvent*) = nullptr;
int x11GrabErrorHandler(Display* d, XErrorEvent* e) {
    if (e->error_code == BadAccess) { s_x11GrabFailed = true; return 0; }
    return s_prevX11Handler ? s_prevX11Handler(d, e) : 0;
}

// Carbon virtual-key -> X11 KeySym. Mirrors the Windows table.
KeySym carbonKeyToKeysym(uint32_t kc) {
    switch (kc) {
    case 0x00: return XK_a; case 0x0B: return XK_b; case 0x08: return XK_c;
    case 0x02: return XK_d; case 0x0E: return XK_e; case 0x03: return XK_f;
    case 0x05: return XK_g; case 0x04: return XK_h; case 0x22: return XK_i;
    case 0x26: return XK_j; case 0x28: return XK_k; case 0x25: return XK_l;
    case 0x2E: return XK_m; case 0x2D: return XK_n; case 0x1F: return XK_o;
    case 0x23: return XK_p; case 0x0C: return XK_q; case 0x0F: return XK_r;
    case 0x01: return XK_s; case 0x11: return XK_t; case 0x20: return XK_u;
    case 0x09: return XK_v; case 0x0D: return XK_w; case 0x07: return XK_x;
    case 0x10: return XK_y; case 0x06: return XK_z;
    case 0x1D: return XK_0; case 0x12: return XK_1; case 0x13: return XK_2;
    case 0x14: return XK_3; case 0x15: return XK_4; case 0x17: return XK_5;
    case 0x16: return XK_6; case 0x1A: return XK_7; case 0x1C: return XK_8;
    case 0x19: return XK_9;
    case 0x7A: return XK_F1;  case 0x78: return XK_F2;  case 0x63: return XK_F3;
    case 0x76: return XK_F4;  case 0x60: return XK_F5;  case 0x61: return XK_F6;
    case 0x62: return XK_F7;  case 0x64: return XK_F8;  case 0x65: return XK_F9;
    case 0x6D: return XK_F10; case 0x67: return XK_F11; case 0x6F: return XK_F12;
    case CarbonKeys::kVK_Escape: return XK_Escape;
    case 0x24: return XK_Return;
    case 0x30: return XK_Tab;
    case 0x31: return XK_space;
    case 0x33: return XK_BackSpace;
    // Carbon codes shared with a PC keyboard.
    case 0x75: return XK_Delete;
    case 0x73: return XK_Home;
    case 0x77: return XK_End;
    case 0x74: return XK_Prior;      // PageUp
    case 0x79: return XK_Next;       // PageDown
    case 0x7B: return XK_Left;
    case 0x7C: return XK_Right;
    case 0x7D: return XK_Down;
    case 0x7E: return XK_Up;
    case 0x69: return XK_F13; case 0x6B: return XK_F14; case 0x71: return XK_F15;
    case 0x6A: return XK_F16; case 0x40: return XK_F17; case 0x4F: return XK_F18;
    case 0x50: return XK_F19; case 0x5A: return XK_F20;
    default:   return NoSymbol;
    }
}

// Qt::Key -> X11 KeySym, for keys stored in the portable kQtKeyFlag space.
KeySym qtKeyToKeysym(int k) {
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return XK_a + (k - Qt::Key_A);
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return XK_0 + (k - Qt::Key_0);
    if (k >= Qt::Key_F1 && k <= Qt::Key_F35) return XK_F1 + (k - Qt::Key_F1);
    switch (k) {
    case Qt::Key_Escape:      return XK_Escape;
    case Qt::Key_Return:
    case Qt::Key_Enter:       return XK_Return;
    case Qt::Key_Tab:         return XK_Tab;
    case Qt::Key_Space:       return XK_space;
    case Qt::Key_Backspace:   return XK_BackSpace;
    case Qt::Key_Delete:      return XK_Delete;
    case Qt::Key_Insert:      return XK_Insert;
    case Qt::Key_Home:        return XK_Home;
    case Qt::Key_End:         return XK_End;
    case Qt::Key_PageUp:      return XK_Prior;
    case Qt::Key_PageDown:    return XK_Next;
    case Qt::Key_Left:        return XK_Left;
    case Qt::Key_Right:       return XK_Right;
    case Qt::Key_Up:          return XK_Up;
    case Qt::Key_Down:        return XK_Down;
    case Qt::Key_Print:       return XK_Print;      // PrintScreen
    case Qt::Key_Pause:       return XK_Pause;
    case Qt::Key_ScrollLock:  return XK_Scroll_Lock;
    case Qt::Key_NumLock:     return XK_Num_Lock;
    case Qt::Key_Menu:        return XK_Menu;
    case Qt::Key_Help:        return XK_Help;
    case Qt::Key_Minus:        return XK_minus;
    case Qt::Key_Equal:        return XK_equal;
    case Qt::Key_BracketLeft:  return XK_bracketleft;
    case Qt::Key_BracketRight: return XK_bracketright;
    case Qt::Key_Backslash:    return XK_backslash;
    case Qt::Key_Semicolon:    return XK_semicolon;
    case Qt::Key_Apostrophe:   return XK_apostrophe;
    case Qt::Key_Comma:        return XK_comma;
    case Qt::Key_Period:       return XK_period;
    case Qt::Key_Slash:        return XK_slash;
    case Qt::Key_QuoteLeft:    return XK_grave;
    default: return NoSymbol;
    }
}

// Persisted key code (any tagged space) -> KeySym.
KeySym persistedKeyToKeysym(uint32_t code) {
    if (HotKeyCode::isQtKey(code))
        return qtKeyToKeysym(static_cast<int>(HotKeyCode::payload(code)));
    if (HotKeyCode::isWinVk(code))
        return NoSymbol;                 // Windows-recorded code; not portable here
    return carbonKeyToKeysym(code);       // legacy Carbon
}

// Carbon modifier mask -> X11 base modifier mask (without lock variants).
// See Settings.h: cmdKey -> Control, controlKey -> Super (Mod4), optionKey -> Alt.
unsigned int carbonModsToX11(uint32_t mods) {
    unsigned int m = 0;
    if (mods & kCmd)  m |= ControlMask;
    if (mods & kCtrl) m |= Mod4Mask;  // Super / "Win" key
    if (mods & kShft) m |= ShiftMask;
    if (mods & kOpt)  m |= Mod1Mask;  // Alt
    return m;
}

// NumLock is conventionally Mod2Mask; CapsLock is LockMask. We grab every
// combination of these so the hotkey fires regardless of lock state.
const unsigned int kLockMasks[] = {
    0,
    LockMask,            // CapsLock
    Mod2Mask,            // NumLock
    LockMask | Mod2Mask, // both
};

} // namespace

class HotkeyNativeFilter : public QAbstractNativeEventFilter {
public:
    static HotkeyNativeFilter& instance() {
        static HotkeyNativeFilter f;
        static bool installed = false;
        if (!installed) {
            qApp->installNativeEventFilter(&f);
            installed = true;
        }
        return f;
    }

    struct Entry { KeyCode keycode; unsigned int baseMods; GlobalHotkey* owner; };
    QList<Entry> entries;

    bool nativeEventFilter(const QByteArray& type, void* message,
                           qintptr* /*result*/) override {
        if (type != "xcb_generic_event_t")
            return false;
        auto* ev = static_cast<xcb_generic_event_t*>(message);
        if ((ev->response_type & ~0x80) != XCB_KEY_PRESS)
            return false;
        auto* kp = reinterpret_cast<xcb_key_press_event_t*>(ev);
        const unsigned int cleaned = kp->state & ~(LockMask | Mod2Mask);
        for (const Entry& e : entries) {
            if (e.keycode == kp->detail && cleaned == e.baseMods) {
                if (e.owner) e.owner->emitActivated();
                return false;
            }
        }
        return false;
    }
};

struct GlobalHotkey::Impl {
    KeyCode keycode = 0;
    unsigned int baseMods = 0;
    bool active = false;
    HotkeyNativeFilter* filter = nullptr; // stored at construction; reused on teardown
};

GlobalHotkey::GlobalHotkey(QObject* parent) : QObject(parent) {
    d = new Impl();
    // Create/install the shared filter once and cache the pointer, so teardown
    // never re-creates the function-local static during static destruction.
    d->filter = &HotkeyNativeFilter::instance();
}

GlobalHotkey::~GlobalHotkey() {
    unregisterHotkey();
    delete d;
}

bool GlobalHotkey::registerHotkey(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    unregisterHotkey();

    Display* dpy = x11Display();
    if (!dpy) { m_registered = false; return false; }

    KeySym sym = persistedKeyToKeysym(carbonKeyCode);
    if (sym == NoSymbol) { m_registered = false; return false; }
    KeyCode kc = XKeysymToKeycode(dpy, sym);
    if (kc == 0) { m_registered = false; return false; }

    unsigned int baseMods = carbonModsToX11(carbonModifiers);
    Window root = DefaultRootWindow(dpy);

    // Grab every NumLock/CapsLock lock-mask variant so the combo fires regardless
    // of lock state, under a temporary error handler so a BadAccess (combo owned
    // by another client) is recorded instead of exiting the app.
    s_x11GrabFailed = false;
    s_prevX11Handler = XSetErrorHandler(x11GrabErrorHandler);
    for (unsigned int lock : kLockMasks) {
        XGrabKey(dpy, kc, baseMods | lock, root, /*owner_events*/ False,
                 GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);   // flush so any BadAccess is delivered before we restore
    XSetErrorHandler(s_prevX11Handler);

    if (s_x11GrabFailed) {
        // Another client owns the combo: undo whatever partial grab we made and
        // fail so applyHotkey() can surface it / roll back.
        for (unsigned int lock : kLockMasks)
            XUngrabKey(dpy, kc, baseMods | lock, root);
        XSync(dpy, False);
        m_registered = false;
        return false;
    }

    d->keycode = kc;
    d->baseMods = baseMods;
    d->active = true;

    if (d->filter)
        d->filter->entries.append({kc, baseMods, this});

    m_registered = true;
    return m_registered;
}

bool GlobalHotkey::reregister(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    return registerHotkey(carbonKeyCode, carbonModifiers);
}

void GlobalHotkey::unregisterHotkey() {
    if (d && d->active) {
        if (Display* dpy = x11Display()) {
            Window root = DefaultRootWindow(dpy);
            for (unsigned int lock : kLockMasks)
                XUngrabKey(dpy, d->keycode, d->baseMods | lock, root);
            XSync(dpy, False);
        }
        d->active = false;
    }
    // Use the stored filter pointer (never re-create the static during teardown).
    if (d && d->filter) {
        auto& entries = d->filter->entries;
        for (int i = entries.size() - 1; i >= 0; --i)
            if (entries[i].owner == this)
                entries.removeAt(i);
    }
    m_registered = false;
}

// ===========================================================================
//  macOS (Carbon — pass-through of the persisted codes)
// ===========================================================================
#elif defined(Q_OS_MACOS)

#include <Carbon/Carbon.h>

// Single process-wide registry: Carbon hotkey id -> owning instance. Unlike the
// Swift source (one InstallEventHandler per HotKey, causing duplicate fires),
// exactly ONE handler is installed for the whole process here.
namespace {

QHash<uint32_t, GlobalHotkey*>& macRegistry() {
    static QHash<uint32_t, GlobalHotkey*> reg;
    return reg;
}

uint32_t nextMacId() {
    static uint32_t counter = 0;
    return ++counter;
}

// Qt::Key -> Carbon kVK_*, for a settings file whose key was recorded in the
// portable kQtKeyFlag space (see Settings.h). Returns -1 when the key has no
// Carbon equivalent (kVK_ANSI_A is 0, so 0 cannot mean "unmappable" here).
int qtKeyToCarbon(int k) {
    switch (k) {
    case Qt::Key_A: return 0x00; case Qt::Key_B: return 0x0B; case Qt::Key_C: return 0x08;
    case Qt::Key_D: return 0x02; case Qt::Key_E: return 0x0E; case Qt::Key_F: return 0x03;
    case Qt::Key_G: return 0x05; case Qt::Key_H: return 0x04; case Qt::Key_I: return 0x22;
    case Qt::Key_J: return 0x26; case Qt::Key_K: return 0x28; case Qt::Key_L: return 0x25;
    case Qt::Key_M: return 0x2E; case Qt::Key_N: return 0x2D; case Qt::Key_O: return 0x1F;
    case Qt::Key_P: return 0x23; case Qt::Key_Q: return 0x0C; case Qt::Key_R: return 0x0F;
    case Qt::Key_S: return 0x01; case Qt::Key_T: return 0x11; case Qt::Key_U: return 0x20;
    case Qt::Key_V: return 0x09; case Qt::Key_W: return 0x0D; case Qt::Key_X: return 0x07;
    case Qt::Key_Y: return 0x10; case Qt::Key_Z: return 0x06;
    case Qt::Key_0: return 0x1D; case Qt::Key_1: return 0x12; case Qt::Key_2: return 0x13;
    case Qt::Key_3: return 0x14; case Qt::Key_4: return 0x15; case Qt::Key_5: return 0x17;
    case Qt::Key_6: return 0x16; case Qt::Key_7: return 0x1A; case Qt::Key_8: return 0x1C;
    case Qt::Key_9: return 0x19;
    case Qt::Key_F1:  return 0x7A; case Qt::Key_F2:  return 0x78; case Qt::Key_F3:  return 0x63;
    case Qt::Key_F4:  return 0x76; case Qt::Key_F5:  return 0x60; case Qt::Key_F6:  return 0x61;
    case Qt::Key_F7:  return 0x62; case Qt::Key_F8:  return 0x64; case Qt::Key_F9:  return 0x65;
    case Qt::Key_F10: return 0x6D; case Qt::Key_F11: return 0x67; case Qt::Key_F12: return 0x6F;
    case Qt::Key_F13: return 0x69; case Qt::Key_F14: return 0x6B; case Qt::Key_F15: return 0x71;
    case Qt::Key_F16: return 0x6A; case Qt::Key_F17: return 0x40; case Qt::Key_F18: return 0x4F;
    case Qt::Key_F19: return 0x50; case Qt::Key_F20: return 0x5A;
    case Qt::Key_Escape:    return 0x35;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return 0x24;
    case Qt::Key_Tab:       return 0x30;
    case Qt::Key_Space:     return 0x31;
    case Qt::Key_Backspace: return 0x33;
    case Qt::Key_Delete:    return 0x75;
    case Qt::Key_Home:      return 0x73;
    case Qt::Key_End:       return 0x77;
    case Qt::Key_PageUp:    return 0x74;
    case Qt::Key_PageDown:  return 0x79;
    case Qt::Key_Left:      return 0x7B;
    case Qt::Key_Right:     return 0x7C;
    case Qt::Key_Down:      return 0x7D;
    case Qt::Key_Up:        return 0x7E;
    case Qt::Key_Help:      return 0x72;
    case Qt::Key_Minus:        return 0x1B;
    case Qt::Key_Equal:        return 0x18;
    case Qt::Key_BracketLeft:  return 0x21;
    case Qt::Key_BracketRight: return 0x1E;
    case Qt::Key_Backslash:    return 0x2A;
    case Qt::Key_Semicolon:    return 0x29;
    case Qt::Key_Apostrophe:   return 0x27;
    case Qt::Key_Comma:        return 0x2B;
    case Qt::Key_Period:       return 0x2F;
    case Qt::Key_Slash:        return 0x2C;
    case Qt::Key_QuoteLeft:    return 0x32;
    default: return -1;   // no Carbon equivalent (PrintScreen, Pause, numpad, ...)
    }
}

EventHandlerRef g_handler = nullptr;

OSStatus macHotkeyHandler(EventHandlerCallRef, EventRef event, void*) {
    EventHotKeyID hkID;
    GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                      nullptr, sizeof(hkID), nullptr, &hkID);
    auto it = macRegistry().find(hkID.id);
    if (it != macRegistry().end() && it.value())
        it.value()->emitActivated();
    return noErr;
}

void installMacHandlerIfNeeded() {
    if (g_handler) return;
    EventTypeSpec spec;
    spec.eventClass = kEventClassKeyboard;
    spec.eventKind  = kEventHotKeyPressed;
    InstallEventHandler(GetApplicationEventTarget(), macHotkeyHandler,
                        1, &spec, nullptr, &g_handler);
}

} // namespace

struct GlobalHotkey::Impl {
    uint32_t id = 0;
    EventHotKeyRef ref = nullptr;
};

GlobalHotkey::GlobalHotkey(QObject* parent) : QObject(parent) {
    d = new Impl();
    d->id = nextMacId();
    installMacHandlerIfNeeded();
}

GlobalHotkey::~GlobalHotkey() {
    unregisterHotkey();
    macRegistry().remove(d->id);
    delete d;
}

bool GlobalHotkey::registerHotkey(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    unregisterHotkey();

    // Decode the tagged key spaces (see Settings.h). macOS recordings store raw
    // Carbon codes, so this normally passes straight through; the branches matter
    // only for a settings file written by another platform's recorder.
    uint32_t keyCode = carbonKeyCode;
    if (HotKeyCode::isQtKey(carbonKeyCode)) {
        const int mapped = qtKeyToCarbon(static_cast<int>(HotKeyCode::payload(carbonKeyCode)));
        if (mapped < 0) { m_registered = false; return false; }
        keyCode = static_cast<uint32_t>(mapped);
    } else if (HotKeyCode::isWinVk(carbonKeyCode)) {
        m_registered = false;   // Windows VK: no meaning to Carbon
        return false;
    }

    macRegistry().insert(d->id, this);

    EventHotKeyID hotKeyID;
    hotKeyID.signature = kSignature;
    hotKeyID.id = d->id;

    // Pass-through: the persisted codes ARE Carbon codes/mods.
    OSStatus st = RegisterEventHotKey(keyCode, carbonModifiers, hotKeyID,
                                      GetApplicationEventTarget(), 0, &d->ref);
    if (st != noErr || d->ref == nullptr) {
        d->ref = nullptr;
        macRegistry().remove(d->id);
        m_registered = false;
        return false;
    }
    m_registered = true;
    return true;
}

bool GlobalHotkey::reregister(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    return registerHotkey(carbonKeyCode, carbonModifiers);
}

void GlobalHotkey::unregisterHotkey() {
    if (d && d->ref) {
        UnregisterEventHotKey(d->ref);
        d->ref = nullptr;
    }
    if (d) macRegistry().remove(d->id);
    m_registered = false;
}

// ===========================================================================
//  Fallback (Wayland / unsupported) — registration always fails, documented.
// ===========================================================================
#else

struct GlobalHotkey::Impl {};

GlobalHotkey::GlobalHotkey(QObject* parent) : QObject(parent) { d = new Impl(); }
GlobalHotkey::~GlobalHotkey() { delete d; }

bool GlobalHotkey::registerHotkey(uint32_t, uint32_t) {
    // No standard global-hotkey protocol (e.g. Wayland without the
    // org.freedesktop.portal.GlobalShortcuts portal). Surface the limitation.
    m_registered = false;
    return false;
}

bool GlobalHotkey::reregister(uint32_t carbonKeyCode, uint32_t carbonModifiers) {
    return registerHotkey(carbonKeyCode, carbonModifiers);
}

void GlobalHotkey::unregisterHotkey() { m_registered = false; }

#endif
