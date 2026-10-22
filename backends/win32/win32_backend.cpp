// anyui Win32 backend: native controls (common controls v6), Direct2D and
// DirectWrite for canvases and drawn icons, our own box/form layout (Win32
// has none), per-monitor DPI awareness.
#include "win32_common.h"

#include <commdlg.h>
#include <richedit.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace anyui::win32
{
namespace
{

constexpr UINT WM_ANYUI_POST = WM_APP + 1;

// GTK-style mnemonics ("_Save", "__" for a literal underscore) to Windows
// ones ("&Save"); a literal '&' doubles.
std::wstring mnemonic(const std::string& s)
{
    std::wstring w = wide(s), out;
    for (size_t i = 0; i < w.size(); ++i)
    {
        if (w[i] == L'&')
            out += L"&&";
        else if (w[i] == L'_' && i + 1 < w.size() && w[i + 1] == L'_')
        {
            out += L'_';
            ++i;
        }
        else if (w[i] == L'_')
            out += L'&';
        else
            out += w[i];
    }
    return out;
}
constexpr UINT WM_ANYUI_FRAME = WM_APP + 2;
constexpr UINT WM_ANYUI_RELAYOUT = WM_APP + 3;
constexpr UINT WM_ANYUI_SELECTION = WM_APP + 4;  // a list's coalesced selection change

HINSTANCE instance()
{
    return GetModuleHandleW(nullptr);
}

// The program's icon: its first icon resource, as Explorer shows it (the
// stock application icon when it has none), at the system size `metric`.
HICON appIcon(int metric)
{
    LPCWSTR name = nullptr;
    EnumResourceNamesW(
        instance(), RT_GROUP_ICON,
        [](HMODULE, LPCWSTR, LPWSTR n, LONG_PTR out) -> BOOL
        {
            *reinterpret_cast<LPCWSTR*>(out) = IS_INTRESOURCE(n) ? n : _wcsdup(n);
            return FALSE;
        },
        reinterpret_cast<LONG_PTR>(&name));
    const int px = GetSystemMetrics(metric);
    if (name)
        if (HICON icon = HICON(LoadImageW(instance(), name, IMAGE_ICON, px, px, LR_DEFAULTCOLOR))) return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

int64_t nowTicks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double nowSeconds()
{
    static LARGE_INTEGER freq = []
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return double(nowTicks()) / double(freq.QuadPart);
}

unsigned keyModifiers()
{
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= ModAlt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= ModMeta;
    return m;
}

KeyEvent keyEvent(WPARAM vk, LPARAM lp, bool press)
{
    KeyEvent e;
    e.press = press;
    e.repeat = press && (lp & (1 << 30));
    e.modifiers = keyModifiers();
    switch (vk)
    {
    case VK_ESCAPE: e.key = Key::Escape; break;
    case VK_RETURN: e.key = Key::Enter; break;
    case VK_TAB: e.key = Key::Tab; break;
    case VK_BACK: e.key = Key::Backspace; break;
    case VK_DELETE: e.key = Key::Delete; break;
    case VK_INSERT: e.key = Key::Insert; break;
    case VK_SPACE: e.key = Key::Space; break;
    case VK_LEFT: e.key = Key::Left; break;
    case VK_RIGHT: e.key = Key::Right; break;
    case VK_UP: e.key = Key::Up; break;
    case VK_DOWN: e.key = Key::Down; break;
    case VK_HOME: e.key = Key::Home; break;
    case VK_END: e.key = Key::End; break;
    case VK_PRIOR: e.key = Key::PageUp; break;
    case VK_NEXT: e.key = Key::PageDown; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F12)
            e.key = Key(int(Key::F1) + int(vk - VK_F1));
        else if (const UINT ch = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR) & 0x7fffffff; ch)
        {
            e.key = Key::Character;
            e.codepoint = char32_t(towlower(wchar_t(ch)));
        }
        else
            e.key = Key::Other;
    }
    return e;
}

// ---------------------------------------------------------------- comctl32 v6

// Without an application manifest the process binds comctl32 v5 at load,
// while our activation context makes the controls themselves v6. Image
// lists handed to v6 controls must come from v6 too (v5 ones have no alpha),
// so these functions are resolved from the v6 DLL once it's active.
struct Comctl6
{
    decltype(&ImageList_Create) create = ImageList_Create;
    decltype(&ImageList_Destroy) destroy = ImageList_Destroy;
    decltype(&ImageList_ReplaceIcon) replaceIcon = ImageList_ReplaceIcon;

    void resolve()
    {
        HMODULE m = LoadLibraryW(L"comctl32.dll");  // under the activation context: v6
        if (!m) return;
        if (auto f = reinterpret_cast<decltype(&ImageList_Create)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_Create")))) create = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_Destroy)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_Destroy")))) destroy = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_ReplaceIcon)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_ReplaceIcon")))) replaceIcon = f;
    }
};
Comctl6 g_comctl;

// ---------------------------------------------------------------- handlers

// Anything that receives notifications from its HWND via the parent window
// (WM_COMMAND, WM_NOTIFY, WM_HSCROLL, WM_DRAWITEM, WM_CTLCOLOR*).
class Handler
{
public:
    virtual ~Handler() = default;
    virtual void command(WORD /*code*/) {}
    virtual LRESULT notify(NMHDR*, bool& handled)
    {
        handled = false;
        return 0;
    }
    virtual void scroll(WORD /*code*/) {}
    virtual bool drawItem(DRAWITEMSTRUCT*) { return false; }
};

std::unordered_map<HWND, Handler*>& handlers()
{
    static std::unordered_map<HWND, Handler*> map;
    return map;
}

Handler* handlerOf(HWND h)
{
    auto& m = handlers();
    auto it = m.find(h);
    return it == m.end() ? nullptr : it->second;
}

class Win;  // forward

// Messages the containers forward to their children's handlers.
bool forwardChildMessage(HWND, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (lp)
            if (Handler* h = handlerOf(reinterpret_cast<HWND>(lp)))
            {
                h->command(HIWORD(wp));
                result = 0;
                return true;
            }
        return false;
    case WM_NOTIFY:
    {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (Handler* h = handlerOf(hdr->hwndFrom))
        {
            bool handled = true;
            result = h->notify(hdr, handled);
            return handled;
        }
        // List headers notify via the list's own window.
        if (Handler* h = handlerOf(GetParent(hdr->hwndFrom)))
        {
            bool handled = true;
            result = h->notify(hdr, handled);
            return handled;
        }
        return false;
    }
    case WM_HSCROLL:
    case WM_VSCROLL:
        if (lp)
            if (Handler* h = handlerOf(reinterpret_cast<HWND>(lp)))
            {
                h->scroll(LOWORD(wp));
                result = 0;
                return true;
            }
        return false;
    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (Handler* h = handlerOf(dis->hwndItem))
            if (h->drawItem(dis))
            {
                result = TRUE;
                return true;
            }
        return false;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        // Read-only multi-line text keeps the text background; statics and
        // checkboxes sit transparently on the window's.
        HWND c = reinterpret_cast<HWND>(lp);
        if (GetWindowLongPtrW(c, GWL_STYLE) & ES_MULTILINE)
        {
            wchar_t cls[16] = {};
            GetClassNameW(c, cls, 16);
            if (!_wcsicmp(cls, L"Edit"))
            {
                SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
                result = reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
                return true;
            }
        }
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        result = reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        return true;
    }
    }
    return false;
}

// A plain child window that hosts other controls (scroll viewports), passing
// their notifications on.
LRESULT CALLBACK hostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r = 0;
    if (forwardChildMessage(h, msg, wp, lp, r)) return r;
    if (auto* fn = reinterpret_cast<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>*>(
            GetWindowLongPtrW(h, GWLP_USERDATA)))
    {
        bool handled = false;
        r = (*fn)(h, msg, wp, lp, handled);
        if (handled) return r;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void registerClasses()
{
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.hInstance = instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpfnWndProc = hostProc;
    wc.lpszClassName = L"anyuiHost";
    RegisterClassExW(&wc);
}

HWND makeHost(HWND parent, DWORD style = 0)
{
    registerClasses();
    return CreateWindowExW(WS_EX_CONTROLPARENT, L"anyuiHost", L"", WS_CHILD | WS_CLIPCHILDREN | style, 0, 0, 0,
                           0, parent, nullptr, instance(), nullptr);
}

// ---------------------------------------------------------------- tooltips

void setTooltipText(HWND control, const std::string& text)
{
    static std::unordered_map<HWND, HWND> tips;  // top-level -> tooltip control
    HWND top = GetAncestor(control, GA_ROOT);
    HWND& tip = tips[top];
    if (!tip)
    {
        tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, top, nullptr, instance(),
                              nullptr);
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 480);
    }
    std::wstring w = wide(text);
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = top;
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = w.data();
    SendMessageW(tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    if (!text.empty()) SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

// ---------------------------------------------------------------- menus

// Appends `items` to `m`; command id `base + n` runs actions[n - 1].
void fillMenu(HMENU m, const Menu& items, std::vector<std::function<void()>>& actions, UINT base = 0)
{
    for (const MenuItem& it : items)
    {
        if (it.isSeparator())
        {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        std::wstring label = mnemonic(it.label);
        if (!it.shortcut.empty()) label += L"\t" + wide(it.shortcut);
        UINT flags = MF_STRING | (it.checked && *it.checked ? MF_CHECKED : 0);
        if (!it.children.empty())
        {
            HMENU sub = CreatePopupMenu();
            fillMenu(sub, it.children, actions, base);
            AppendMenuW(m, flags | (it.enabled ? 0 : MF_GRAYED) | MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                        label.c_str());
            continue;
        }
        actions.push_back(it.action);
        const UINT id = base + UINT(actions.size());
        AppendMenuW(m, flags | (it.enabled && it.action ? 0 : MF_GRAYED), id, label.c_str());
        if (it.radio)
        {
            MENUITEMINFOW mi{sizeof mi};
            mi.fMask = MIIM_FTYPE;
            mi.fType = MFT_STRING | MFT_RADIOCHECK;
            SetMenuItemInfoW(m, id, FALSE, &mi);
        }
    }
}

HMENU buildMenu(const Menu& items, std::vector<std::function<void()>>& actions)
{
    HMENU m = CreatePopupMenu();
    fillMenu(m, items, actions);
    return m;
}

void popupMenu(HWND owner, const Menu& items, POINT screen)
{
    if (items.empty()) return;
    std::vector<std::function<void()>> actions;
    HMENU m = buildMenu(items, actions);
    const UINT id = UINT(TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, owner, nullptr));
    DestroyMenu(m);
    if (id > 0 && id <= actions.size() && actions[id - 1]) actions[id - 1]();
}

// ---------------------------------------------------------------- window base

// What every widget needs from its window: scale, fonts, relayout.
class WinContext
{
public:
    virtual ~WinContext() = default;
    virtual HWND hwnd() const = 0;
    virtual void relayout() = 0;
    // Lays out and repaints now, in one go (a different tab page showing).
    virtual void relayoutAtOnce() = 0;
    float scale() const { return scaleOf(hwnd()); }
    UINT dpi() const { return dpiOf(hwnd()); }
    int px(float logical) const { return int(std::lround(logical * scale())); }
};

SIZE textExtent(HWND h, const std::wstring& s, bool multiline = false, int maxWidth = 0)
{
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    RECT r{0, 0, maxWidth > 0 ? maxWidth : 100000, 0};
    DrawTextW(dc, s.empty() ? L" " : s.c_str(), -1, &r,
              DT_CALCRECT | DT_NOPREFIX | (multiline ? DT_WORDBREAK : DT_SINGLELINE));
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return {r.right - r.left, r.bottom - r.top};
}

// Moving controls. A control already where it belongs isn't touched (each
// move costs an accessibility event, milliseconds apiece whenever an
// automation client listens); during a layout pass the rest are batched
// per parent and applied together.
struct MoveBatch
{
    std::map<HWND, std::vector<std::pair<HWND, RECT>>> moves;  // parent -> its children's moves
};
MoveBatch* g_moves = nullptr;

// A moved control repaints whole: Windows would otherwise carry its old
// pixels along and repaint only what's newly uncovered, and a label whose
// text changed as it moved shows a mix of old and new text.
constexpr UINT kMoveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;

bool placedAt(HWND h, int x, int y, int w, int hgt)
{
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(HWND_DESKTOP, GetParent(h), reinterpret_cast<POINT*>(&r), 2);
    return r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == hgt;
}

// Now, for containers that size their contents from their own client area.
void moveNow(HWND h, int x, int y, int w, int hgt)
{
    if (!placedAt(h, x, y, w, hgt)) SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

void moveControl(HWND h, int x, int y, int w, int hgt)
{
    if (placedAt(h, x, y, w, hgt)) return;
    if (g_moves)
        g_moves->moves[GetParent(h)].push_back({h, RECT{x, y, x + w, y + hgt}});
    else
        SetWindowPos(h, nullptr, x, y, w, hgt, kMoveFlags);
}

// Batches the moves of a layout pass (nested passes join the outer one).
class MovesInOnePass
{
public:
    MovesInOnePass()
    {
        if (!g_moves) g_moves = &batch_, owner_ = true;
    }
    ~MovesInOnePass()
    {
        if (!owner_) return;
        g_moves = nullptr;
        for (auto& [parent, list] : batch_.moves)
        {
            HDWP dwp = BeginDeferWindowPos(int(list.size()));
            for (auto& [h, r] : list)
                if (dwp)
                    dwp = DeferWindowPos(dwp, h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, kMoveFlags);
            if (dwp) EndDeferWindowPos(dwp);
        }
    }

private:
    MoveBatch batch_;
    bool owner_ = false;
};

// Shows or hides a control. While its window is still hidden (being built)
// that's just the style bit: ShowWindow raises an accessibility event per
// control, milliseconds apiece whenever an automation client listens.
void showControl(HWND h, bool shown)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (bool(style & WS_VISIBLE) == shown) return;
    if (IsWindowVisible(GetAncestor(h, GA_ROOT)))
        ShowWindow(h, shown ? SW_SHOWNA : SW_HIDE);
    else
        SetWindowLongPtrW(h, GWL_STYLE, shown ? style | WS_VISIBLE : style & ~WS_VISIBLE);
}

HWND createControl(WinContext& win, HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style,
                   DWORD exStyle = 0)
{
    HWND h = CreateWindowExW(exStyle, cls, text.c_str(), WS_CHILD | style, 0, 0, 0, 0, parent, nullptr,
                             instance(), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont(win.dpi())), FALSE);
    return h;
}

// A widget backed by one HWND, laid out as a leaf.
template <class Base>
class Leaf : public Base, public Item, public Handler
{
public:
    Leaf(WinContext& win)
        : win_(win)
    {
    }
    ~Leaf() override
    {
        if (h_)
        {
            handlers().erase(h_);
            DestroyWindow(h_);
        }
    }

    void setEnabled(bool e) override { EnableWindow(h_, e); }
    void setVisible(bool v) override
    {
        if (visible == v) return;
        visible = v;
        win_.relayout();
    }
    void setTooltip(const std::string& t) override { setTooltipText(h_, t); }
    void focus() override { SetFocus(h_); }

    void place(RECT r) override
    {
        moveControl(h_, r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    void showAll(bool shown) override { showControl(h_, shown && visible); }

    HWND hwnd() const { return h_; }

protected:
    void adopt(HWND h)
    {
        h_ = h;
        handlers()[h] = this;
    }
    int px(float v) const { return win_.px(v); }

    WinContext& win_;
    HWND h_ = nullptr;
};

// A container laid out as an Item but with no interface of its own; a
// handle on it gets this.
class PlainWidget : public Widget
{
public:
    PlainWidget(Item& item, WinContext& win)
        : item_(item)
        , win_(win)
    {
    }
    void setEnabled(bool) override {}
    void setVisible(bool v) override
    {
        if (item_.visible == v) return;
        item_.visible = v;
        win_.relayout();
    }
    void setTooltip(const std::string&) override {}
    void focus() override {}

private:
    Item& item_;
    WinContext& win_;
};

// ---------------------------------------------------------------- layout containers

class BoxItem : public Item
{
public:
    BoxItem(Orientation dir, int spacing, int margin = 0)
        : dir_(dir)
        , spacing_(spacing)
        , margin_(margin)
    {
    }
    std::vector<Item*> children;

    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }

    SIZE measure() override
    {
        long main = 0, cross = 0;
        int n = 0;
        for (Item* c : children)
        {
            if (!c->visible) continue;
            const SIZE s = sizeOf(c);
            main += along(s);
            cross = std::max(cross, across(s));
            ++n;
        }
        if (n > 1) main += spacing_ * (n - 1);
        main += 2 * margin_;
        cross += 2 * (margin_ / 2);
        return dir_ == Orientation::Horizontal ? SIZE{main, cross} : SIZE{cross, main};
    }

    void place(RECT r) override
    {
        if (dir_ == Orientation::Horizontal)
            InflateRect(&r, -margin_, -(margin_ / 2));
        else
            InflateRect(&r, -(margin_ / 2), -margin_);
        std::vector<Item*> shown;
        for (Item* c : children)
            if (c->visible) shown.push_back(c);
        if (shown.empty()) return;
        const long total = dir_ == Orientation::Horizontal ? r.right - r.left : r.bottom - r.top;
        const long crossTotal = dir_ == Orientation::Horizontal ? r.bottom - r.top : r.right - r.left;
        std::vector<long> sizes;
        long used = spacing_ * long(shown.size() - 1);
        int stretchSum = 0;
        for (Item* c : shown)
        {
            sizes.push_back(along(sizeOf(c)));
            used += sizes.back();
            stretchSum += c->stretch;
        }
        long spare = std::max(0L, total - used);
        if (stretchSum > 0)
        {
            long given = 0;
            int seen = 0;
            for (size_t i = 0; i < shown.size(); ++i)
            {
                if (!shown[i]->stretch) continue;
                seen += shown[i]->stretch;
                const long target = spare * seen / stretchSum;
                sizes[i] += target - given;
                given = target;
            }
        }
        else if (used > total)
        {
            // Too small: shrink the last item first (what overflows gets clipped).
            long over = used - total;
            for (size_t i = shown.size(); i-- > 0 && over > 0;)
            {
                const long cut = std::min(over, sizes[i]);
                sizes[i] -= cut;
                over -= cut;
            }
        }
        long pos = dir_ == Orientation::Horizontal ? r.left : r.top;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            Item* c = shown[i];
            const SIZE nat = sizeOf(c);
            long crossSize = across(nat), crossPos;
            const bool fill = c->fills() || (dir_ == Orientation::Vertical && c->expandsInForm());
            if (fill || crossSize > crossTotal) crossSize = crossTotal;
            // Single-line things sit centred across a row; in a column, left-aligned.
            if (dir_ == Orientation::Horizontal)
                crossPos = r.top + (crossTotal - crossSize) / 2;
            else
                crossPos = r.left;
            RECT cr = dir_ == Orientation::Horizontal ? RECT{pos, crossPos, pos + sizes[i], crossPos + crossSize}
                                                      : RECT{crossPos, pos, crossPos + crossSize, pos + sizes[i]};
            c->place(cr);
            pos += sizes[i] + spacing_;
        }
    }

    void showAll(bool shown) override
    {
        for (Item* c : children) c->showAll(shown && visible);
    }

private:
    SIZE sizeOf(Item* c)
    {
        SIZE s = c->natural();
        if (c->fixedW > 0) s.cx = c->fixedW;
        if (c->fixedH > 0) s.cy = c->fixedH;
        return s;
    }
    long along(SIZE s) const { return dir_ == Orientation::Horizontal ? s.cx : s.cy; }
    long across(SIZE s) const { return dir_ == Orientation::Horizontal ? s.cy : s.cx; }

    Orientation dir_;
    int spacing_;
    int margin_;
};

// Labels right-aligned in the first column, fields in the second; an empty
// label lets its field span both.
class FormItem : public Item
{
public:
    FormItem(int gap, int rowGap)
        : gap_(gap)
        , rowGap_(rowGap)
    {
    }
    struct Row
    {
        Item* label = nullptr;  // null: field spans both columns
        Item* field = nullptr;
    };
    std::vector<Row> rows;

    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }

    SIZE measure() override
    {
        long labelW = 0, fieldW = 0, spanW = 0, h = 0;
        int n = 0;
        for (Row& r : rows)
        {
            if (!r.field->visible) continue;
            const SIZE f = sizeOf(r.field);
            long rowH = f.cy;
            if (r.label)
            {
                const SIZE l = r.label->natural();
                labelW = std::max(labelW, l.cx);
                fieldW = std::max(fieldW, f.cx);
                rowH = std::max(rowH, l.cy);
            }
            else
                spanW = std::max(spanW, f.cx);
            h += rowH;
            ++n;
        }
        if (n > 1) h += rowGap_ * (n - 1);
        const long both = labelW ? labelW + gap_ + fieldW : fieldW;
        return {std::max(both, spanW), h};
    }

    void place(RECT r) override
    {
        long labelW = 0;
        for (Row& row : rows)
            if (row.label && row.field->visible) labelW = std::max(labelW, row.label->natural().cx);
        long y = r.top;
        for (Row& row : rows)
        {
            if (!row.field->visible)
            {
                if (row.label) row.label->place({0, 0, 0, 0});
                continue;
            }
            const SIZE f = sizeOf(row.field);
            const long fx = row.label ? r.left + labelW + gap_ : r.left;
            long rowH = f.cy;
            if (row.label) rowH = std::max(rowH, row.label->natural().cy);
            long fw = row.field->expandsInForm() || row.field->stretch ? r.right - fx : std::min(f.cx, r.right - fx);
            row.field->place({fx, y + (rowH - f.cy) / 2, fx + fw, y + (rowH - f.cy) / 2 + f.cy});
            if (row.label)
            {
                const SIZE l = row.label->natural();
                row.label->place({r.left + labelW - l.cx, y + (rowH - l.cy) / 2, r.left + labelW, y + (rowH - l.cy) / 2 + l.cy});
            }
            y += rowH + rowGap_;
        }
    }

    void showAll(bool shown) override
    {
        for (Row& r : rows)
        {
            r.field->showAll(shown && visible);
            if (r.label) r.label->showAll(shown && visible && r.field->visible);
        }
    }

private:
    SIZE sizeOf(Item* c)
    {
        SIZE s = c->natural();
        if (c->fixedW > 0) s.cx = c->fixedW;
        if (c->fixedH > 0) s.cy = c->fixedH;
        return s;
    }
    int gap_, rowGap_;
};

class SpacerItem : public Item
{
public:
    SpacerItem() { stretch = 1; }
    SIZE measure() override { return {0, 0}; }
    void place(RECT) override {}
    void showAll(bool) override {}
};

// ---------------------------------------------------------------- simple controls

class WLabel : public Leaf<anyui::Label>
{
public:
    WLabel(WinContext& win, HWND parent, const spec::Label& s)
        : Leaf(win)
        , ellipsize_(s.ellipsize)
    {
        if (s.selectable)
            adopt(createControl(win, parent, L"EDIT", wide(s.text), ES_READONLY | ES_AUTOHSCROLL));
        else
            adopt(createControl(win, parent, L"STATIC", wide(s.text),
                                SS_LEFT | SS_NOPREFIX | (s.ellipsize ? SS_ENDELLIPSIS : 0)));
    }
    void setText(const std::string& t) override
    {
        SetWindowTextW(h_, wide(t).c_str());
        if (!ellipsize_) win_.relayout();
    }
    SIZE measure() override
    {
        // Line breaks in the text are lines (measured single-line, the
        // label asked for one line's height and showed only the first).
        const std::wstring text = windowText(h_);
        SIZE s = textExtent(h_, text, text.find(L'\n') != std::wstring::npos);
        if (ellipsize_) s.cx = std::min<long>(s.cx, px(40));
        return s;
    }

private:
    bool ellipsize_;
};

class WButton : public Leaf<anyui::Button>
{
public:
    WButton(WinContext& win, HWND parent, const std::string& text, DWORD extra = 0)
        : Leaf(win)
    {
        adopt(createControl(win, parent, L"BUTTON", mnemonic(text), BS_PUSHBUTTON | WS_TABSTOP | extra));
    }
    ~WButton() override
    {
        if (images_) g_comctl.destroy(images_);
    }
    void setText(const std::string& t) override
    {
        SetWindowTextW(h_, mnemonic(t).c_str());
        win_.relayout();
    }
    void setIcon(const Icon& icon) override
    {
        icon_ = icon;
        if (images_) g_comctl.destroy(images_);
        images_ = nullptr;
        if (!icon) return;
        const int size = px(16);
        images_ = g_comctl.create(size, size, ILC_COLOR32, 1, 0);
        if (HICON hi = renderIconHandle(icon, size, win_.scale()))
        {
            g_comctl.replaceIcon(images_, -1, hi);
            DestroyIcon(hi);
        }
        BUTTON_IMAGELIST bil{images_, {px(2), 0, px(2), 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
        SendMessageW(h_, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
        win_.relayout();
    }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == BN_CLICKED && click_) click_();
    }
    SIZE measure() override
    {
        const SIZE t = textExtent(h_, windowText(h_));
        const bool hasText = GetWindowTextLengthW(h_) > 0;
        long w = (hasText ? t.cx : 0) + px(hasText ? 24 : 10) + (images_ ? px(hasText ? 20 : 16) : 0);
        if (hasText) w = std::max<long>(w, px(75));
        return {w, std::max<long>(px(23), t.cy + px(8))};
    }

    bool isDefault = false;
    DialogRole role = DialogRole::None;

private:
    std::function<void()> click_;
    Icon icon_;
    HIMAGELIST images_ = nullptr;
};

class WCheck : public Leaf<anyui::Check>
{
public:
    WCheck(WinContext& win, HWND parent, const spec::Check& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, L"BUTTON", mnemonic(s.text), BS_AUTO3STATE | WS_TABSTOP));
        setState(s.state);
    }
    bool checked() const override { return SendMessageW(h_, BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void setChecked(bool c) override { setState(c ? CheckState::Checked : CheckState::Unchecked); }
    CheckState state() const override
    {
        switch (SendMessageW(h_, BM_GETCHECK, 0, 0))
        {
        case BST_CHECKED: return CheckState::Checked;
        case BST_INDETERMINATE: return CheckState::Indeterminate;
        default: return CheckState::Unchecked;
        }
    }
    void setState(CheckState s) override
    {
        SendMessageW(h_, BM_SETCHECK,
                     s == CheckState::Checked         ? BST_CHECKED
                     : s == CheckState::Indeterminate ? BST_INDETERMINATE
                                                      : BST_UNCHECKED,
                     0);
    }
    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        // Clicks only toggle on/off: a mixed box goes to checked, never back to mixed.
        if (SendMessageW(h_, BM_GETCHECK, 0, 0) == BST_INDETERMINATE) SendMessageW(h_, BM_SETCHECK, BST_UNCHECKED, 0);
        if (toggled_) toggled_(checked());
    }
    SIZE measure() override
    {
        const SIZE t = textExtent(h_, windowText(h_));
        return {t.cx + px(GetWindowTextLengthW(h_) ? 22 : 14), std::max<long>(t.cy, px(17))};
    }

private:
    std::function<void(bool)> toggled_;
};

// Edit controls report Enter only to a subclass.
LRESULT CALLBACK enterSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN)
    {
        auto* fn = reinterpret_cast<std::function<void()>*>(data);
        if (fn && *fn)
        {
            (*fn)();
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == VK_RETURN)
    {
        auto* fn = reinterpret_cast<std::function<void()>*>(data);
        if (fn && *fn) return 0;  // no beep
    }
    return DefSubclassProc(h, msg, wp, lp);
}

class WEntry : public Leaf<anyui::Entry>
{
public:
    WEntry(WinContext& win, HWND parent, const spec::Entry& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, L"EDIT", wide(s.text), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE));
        setPlaceholder(s.placeholder);
        SetWindowSubclass(h_, enterSubclass, 1, reinterpret_cast<DWORD_PTR>(&activate_));
    }
    std::string text() const override { return narrow(windowText(h_)); }
    void setText(const std::string& t) override
    {
        quiet_ = true;
        SetWindowTextW(h_, wide(t).c_str());
        quiet_ = false;
    }
    void setPlaceholder(const std::string& p) override
    {
        SendMessageW(h_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(wide(p).c_str()));
    }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_CHANGE && !quiet_ && changed_) changed_(text());
    }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = fontMetrics(win_.dpi());
        return {m.cx * 20 + px(8), m.cy + px(9)};
    }

private:
    bool quiet_ = false;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class WSelect : public Leaf<anyui::Select>
{
public:
    WSelect(WinContext& win, HWND parent, const spec::Select& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP));
        current_ = s.current;
        setItems(s.items);
    }
    // Until it's opened or focused the box holds just the shown item: each
    // item added costs an accessibility event, milliseconds apiece whenever
    // an automation client listens, and a dialog has many of these.
    void setItems(std::vector<std::string> items) override
    {
        items_ = std::move(items);
        filled_ = false;
        showCurrentOnly();
        win_.relayout();
    }
    int current() const override { return filled_ ? int(SendMessageW(h_, CB_GETCURSEL, 0, 0)) : current_; }
    void setCurrent(int i) override
    {
        current_ = i;
        if (filled_)
            SendMessageW(h_, CB_SETCURSEL, WPARAM(i), 0);
        else
            showCurrentOnly();
    }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == CBN_DROPDOWN || code == CBN_SETFOCUS) fill();  // the keys choose from all of them too
        if (code == CBN_SELCHANGE)
        {
            current_ = current();
            if (changed_) changed_(current_);
        }
    }
    SIZE measure() override
    {
        long w = px(60);
        for (const std::string& i : items_) w = std::max(w, textExtent(h_, wide(i)).cx);
        return {w + px(32), fontMetrics(win_.dpi()).cy + px(10)};
    }

private:
    void showCurrentOnly()
    {
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        if (current_ >= 0 && size_t(current_) < items_.size())
        {
            SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(items_[size_t(current_)]).c_str()));
            SendMessageW(h_, CB_SETCURSEL, 0, 0);
        }
    }
    void fill()
    {
        if (filled_) return;
        filled_ = true;
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        for (const std::string& i : items_) SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(i).c_str()));
        SendMessageW(h_, CB_SETCURSEL, WPARAM(current_), 0);
    }

    std::vector<std::string> items_;
    int current_ = -1;
    bool filled_ = false;
    std::function<void(int)> changed_;
};

class WCombo : public Leaf<anyui::Combo>
{
public:
    WCombo(WinContext& win, HWND parent, const spec::Combo& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP));
        setItems(s.items);
        setText(s.text);
        COMBOBOXINFO info{};
        info.cbSize = sizeof info;
        if (GetComboBoxInfo(h_, &info) && info.hwndItem)
            SetWindowSubclass(info.hwndItem, enterSubclass, 1, reinterpret_cast<DWORD_PTR>(&activate_));
    }
    void setItems(std::vector<std::string> items) override
    {
        const std::wstring keep = windowText(h_);
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        for (const std::string& i : items) SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(i).c_str()));
        SetWindowTextW(h_, keep.c_str());
    }
    std::string text() const override { return narrow(windowText(h_)); }
    void setText(const std::string& t) override
    {
        quiet_ = true;
        SetWindowTextW(h_, wide(t).c_str());
        quiet_ = false;
    }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }
    void command(WORD code) override
    {
        if (quiet_ || !changed_) return;
        if (code == CBN_EDITCHANGE)
            changed_(text());
        else if (code == CBN_SELCHANGE)
        {
            const int i = int(SendMessageW(h_, CB_GETCURSEL, 0, 0));
            const int n = int(SendMessageW(h_, CB_GETLBTEXTLEN, WPARAM(i), 0));
            std::wstring w(size_t(std::max(n, 0)) + 1, L'\0');
            SendMessageW(h_, CB_GETLBTEXT, WPARAM(i), reinterpret_cast<LPARAM>(w.data()));
            w.resize(size_t(std::max(n, 0)));
            changed_(narrow(w));
        }
    }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = fontMetrics(win_.dpi());
        return {m.cx * 20 + px(32), m.cy + px(10)};
    }

private:
    bool quiet_ = false;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

// Multi-line text in a RichEdit control, so styled spans (diff colours)
// show. Offsets in the API are UTF-8 bytes; RichEdit counts UTF-16 units
// with each line break as one character.
class WTextView : public Leaf<anyui::TextView>
{
public:
    WTextView(WinContext& win, HWND parent, const spec::TextView& s)
        : Leaf(win)
        , mono_(s.monospace)
    {
        static HMODULE lib = LoadLibraryW(L"Msftedit.dll");
        (void)lib;
        adopt(createControl(win, parent, L"RICHEDIT50W", L"",
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP | ES_NOHIDESEL |
                                (s.wrap ? 0 : ES_AUTOHSCROLL | WS_HSCROLL) | (s.readOnly ? ES_READONLY : 0),
                            WS_EX_CLIENTEDGE));
        SendMessageW(h_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        // RichEdit holds 32,767 characters unless told otherwise, and cuts
        // longer text off silently (a big diff showed only its start).
        SendMessageW(h_, EM_EXLIMITTEXT, 0, 0x7ffffffe);
        SendMessageW(h_, EM_SETBKGNDCOLOR, 0, LPARAM(GetSysColor(COLOR_WINDOW)));
        if (mono_) SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont(win.dpi(), false, true)), FALSE);
        SetWindowSubclass(h_, wheelProc, 1, reinterpret_cast<DWORD_PTR>(this));
        // No error beep for typing into a read-only view (RichEdit allows
        // beeps by default); that it can't be edited is plain to see.
        SendMessageW(h_, EM_SETEDITSTYLE, 0, SES_ALLOWBEEPS);
        setText(s.text, {});
    }

    // RichEdit animates wheel scrolling: each notch glides, and quick
    // wheeling queues glides, so the view lags behind the wheel. Scroll by
    // whole lines at once instead, as many per notch as the system setting
    // says (a page when it says so); high-resolution wheels send fractions
    // of a notch, which add up.
    static LRESULT CALLBACK wheelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        if (msg != WM_MOUSEWHEEL || (GET_KEYSTATE_WPARAM(wp) & (MK_CONTROL | MK_SHIFT)))
            return DefSubclassProc(h, msg, wp, lp);  // Ctrl zooms, Shift scrolls sideways
        auto* self = reinterpret_cast<WTextView*>(data);
        UINT perNotch = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &perNotch, 0);
        if (perNotch == WHEEL_PAGESCROLL)
        {
            SendMessageW(h, WM_VSCROLL, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            return 0;
        }
        self->wheel_ += GET_WHEEL_DELTA_WPARAM(wp) * int(perNotch);
        const int lines = self->wheel_ / WHEEL_DELTA;
        self->wheel_ -= lines * WHEEL_DELTA;
        if (lines) SendMessageW(h, EM_LINESCROLL, 0, -lines);
        return 0;
    }
    std::string text() const override
    {
        std::wstring w = windowText(h_);
        for (wchar_t& c : w)
            if (c == L'\r') c = L'\n';
        return narrow(w);
    }
    void setText(const std::string& t, std::vector<Span> spans) override
    {
        quiet_ = true;
        if (spans.empty())
            SetWindowTextW(h_, wide(t).c_str());
        else
            streamRtf(rtf(t, std::move(spans)));
        SendMessageW(h_, EM_SETSEL, 0, 0);
        quiet_ = false;
    }
    void append(const std::string& t, const TextStyle& style) override
    {
        quiet_ = true;
        // Positions from RichEdit itself: GetWindowTextLength counts each
        // line break as CR LF, two to RichEdit's one, which put the style a
        // character late per line ("Success" with a black S).
        CHARRANGE end{-1, -1};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
        CHARRANGE at{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&at));
        SendMessageW(h_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(wide(t).c_str()));
        CHARRANGE after{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&after));
        format(at.cpMin, after.cpMax, style);
        quiet_ = false;
    }
    void setReadOnly(bool r) override { SendMessageW(h_, EM_SETREADONLY, r, 0); }
    std::pair<size_t, size_t> selection() const override
    {
        CHARRANGE cr{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        const std::string t = text();
        return {bytesAt(t, cr.cpMin), bytesAt(t, cr.cpMax)};
    }
    void select(size_t begin, size_t end) override
    {
        const std::vector<long> pos = units(text(), {begin, end});
        CHARRANGE cr{pos[0], pos[1]};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        SendMessageW(h_, EM_SCROLLCARET, 0, 0);
    }
    void onChanged(std::function<void()> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_CHANGE && !quiet_ && changed_) changed_();
    }
    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = fontMetrics(win_.dpi());
        return {m.cx * 40, m.cy * 6 + px(8)};
    }

private:
    // RichEdit positions count UTF-16 units, with CR LF as one.
    static int unitsOf(const std::string& t, size_t i)
    {
        const auto c = uint8_t(t[i]);
        if ((c & 0xc0) == 0x80) return 0;  // continuation byte
        if (c == '\n' && i > 0 && t[i - 1] == '\r') return 0;
        return c >= 0xf0 ? 2 : 1;  // four-byte sequences are surrogate pairs
    }
    // UTF-8 byte offsets -> RichEdit positions, in one pass.
    static std::vector<long> units(const std::string& t, const std::vector<size_t>& bytes)
    {
        std::vector<size_t> order(bytes.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return bytes[a] < bytes[b]; });
        std::vector<long> out(bytes.size());
        size_t at = 0;
        long n = 0;
        for (size_t i : order)
        {
            const size_t target = std::min(bytes[i], t.size());
            for (; at < target; ++at) n += unitsOf(t, at);
            out[i] = n;
        }
        return out;
    }
    // A RichEdit position -> the UTF-8 byte offset where it falls.
    static size_t bytesAt(const std::string& t, long pos)
    {
        long n = 0;
        size_t at = 0;
        for (; at < t.size() && n < pos; ++at) n += unitsOf(t, at);
        while (at < t.size() && (uint8_t(t[at]) & 0xc0) == 0x80) ++at;  // finish the character
        return at;
    }
    // Styled text goes in as one RTF stream. Formatting each span with
    // EM_SETCHARFORMAT made RichEdit lay out again per span: seconds for a
    // diff of a few thousand coloured lines.
    std::string rtf(const std::string& t, std::vector<Span> spans) const
    {
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
        // The control's own font, so styled text looks like plain text.
        LOGFONTW lf{};
        GetObjectW(uiFont(win_.dpi(), false, mono_), sizeof lf, &lf);
        const int halfPoints = lf.lfHeight ? int(std::lround(std::abs(lf.lfHeight) * 144.0 / win_.dpi())) : 18;
        std::vector<COLORREF> colours;
        auto colourIndex = [&](const Paint& p)
        {
            const COLORREF c = colorRef(std::holds_alternative<Color>(p) ? std::get<Color>(p) : themeColor(std::get<ThemeColor>(p)));
            auto it = std::find(colours.begin(), colours.end(), c);
            if (it == colours.end()) it = colours.insert(colours.end(), c);
            return int(it - colours.begin()) + 1;  // 0 is the default colour
        };
        std::string body;
        body.reserve(t.size() + t.size() / 4);
        auto text = [&](size_t from, size_t to)
        {
            for (size_t i = from; i < to && i < t.size();)
            {
                const auto c = uint8_t(t[i]);
                if (c < 0x80)
                {
                    ++i;
                    if (c == '\\' || c == '{' || c == '}') body += '\\', body += char(c);
                    else if (c == '\n') body += "\\par\n";
                    else if (c == '\t') body += "\\tab ";
                    else if (c != '\r') body += char(c);
                    continue;
                }
                // UTF-8 -> code point -> \uN with UTF-16 units (signed, as RTF has them).
                const int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
                char32_t cp = len == 1 ? 0xfffd : char32_t(c & (0x7f >> len));
                for (int k = 1; k < len && i + k < t.size(); ++k) cp = (cp << 6) | (uint8_t(t[i + k]) & 0x3f);
                i += len;
                auto unit = [&](int u) { body += "\\u" + std::to_string(int16_t(u)) + ' '; };
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    unit(0xd800 + int(cp >> 10));
                    unit(0xdc00 + int(cp & 0x3ff));
                }
                else
                    unit(int(cp));
            }
        };
        size_t at = 0;
        for (const Span& sp : spans)
        {
            const size_t begin = std::max(sp.begin, at), end = std::min(sp.end, t.size());
            if (end <= begin) continue;
            text(at, begin);
            body += '{';
            if (sp.style.fg) body += "\\cf" + std::to_string(colourIndex(*sp.style.fg));
            if (sp.style.bg) body += "\\highlight" + std::to_string(colourIndex(*sp.style.bg));
            if (sp.style.bold) body += "\\b";
            if (sp.style.italic) body += "\\i";
            if (sp.style.monospace && !mono_) body += "\\f1";
            body += ' ';
            text(begin, end);
            body += '}';
            at = end;
        }
        text(at, t.size());

        std::string out = "{\\rtf1\\ansi\\uc0{\\fonttbl{\\f0 " + narrow(lf.lfFaceName) + ";}{\\f1 Consolas;}}{\\colortbl;";
        for (COLORREF c : colours)
            out += "\\red" + std::to_string(GetRValue(c)) + "\\green" + std::to_string(GetGValue(c)) + "\\blue" +
                   std::to_string(GetBValue(c)) + ';';
        out += "}\\f0\\fs" + std::to_string(halfPoints) + ' ' + body + '}';
        return out;
    }
    void streamRtf(const std::string& rtf)
    {
        struct Reader
        {
            const std::string* data;
            size_t at = 0;
        } reader{&rtf};
        EDITSTREAM es{};
        es.dwCookie = reinterpret_cast<DWORD_PTR>(&reader);
        es.pfnCallback = [](DWORD_PTR cookie, LPBYTE buf, LONG size, LONG* got) -> DWORD
        {
            auto* r = reinterpret_cast<Reader*>(cookie);
            const size_t n = std::min<size_t>(size_t(size), r->data->size() - r->at);
            std::memcpy(buf, r->data->data() + r->at, n);
            r->at += n;
            *got = LONG(n);
            return 0;
        };
        SendMessageW(h_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
    }

    void format(long a, long b, const TextStyle& st)
    {
        if (b <= a) return;
        CHARRANGE cr{a, b};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_BOLD | CFM_ITALIC;
        cf.dwEffects = (st.bold ? CFE_BOLD : 0) | (st.italic ? CFE_ITALIC : 0);
        auto colour = [](const Paint& p) { return std::holds_alternative<Color>(p) ? std::get<Color>(p) : themeColor(std::get<ThemeColor>(p)); };
        if (st.fg)
        {
            cf.dwMask |= CFM_COLOR;
            cf.crTextColor = colorRef(colour(*st.fg));
        }
        if (st.bg)
        {
            cf.dwMask |= CFM_BACKCOLOR;
            cf.crBackColor = colorRef(colour(*st.bg));
        }
        if (st.monospace && !mono_)
        {
            cf.dwMask |= CFM_FACE;
            wcscpy(cf.szFaceName, L"Consolas");
        }
        SendMessageW(h_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }

    int wheel_ = 0;  // wheel delta × lines per notch not yet scrolled
    bool mono_;
    bool quiet_ = false;
    std::function<void()> changed_;
};

class SeparatorItem : public Item
{
public:
    SeparatorItem(WinContext& win, HWND parent, Orientation lineDir)
        : win_(win)
        , dir_(lineDir)
    {
        h_ = createControl(win, parent, L"STATIC", L"",
                           lineDir == Orientation::Horizontal ? SS_ETCHEDHORZ : SS_ETCHEDVERT);
    }
    ~SeparatorItem() override { DestroyWindow(h_); }
    SIZE measure() override
    {
        const int t = std::max(2, win_.px(2)), pad = win_.px(4);
        return dir_ == Orientation::Horizontal ? SIZE{pad, t + 2 * pad} : SIZE{t + 2 * pad, pad};
    }
    bool fills() const override { return true; }
    void place(RECT r) override
    {
        const int pad = win_.px(4);
        if (dir_ == Orientation::Horizontal)
            moveControl(h_, r.left, (r.top + r.bottom) / 2, r.right - r.left, 2);
        else
            moveControl(h_, (r.left + r.right) / 2, r.top + pad, 2, r.bottom - r.top - 2 * pad);
    }
    void showAll(bool shown) override { showControl(h_, shown && visible); }

private:
    WinContext& win_;
    Orientation dir_;
    HWND h_;
};

// A titled frame around one item.
class GroupItem : public Item
{
public:
    GroupItem(WinContext& win, HWND parent, const std::string& title)
        : win_(win)
    {
        // Created first so the controls inside sit above it in z-order.
        h_ = createControl(win, parent, L"BUTTON", wide(title), BS_GROUPBOX, WS_EX_TRANSPARENT);
    }
    ~GroupItem() override { DestroyWindow(h_); }
    Item* content = nullptr;
    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        const SIZE c = content ? content->natural() : SIZE{0, 0};
        const SIZE t = textExtent(h_, windowText(h_));
        return {std::max(c.cx, t.cx + win_.px(16)) + 2 * pad(), c.cy + top() + pad()};
    }
    void place(RECT r) override
    {
        moveControl(h_, r.left, r.top, r.right - r.left, r.bottom - r.top);
        if (content) content->place({r.left + pad(), r.top + top(), r.right - pad(), r.bottom - pad()});
    }
    void showAll(bool shown) override
    {
        showControl(h_, shown && visible);
        if (content) content->showAll(shown && visible);
    }

private:
    int pad() const { return win_.px(9); }
    int top() const { return fontMetrics(win_.dpi()).cy + win_.px(4); }
    WinContext& win_;
    HWND h_;
};

class WProgressBar : public Leaf<anyui::ProgressBar>
{
public:
    WProgressBar(WinContext& win, HWND parent)
        : Leaf(win)
    {
        adopt(createControl(win, parent, PROGRESS_CLASSW, L"", 0));
        SendMessageW(h_, PBM_SETRANGE32, 0, 1000);
    }
    void setFraction(float f) override
    {
        if (marquee_)
        {
            SetWindowLongPtrW(h_, GWL_STYLE, GetWindowLongPtrW(h_, GWL_STYLE) & ~PBS_MARQUEE);
            SendMessageW(h_, PBM_SETMARQUEE, FALSE, 0);
            marquee_ = false;
        }
        // The themed bar animates every increase, so an instant operation's
        // 100% still crawled across. A decrease shows at once: go one past
        // the value, then back (at 100%, the range grows by one meanwhile).
        const int pos = int(std::lround(std::clamp(f, 0.f, 1.f) * 1000));
        if (pos == 1000) SendMessageW(h_, PBM_SETRANGE32, 0, 1001);
        SendMessageW(h_, PBM_SETPOS, WPARAM(pos + 1), 0);
        SendMessageW(h_, PBM_SETPOS, WPARAM(pos), 0);
        if (pos == 1000) SendMessageW(h_, PBM_SETRANGE32, 0, 1000);
    }
    void pulse() override
    {
        if (marquee_) return;
        SetWindowLongPtrW(h_, GWL_STYLE, GetWindowLongPtrW(h_, GWL_STYLE) | PBS_MARQUEE);
        SendMessageW(h_, PBM_SETMARQUEE, TRUE, 30);
        marquee_ = true;
    }
    SIZE measure() override { return {px(160), px(16)}; }

private:
    bool marquee_ = false;
};

class WDatePicker : public Leaf<anyui::DatePicker>
{
public:
    WDatePicker(WinContext& win, HWND parent, const spec::DatePicker& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT | WS_TABSTOP));
        setDate(s.date);
    }
    Date date() const override
    {
        SYSTEMTIME st{};
        SendMessageW(h_, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&st));
        return Date{st.wYear, st.wMonth, st.wDay};
    }
    void setDate(Date d) override
    {
        SYSTEMTIME st{};
        st.wYear = WORD(d.year);
        st.wMonth = WORD(d.month);
        st.wDay = WORD(d.day);
        quiet_ = true;
        SendMessageW(h_, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&st));
        quiet_ = false;
    }
    void onChanged(std::function<void(Date)> f) override { changed_ = std::move(f); }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == DTN_DATETIMECHANGE;
        if (handled && !quiet_ && changed_) changed_(date());
        return 0;
    }
    SIZE measure() override { return {fontMetrics(win_.dpi()).cx * 12 + px(30), fontMetrics(win_.dpi()).cy + px(10)}; }

private:
    bool quiet_ = false;
    std::function<void(Date)> changed_;
};

// A number in an edit with an up/down control beside it (the edit is the
// HWND laid out; the arrows ride on its right edge).
class WSpin : public Leaf<anyui::Spin>
{
public:
    WSpin(WinContext& win, HWND parent, const spec::Spin& s)
        : Leaf(win)
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : 1)
        , digits_(std::max(0, s.digits))
        , value_(s.value)
    {
        adopt(createControl(win, parent, L"EDIT", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE));
        updown_ = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | UDS_ARROWKEYS | UDS_ALIGNRIGHT, 0, 0, 0, 0, parent,
                                  nullptr, instance(), nullptr);
        SendMessageW(updown_, UDM_SETBUDDY, reinterpret_cast<WPARAM>(h_), 0);
        SendMessageW(updown_, UDM_SETRANGE32, 0, 100);  // we step ourselves
        SendMessageW(updown_, UDM_SETPOS32, 0, 50);
        handlers()[updown_] = this;
        show();
    }
    ~WSpin() override
    {
        handlers().erase(updown_);
        DestroyWindow(updown_);
    }
    double value() const override { return value_; }
    void setValue(double v) override
    {
        value_ = std::clamp(v, min_, max_);
        show();
    }
    void setRange(double mn, double mx) override
    {
        min_ = mn;
        max_ = mx;
        setValue(value_);
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_KILLFOCUS) commit();
    }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == UDN_DELTAPOS;
        if (!handled) return 0;
        auto* d = reinterpret_cast<NMUPDOWN*>(n);
        commit();
        set(value_ + (d->iDelta > 0 ? step_ : -step_));
        return 1;  // we moved the value; keep the arrows' own position
    }
    void place(RECT r) override
    {
        const int arrows = px(18);
        Leaf::place({r.left, r.top, r.right - arrows, r.bottom});
        moveControl(updown_, r.right - arrows, r.top, arrows, r.bottom - r.top);
    }
    void showAll(bool shown) override
    {
        Leaf::showAll(shown);
        showControl(updown_, shown && visible);
    }
    void setEnabled(bool e) override
    {
        Leaf::setEnabled(e);
        EnableWindow(updown_, e);
    }
    SIZE measure() override
    {
        const SIZE m = fontMetrics(win_.dpi());
        return {m.cx * 8 + px(26), m.cy + px(9)};
    }

private:
    void show()
    {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.*f", digits_, value_);
        quiet_ = true;
        SetWindowTextW(h_, buf);
        quiet_ = false;
    }
    void set(double v)
    {
        v = std::clamp(v, min_, max_);
        const bool changed = v != value_;
        value_ = v;
        show();
        if (changed && changed_) changed_(value_);
    }
    void commit()
    {
        const std::wstring t = windowText(h_);
        wchar_t* end = nullptr;
        const double v = wcstod(t.c_str(), &end);
        if (end != t.c_str()) set(v);
        else show();
    }

    HWND updown_ = nullptr;
    double min_, max_, step_;
    int digits_;
    double value_;
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

// A trackbar works in integers: positions are value / step.
class WSlider : public Leaf<anyui::Slider>
{
public:
    WSlider(WinContext& win, HWND parent, const spec::Slider& s)
        : Leaf(win)
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : (s.max - s.min) / 1000)
        , showValue_(s.showValue)
        , digits_(s.digits)
    {
        adopt(createControl(win, parent, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP));
        marks_ = s.marks;
        applyRange();
        SendMessageW(h_, TBM_SETLINESIZE, 0, 1);
        SendMessageW(h_, TBM_SETPAGESIZE, 0, LPARAM(std::max(1.0, std::round(s.page / step_))));
        setValue(s.value);
        if (showValue_) value_label_ = createControl(win, parent, L"STATIC", L"", SS_RIGHT);
        updateLabel();
    }
    ~WSlider() override
    {
        if (value_label_) DestroyWindow(value_label_);
    }
    double value() const override { return min_ + double(SendMessageW(h_, TBM_GETPOS, 0, 0)) * step_; }
    void setValue(double v) override
    {
        SendMessageW(h_, TBM_SETPOS, TRUE, LPARAM(std::lround((std::clamp(v, min_, max_) - min_) / step_)));
        updateLabel();
    }
    void setRange(double mn, double mx) override
    {
        const double v = value();
        min_ = mn;
        max_ = mx;
        applyRange();
        setValue(v);
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }
    void scroll(WORD) override
    {
        updateLabel();
        if (changed_) changed_(value());
    }
    void place(RECT r) override
    {
        const int label = value_label_ ? labelWidth() : 0;
        Leaf::place({r.left, r.top, r.right - label, r.bottom});
        if (value_label_)
            moveControl(value_label_, r.right - label, r.top + (r.bottom - r.top - fontMetrics(win_.dpi()).cy) / 2, label, fontMetrics(win_.dpi()).cy);
    }
    void showAll(bool shown) override
    {
        Leaf::showAll(shown);
        if (value_label_) showControl(value_label_, shown && visible);
    }
    SIZE measure() override { return {px(140) + (value_label_ ? labelWidth() : 0), px(marks_.empty() ? 26 : 30)}; }

private:
    int labelWidth() const { return fontMetrics(win_.dpi()).cx * (digits_ + 5); }
    void applyRange()
    {
        const LPARAM n = LPARAM(std::lround((max_ - min_) / step_));
        SendMessageW(h_, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(h_, TBM_SETRANGEMAX, TRUE, n);
        SendMessageW(h_, TBM_CLEARTICS, FALSE, 0);
        if (!marks_.empty())
        {
            SetWindowLongPtrW(h_, GWL_STYLE, (GetWindowLongPtrW(h_, GWL_STYLE) & ~TBS_NOTICKS) | TBS_BOTTOM);
            for (auto& [at, label] : marks_)
                SendMessageW(h_, TBM_SETTIC, 0, LPARAM(std::lround((at - min_) / step_)));
        }
    }
    void updateLabel()
    {
        if (!value_label_) return;
        wchar_t buf[64];
        swprintf(buf, 64, L"%.*f", digits_, value());
        SetWindowTextW(value_label_, buf);
    }

    double min_, max_, step_;
    bool showValue_;
    int digits_;
    std::vector<std::pair<double, std::string>> marks_;
    HWND value_label_ = nullptr;
    std::function<void(double)> changed_;
};

// An owner-drawn button showing a swatch; clicking opens ChooseColor.
class WColorButton : public Leaf<anyui::ColorButton>
{
public:
    WColorButton(WinContext& win, HWND parent, const spec::ColorButton& s)
        : Leaf(win)
        , color_(s.color)
    {
        adopt(createControl(win, parent, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP));
    }
    Color color() const override { return color_; }
    void setColor(Color c) override
    {
        color_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    void onChanged(std::function<void(Color)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        static COLORREF custom[16];
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = GetAncestor(h_, GA_ROOT);
        cc.rgbResult = colorRef(color_);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (!ChooseColorW(&cc)) return;
        color_ = Color{GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult), color_.a};
        InvalidateRect(h_, nullptr, TRUE);
        if (changed_) changed_(color_);
    }
    bool drawItem(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        DrawFrameControl(d->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((d->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        InflateRect(&r, -px(5), -px(5));
        HBRUSH b = CreateSolidBrush(colorRef(color_));
        FillRect(d->hDC, &r, b);
        DeleteObject(b);
        FrameRect(d->hDC, &r, GetSysColorBrush(COLOR_BTNSHADOW));
        if (d->itemState & ODS_FOCUS)
        {
            InflateRect(&r, px(2), px(2));
            DrawFocusRect(d->hDC, &r);
        }
        return true;
    }
    SIZE measure() override { return {px(44), px(24)}; }

private:
    Color color_;
    std::function<void(Color)> changed_;
};

class WMenuButton : public Leaf<anyui::MenuButton>
{
public:
    WMenuButton(WinContext& win, HWND parent, const spec::MenuButton& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, L"BUTTON", mnemonic(s.text) + L"  ▾", BS_PUSHBUTTON | WS_TABSTOP));
        text_ = s.text;
        if (s.icon) setIcon(s.icon);
        setPushed(s.pushed);
    }
    ~WMenuButton() override
    {
        if (images_) g_comctl.destroy(images_);
    }
    void setText(const std::string& t) override
    {
        text_ = t;
        SetWindowTextW(h_, (mnemonic(t) + L"  ▾").c_str());
        win_.relayout();
    }
    void setIcon(const Icon& icon) override
    {
        if (images_) g_comctl.destroy(images_);
        images_ = nullptr;
        if (!icon) return;
        const int size = px(16);
        images_ = g_comctl.create(size, size, ILC_COLOR32, 1, 0);
        if (HICON hi = renderIconHandle(icon, size, win_.scale()))
        {
            g_comctl.replaceIcon(images_, -1, hi);
            DestroyIcon(hi);
        }
        BUTTON_IMAGELIST bil{images_, {px(2), 0, px(2), 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
        SendMessageW(h_, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
    }
    void setPushed(bool p) override
    {
        pushed_ = p;
        SendMessageW(h_, BM_SETSTATE, p, 0);
    }
    void onMenu(std::function<Menu()> f) override { menu_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED || !menu_) return;
        RECT r;
        GetWindowRect(h_, &r);
        SendMessageW(h_, BM_SETSTATE, TRUE, 0);
        popupMenu(GetAncestor(h_, GA_ROOT), menu_(), POINT{r.left, r.bottom});
        SendMessageW(h_, BM_SETSTATE, pushed_, 0);
    }
    SIZE measure() override
    {
        const SIZE t = textExtent(h_, windowText(h_));
        return {t.cx + px(24) + (images_ ? px(20) : 0), std::max<long>(px(23), t.cy + px(8))};
    }

private:
    std::string text_;
    bool pushed_ = false;
    std::function<Menu()> menu_;
    HIMAGELIST images_ = nullptr;
};

// Flat, owner-drawn toolbar button: icon only, hover and pressed/checked
// backgrounds, tooltip with the label.
class WToolButton : public Leaf<anyui::ToolButton>
{
public:
    WToolButton(WinContext& win, HWND parent, const spec::ToolButton& s)
        : Leaf(win)
        , icon_(s.icon)
        , toggle_(s.toggle)
        , checked_(s.checked)
    {
        adopt(createControl(win, parent, L"BUTTON", wide(s.label), BS_OWNERDRAW));
        setTooltipText(h_, s.label);
        SetWindowSubclass(h_, hoverProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    void setIcon(const Icon& icon) override
    {
        icon_ = icon;
        InvalidateRect(h_, nullptr, TRUE);
    }
    bool checked() const override { return toggle_ && checked_; }
    void setChecked(bool c) override
    {
        if (!toggle_) return;
        checked_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }
    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        if (toggle_)
        {
            checked_ = !checked_;
            InvalidateRect(h_, nullptr, TRUE);
            if (toggled_) toggled_(checked_);
        }
        if (click_) click_();
    }
    bool drawItem(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        FillRect(d->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
        const bool pressed = d->itemState & ODS_SELECTED;
        const bool disabled = d->itemState & ODS_DISABLED;
        if (pressed || checked_ || hot_)
        {
            const Color base = themeColor(ThemeColor::Highlight);
            const float a = pressed ? 0.35f : checked_ ? 0.25f : 0.12f;
            const Color face = themeColor(ThemeColor::Window);
            auto mix = [a](uint8_t x, uint8_t y) { return uint8_t(x * (1 - a) + y * a); };
            HBRUSH b = CreateSolidBrush(RGB(mix(face.r, base.r), mix(face.g, base.g), mix(face.b, base.b)));
            RECT in = r;
            InflateRect(&in, -px(1), -px(1));
            FillRect(d->hDC, &in, b);
            DeleteObject(b);
        }
        const int icon = px(16);
        RECT ir{r.left + (r.right - r.left - icon) / 2, r.top + (r.bottom - r.top - icon) / 2, 0, 0};
        ir.right = ir.left + icon;
        ir.bottom = ir.top + icon;
        drawIconOnDC(d->hDC, ir, icon_, win_.scale(),
                     disabled ? themeColor(ThemeColor::PlaceholderText) : themeColor(ThemeColor::WindowText));
        return true;
    }
    SIZE measure() override { return {px(28), px(28)}; }

private:
    static LRESULT CALLBACK hoverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WToolButton*>(data);
        if (msg == WM_MOUSEMOVE && !self->hot_)
        {
            self->hot_ = true;
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        else if (msg == WM_MOUSELEAVE)
        {
            self->hot_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    Icon icon_;
    bool toggle_, checked_;
    bool hot_ = false;
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
};

// ---------------------------------------------------------------- list

class WList;
WList* g_dragging = nullptr;  // the list with a row drag in progress, for Esc

// A virtual report-style ListView: the model is asked for visible cells only.
class WList : public Leaf<anyui::List>, public ListObserver
{
public:
    WList(WinContext& win, HWND parent, const spec::List& s)
        : Leaf(win)
        , columns_(s.columns)
    {
        DWORD style = LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP;
        if (s.selection == SelectionMode::Single || s.selection == SelectionMode::None) style |= LVS_SINGLESEL;
        if (!s.headers) style |= LVS_NOCOLUMNHEADER;
        adopt(createControl(win, parent, WC_LISTVIEWW, L"", style, WS_EX_CLIENTEDGE));
        ListView_SetExtendedListViewStyle(h_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        visible_.assign(columns_.size(), true);
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            const Column& c = columns_[i];
            std::wstring title = wide(c.title);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.pszText = title.data();
            col.fmt = c.align == Align::End ? LVCFMT_RIGHT : c.align == Align::Center ? LVCFMT_CENTER : LVCFMT_LEFT;
            int w = extentPx(win.hwnd(), c.width, false);
            col.cx = w > 0 ? w : px(100);
            widths_.push_back(col.cx);
            ListView_InsertColumn(h_, int(i), &col);
        }
        setModel(s.model);
        soft_ = s.softSelection;
        if (soft_) SetWindowSubclass(h_, hotProc, 3, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(h_, selectionProc, 4, reinterpret_cast<DWORD_PTR>(this));
    }
    ~WList() override
    {
        if (g_dragging == this) g_dragging = nullptr;
        if (model_) model_->removeObserver(this);
    }

    void setModel(ListModel* m) override
    {
        if (model_) model_->removeObserver(this);
        model_ = m;
        if (model_) model_->addObserver(this);
        modelReset();
    }
    std::vector<size_t> selection() const override
    {
        std::vector<size_t> out;
        for (int i = ListView_GetNextItem(h_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(h_, i, LVNI_SELECTED))
            out.push_back(size_t(i));
        return out;
    }
    std::optional<size_t> current() const override
    {
        const int i = ListView_GetNextItem(h_, -1, LVNI_FOCUSED);
        return i < 0 ? std::nullopt : std::optional<size_t>(size_t(i));
    }
    void select(const std::vector<size_t>& rows) override
    {
        quiet_ = true;
        ListView_SetItemState(h_, -1, 0, LVIS_SELECTED);
        for (size_t r : rows) ListView_SetItemState(h_, int(r), LVIS_SELECTED, LVIS_SELECTED);
        quiet_ = false;
    }
    void setCurrent(size_t row) override { ListView_SetItemState(h_, int(row), LVIS_FOCUSED, LVIS_FOCUSED); }
    void scrollTo(size_t row) override { ListView_EnsureVisible(h_, int(row), FALSE); }
    void setSortIndicator(std::optional<size_t> column, SortOrder order) override
    {
        HWND header = ListView_GetHeader(h_);
        for (int i = 0; i < int(columns_.size()); ++i)
        {
            HDITEMW hd{};
            hd.mask = HDI_FORMAT;
            Header_GetItem(header, i, &hd);
            hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (column && size_t(i) == *column) hd.fmt |= order == SortOrder::Ascending ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(header, i, &hd);
        }
    }
    std::vector<float> columnWidths() const override
    {
        std::vector<float> out;
        for (size_t i = 0; i < columns_.size(); ++i)
            out.push_back((visible_[i] ? ListView_GetColumnWidth(h_, int(i)) : widths_[i]) / win_.scale());
        return out;
    }
    void setColumnWidths(const std::vector<float>& w) override
    {
        for (size_t i = 0; i < w.size() && i < columns_.size(); ++i)
        {
            widths_[i] = px(w[i]);
            if (visible_[i]) ListView_SetColumnWidth(h_, int(i), widths_[i]);
        }
        fitted_ = true;  // restored widths win over the first fit
    }
    bool columnVisible(size_t c) const override { return c < visible_.size() && visible_[c]; }
    void setColumnVisible(size_t c, bool v) override
    {
        if (c >= visible_.size() || visible_[c] == v) return;
        if (!v) widths_[c] = ListView_GetColumnWidth(h_, int(c));
        visible_[c] = v;
        ListView_SetColumnWidth(h_, int(c), v ? widths_[c] : 0);
    }
    void onSelectionChanged(std::function<void()> f) override { selChanged_ = std::move(f); }
    void onActivate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void onSortRequested(std::function<void(size_t, SortOrder)> f) override { sort_ = std::move(f); }
    void onContextMenu(std::function<Menu(std::optional<size_t>)> f) override { context_ = std::move(f); }
    void onHeaderMenu(std::function<Menu(size_t)> f) override { headerMenu_ = std::move(f); }
    void onReorder(ReorderFn f) override
    {
        reorder_ = std::move(f);
        reorderable_ = bool(reorder_);
    }
    void setReorderable(bool on) override { reorderable_ = on && reorder_; }

    void modelReset() override
    {
        ListView_SetItemCountEx(h_, model_ ? int(model_->rowCount()) : 0, 0);
        InvalidateRect(h_, nullptr, TRUE);
    }
    void rowsChanged(size_t first, size_t count) override
    {
        ListView_RedrawItems(h_, int(first), int(first + count - 1));
    }
    void rowsInserted(size_t, size_t) override { modelReset(); }
    void rowsRemoved(size_t, size_t) override { modelReset(); }

    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = true;
        switch (n->code)
        {
        case LVN_GETDISPINFOW:
        {
            auto* d = reinterpret_cast<NMLVDISPINFOW*>(n);
            if ((d->item.mask & LVIF_TEXT) && model_ && size_t(d->item.iItem) < model_->rowCount())
            {
                const std::size_t col = size_t(d->item.iSubItem);
                text_ = col < columns_.size() && columns_[col].painted
                            ? std::wstring()
                            : wide(model_->text(size_t(d->item.iItem), col));
                d->item.pszText = text_.data();
            }
            return 0;
        }
        case LVN_ITEMCHANGED:
        {
            auto* v = reinterpret_cast<NMLISTVIEW*>(n);
            if (!quiet_ && (v->uChanged & LVIF_STATE) && ((v->uOldState ^ v->uNewState) & LVIS_SELECTED))
                selectionChanged();
            return 0;
        }
        case LVN_ODSTATECHANGED:
            if (!quiet_) selectionChanged();
            return 0;
        case LVN_ITEMACTIVATE:
        {
            auto* a = reinterpret_cast<NMITEMACTIVATE*>(n);
            if (activate_ && a->iItem >= 0) activate_(size_t(a->iItem));
            return 0;
        }
        case LVN_COLUMNCLICK:
        {
            auto* v = reinterpret_cast<NMLISTVIEW*>(n);
            if (sort_ && v->iSubItem >= 0 && columns_[size_t(v->iSubItem)].sortable)
            {
                const size_t c = size_t(v->iSubItem);
                const SortOrder o = sortColumn_ == int(c) && sortOrder_ == SortOrder::Ascending ? SortOrder::Descending
                                                                                               : SortOrder::Ascending;
                sortColumn_ = int(c);
                sortOrder_ = o;
                sort_(c, o);
            }
            return 0;
        }
        case NM_RCLICK:
            if (n->hwndFrom == ListView_GetHeader(h_))
            {
                if (!headerMenu_) return 0;
                POINT pt;
                GetCursorPos(&pt);
                POINT local = pt;
                ScreenToClient(n->hwndFrom, &local);
                HDHITTESTINFO hit{};
                hit.pt = local;
                SendMessageW(n->hwndFrom, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));
                if (hit.iItem >= 0) popupMenu(GetAncestor(h_, GA_ROOT), headerMenu_(size_t(hit.iItem)), pt);
                return 1;
            }
            if (context_)
            {
                auto* a = reinterpret_cast<NMITEMACTIVATE*>(n);
                POINT pt;
                GetCursorPos(&pt);
                popupMenu(GetAncestor(h_, GA_ROOT),
                          context_(a->iItem >= 0 ? std::optional<size_t>(size_t(a->iItem)) : std::nullopt), pt);
            }
            return 0;
        case NM_CUSTOMDRAW:
            return customDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(n));
        case LVN_BEGINDRAG:
            if (reorderable_ && reorder_)
            {
                dragging_ = true;
                g_dragging = this;
                SetCapture(h_);
                SetWindowSubclass(h_, dragProc, 2, reinterpret_cast<DWORD_PTR>(this));
            }
            return 0;
        }
        handled = false;
        return 0;
    }

    void place(RECT r) override
    {
        Leaf::place(r);
        fitStretch();
    }
    // Stretch columns share the width the others leave, once, when the list
    // first has a size. After that only the user changes column widths:
    // resizing one never resizes the others, and a list wider than its
    // window scrolls sideways.
    void fitStretch()
    {
        RECT c;
        GetClientRect(h_, &c);
        if (fitted_ || c.right <= 0) return;
        fitted_ = true;
        long fixed = 0;
        int stretchers = 0;
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            if (!visible_[i]) continue;
            if (columns_[i].stretch)
                ++stretchers;
            else
                fixed += ListView_GetColumnWidth(h_, int(i));
        }
        if (!stretchers) return;
        const long each = std::max<long>(px(40), (c.right - fixed) / stretchers);
        for (size_t i = 0; i < columns_.size(); ++i)
            if (visible_[i] && columns_[i].stretch) ListView_SetColumnWidth(h_, int(i), each);
    }
    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        // A modest minimum, not the columns' total: that grows with every
        // column the user widens and, restored, would widen the window.
        return {px(200), fontMetrics(win_.dpi()).cy * 8};
    }

private:
    // The list paints all its rows itself, in one Direct2D pass per paint:
    // a software DC target bound once to the area being repainted (binding
    // one per cell cost ~0.5 ms apiece, a hardware one ~2.5 ms for the GPU
    // readback). The ListView keeps scrolling, selection, the header,
    // keyboard and accessibility; its own item drawing is skipped. Rows look
    // the same with or without focus.
    LRESULT customDraw(NMLVCUSTOMDRAW* cd)
    {
        if (cd->nmcd.dwDrawStage != CDDS_PREPAINT || !model_) return CDRF_DODEFAULT;
        paintRows(cd->nmcd.hdc);
        return CDRF_SKIPDEFAULT;
    }

    struct RowColors
    {
        Color background;
        std::optional<Color> text;  // replaces ThemeColor::Text (solid selection)
    };
    // `c` with its HSL lightness set to `l` (hue and saturation kept).
    static Color withLightness(Color c, float l)
    {
        const float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
        const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
        const float l0 = (hi + lo) / 2;
        const float sat = d == 0 ? 0 : d / (1 - std::abs(2 * l0 - 1));
        float h = 0;  // in sixths of a turn
        if (d > 0)
            h = hi == r ? std::fmod((g - b) / d + 6, 6.f) : hi == g ? (b - r) / d + 2 : (r - g) / d + 4;
        const float chroma = (1 - std::abs(2 * l - 1)) * sat;
        const float x = chroma * (1 - std::abs(std::fmod(h, 2.f) - 1)), m = l - chroma / 2;
        float rgb[3]{};
        const int sector = int(h) % 6;
        const int order[6][3] = {{0, 1, 2}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}, {1, 2, 0}, {0, 2, 1}};
        const float parts[3] = {chroma, x, 0};  // largest, middle, smallest
        for (int i = 0; i < 3; ++i) rgb[i] = parts[order[sector][i]] + m;
        auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
        return Color{byte(rgb[0]), byte(rgb[1]), byte(rgb[2]), 255};
    }
    RowColors rowColors(bool selected, bool hot) const
    {
        const Color base = themeColor(ThemeColor::Base), accent = themeColor(ThemeColor::Accent);
        const bool dark = base.r * 30 + base.g * 59 + base.b * 11 < 12800;
        auto mixed = [&](float t)
        {
            auto m = [t](uint8_t a, uint8_t b) { return uint8_t(std::lround(a + (b - a) * t)); };
            return Color{m(base.r, accent.r), m(base.g, accent.g), m(base.b, accent.b), 255};
        };
        // Soft selection is the theme's highlight with its hue and
        // saturation kept and its lightness raised: Windows' default
        // #0078D7 at 90% is Explorer's selected #CCE8FF, at 95% its hover
        // #E5F3FF. (Mixing toward white also drains the saturation and
        // reads grey.) Dark themes keep a mix of the base colour.
        const Color highlight = themeColor(ThemeColor::Highlight);
        if (!selected)
        {
            if (!hot || !soft_) return {base, std::nullopt};
            return {dark ? mixed(0.16f) : withLightness(highlight, 0.95f), std::nullopt};
        }
        if (soft_)
            return {dark ? mixed(hot ? 0.46f : 0.38f) : withLightness(highlight, hot ? 0.88f : 0.90f), std::nullopt};
        return {themeColor(ThemeColor::Highlight), themeColor(ThemeColor::HighlightText)};
    }

    IDWriteTextFormat* textFormat(const TextStyle& st, Align align)
    {
        const int key = (st.bold ? 1 : 0) | (st.italic ? 2 : 0) | (st.monospace ? 4 : 0) | (int(align) << 3);
        Com<IDWriteTextFormat>& f = formats_[key];
        if (!f)
        {
            dwriteFactory()->CreateTextFormat(st.monospace ? L"Consolas" : uiFontFamily().c_str(), nullptr,
                                              st.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                              st.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, uiFontSizeDip(), L"", f.put());
            if (!f) return nullptr;
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetTextAlignment(align == Align::End      ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            Com<IDWriteInlineObject> ellipsis;
            dwriteFactory()->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
        return f.get();
    }

    void paintRows(HDC dc)
    {
        RECT clip{};
        if (GetClipBox(dc, &clip) == NULLREGION || IsRectEmpty(&clip)) return;
        if (!rowTarget_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96, 96);
            if (FAILED(d2dFactory()->CreateDCRenderTarget(&props, rowTarget_.put()))) return;
            // Crisp GDI-style glyphs, like the ListView's own text.
            Com<IDWriteRenderingParams> def, params;
            dwriteFactory()->CreateRenderingParams(def.put());
            if (def &&
                SUCCEEDED(dwriteFactory()->CreateCustomRenderingParams(
                    def->GetGamma(), def->GetEnhancedContrast(), def->GetClearTypeLevel(), def->GetPixelGeometry(),
                    DWRITE_RENDERING_MODE_GDI_CLASSIC, params.put())))
                rowTarget_->SetTextRenderingParams(params.get());
            rowTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        }
        ID2D1DCRenderTarget* rt = rowTarget_.get();
        if (FAILED(rt->BindDC(dc, &clip))) return;
        const float s = win_.scale();
        auto dip = [s](long v) { return v / s; };

        // Column spans in client px: the header's item rects (in display
        // order, hidden ones zero-wide) shifted by the horizontal scroll.
        struct Span
        {
            size_t col;
            long left, right;
        };
        std::vector<Span> spans;
        HWND header = ListView_GetHeader(h_);
        const size_t count = model_->rowCount();
        RECT first{};
        long scrollX = 0;
        if (count && ListView_GetItemRect(h_, ListView_GetTopIndex(h_), &first, LVIR_BOUNDS)) scrollX = first.left;
        for (size_t c = 0; c < columns_.size(); ++c)
        {
            RECT hr{};
            if (!visible_[c] || !Header_GetItemRect(header, int(c), &hr) || hr.right <= hr.left) continue;
            spans.push_back({c, hr.left + scrollX, hr.right + scrollX});
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const Color base = themeColor(ThemeColor::Base);
        rt->Clear(D2D1::ColorF(base.r / 255.f, base.g / 255.f, base.b / 255.f));
        {
            D2DPainter painter(rt, s);  // sets the target's DPI: coordinates below are DIPs
            const D2D1_MATRIX_3X2_F origin = D2D1::Matrix3x2F::Translation(-dip(clip.left), -dip(clip.top));
            Com<ID2D1SolidColorBrush> brush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush.put());
            auto fill = [&](RECT r, Color c)
            {
                brush->SetColor(D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f));
                rt->SetTransform(origin);
                rt->FillRectangle(D2D1::RectF(dip(r.left), dip(r.top), dip(r.right), dip(r.bottom)), brush.get());
            };

            for (int row = count ? ListView_GetTopIndex(h_) : 0; size_t(row) < count; ++row)
            {
                RECT rr{};
                if (!ListView_GetItemRect(h_, row, &rr, LVIR_BOUNDS) || rr.top >= clip.bottom) break;
                if (rr.bottom <= clip.top) continue;
                const bool selected = ListView_GetItemState(h_, row, LVIS_SELECTED) & LVIS_SELECTED;
                const RowColors colors = rowColors(selected, row == hot_);
                fill(RECT{clip.left, rr.top, clip.right, rr.bottom}, colors.background);

                for (const Span& sp : spans)
                {
                    if (sp.right <= clip.left || sp.left >= clip.right) continue;
                    const RECT cell{sp.left, rr.top, sp.right, rr.bottom};
                    const TextStyle st = model_->style(size_t(row), sp.col);
                    if (st.bg && !selected) fill(cell, painter.resolve(*st.bg));
                    if (columns_[sp.col].painted)
                    {
                        D2DPainter cellPainter(rt, s);
                        if (soft_)
                        {
                            cellPainter.overrideColor(ThemeColor::Highlight, colors.background);
                            cellPainter.overrideColor(ThemeColor::HighlightText, themeColor(ThemeColor::Text));
                        }
                        if (colors.text) cellPainter.overrideColor(ThemeColor::Text, *colors.text);
                        rt->SetTransform(D2D1::Matrix3x2F::Translation(dip(cell.left), dip(cell.top)) * origin);
                        const SizeF size{dip(cell.right - cell.left), dip(cell.bottom - cell.top)};
                        cellPainter.pushClip({0, 0, size.w, size.h});
                        model_->paint(size_t(row), sp.col, cellPainter, CellInfo{size, selected, false});
                        cellPainter.popClip();
                        continue;
                    }
                    const std::wstring text = wide(model_->text(size_t(row), sp.col));
                    if (text.empty()) continue;
                    IDWriteTextFormat* format = textFormat(st, columns_[sp.col].align);
                    if (!format) continue;
                    const Color fg = colors.text && !st.fg ? *colors.text
                                                           : painter.resolve(st.fg ? *st.fg : Paint{ThemeColor::Text});
                    brush->SetColor(D2D1::ColorF(fg.r / 255.f, fg.g / 255.f, fg.b / 255.f, fg.a / 255.f));
                    rt->SetTransform(origin);
                    const float pad = 6;  // the ListView's own cell padding, in DIPs
                    rt->DrawText(text.c_str(), UINT32(text.size()), format,
                                 D2D1::RectF(dip(cell.left) + pad, dip(cell.top), dip(cell.right) - pad, dip(cell.bottom)),
                                 brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }

            // The drop line while rows are dragged (the ListView would draw
            // it, but its item painting is skipped).
            RECT mark{};
            if (dragging_ && SendMessageW(h_, LVM_GETINSERTMARKRECT, 0, reinterpret_cast<LPARAM>(&mark)) &&
                !IsRectEmpty(&mark))
                fill(mark, themeColor(ThemeColor::Text));
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) rowTarget_.reset();
    }

    // A click changes the selection in steps (everything off, then the new
    // row on), and each step notified the app, which redrew its details
    // twice. Changes are reported once, after the ListView is done: posted
    // messages run before the next paint, so the details still land in the
    // same frame as the highlight.
    void selectionChanged()
    {
        if (!selChanged_ || selPending_) return;
        selPending_ = true;
        PostMessageW(h_, WM_ANYUI_SELECTION, 0, 0);
    }
    static LRESULT CALLBACK selectionProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        if (msg != WM_ANYUI_SELECTION) return DefSubclassProc(h, msg, wp, lp);
        auto* self = reinterpret_cast<WList*>(data);
        self->selPending_ = false;
        if (self->selChanged_) self->selChanged_();
        return 0;
    }

    // The row under the pointer gets a light tint (soft-selection lists,
    // like Explorer's and TortoiseGit's).
    void setHot(int row)
    {
        if (row == hot_) return;
        for (int r : {hot_, row})
        {
            RECT rr{};
            if (r >= 0 && ListView_GetItemRect(h_, r, &rr, LVIR_BOUNDS)) InvalidateRect(h_, &rr, FALSE);
        }
        hot_ = row;
    }
    static LRESULT CALLBACK hotProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WList*>(data);
        if (msg == WM_MOUSEMOVE)
        {
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int row = ListView_HitTest(h, &hit);
            self->setHot(row >= 0 && (hit.flags & LVHT_ONITEM) ? row : -1);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        else if (msg == WM_MOUSELEAVE)
            self->setHot(-1);
        return DefSubclassProc(h, msg, wp, lp);
    }

    // Where dragged rows would land for a point in client coordinates: the
    // row they go in front of (rowCount() = the end). Above the rows (over the
    // header) means the first visible row; only space below the last row
    // means the end.
    size_t dropTarget(POINT pt) const
    {
        const size_t count = model_ ? model_->rowCount() : 0;
        if (!count) return 0;
        const int top = ListView_GetTopIndex(h_);
        RECT first{};
        ListView_GetItemRect(h_, top, &first, LVIR_BOUNDS);
        if (pt.y < first.top) return size_t(top);
        LVINSERTMARK mark{sizeof mark, 0, -1, 0};
        SendMessageW(h_, LVM_INSERTMARKHITTEST, reinterpret_cast<WPARAM>(&pt), reinterpret_cast<LPARAM>(&mark));
        if (mark.iItem < 0) return count;
        return size_t(mark.iItem) + ((mark.dwFlags & LVIM_AFTER) ? 1 : 0);
    }
    // A contiguous selection dropped onto its own place moves nothing.
    bool noop(size_t target) const
    {
        const std::vector<size_t> rows = selection();
        if (rows.empty()) return true;
        const bool contiguous = rows.back() - rows.front() + 1 == rows.size();
        return contiguous && target >= rows.front() && target <= rows.back() + 1;
    }
    void showMark(size_t target)
    {
        if (noop(target))
        {
            LVINSERTMARK none{sizeof none, 0, -1, 0};
            SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&none));
            SetCursor(LoadCursorW(nullptr, IDC_NO));
            return;
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        const size_t count = model_ ? model_->rowCount() : 0;
        LVINSERTMARK mark{sizeof mark, 0, -1, 0};
        if (count)
        {
            mark.iItem = int(std::min(target, count - 1));
            mark.dwFlags = target >= count ? LVIM_AFTER : 0;
        }
        SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
    }
public:
    void cancelDrag() { endDrag(false, POINT{}); }

private:
    void endDrag(bool drop, POINT pt)
    {
        if (!dragging_) return;
        dragging_ = false;
        if (g_dragging == this) g_dragging = nullptr;
        LVINSERTMARK none{sizeof none, 0, -1, 0};
        SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&none));
        RemoveWindowSubclass(h_, dragProc, 2);
        if (GetCapture() == h_) ReleaseCapture();
        if (drop && reorder_)
        {
            const std::vector<size_t> rows = selection();
            const size_t target = dropTarget(pt);
            if (!rows.empty() && !noop(target)) reorder_(rows, target);
        }
    }
    static LRESULT CALLBACK dragProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WList*>(data);
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg)
        {
        case WM_MOUSEMOVE:
        {
            self->showMark(self->dropTarget(pt));
            RECT c;
            GetClientRect(h, &c);
            const int edge = self->px(20);
            if (pt.y < edge) ListView_Scroll(h, 0, -self->px(8));
            else if (pt.y > c.bottom - edge) ListView_Scroll(h, 0, self->px(8));
            return 0;
        }
        case WM_LBUTTONUP: self->endDrag(true, pt); return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: self->endDrag(false, pt); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
            {
                self->endDrag(false, pt);
                return 0;
            }
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    ReorderFn reorder_;
    bool reorderable_ = false, dragging_ = false;
    std::vector<Column> columns_;
    std::vector<bool> visible_;
    std::vector<int> widths_;
    ListModel* model_ = nullptr;
    std::wstring text_;  // LVN_GETDISPINFO's buffer
    bool quiet_ = false;
    bool fitted_ = false;  // stretch columns have had their share of the width
    bool selPending_ = false;  // a selection change is posted, not yet reported
    Com<ID2D1DCRenderTarget> rowTarget_;
    std::map<int, Com<IDWriteTextFormat>> formats_;  // by style and alignment
    bool soft_ = false;
    int hot_ = -1;  // the row under the pointer
    int sortColumn_ = -1;
    SortOrder sortOrder_ = SortOrder::Ascending;
    std::function<void()> selChanged_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, SortOrder)> sort_;
    std::function<Menu(std::optional<size_t>)> context_;
    std::function<Menu(size_t)> headerMenu_;
};

// ---------------------------------------------------------------- tabs

class WTabs : public Leaf<anyui::Tabs>
{
public:
    WTabs(WinContext& win, HWND parent, const spec::Tabs& s)
        : Leaf(win)
    {
        adopt(createControl(win, parent, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP));
        for (size_t i = 0; i < s.titles.size(); ++i)
        {
            std::wstring t = mnemonic(s.titles[i]);
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = t.data();
            TabCtrl_InsertItem(h_, int(i), &item);
        }
    }
    std::vector<Item*> pages;
    // Lazy pages: their builders until built, and whether each page is one.
    std::vector<std::function<Node()>> lazy;
    std::vector<bool> isLazy;
    std::function<Item*(const Node&)> buildPage;

    int current() const override { return TabCtrl_GetCurSel(h_); }
    void setCurrent(int i) override
    {
        TabCtrl_SetCurSel(h_, i);
        ensureBuilt(i);
        win_.relayoutAtOnce();
    }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == TCN_SELCHANGE;
        if (handled)
        {
            ensureBuilt(current());
            win_.relayoutAtOnce();
            if (changed_) changed_(current());
        }
        return 0;
    }
    void ensureBuilt(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto build = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        pages[size_t(i)] = buildPage(build());
        // New controls go above the tab control, as the first pages did.
        SetWindowPos(h_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        win_.relayout();
    }
    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        SIZE in{0, 0};
        for (size_t i = 0; i < pages.size(); ++i)
        {
            if (i < isLazy.size() && isLazy[i]) continue;  // they take the area they're given
            const SIZE s = pages[i]->natural();
            in.cx = std::max(in.cx, s.cx);
            in.cy = std::max(in.cy, s.cy);
        }
        RECT r{0, 0, in.cx + 2 * pad(), in.cy + 2 * pad()};
        TabCtrl_AdjustRect(h_, TRUE, &r);
        return {r.right - r.left, r.bottom - r.top};
    }
    void place(RECT r) override
    {
        Leaf::place(r);
        RECT in{0, 0, r.right - r.left, r.bottom - r.top};
        TabCtrl_AdjustRect(h_, FALSE, &in);
        OffsetRect(&in, r.left, r.top);
        InflateRect(&in, -pad(), -pad());
        for (Item* p : pages) p->place(in);
    }
    void showAll(bool shown) override
    {
        Leaf::showAll(shown);
        if (shown && visible) ensureBuilt(current());  // the first time a lazy page is the one showing
        for (size_t i = 0; i < pages.size(); ++i) pages[i]->showAll(shown && visible && int(i) == current());
    }

private:
    int pad() const { return px(6); }
    std::function<void(int)> changed_;
};

// Tabs as a list of page names down the left, the page beside it.
class WSideTabs : public Leaf<anyui::Tabs>
{
public:
    WSideTabs(WinContext& win, HWND parent, const spec::Tabs& s)
        : Leaf(win)
    {
        // Owner-drawn only to centre the names in rows taller than the text
        // (a plain list box draws them at the top of each row).
        adopt(createControl(win, parent, L"LISTBOX", L"",
                            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_TABSTOP,
                            WS_EX_CLIENTEDGE));
        for (const std::string& t : s.titles)
        {
            // Mnemonics mean nothing in a list: "_General" shows as "General".
            std::wstring title = mnemonic(t);
            title.erase(std::remove(title.begin(), title.end(), L'&'), title.end());
            titles_.push_back(title);
            SendMessageW(h_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.c_str()));
        }
        SendMessageW(h_, LB_SETITEMHEIGHT, 0, fontMetrics(win.dpi()).cy + px(12));
        SendMessageW(h_, LB_SETCURSEL, 0, 0);
    }
    std::vector<Item*> pages;
    std::vector<std::function<Node()>> lazy;
    std::vector<bool> isLazy;
    std::function<Item*(const Node&)> buildPage;

    int current() const override { return std::max(0, int(SendMessageW(h_, LB_GETCURSEL, 0, 0))); }
    void setCurrent(int i) override
    {
        SendMessageW(h_, LB_SETCURSEL, WPARAM(i), 0);
        if (i == shown_) return;
        shown_ = i;
        ensureBuilt(i);
        win_.relayoutAtOnce();
    }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        // A click on the empty space below the names reports a change too.
        if (code != LBN_SELCHANGE || current() == shown_) return;
        shown_ = current();
        ensureBuilt(current());
        win_.relayoutAtOnce();
        if (changed_) changed_(current());
    }
    void ensureBuilt(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto build = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        pages[size_t(i)] = buildPage(build());
        win_.relayout();
    }
    bool drawItem(DRAWITEMSTRUCT* d) override
    {
        if (d->itemID == UINT(-1) || d->itemID >= titles_.size()) return false;
        const bool selected = d->itemState & ODS_SELECTED;
        FillRect(d->hDC, &d->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        HGDIOBJ old = SelectObject(d->hDC, uiFont(win_.dpi()));
        RECT text = d->rcItem;
        text.left += px(8);
        DrawTextW(d->hDC, titles_[d->itemID].c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(d->hDC, old);
        if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) DrawFocusRect(d->hDC, &d->rcItem);
        return true;
    }
    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        SIZE in{0, 0};
        for (size_t i = 0; i < pages.size(); ++i)
        {
            if (i < isLazy.size() && isLazy[i]) continue;  // they take the area they're given
            const SIZE s = pages[i]->natural();
            in.cx = std::max(in.cx, s.cx);
            in.cy = std::max(in.cy, s.cy);
        }
        return {listWidth() + gap() + in.cx, std::max<long>(in.cy, long(titles_.size()) * (fontMetrics(win_.dpi()).cy + px(12)))};
    }
    void place(RECT r) override
    {
        const int lw = listWidth();
        Leaf::place({r.left, r.top, r.left + lw, r.bottom});
        for (Item* p : pages) p->place({r.left + lw + gap(), r.top, r.right, r.bottom});
    }
    void showAll(bool shown) override
    {
        Leaf::showAll(shown);
        if (shown && visible) ensureBuilt(current());
        for (size_t i = 0; i < pages.size(); ++i) pages[i]->showAll(shown && visible && int(i) == current());
    }

private:
    int gap() const { return px(12); }
    int listWidth()
    {
        long w = px(120);
        for (const std::wstring& t : titles_) w = std::max(w, textExtent(h_, t).cx + px(36));
        return int(w);
    }

    std::vector<std::wstring> titles_;
    int shown_ = 0;  // the page showing
    std::function<void(int)> changed_;
};

// ---------------------------------------------------------------- splitter

// Panes side by side with draggable bars between them. Sizes are device px
// per pane; the window keeps the panes' proportions until a drag.
class WSplitter : public anyui::Splitter, public Item
{
public:
    WSplitter(WinContext& win, HWND parent, Orientation dir)
        : win_(win)
        , parent_(parent)
        , dir_(dir)
    {
    }
    ~WSplitter() override
    {
        for (HWND b : bars_) DestroyWindow(b);
    }
    std::vector<Item*> panes;

    void addBar()
    {
        registerClasses();
        HWND bar = makeHost(parent_);
        auto* fn = new std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>(
            [this, index = bars_.size()](HWND h, UINT msg, WPARAM, LPARAM lp, bool& handled) -> LRESULT
            { return barMessage(index, h, msg, lp, handled); });
        SetWindowLongPtrW(bar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fn));
        bars_.push_back(bar);
        fns_.emplace_back(fn);
    }

    void setEnabled(bool) override {}
    void setVisible(bool v) override
    {
        if (visible == v) return;
        visible = v;
        win_.relayout();
    }
    void setTooltip(const std::string&) override {}
    void focus() override {}
    std::vector<float> sizes() const override
    {
        std::vector<float> out;
        for (long s : sizes_) out.push_back(s / win_.scale());
        return out;
    }
    void setSizes(const std::vector<float>& s) override
    {
        sizes_.clear();
        for (float v : s) sizes_.push_back(long(v * win_.scale()));
        win_.relayout();
    }

    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        long main = 0, cross = 0;
        for (Item* p : panes)
        {
            if (!p->visible) continue;
            const SIZE s = p->natural();
            main += dir_ == Orientation::Horizontal ? s.cx : s.cy;
            cross = std::max(cross, dir_ == Orientation::Horizontal ? s.cy : s.cx);
        }
        main += bar() * long(panes.size() - 1);
        return dir_ == Orientation::Horizontal ? SIZE{main, cross} : SIZE{cross, main};
    }
    void place(RECT r) override
    {
        rect_ = r;
        std::vector<size_t> shown;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i]->visible) shown.push_back(i);
        const long total = dir_ == Orientation::Horizontal ? r.right - r.left : r.bottom - r.top;
        // Sizes: remembered ones where we have them; the first pane takes the rest.
        std::vector<long> sz(panes.size(), 0);
        long rest = total - bar() * long(std::max<size_t>(shown.size(), 1) - 1);
        if (sizes_.size() != panes.size())
        {
            int stretchSum = 0;
            for (size_t i : shown) stretchSum += std::max(panes[i]->stretch, 0);
            long fixed = 0;
            for (size_t i : shown)
                if (!panes[i]->stretch)
                {
                    const SIZE n = panes[i]->natural();
                    sz[i] = dir_ == Orientation::Horizontal ? n.cx : n.cy;
                    fixed += sz[i];
                }
            for (size_t i : shown)
                if (panes[i]->stretch) sz[i] = stretchSum ? (rest - fixed) * panes[i]->stretch / stretchSum : 0;
        }
        else
        {
            long given = 0;
            for (size_t k = 1; k < shown.size(); ++k)
            {
                sz[shown[k]] = sizes_[shown[k]];
                given += sz[shown[k]];
            }
            if (!shown.empty()) sz[shown[0]] = std::max(0L, rest - given);
        }
        long pos = dir_ == Orientation::Horizontal ? r.left : r.top;
        size_t b = 0;
        for (size_t k = 0; k < shown.size(); ++k)
        {
            const size_t i = shown[k];
            const long s = std::max(0L, sz[i]);
            panes[i]->place(dir_ == Orientation::Horizontal ? RECT{pos, r.top, pos + s, r.bottom}
                                                            : RECT{r.left, pos, r.right, pos + s});
            pos += s;
            if (k + 1 < shown.size() && b < bars_.size())
            {
                HWND bh = bars_[b++];
                if (dir_ == Orientation::Horizontal)
                    moveControl(bh, pos, r.top, bar(), r.bottom - r.top);
                else
                    moveControl(bh, r.left, pos, r.right - r.left, bar());
                showControl(bh, true);
                pos += bar();
            }
        }
        for (; b < bars_.size(); ++b) showControl(bars_[b], false);
        placed_ = sz;
    }
    void showAll(bool shown) override
    {
        for (Item* p : panes) p->showAll(shown && visible);
        if (!(shown && visible))
            for (HWND b : bars_) showControl(b, false);
    }

private:
    int bar() const { return win_.px(5); }
    LRESULT barMessage(size_t index, HWND h, UINT msg, LPARAM lp, bool& handled)
    {
        handled = true;
        switch (msg)
        {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, dir_ == Orientation::Horizontal ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        case WM_LBUTTONDOWN:
            SetCapture(h);
            dragging_ = true;
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            dragging_ = false;
            return 0;
        case WM_MOUSEMOVE:
        {
            if (!dragging_) break;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            MapWindowPoints(h, parent_, &pt, 1);
            dragBar(index, dir_ == Orientation::Horizontal ? pt.x : pt.y);
            return 0;
        }
        }
        handled = false;
        return 0;
    }

    // Bar `index` follows the pointer at `at` (parent coordinates): the pane
    // above it takes the difference, the panes between keep their sizes, and
    // the last pane gives or takes the rest; once the last pane is empty the
    // ones above it shrink in turn, nearest first. Matches GTK's nested
    // panes, so a middle pane stays put while the bar above it moves.
    void dragBar(size_t index, long at)
    {
        std::vector<size_t> shown;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i]->visible) shown.push_back(i);
        if (index + 1 >= shown.size() || placed_.size() != panes.size()) return;
        std::vector<long> sz(shown.size());
        for (size_t k = 0; k < shown.size(); ++k) sz[k] = std::max(0L, placed_[shown[k]]);

        long barStart = dir_ == Orientation::Horizontal ? rect_.left : rect_.top;
        for (size_t k = 0; k <= index; ++k) barStart += sz[k] + (k < index ? bar() : 0);
        long delta = at - bar() / 2 - barStart;
        long below = 0;  // what the panes after the bar can give up
        for (size_t k = index + 1; k < sz.size(); ++k) below += sz[k];
        delta = std::clamp(delta, -sz[index], below);
        if (delta == 0) return;

        sz[index] += delta;
        if (delta < 0) sz.back() -= delta;  // growing: the last pane takes it all
        else
            for (size_t k = sz.size() - 1; delta > 0 && k > index; --k)
            {
                const long take = std::min(delta, sz[k]);
                sz[k] -= take;
                delta -= take;
            }
        sizes_.assign(panes.size(), 0);
        for (size_t k = 0; k < shown.size(); ++k) sizes_[shown[k]] = sz[k];
        win_.relayout();
        // Lay out and paint now: a posted relayout, and WM_PAINT after it,
        // wait behind the stream of mouse moves, leaving stale pixels where
        // the bar has passed.
        SendMessageW(win_.hwnd(), WM_ANYUI_RELAYOUT, 0, 0);
        RedrawWindow(win_.hwnd(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    WinContext& win_;
    HWND parent_;
    Orientation dir_;
    std::vector<HWND> bars_;
    std::vector<std::unique_ptr<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>>> fns_;
    std::vector<long> sizes_, placed_;
    RECT rect_{};
    bool dragging_ = false;
};

// ---------------------------------------------------------------- scroll

// A child window that clips its content and scrolls it vertically (and
// horizontally if asked).
class ScrollItem : public Item
{
public:
    ScrollItem(WinContext& win, HWND parent, bool horizontal)
        : win_(win)
        , horizontal_(horizontal)
    {
        host_ = makeHost(parent, WS_VSCROLL | (horizontal ? WS_HSCROLL : 0));
        fn_ = std::make_unique<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>>(
            [this](HWND, UINT msg, WPARAM wp, LPARAM, bool& handled) -> LRESULT { return message(msg, wp, handled); });
        SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fn_.get()));
        // The content sits on one panel the size of all of it, and scrolling
        // moves just that: moving each control would cost an accessibility
        // event apiece, which is slow whenever an automation client listens.
        // The panel erases under its children: group boxes are transparent
        // and never erase their insides.
        panel_ = makeHost(host_);
        SetWindowLongPtrW(panel_, GWL_STYLE, GetWindowLongPtrW(panel_, GWL_STYLE) & ~WS_CLIPCHILDREN);
        showControl(panel_, true);
    }
    ~ScrollItem() override { DestroyWindow(host_); }
    HWND host() const { return panel_; }  // where the content's controls go
    Item* content = nullptr;

    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override
    {
        SIZE c = content ? content->natural() : SIZE{0, 0};
        c.cx += GetSystemMetrics(SM_CXVSCROLL);
        c.cy = std::min<long>(c.cy, fontMetrics(win_.dpi()).cy * 10);  // asks for a modest height
        return c;
    }
    void place(RECT r) override
    {
        moveNow(host_, r.left, r.top, r.right - r.left, r.bottom - r.top);
        layoutContent();
    }
    void showAll(bool shown) override
    {
        showControl(host_, shown && visible);
        if (content) content->showAll(shown && visible);
    }

private:
    void layoutContent()
    {
        if (!content) return;
        RECT client;
        GetClientRect(host_, &client);
        const SIZE n = content->natural();
        const long h = std::max<long>(n.cy, client.bottom), w = horizontal_ ? std::max<long>(n.cx, client.right) : client.right;
        SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, int(h - 1), UINT(client.bottom), 0, 0};
        y_ = std::clamp(y_, 0L, std::max(0L, h - client.bottom));
        si.nPos = int(y_);
        SetScrollInfo(host_, SB_VERT, &si, TRUE);
        if (horizontal_)
        {
            SCROLLINFO sh{sizeof sh, SIF_RANGE | SIF_PAGE | SIF_POS, 0, int(w - 1), UINT(client.right), 0, 0};
            x_ = std::clamp(x_, 0L, std::max(0L, w - client.right));
            sh.nPos = int(x_);
            SetScrollInfo(host_, SB_HORZ, &sh, TRUE);
        }
        moveNow(panel_, -x_, -y_, w, h);
        content->place({0, 0, w, h});
        contentW_ = w, contentH_ = h;
    }
    // Scrolling moves what's already drawn: Windows shifts the pixels and the
    // panel together, and only the strip that comes into view paints.
    void scrollTo(long x, long y)
    {
        RECT client;
        GetClientRect(host_, &client);
        x = std::clamp(x, 0L, std::max(0L, contentW_ - client.right));
        y = std::clamp(y, 0L, std::max(0L, contentH_ - client.bottom));
        const int dx = int(x_ - x), dy = int(y_ - y);
        if (!dx && !dy) return;
        x_ = x, y_ = y;
        SetScrollPos(host_, SB_VERT, int(y_), TRUE);
        if (horizontal_) SetScrollPos(host_, SB_HORZ, int(x_), TRUE);
        HRGN exposed = CreateRectRgn(0, 0, 0, 0);
        ScrollWindowEx(host_, dx, dy, nullptr, nullptr, exposed, nullptr, SW_SCROLLCHILDREN);
        RedrawWindow(host_, nullptr, exposed, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        DeleteObject(exposed);
    }

    LRESULT message(UINT msg, WPARAM wp, bool& handled)
    {
        handled = true;
        switch (msg)
        {
        case WM_SIZE: layoutContent(); return 0;
        case WM_VSCROLL:
        case WM_HSCROLL:
        {
            const bool v = msg == WM_VSCROLL;
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(host_, v ? SB_VERT : SB_HORZ, &si);
            long pos = v ? y_ : x_;
            const long line = fontMetrics(win_.dpi()).cy;
            switch (LOWORD(wp))
            {
            case SB_LINEUP: pos -= line; break;
            case SB_LINEDOWN: pos += line; break;
            case SB_PAGEUP: pos -= long(si.nPage); break;
            case SB_PAGEDOWN: pos += long(si.nPage); break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            if (v)
                scrollTo(x_, pos);
            else
                scrollTo(pos, y_);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scrollTo(x_, y_ - GET_WHEEL_DELTA_WPARAM(wp) * fontMetrics(win_.dpi()).cy * 3 / WHEEL_DELTA);
            return 0;
        }
        handled = false;
        return 0;
    }

    WinContext& win_;
    bool horizontal_;
    HWND host_, panel_;
    std::unique_ptr<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>> fn_;
    long x_ = 0, y_ = 0;
    long contentW_ = 0, contentH_ = 0;
};

// ---------------------------------------------------------------- frames

// One display-paced clock for every frame subscriber: a thread waits on the
// compositor (DwmFlush) and posts a tick to the UI thread.
class FrameClock
{
public:
    explicit FrameClock(HWND target)
        : target_(target)
    {
    }
    ~FrameClock() { stopThread(); }

    Subscription subscribe(std::function<void(double)> fn)
    {
        const int id = ++next_;
        subscribers_[id] = std::move(fn);
        startThread();
        return Subscription(
            [this, id, alive = alive_]
            {
                if (!*alive) return;
                subscribers_.erase(id);
            });
    }
    void tick()
    {
        pending_ = false;
        if (subscribers_.empty())
        {
            stopThread();
            return;
        }
        const double t = nowSeconds();
        auto copy = subscribers_;  // callbacks may unsubscribe
        for (auto& [id, fn] : copy)
            if (subscribers_.count(id)) fn(t);
    }

private:
    void startThread()
    {
        if (running_) return;
        running_ = true;
        thread_ = std::thread(
            [this]
            {
                while (running_)
                {
                    if (FAILED(DwmFlush())) Sleep(16);
                    if (!pending_.exchange(true)) PostMessageW(target_, WM_ANYUI_FRAME, 0, 0);
                }
            });
    }
    void stopThread()
    {
        if (!running_) return;
        running_ = false;
        if (thread_.joinable()) thread_.join();
        *alive_ = true;
    }

    HWND target_;
    std::atomic<bool> running_{false}, pending_{false};
    std::thread thread_;
    std::map<int, std::function<void(double)>> subscribers_;
    int next_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

FrameClock* g_frames = nullptr;

// ---------------------------------------------------------------- canvas

class WCanvas : public Leaf<anyui::Canvas>
{
public:
    WCanvas(WinContext& win, HWND parent, const spec::Canvas& s)
        : Leaf(win)
        , focusable_(s.focusable)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.style = CS_DBLCLKS;
            wc.hInstance = instance();
            wc.lpfnWndProc = proc;
            wc.lpszClassName = L"anyuiCanvas";
            RegisterClassExW(&wc);
            registered = true;
        }
        adopt(CreateWindowExW(0, L"anyuiCanvas", L"", WS_CHILD | (s.focusable ? WS_TABSTOP : 0), 0, 0, 0, 0, parent,
                              nullptr, instance(), this));
    }
    ~WCanvas() override { *alive_ = false; }

    void invalidate() override { InvalidateRect(h_, nullptr, FALSE); }
    SizeF size() const override
    {
        RECT r;
        GetClientRect(h_, &r);
        const float s = win_.scale();
        return {r.right / s, r.bottom / s};
    }
    float deviceScale() const override { return win_.scale(); }
    void setCursor(Cursor c) override
    {
        cursor_ = c;
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == h_) applyCursor();
    }
    void onPaint(std::function<void(Painter&, SizeF)> f) override { paint_ = std::move(f); }
    void onResize(std::function<void(SizeF)> f) override { resize_ = std::move(f); }
    void onPointer(std::function<bool(const PointerEvent&)> f) override { pointer_ = std::move(f); }
    void onScroll(std::function<bool(const ScrollEvent&)> f) override { scroll_ = std::move(f); }
    void onKey(std::function<bool(const KeyEvent&)> f) override { key_ = std::move(f); }
    Subscription onFrame(std::function<void(double)> f) override
    {
        return g_frames ? g_frames->subscribe(std::move(f)) : Subscription();
    }

    bool fills() const override { return true; }
    bool expandsInForm() const override { return true; }
    SIZE measure() override { return {px(64), px(64)}; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<WCanvas*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self)
        {
            bool handled = true;
            const LRESULT r = self->message(msg, wp, lp, handled);
            if (handled) return r;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    PointerEvent pointerEvent(PointerEvent::Type t, LPARAM lp, int button) const
    {
        const float s = win_.scale();
        return PointerEvent{t, {GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s}, button, 1, keyModifiers()};
    }
    bool press(LPARAM lp, int button)
    {
        if (focusable_ && GetFocus() != h_) SetFocus(h_);
        SetCapture(h_);
        PointerEvent e = pointerEvent(PointerEvent::Type::Press, lp, button);
        const DWORD time = GetMessageTime();
        const bool again = button == lastButton_ && time - lastTime_ <= GetDoubleClickTime() &&
                           std::abs(GET_X_LPARAM(lp) - lastPos_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           std::abs(GET_Y_LPARAM(lp) - lastPos_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        clicks_ = again ? clicks_ + 1 : 1;
        e.clicks = clicks_;
        lastButton_ = button;
        lastTime_ = time;
        lastPos_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        return pointer_ && pointer_(e);
    }
    bool release(LPARAM lp, int button)
    {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000) && !(GetKeyState(VK_RBUTTON) & 0x8000) &&
            !(GetKeyState(VK_MBUTTON) & 0x8000))
            ReleaseCapture();
        return pointer_ && pointer_(pointerEvent(PointerEvent::Type::Release, lp, button));
    }
    void applyCursor()
    {
        const wchar_t* id = IDC_ARROW;
        switch (cursor_)
        {
        case Cursor::Default: id = IDC_ARROW; break;
        case Cursor::Pointer: id = IDC_HAND; break;
        case Cursor::Text: id = IDC_IBEAM; break;
        case Cursor::Crosshair: id = IDC_CROSS; break;
        case Cursor::Grab:
        case Cursor::Grabbing:
        case Cursor::Move: id = IDC_SIZEALL; break;
        case Cursor::ZoomIn:
        case Cursor::ZoomOut: id = IDC_CROSS; break;
        case Cursor::Wait: id = IDC_WAIT; break;
        case Cursor::Hidden: SetCursor(nullptr); return;
        }
        SetCursor(LoadCursorW(nullptr, id));
    }

    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled)
    {
        using T = PointerEvent::Type;
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(h_, &ps);
            render();
            EndPaint(h_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (resize_) resize_(size());
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: press(lp, 1); return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK: press(lp, 2); return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK: press(lp, 3); return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK: press(lp, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 4 : 5); return TRUE;
        case WM_LBUTTONUP: release(lp, 1); return 0;
        case WM_MBUTTONUP: release(lp, 2); return 0;
        case WM_RBUTTONUP: release(lp, 3); return 0;
        case WM_XBUTTONUP: release(lp, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 4 : 5); return TRUE;
        case WM_MOUSEMOVE:
            if (!inside_)
            {
                inside_ = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h_, 0};
                TrackMouseEvent(&t);
                if (pointer_) pointer_(pointerEvent(T::Enter, lp, 0));
            }
            if (pointer_) pointer_(pointerEvent(T::Move, lp, 0));
            return 0;
        case WM_MOUSELEAVE:
            inside_ = false;
            if (pointer_) pointer_(PointerEvent{T::Leave, {}, 0, 1, keyModifiers()});
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            if (!scroll_) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h_, &pt);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const float s = win_.scale();
            ScrollEvent e{{pt.x / s, pt.y / s}, 0, 0, delta % WHEEL_DELTA != 0, keyModifiers()};
            // Windows: positive = away from the user (up/right); anyui: +1 = down/right.
            if (msg == WM_MOUSEWHEEL)
                e.dy = -float(delta) / WHEEL_DELTA;
            else
                e.dx = float(delta) / WHEEL_DELTA;
            scroll_(e);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT)
            {
                applyCursor();
                return TRUE;
            }
            break;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (key_ && key_(keyEvent(wp, lp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))) return 0;
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS: return 0;
        }
        handled = false;
        return 0;
    }

    void render()
    {
        RECT r;
        GetClientRect(h_, &r);
        if (!rt_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            d2dFactory()->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(h_, D2D1::SizeU(UINT32(r.right), UINT32(r.bottom)),
                                                        D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                rt_.put());
            if (!rt_) return;
        }
        rt_->BeginDraw();
        rt_->Clear(d2dColor(themeColor(ThemeColor::Window)));
        if (paint_)
        {
            D2DPainter p(rt_.get(), win_.scale());
            paint_(p, size());
        }
        if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) rt_.reset();
    }

    bool focusable_;
    Com<ID2D1HwndRenderTarget> rt_;
    Cursor cursor_ = Cursor::Default;
    bool inside_ = false;
    int clicks_ = 0, lastButton_ = 0;
    DWORD lastTime_ = 0;
    POINT lastPos_{};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::function<void(Painter&, SizeF)> paint_;
    std::function<void(SizeF)> resize_;
    std::function<bool(const PointerEvent&)> pointer_;
    std::function<bool(const ScrollEvent&)> scroll_;
    std::function<bool(const KeyEvent&)> key_;
};

// ---------------------------------------------------------------- window

class Win : public anyui::Window, public WinContext
{
public:
    Win(const WindowSpec& spec, Win* parent)
        : parent_(parent)
        , dialog_(spec.dialog)
        , margins_(spec.margins)
        , modalSpec_(spec.modal)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.hInstance = instance();
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
            wc.lpfnWndProc = proc;
            wc.lpszClassName = L"anyuiWindow";
            wc.hIcon = appIcon(SM_CXICON);
            wc.hIconSm = appIcon(SM_CXSMICON);
            RegisterClassExW(&wc);
            registered = true;
        }
        DWORD style = dialog_ ? (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME) : WS_OVERLAPPEDWINDOW;
        if (!spec.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT | (dialog_ ? WS_EX_DLGMODALFRAME : 0), L"anyuiWindow",
                                wide(spec.title).c_str(), style | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                parent ? parent->hwnd_ : nullptr, nullptr, instance(), this);
        initialW_ = spec.width;
        initialH_ = spec.height;
        windows().insert(this);
    }
    ~Win() override
    {
        windows().erase(this);
        items.clear();
        widgets.clear();
        if (hwnd_)
        {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            if (modal_ && parent_) EnableWindow(parent_->hwnd_, TRUE);
            if (menuBar_ && GetMenu(hwnd_) != menuBar_) DestroyMenu(menuBar_);  // detached: ours to free
            DestroyWindow(hwnd_);
        }
    }
    static std::set<Win*>& windows()
    {
        static std::set<Win*> w;
        return w;
    }

    HWND hwnd() const override { return hwnd_; }
    void relayout() override
    {
        ++g_layoutPass;
        if (relayoutPending_) return;
        relayoutPending_ = true;
        PostMessageW(hwnd_, WM_ANYUI_RELAYOUT, 0, 0);
    }
    // With drawing off, the old page goes and the new one comes (through
    // the style bit, silently) and everything then paints in one pass,
    // rather than control by control across several frames.
    void relayoutAtOnce() override
    {
        if (!IsWindowVisible(hwnd_))
        {
            relayout();
            return;
        }
        SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
        layoutNow();
        SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    void layoutNow()
    {
        relayoutPending_ = false;
        if (!root) return;
        RECT r;
        GetClientRect(hwnd_, &r);
        const int m = margin();
        InflateRect(&r, -m, -m);
        MovesInOnePass batch;
        root->showAll(true);
        root->place(r);
    }

    // Owned parts of the window.
    Item* root = nullptr;
    std::vector<ItemPtr> items;
    std::vector<std::unique_ptr<Widget>> widgets;  // plain handles for containers
    std::vector<WButton*> roleButtons;

    void setTitle(const std::string& t) override { SetWindowTextW(hwnd_, wide(t).c_str()); }
    void show() override
    {
        finished_ = false;
        if (!shownOnce_)
        {
            shownOnce_ = true;
            // First size: one set by resize() before showing (a restored
            // size) as it is; otherwise the spec's, but never smaller than
            // the content.
            const SIZE n = root ? root->natural() : SIZE{200, 100};
            const int m = margin();
            long w = std::max<long>(n.cx + 2 * m, extentPx(hwnd_, initialW_, false));
            long h = std::max<long>(n.cy + 2 * m, extentPx(hwnd_, initialH_, true));
            if (requested_) w = requested_->cx, h = requested_->cy;
            RECT r{0, 0, w, h};
            AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                               DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int x = 0, y = 0;
            if (parent_)
            {
                // Centred over the parent.
                RECT pr;
                GetWindowRect(parent_->hwnd_, &pr);
                x = (pr.left + pr.right - (r.right - r.left)) / 2;
                y = (pr.top + pr.bottom - (r.bottom - r.top)) / 2;
            }
            else
                flags |= SWP_NOMOVE;
            SetWindowPos(hwnd_, nullptr, x, y, r.right - r.left, r.bottom - r.top, flags);
            layoutNow();
        }
        if (dialog_ && parent_ && modalSpec_)
        {
            modal_ = true;
            EnableWindow(parent_->hwnd_, FALSE);
        }
        const bool snapshot = std::getenv("ANYUI_SNAPSHOT") != nullptr;
        ShowWindow(hwnd_, snapshot ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!snapshot) SetForegroundWindow(hwnd_);
        lastShown() = this;
        snapshotIfAsked();
    }
    void close() override
    {
        if (modal_ && parent_)
        {
            EnableWindow(parent_->hwnd_, TRUE);
            modal_ = false;
            SetForegroundWindow(parent_->hwnd_);
        }
        ShowWindow(hwnd_, SW_HIDE);
    }
    SizeF size() const override
    {
        RECT r;
        GetClientRect(hwnd_, &r);
        const float s = scale();
        return {r.right / s, r.bottom / s};
    }
    void resize(SizeF sz) override
    {
        if (fullscreen_ || IsZoomed(hwnd_)) return;
        RECT r{0, 0, px(sz.w), px(sz.h)};
        if (!shownOnce_) requested_ = SIZE{r.right, r.bottom};
        adjust(r);
        RECT now;
        GetWindowRect(hwnd_, &now);
        placeOnScreen(now.left, now.top, r.right - r.left, r.bottom - r.top);
    }
    void setMinSize(SizeF s) override { minSize_ = s; }
    std::optional<PointF> position() const override
    {
        RECT r;
        GetWindowRect(hwnd_, &r);
        const float s = scale();
        return PointF{r.left / s, r.top / s};
    }
    void move(PointF p) override
    {
        RECT now;
        GetWindowRect(hwnd_, &now);
        placeOnScreen(px(p.x), px(p.y), now.right - now.left, now.bottom - now.top);
    }
    // A window keeps clear of the taskbar: what would pass the edge of its
    // monitor's work area slides back inside (the visible frame, that is;
    // Windows' resize borders are invisible and may overhang).
    void placeOnScreen(int x, int y, int w, int h)
    {
        RECT want{x, y, x + w, y + h};
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &mi);
        RECT inset{0, 0, 0, 0};  // invisible border on each side
        RECT outer, visible;
        using DwmGet = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
        static DwmGet dwmGet = reinterpret_cast<DwmGet>(
            reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
        if (dwmGet && GetWindowRect(hwnd_, &outer) &&
            SUCCEEDED(dwmGet(hwnd_, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &visible, sizeof visible)) && IsWindowVisible(hwnd_))
            inset = RECT{visible.left - outer.left, visible.top - outer.top, outer.right - visible.right, outer.bottom - visible.bottom};
        const RECT& work = mi.rcWork;
        const auto fit = [](int pos, int size, int lo, int hi, int before, int after)
        {
            if (pos + size - after > hi) pos = hi - size + after;  // past the far edge: back inside
            if (pos + before < lo) pos = lo - before;                 // the near edge wins if it's too big
            return pos;
        };
        x = fit(x, w, int(work.left), int(work.right), int(inset.left), int(inset.right));
        y = fit(y, h, int(work.top), int(work.bottom), int(inset.top), int(inset.bottom));
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RectF workArea() const override
    {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const float s = scale();
        return {mi.rcWork.left / s, mi.rcWork.top / s, (mi.rcWork.right - mi.rcWork.left) / s,
                (mi.rcWork.bottom - mi.rcWork.top) / s};
    }
    bool fullscreen() const override { return fullscreen_; }
    void setFullscreen(bool on) override
    {
        if (on == fullscreen_) return;
        fullscreen_ = on;
        if (on)
        {
            if (menuBar_) SetMenu(hwnd_, nullptr);  // no chrome in fullscreen
            placement_.length = sizeof placement_;
            GetWindowPlacement(hwnd_, &placement_);
            style_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, (style_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        }
        else
        {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style_);
            if (menuBar_ && menuBarWanted_) SetMenu(hwnd_, menuBar_);
            SetWindowPlacement(hwnd_, &placement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        if (onFullscreen_) onFullscreen_(on);
    }
    void onFullscreenChanged(std::function<void(bool)> f) override { onFullscreen_ = std::move(f); }

    // The menu bar lives in the frame (SetMenu), outside the layout. Each
    // menu is refilled as it opens (WM_INITMENUPOPUP), so it shows the
    // app's state as it is then; its commands have ids from kMenuBarIds up,
    // a block per menu.
    static constexpr UINT kMenuBarIds = 0x4000, kMenuBarBlock = 0x400;
    void setMenuBar(const std::vector<MenuBarItem>& menus)
    {
        if (menus.empty()) return;
        menuBar_ = CreateMenu();
        for (const MenuBarItem& m : menus)
        {
            BarMenu bar{m, CreatePopupMenu(), {}};
            AppendMenuW(menuBar_, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(bar.popup), mnemonic(m.title).c_str());
            bars_.push_back(std::move(bar));
        }
        SetMenu(hwnd_, menuBar_);
    }
    void setMenuBarVisible(bool on) override
    {
        menuBarWanted_ = on;
        if (menuBar_ && !fullscreen_) SetMenu(hwnd_, on ? menuBar_ : nullptr);
    }
    bool menuOpening(HMENU popup)
    {
        for (size_t i = 0; i < bars_.size(); ++i)
        {
            BarMenu& bar = bars_[i];
            if (bar.popup != popup) continue;
            while (GetMenuItemCount(popup) > 0) DeleteMenu(popup, 0, MF_BYPOSITION);  // destroys submenus too
            bar.actions.clear();
            fillMenu(popup, bar.item.menu ? bar.item.menu() : Menu{}, bar.actions, kMenuBarIds + UINT(i) * kMenuBarBlock);
            return true;
        }
        return false;
    }
    bool menuCommand(UINT id)
    {
        if (id <= kMenuBarIds) return false;
        const size_t i = (id - kMenuBarIds - 1) / kMenuBarBlock, n = (id - kMenuBarIds) - i * kMenuBarBlock;
        if (i >= bars_.size() || n == 0 || n > bars_[i].actions.size()) return false;
        if (auto action = bars_[i].actions[n - 1]) action();  // a copy: the action may rebuild the menus
        return true;
    }
    void setIcon(const Icon& icon) override
    {
        if (!icon) return;
        HICON big, small;
        if (!icon.draw)
        {
            // Windows has no icon theme: a named window icon is the program's
            // icon resource of that name (e.g. `MYAPP ICON "myapp.ico"`), and
            // without one the window keeps the program's own icon.
            const std::wstring name = wide(icon.name);
            auto load = [&](int metric)
            {
                const int px = GetSystemMetrics(metric);
                return HICON(LoadImageW(instance(), name.c_str(), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
            };
            big = load(SM_CXICON);
            small = load(SM_CXSMICON);
            if (!big && !small) return;
        }
        else
        {
            big = renderIconHandle(icon, GetSystemMetrics(SM_CXICON), scale());
            small = renderIconHandle(icon, GetSystemMetrics(SM_CXSMICON), scale());
        }
        if (big) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (small) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    }
    void onKey(std::function<bool(const KeyEvent&)> f) override { onKey_ = std::move(f); }
    void onFinished(std::function<void(DialogRole)> f) override { onFinished_ = std::move(f); }
    void onCloseRequest(std::function<bool()> f) override { onCloseRequest_ = std::move(f); }

    // From the message loop, before the focused control sees the key.
    bool windowKey(const MSG& m)
    {
        if (!onKey_) return false;
        wchar_t cls[32] = {};
        GetClassNameW(GetFocus(), cls, 32);
        const bool typing = (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"RICHEDIT50W")) &&
                            !(GetWindowLongPtrW(GetFocus(), GWL_STYLE) & ES_READONLY);
        const bool notText = m.wParam == VK_ESCAPE || (m.wParam >= VK_F1 && m.wParam <= VK_F12);
        if (typing && !notText) return false;
        return onKey_(keyEvent(m.wParam, m.lParam, m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN));
    }

    void finish(DialogRole role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = onFinished_) f(role);  // may delete this window: last
    }

private:
    static Win*& lastShown()
    {
        static Win* w = nullptr;
        return w;
    }

    int margin() const
    {
        if (!margins_) return 0;
        const Metrics m = Metrics::forScale(scale());
        return dialog_ ? m.dialogMargin : m.margin;
    }
    void adjust(RECT& r) const
    {
        AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                           DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    }
    void closeRequested()
    {
        if (onCloseRequest_ && !onCloseRequest_()) return;
        if (dialog_)
            finish(DialogRole::Cancel);
        else
            close();
    }

    // ANYUI_SNAPSHOT=file.png: after ANYUI_SNAPSHOT_DELAY ms (default 500),
    // save the window shown last as PNG and quit. For checking without a screen grabber.
    void snapshotIfAsked()
    {
        static bool taken = false;
        const char* path = std::getenv("ANYUI_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("ANYUI_SNAPSHOT_DELAY");
        SetTimer(hwnd_, 0xA11, delay ? UINT(std::atoi(delay)) : 500u, nullptr);
    }
    static void takeSnapshot()
    {
        Win* w = lastShown();
        const char* path = std::getenv("ANYUI_SNAPSHOT");
        if (!w || !path) return;
        RECT r;
        GetWindowRect(w->hwnd_, &r);
        const int width = r.right - r.left, height = r.bottom - r.top;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(w->hwnd_, dc, 2 /* PW_RENDERFULLCONTENT */);
        SelectObject(dc, old);
        // Opaque: PrintWindow leaves alpha undefined.
        auto* px = static_cast<uint8_t*>(bits);
        for (int i = 0; i < width * height; ++i) px[i * 4 + 3] = 255;
        Com<IWICBitmapEncoder> enc;
        Com<IWICStream> stream;
        if (wicFactory() && SUCCEEDED(wicFactory()->CreateStream(stream.put())) &&
            SUCCEEDED(stream->InitializeFromFilename(wide(path).c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wicFactory()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) &&
            SUCCEEDED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)))
        {
            Com<IWICBitmapFrameEncode> frame;
            if (SUCCEEDED(enc->CreateNewFrame(frame.put(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)))
            {
                frame->SetSize(UINT(width), UINT(height));
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&fmt);
                frame->WritePixels(UINT(height), UINT(width) * 4, UINT(width * height * 4), px);
                frame->Commit();
                enc->Commit();
            }
        }
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        std::fprintf(stderr, "snapshot %s: %dx%d\n", path, width, height);
        PostQuitMessage(0);
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Win*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        LRESULT r = 0;
        if (self && forwardChildMessage(h, msg, wp, lp, r)) return r;
        if (self)
        {
            switch (msg)
            {
            case WM_SIZE:
                if (self->root) self->layoutNow();
                return 0;
            case WM_ANYUI_RELAYOUT: self->layoutNow(); return 0;
            case WM_GETMINMAXINFO:
            {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                RECT rc{0, 0, self->px(self->minSize_.w), self->px(self->minSize_.h)};
                if (rc.right > 0 || rc.bottom > 0)
                {
                    self->adjust(rc);
                    mm->ptMinTrackSize.x = std::max<LONG>(mm->ptMinTrackSize.x, rc.right - rc.left);
                    mm->ptMinTrackSize.y = std::max<LONG>(mm->ptMinTrackSize.y, rc.bottom - rc.top);
                }
                return 0;
            }
            case WM_DPICHANGED:
            {
                auto* nr = reinterpret_cast<RECT*>(lp);
                self->refont(HIWORD(wp));
                SetWindowPos(h, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_CLOSE: self->closeRequested(); return 0;
            case WM_INITMENUPOPUP:
                if (self->menuOpening(reinterpret_cast<HMENU>(wp))) return 0;
                break;
            case WM_COMMAND:
                if (!lp && HIWORD(wp) == 0 && self->menuCommand(LOWORD(wp))) return 0;
                if (LOWORD(wp) == IDCANCEL && !lp)
                {
                    if (self->dialog_) self->closeRequested();
                    return 0;
                }
                if (LOWORD(wp) == IDOK && !lp)
                {
                    for (WButton* b : self->roleButtons)
                        if (b->isDefault) SendMessageW(b->hwnd(), BM_CLICK, 0, 0);
                    return 0;
                }
                break;
            case WM_TIMER:
                if (wp == 0xA11)
                {
                    KillTimer(h, wp);
                    takeSnapshot();
                    return 0;
                }
                break;
            case WM_DESTROY:
                if (lastShown() == self) lastShown() = nullptr;
                break;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Every control gets the font for the new DPI.
    void refont(UINT dpi)
    {
        ++g_layoutPass;  // new fonts, new sizes
        EnumChildWindows(
            hwnd_,
            +[](HWND c, LPARAM d) -> BOOL
            {
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont(UINT(d))), TRUE);
                return TRUE;
            },
            LPARAM(dpi));
    }

public:
    Win* parent_;

private:
    HWND hwnd_ = nullptr;
    bool dialog_, margins_, modalSpec_;
    bool modal_ = false, finished_ = false, shownOnce_ = false, fullscreen_ = false, relayoutPending_ = false;
    std::optional<SIZE> requested_;  // client size from resize() before the first show
    struct BarMenu
    {
        MenuBarItem item;
        HMENU popup;
        std::vector<std::function<void()>> actions;
    };
    HMENU menuBar_ = nullptr;  // destroyed with the window while attached
    std::vector<BarMenu> bars_;
    bool menuBarWanted_ = true;
    Extent initialW_, initialH_;
    SizeF minSize_{};
    WINDOWPLACEMENT placement_{};
    LONG_PTR style_ = 0;
    std::function<void(bool)> onFullscreen_;
    std::function<bool(const KeyEvent&)> onKey_;
    std::function<void(DialogRole)> onFinished_;
    std::function<bool()> onCloseRequest_;
};

// ---------------------------------------------------------------- builder

const wchar_t* stockLabel(DialogRole r)
{
    switch (r)
    {
    case DialogRole::Ok: return L"OK";
    case DialogRole::Cancel: return L"Cancel";
    case DialogRole::Apply: return L"&Apply";
    case DialogRole::Close: return L"Close";
    case DialogRole::Yes: return L"&Yes";
    case DialogRole::No: return L"&No";
    case DialogRole::Help: return L"&Help";
    case DialogRole::Reset: return L"&Reset";
    default: return L"";
    }
}

// Windows order: OK Cancel Apply, right-aligned; Help and Reset on the left.
int windowsOrder(DialogRole r)
{
    switch (r)
    {
    case DialogRole::Help: return 0;
    case DialogRole::Reset: return 1;
    case DialogRole::Ok:
    case DialogRole::Yes: return 10;
    case DialogRole::No: return 11;
    case DialogRole::Cancel: return 12;
    case DialogRole::Close: return 13;
    case DialogRole::Apply: return 14;
    default: return 5;
    }
}

class Builder
{
public:
    Builder(Win& win, BuildSink& sink)
        : win_(win)
        , sink_(sink)
        , metrics_(Metrics::forScale(win.scale()))
    {
    }

    Item* build(const Node& n, HWND parent, Orientation parentDir)
    {
        Item* item = std::visit([&](const auto& s) { return make(n, s, parent, parentDir); }, n.spec);
        item->stretch = n.common.stretch;
        item->fixedW = extentPx(win_.hwnd(), n.common.width, false);
        item->fixedH = extentPx(win_.hwnd(), n.common.height, true);
        return item;
    }

private:
    template <class T, class... Args>
    T* own(Args&&... args)
    {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = p.get();
        win_.items.push_back(std::move(p));
        return raw;
    }
    template <class T, class... Args>
    T* widget(const Node& n, Args&&... args)
    {
        T* w = own<T>(std::forward<Args>(args)...);
        sink_.created(n, *w);
        return w;
    }
    void handle(const Node& n, Item* item)
    {
        if (!n.common.handle) return;
        auto w = std::make_unique<PlainWidget>(*item, win_);
        sink_.created(n, *w);
        win_.widgets.push_back(std::move(w));
    }

    Item* make(const Node& n, const spec::Box& s, HWND parent, Orientation)
    {
        auto* box = own<BoxItem>(s.dir, metrics_.spacing, s.margins ? metrics_.margin : 0);
        for (const Node& c : s.items) box->children.push_back(build(c, parent, s.dir));
        handle(n, box);
        return box;
    }
    Item* make(const Node& n, const spec::Form& s, HWND parent, Orientation)
    {
        auto* form = own<FormItem>(metrics_.formColumnGap, metrics_.rowGap);
        for (size_t i = 0; i < s.fields.size(); ++i)
        {
            FormItem::Row row;
            if (i < s.labels.size() && !s.labels[i].empty())
            {
                auto* l = own<WLabel>(win_, parent, spec::Label{s.labels[i]});
                row.label = l;
            }
            row.field = build(s.fields[i], parent, Orientation::Horizontal);
            form->rows.push_back(row);
        }
        handle(n, form);
        return form;
    }
    Item* make(const Node& n, const spec::Splitter& s, HWND parent, Orientation)
    {
        auto* sp = own<WSplitter>(win_, parent, s.dir);
        for (size_t i = 0; i < s.panes.size(); ++i)
        {
            sp->panes.push_back(build(s.panes[i], parent, s.dir));
            if (i + 1 < s.panes.size()) sp->addBar();
        }
        sink_.created(n, *sp);
        return sp;
    }
    Item* make(const Node& n, const spec::Group& s, HWND parent, Orientation)
    {
        auto* g = own<GroupItem>(win_, parent, s.title);
        if (!s.content.empty()) g->content = build(s.content.front(), parent, Orientation::Vertical);
        handle(n, g);
        return g;
    }
    Item* make(const Node& n, const spec::Scroll& s, HWND parent, Orientation)
    {
        auto* sc = own<ScrollItem>(win_, parent, s.horizontal);
        if (!s.content.empty()) sc->content = build(s.content.front(), sc->host(), Orientation::Vertical);
        handle(n, sc);
        return sc;
    }
    Item* make(const Node& n, const spec::Tabs& s, HWND parent, Orientation)
    {
        if (s.sidebar) return pagesFor(widget<WSideTabs>(n, win_, parent, s), s, parent);
        auto* t = pagesFor(widget<WTabs>(n, win_, parent, s), s, parent);
        // The tab control was created first; pages must paint above it.
        SetWindowPos(t->hwnd(), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return t;
    }
    template <class T>
    T* pagesFor(T* t, const spec::Tabs& s, HWND parent)
    {
        for (const Node& p : s.pages) t->pages.push_back(build(p, parent, Orientation::Vertical));
        t->lazy = s.lazy;
        for (const auto& l : s.lazy) t->isLazy.push_back(bool(l));
        t->buildPage = [&win = win_, &sink = sink_, parent](const Node& page)
        { return Builder(win, sink).build(page, parent, Orientation::Vertical); };
        return t;
    }
    Item* make(const Node&, const spec::Spacer&, HWND, Orientation)
    {
        auto* sp = own<SpacerItem>();
        sp->isSpacer = true;
        return sp;
    }
    Item* make(const Node&, const spec::Separator&, HWND parent, Orientation parentDir)
    {
        return own<SeparatorItem>(win_, parent,
                                  parentDir == Orientation::Vertical ? Orientation::Horizontal : Orientation::Vertical);
    }
    Item* make(const Node& n, const spec::Label& s, HWND parent, Orientation) { return widget<WLabel>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::Button& s, HWND parent, Orientation)
    {
        auto* b = widget<WButton>(n, win_, parent, s.iconOnly ? std::string() : s.text, s.flat ? BS_FLAT : 0);
        if (s.icon) b->setIcon(s.icon);
        if (s.iconOnly && n.common.tooltip.empty()) b->setTooltip(s.text);
        return b;
    }
    Item* make(const Node& n, const spec::Toolbar& s, HWND parent, Orientation)
    {
        auto* box = own<BoxItem>(Orientation::Horizontal, win_.px(2));
        for (const Node& c : s.items)
        {
            Item* it = build(c, parent, Orientation::Horizontal);
            if (std::holds_alternative<spec::Spacer>(c.spec)) it->stretch = 1;
            box->children.push_back(it);
        }
        handle(n, box);
        return box;
    }
    Item* make(const Node& n, const spec::ToolButton& s, HWND parent, Orientation)
    {
        return widget<WToolButton>(n, win_, parent, s);
    }
    Item* make(const Node& n, const spec::Slider& s, HWND parent, Orientation) { return widget<WSlider>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::Spin& s, HWND parent, Orientation) { return widget<WSpin>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::ColorButton& s, HWND parent, Orientation)
    {
        return widget<WColorButton>(n, win_, parent, s);
    }
    Item* make(const Node& n, const spec::Check& s, HWND parent, Orientation) { return widget<WCheck>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::Entry& s, HWND parent, Orientation) { return widget<WEntry>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::Select& s, HWND parent, Orientation) { return widget<WSelect>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::Combo& s, HWND parent, Orientation) { return widget<WCombo>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::TextView& s, HWND parent, Orientation)
    {
        return widget<WTextView>(n, win_, parent, s);
    }
    Item* make(const Node& n, const spec::List& s, HWND parent, Orientation) { return widget<WList>(n, win_, parent, s); }
    Item* make(const Node& n, const spec::MenuButton& s, HWND parent, Orientation)
    {
        return widget<WMenuButton>(n, win_, parent, s);
    }
    Item* make(const Node& n, const spec::ProgressBar&, HWND parent, Orientation)
    {
        return widget<WProgressBar>(n, win_, parent);
    }
    Item* make(const Node& n, const spec::DatePicker& s, HWND parent, Orientation)
    {
        return widget<WDatePicker>(n, win_, parent, s);
    }
    Item* make(const Node& n, const spec::Canvas& s, HWND parent, Orientation) { return widget<WCanvas>(n, win_, parent, s); }
    Item* make(const Node&, const spec::Buttons& s, HWND parent, Orientation)
    {
        auto* row = own<BoxItem>(Orientation::Horizontal, metrics_.spacing);
        std::vector<const spec::DialogButton*> order;
        for (const auto& b : s.buttons) order.push_back(&b);
        std::stable_sort(order.begin(), order.end(),
                         [](auto* a, auto* b) { return windowsOrder(a->role) < windowsOrder(b->role); });
        bool spaced = false;
        for (const spec::DialogButton* b : order)
        {
            if (!spaced && windowsOrder(b->role) >= 5)
            {
                row->children.push_back(own<SpacerItem>());
                spaced = true;
            }
            const std::string text = b->text.empty() ? narrow(stockLabel(b->role)) : b->text;
            const bool isDefault = b->role == DialogRole::Ok || b->role == DialogRole::Yes;
            auto* w = own<WButton>(win_, parent, text, isDefault ? BS_DEFPUSHBUTTON : 0);
            w->isDefault = isDefault;
            w->role = b->role;
            if (b->onClick)
                w->onClick(b->onClick);
            else
            {
                const DialogRole role = b->role;
                Win* win = &win_;
                w->onClick([win, role] { win->finish(role); });
            }
            sink_.created(*b, *w);
            win_.roleButtons.push_back(w);
            row->children.push_back(w);
        }
        if (!spaced) row->children.insert(row->children.begin(), own<SpacerItem>());
        return row;
    }

    Win& win_;
    BuildSink& sink_;
    Metrics metrics_;
};

// ---------------------------------------------------------------- backend

class Win32Backend : public Backend
{
public:
    static Backend* create(int*, char**)
    {
        // Per-monitor DPI awareness (v2) where the system has it.
        using Fn = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
            fn(reinterpret_cast<HANDLE>(-4) /* PER_MONITOR_AWARE_V2 */);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        enableVisualStyles();
        g_comctl.resolve();
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_PROGRESS_CLASS |
                                                 ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
                                                 ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        if (!d2dFactory() || !dwriteFactory()) return nullptr;
        return new Win32Backend;
    }

    Win32Backend()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.hInstance = instance();
        wc.lpfnWndProc = messageProc;
        wc.lpszClassName = L"anyuiMessages";
        RegisterClassExW(&wc);
        messages_ = CreateWindowExW(0, L"anyuiMessages", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance(), this);
        frames_ = std::make_unique<FrameClock>(messages_);
        g_frames = frames_.get();
    }
    ~Win32Backend() override
    {
        g_frames = nullptr;
        frames_.reset();
        DestroyWindow(messages_);
    }

    std::string_view name() const override { return "win32"; }

    std::unique_ptr<anyui::Window> createWindow(const WindowSpec& spec, anyui::Window* parent, BuildSink& sink) override
    {
        auto win = std::make_unique<Win>(spec, static_cast<Win*>(parent));
        win->setMenuBar(spec.menuBar);
        win->root = Builder(*win, sink).build(spec.content, win->hwnd(), Orientation::Vertical);
        return win;
    }

    int run() override
    {
        if (quit_) return code_;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
        {
            // Esc during a row drag cancels it, before window keys or a
            // dialog's Esc-to-close see it.
            if (g_dragging && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE)
            {
                g_dragging->cancelDrag();
                continue;
            }
            if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && m.message != WM_CHAR)
            {
                HWND top = GetAncestor(m.hwnd, GA_ROOT);
                bool consumed = false;
                for (Win* w : Win::windows())
                    if (w->hwnd() == top)
                    {
                        consumed = w->windowKey(m);
                        break;
                    }
                if (consumed) continue;
            }
            HWND top = GetAncestor(m.hwnd, GA_ROOT);
            if (top && IsDialogMessageW(top, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        return code_;
    }
    void quit(int code) override
    {
        quit_ = true;
        code_ = code;
        PostQuitMessage(code);
    }
    void post(std::function<void()> f) override
    {
        PostMessageW(messages_, WM_ANYUI_POST, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(f))));
    }

    void message(anyui::Window* parent, const MessageSpec& m, std::function<void(DialogRole)> done) override
    {
        UINT type = MB_OK;
        auto has = [&](DialogRole r) { return std::find(m.buttons.begin(), m.buttons.end(), r) != m.buttons.end(); };
        if (has(DialogRole::Yes) && has(DialogRole::No))
            type = has(DialogRole::Cancel) ? MB_YESNOCANCEL : MB_YESNO;
        else if (has(DialogRole::Ok) && has(DialogRole::Cancel))
            type = MB_OKCANCEL;
        switch (m.icon)
        {
        case MessageSpec::Icon::Info: type |= MB_ICONINFORMATION; break;
        case MessageSpec::Icon::Warning: type |= MB_ICONWARNING; break;
        case MessageSpec::Icon::Error: type |= MB_ICONERROR; break;
        case MessageSpec::Icon::Question: type |= MB_ICONQUESTION; break;
        }
        const std::wstring text = wide(m.text + (m.detail.empty() ? "" : "\n\n" + m.detail));
        const int r = MessageBoxW(parent ? static_cast<Win*>(parent)->hwnd() : nullptr, text.c_str(), wide(m.title).c_str(), type);
        if (!done) return;
        switch (r)
        {
        case IDOK: done(DialogRole::Ok); break;
        case IDYES: done(DialogRole::Yes); break;
        case IDNO: done(DialogRole::No); break;
        default: done(DialogRole::Cancel); break;
        }
    }

    void chooseFiles(anyui::Window* parent, const FileDialogSpec& f, std::function<void(std::vector<std::string>)> done) override
    {
        std::vector<std::string> out;
        const bool save = f.mode == FileDialogSpec::Mode::Save;
        Com<IFileDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
                                       reinterpret_cast<void**>(dlg.put()))))
        {
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM;
            if (f.mode == FileDialogSpec::Mode::OpenMany) opts |= FOS_ALLOWMULTISELECT;
            if (f.mode == FileDialogSpec::Mode::Folder) opts |= FOS_PICKFOLDERS;
            dlg->SetOptions(opts);
            if (!f.title.empty()) dlg->SetTitle(wide(f.title).c_str());
            std::vector<std::wstring> names, specs;
            for (const auto& flt : f.filters)
            {
                names.push_back(wide(flt.name));
                specs.push_back(wide(flt.pattern));
            }
            std::vector<COMDLG_FILTERSPEC> fs;
            for (size_t i = 0; i < names.size(); ++i) fs.push_back({names[i].c_str(), specs[i].c_str()});
            if (!fs.empty()) dlg->SetFileTypes(UINT(fs.size()), fs.data());
            // Saving: a name typed without an extension gets the chosen
            // type's (the dialog follows the type from this default).
            if (f.mode == FileDialogSpec::Mode::Save && !f.filters.empty())
            {
                const std::string& first = f.filters.front().pattern;
                const size_t dot = first.find("*.");
                const std::string ext = dot == std::string::npos ? "" : first.substr(dot + 2, first.find(';', dot) - dot - 2);
                if (!ext.empty() && ext.find_first_of("*?") == std::string::npos) dlg->SetDefaultExtension(wide(ext).c_str());
            }
            if (!f.initialPath.empty())
            {
                const std::wstring path = wide(f.initialPath);
                const DWORD attrs = GetFileAttributesW(path.c_str());
                std::wstring folder = path, file;
                if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                {
                    const size_t slash = path.find_last_of(L"\\/");
                    folder = slash == std::wstring::npos ? L"" : path.substr(0, slash);
                    file = slash == std::wstring::npos ? path : path.substr(slash + 1);
                }
                Com<IShellItem> item;
                if (!folder.empty() &&
                    SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem,
                                                          reinterpret_cast<void**>(item.put()))))
                    dlg->SetFolder(item.get());
                if (!file.empty()) dlg->SetFileName(file.c_str());
            }
            HWND owner = parent ? static_cast<Win*>(parent)->hwnd() : nullptr;
            if (SUCCEEDED(dlg->Show(owner)))
            {
                auto pathOf = [](IShellItem* item)
                {
                    PWSTR p = nullptr;
                    std::string s;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                    {
                        s = narrow(p);
                        CoTaskMemFree(p);
                    }
                    return s;
                };
                if (f.mode == FileDialogSpec::Mode::OpenMany)
                {
                    Com<IShellItemArray> items;
                    if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg.get())->GetResults(items.put())))
                    {
                        DWORD n = 0;
                        items->GetCount(&n);
                        for (DWORD i = 0; i < n; ++i)
                        {
                            Com<IShellItem> it;
                            if (SUCCEEDED(items->GetItemAt(i, it.put()))) out.push_back(pathOf(it.get()));
                        }
                    }
                }
                else
                {
                    Com<IShellItem> it;
                    if (SUCCEEDED(dlg->GetResult(it.put()))) out.push_back(pathOf(it.get()));
                }
            }
        }
        if (done) done(std::move(out));
    }

    void setClipboard(const std::string& s) override
    {
        const std::wstring w = wide(s);
        if (!OpenClipboard(messages_)) return;
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
    }

    void setClipboardImage(int w, int h, PixelFormat format, const uint8_t* pixels, size_t stride) override
    {
        // CF_DIBV5, 32-bit with straight alpha, bottom-up.
        const size_t bytes = size_t(w) * h * 4;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + bytes);
        auto* hdr = static_cast<BITMAPV5HEADER*>(GlobalLock(mem));
        *hdr = BITMAPV5HEADER{};
        hdr->bV5Size = sizeof(BITMAPV5HEADER);
        hdr->bV5Width = w;
        hdr->bV5Height = h;
        hdr->bV5Planes = 1;
        hdr->bV5BitCount = 32;
        hdr->bV5Compression = BI_BITFIELDS;
        hdr->bV5RedMask = 0x00ff0000;
        hdr->bV5GreenMask = 0x0000ff00;
        hdr->bV5BlueMask = 0x000000ff;
        hdr->bV5AlphaMask = 0xff000000;
        hdr->bV5CSType = LCS_sRGB;
        hdr->bV5SizeImage = DWORD(bytes);
        auto* dst = reinterpret_cast<uint8_t*>(hdr + 1);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* src = pixels + size_t(y) * stride;
            uint8_t* row = dst + size_t(h - 1 - y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                const uint8_t* p = src + x * 4;
                if (format == PixelFormat::Rgba8)
                {
                    row[x * 4 + 0] = p[2];
                    row[x * 4 + 1] = p[1];
                    row[x * 4 + 2] = p[0];
                    row[x * 4 + 3] = p[3];
                }
                else
                {
                    const unsigned a = p[3];
                    auto un = [a](unsigned c) { return uint8_t(a ? std::min(255u, (c * 255 + a / 2) / a) : 0); };
                    row[x * 4 + 0] = un(p[0]);
                    row[x * 4 + 1] = un(p[1]);
                    row[x * 4 + 2] = un(p[2]);
                    row[x * 4 + 3] = uint8_t(a);
                }
            }
        }
        GlobalUnlock(mem);
        if (!OpenClipboard(messages_))
        {
            GlobalFree(mem);
            return;
        }
        EmptyClipboard();
        SetClipboardData(CF_DIBV5, mem);
        CloseClipboard();
    }

    std::unique_ptr<anyui::Image> createImage(int w, int h, PixelFormat format, const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<D2DImage>(w, h, format, pixels, stride);
    }

    Subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        const UINT_PTR id = ++nextTimer_;
        timers_[id] = {std::move(fn), repeat};
        SetTimer(messages_, id, UINT(std::max(ms, 1)), nullptr);
        return Subscription(
            [this, id, alive = alive_]
            {
                if (!*alive) return;
                if (timers_.erase(id)) KillTimer(messages_, id);
            });
    }

private:
    // Common controls v6 (themed controls) without an application manifest:
    // activate the one shell32 carries for itself.
    static void enableVisualStyles()
    {
        wchar_t dir[MAX_PATH];
        GetSystemDirectoryW(dir, MAX_PATH);
        const std::wstring shell32 = std::wstring(dir) + L"\\shell32.dll";
        ACTCTXW ctx{};
        ctx.cbSize = sizeof ctx;
        ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_SET_PROCESS_DEFAULT | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
        ctx.lpSource = shell32.c_str();
        ctx.lpAssemblyDirectory = dir;
        ctx.lpResourceName = MAKEINTRESOURCEW(124);
        HANDLE h = CreateActCtxW(&ctx);
        if (h != INVALID_HANDLE_VALUE)
        {
            ULONG_PTR cookie = 0;
            ActivateActCtx(h, &cookie);
        }
    }

    static LRESULT CALLBACK messageProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Win32Backend*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (msg)
        {
        case WM_ANYUI_POST:
        {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
            (*fn)();
            return 0;
        }
        case WM_ANYUI_FRAME:
            if (self && self->frames_) self->frames_->tick();
            return 0;
        case WM_TIMER:
            if (self)
            {
                auto it = self->timers_.find(wp);
                if (it == self->timers_.end())
                {
                    KillTimer(h, wp);
                    return 0;
                }
                auto fn = it->second.fn;  // may cancel itself
                if (!it->second.repeat)
                {
                    KillTimer(h, wp);
                    self->timers_.erase(it);
                }
                fn();
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    struct Timer
    {
        std::function<void()> fn;
        bool repeat;
    };
    HWND messages_ = nullptr;
    std::unique_ptr<FrameClock> frames_;
    std::map<UINT_PTR, Timer> timers_;
    UINT_PTR nextTimer_ = 0x100;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace anyui::win32

ANYUI_BACKEND_ENTRY(win32, anyui::win32::Win32Backend)
