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
    default:   return 0;
    }
}

// Carbon modifier mask -> Win32 MOD_* mask. cmdKey -> MOD_CONTROL so that the
// persisted "⌘" combo maps to Ctrl on Windows (Qt::ControlModifier convention).
UINT carbonModsToWin(uint32_t mods) {
    UINT m = 0;
    if (mods & kCmd)  m |= MOD_CONTROL;
    if (mods & kCtrl) m |= MOD_CONTROL;
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

    UINT vk = carbonKeyToVk(carbonKeyCode);
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

Display* x11Display() {
    static Display* dpy = XOpenDisplay(nullptr);
    return dpy;
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
    default:   return NoSymbol;
    }
}

// Carbon modifier mask -> X11 base modifier mask (without lock variants).
// cmdKey -> ControlMask (Qt::ControlModifier convention on Linux).
unsigned int carbonModsToX11(uint32_t mods) {
    unsigned int m = 0;
    if (mods & kCmd)  m |= ControlMask;
    if (mods & kCtrl) m |= ControlMask;
    if (mods & kShft) m |= ShiftMask;
    if (mods & kOpt)  m |= Mod1Mask; // Alt
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

    KeySym sym = carbonKeyToKeysym(carbonKeyCode);
    if (sym == NoSymbol) { m_registered = false; return false; }
    KeyCode kc = XKeysymToKeycode(dpy, sym);
    if (kc == 0) { m_registered = false; return false; }

    unsigned int baseMods = carbonModsToX11(carbonModifiers);
    Window root = DefaultRootWindow(dpy);

    // Grab every NumLock/CapsLock lock-mask variant so the combo fires
    // regardless of lock state. XGrabKey returns void (errors arrive
    // asynchronously as BadAccess if another client owns the combo); reaching
    // a clean XSync is treated as success.
    for (unsigned int lock : kLockMasks) {
        XGrabKey(dpy, kc, baseMods | lock, root, /*owner_events*/ False,
                 GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);

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

    macRegistry().insert(d->id, this);

    EventHotKeyID hotKeyID;
    hotKeyID.signature = kSignature;
    hotKeyID.id = d->id;

    // Pass-through: the persisted codes ARE Carbon codes/mods.
    OSStatus st = RegisterEventHotKey(carbonKeyCode, carbonModifiers, hotKeyID,
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
