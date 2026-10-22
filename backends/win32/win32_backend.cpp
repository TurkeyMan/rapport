// rapport Win32 backend: native controls (common controls v6), Direct2D and
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

namespace rap::win32
{
namespace
{

constexpr UINT wm_rapport_post = WM_APP + 1;

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
constexpr UINT wm_rapport_frame = WM_APP + 2;
constexpr UINT wm_rapport_relayout = WM_APP + 3;
constexpr UINT wm_rapport_selection = WM_APP + 4;  // a list's coalesced selection change
constexpr UINT wm_rapport_fill_last = WM_APP + 5;  // a list's last column refits, once a header drag lands

HINSTANCE instance()
{
    return GetModuleHandleW(nullptr);
}

// The program's icon: its first icon resource, as Explorer shows it (the
// stock application icon when it has none), at the system size `metric`.
HICON app_icon(int metric)
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

int64_t now_ticks()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double now_seconds()
{
    static LARGE_INTEGER freq = []
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return double(now_ticks()) / double(freq.QuadPart);
}

unsigned key_modifiers()
{
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= mod_shift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= mod_ctrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= mod_alt;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= mod_meta;
    return m;
}

key_event make_key_event(WPARAM vk, LPARAM lp, bool press)
{
    key_event e;
    e.press = press;
    e.repeat = press && (lp & (1 << 30));
    e.modifiers = key_modifiers();
    switch (vk)
    {
    case VK_ESCAPE: e.key = key::escape; break;
    case VK_RETURN: e.key = key::enter; break;
    case VK_TAB: e.key = key::tab; break;
    case VK_BACK: e.key = key::backspace; break;
    case VK_DELETE: e.key = key::delete_; break;
    case VK_INSERT: e.key = key::insert; break;
    case VK_SPACE: e.key = key::space; break;
    case VK_LEFT: e.key = key::left; break;
    case VK_RIGHT: e.key = key::right; break;
    case VK_UP: e.key = key::up; break;
    case VK_DOWN: e.key = key::down; break;
    case VK_HOME: e.key = key::home; break;
    case VK_END: e.key = key::end; break;
    case VK_PRIOR: e.key = key::page_up; break;
    case VK_NEXT: e.key = key::page_down; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F12)
            e.key = key(int(key::f1) + int(vk - VK_F1));
        else if (const UINT ch = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR) & 0x7fffffff; ch)
        {
            e.key = key::character;
            e.codepoint = char32_t(towlower(wchar_t(ch)));
        }
        else
            e.key = key::other;
    }
    return e;
}

// -- comctl32 v6 --------------------------------------------------------------

// Without an application manifest the process binds comctl32 v5 at load,
// while our activation context makes the controls themselves v6. Image
// lists handed to v6 controls must come from v6 too (v5 ones have no alpha),
// so these functions are resolved from the v6 DLL once it's active.
struct comctl6
{
    decltype(&ImageList_Create) create = ImageList_Create;
    decltype(&ImageList_Destroy) destroy = ImageList_Destroy;
    decltype(&ImageList_ReplaceIcon) replace_icon = ImageList_ReplaceIcon;

    void resolve()
    {
        HMODULE m = LoadLibraryW(L"comctl32.dll");  // under the activation context: v6
        if (!m) return;
        if (auto f = reinterpret_cast<decltype(&ImageList_Create)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_Create")))) create = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_Destroy)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_Destroy")))) destroy = f;
        if (auto f = reinterpret_cast<decltype(&ImageList_ReplaceIcon)>(reinterpret_cast<void*>(GetProcAddress(m, "ImageList_ReplaceIcon")))) replace_icon = f;
    }
};
comctl6 g_comctl;

// -- handlers -----------------------------------------------------------------

// Anything that receives notifications from its HWND via the parent window
// (WM_COMMAND, WM_NOTIFY, WM_HSCROLL, WM_DRAWITEM, WM_CTLCOLOR*).
class handler
{
public:
    virtual ~handler() = default;
    virtual void command(WORD /*code*/) {}
    virtual LRESULT notify(NMHDR*, bool& handled)
    {
        handled = false;
        return 0;
    }
    virtual void scroll(WORD /*code*/) {}
    virtual bool draw_item(DRAWITEMSTRUCT*) { return false; }
};

std::unordered_map<HWND, handler*>& handlers()
{
    static std::unordered_map<HWND, handler*> map;
    return map;
}

handler* handler_of(HWND h)
{
    auto& m = handlers();
    auto it = m.find(h);
    return it == m.end() ? nullptr : it->second;
}

class window_impl;  // forward

// Messages the containers forward to their children's handlers.
bool forward_child_message(HWND, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (lp)
            if (handler* h = handler_of(reinterpret_cast<HWND>(lp)))
            {
                h->command(HIWORD(wp));
                result = 0;
                return true;
            }
        return false;
    case WM_NOTIFY:
    {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (handler* h = handler_of(hdr->hwndFrom))
        {
            bool handled = true;
            result = h->notify(hdr, handled);
            return handled;
        }
        // List headers notify via the list's own window.
        if (handler* h = handler_of(GetParent(hdr->hwndFrom)))
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
            if (handler* h = handler_of(reinterpret_cast<HWND>(lp)))
            {
                h->scroll(LOWORD(wp));
                result = 0;
                return true;
            }
        return false;
    case WM_DRAWITEM:
    {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (handler* h = handler_of(dis->hwndItem))
            if (h->draw_item(dis))
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
LRESULT CALLBACK host_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r = 0;
    if (forward_child_message(h, msg, wp, lp, r)) return r;
    if (auto* fn = reinterpret_cast<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>*>(
            GetWindowLongPtrW(h, GWLP_USERDATA)))
    {
        bool handled = false;
        r = (*fn)(h, msg, wp, lp, handled);
        if (handled) return r;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void register_classes()
{
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.hInstance = instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpfnWndProc = host_proc;
    wc.lpszClassName = L"rapportHost";
    RegisterClassExW(&wc);
}

HWND make_host(HWND parent, DWORD style = 0)
{
    register_classes();
    return CreateWindowExW(WS_EX_CONTROLPARENT, L"rapportHost", L"", WS_CHILD | WS_CLIPCHILDREN | style, 0, 0, 0,
                           0, parent, nullptr, instance(), nullptr);
}

// -- tooltips -----------------------------------------------------------------

void set_tooltip_text(HWND control, const std::string& text)
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

// -- menus --------------------------------------------------------------------

// Appends `items` to `m`; command id `base + n` runs actions[n - 1].
void fill_menu(HMENU m, const menu& items, std::vector<std::function<void()>>& actions, UINT base = 0)
{
    for (const menu_item& it : items)
    {
        if (it.is_separator())
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
            fill_menu(sub, it.children, actions, base);
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

HMENU build_menu(const menu& items, std::vector<std::function<void()>>& actions)
{
    HMENU m = CreatePopupMenu();
    fill_menu(m, items, actions);
    return m;
}

void show_popup_menu(HWND owner, const menu& items, POINT screen)
{
    if (items.empty()) return;
    std::vector<std::function<void()>> actions;
    HMENU m = build_menu(items, actions);
    const UINT id = UINT(TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, owner, nullptr));
    DestroyMenu(m);
    if (id > 0 && id <= actions.size() && actions[id - 1]) actions[id - 1]();
}

// -- window base --------------------------------------------------------------

// What every widget needs from its window: scale, fonts, relayout.
class win_context
{
public:
    virtual ~win_context() = default;
    virtual HWND hwnd() const = 0;
    virtual void relayout() = 0;
    // Lays out and repaints now, in one go (a different tab page showing).
    virtual void relayout_at_once() = 0;
    float scale() const { return scale_of(hwnd()); }
    UINT dpi() const { return dpi_of(hwnd()); }
    int px(float logical) const { return int(std::lround(logical * scale())); }
};

SIZE text_extent(HWND h, const std::wstring& s, bool multiline = false, int max_width = 0)
{
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    RECT r{0, 0, max_width > 0 ? max_width : 100000, 0};
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
struct move_batch
{
    std::map<HWND, std::vector<std::pair<HWND, RECT>>> moves;  // parent -> its children's moves
};
move_batch* g_moves = nullptr;

// A moved control repaints whole: Windows would otherwise carry its old
// pixels along and repaint only what's newly uncovered, and a label whose
// text changed as it moved shows a mix of old and new text.
constexpr UINT move_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;

bool placed_at(HWND h, int x, int y, int w, int hgt)
{
    RECT r;
    GetWindowRect(h, &r);
    MapWindowPoints(HWND_DESKTOP, GetParent(h), reinterpret_cast<POINT*>(&r), 2);
    return r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == hgt;
}

// Now, for containers that size their contents from their own client area.
void move_now(HWND h, int x, int y, int w, int hgt)
{
    if (!placed_at(h, x, y, w, hgt)) SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

void move_control(HWND h, int x, int y, int w, int hgt)
{
    if (placed_at(h, x, y, w, hgt)) return;
    if (g_moves)
        g_moves->moves[GetParent(h)].push_back({h, RECT{x, y, x + w, y + hgt}});
    else
        SetWindowPos(h, nullptr, x, y, w, hgt, move_flags);
}

// Batches the moves of a layout pass (nested passes join the outer one).
class moves_in_one_pass
{
public:
    moves_in_one_pass()
    {
        if (!g_moves) g_moves = &batch_, owner_ = true;
    }
    ~moves_in_one_pass()
    {
        if (!owner_) return;
        g_moves = nullptr;
        for (auto& [parent, list] : batch_.moves)
        {
            HDWP dwp = BeginDeferWindowPos(int(list.size()));
            for (auto& [h, r] : list)
                if (dwp)
                    dwp = DeferWindowPos(dwp, h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, move_flags);
            if (dwp) EndDeferWindowPos(dwp);
        }
    }

private:
    move_batch batch_;
    bool owner_ = false;
};

// Shows or hides a control. While its window is still hidden (being built)
// that's just the style bit: ShowWindow raises an accessibility event per
// control, milliseconds apiece whenever an automation client listens.
void show_control(HWND h, bool shown)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (bool(style & WS_VISIBLE) == shown) return;
    if (IsWindowVisible(GetAncestor(h, GA_ROOT)))
        ShowWindow(h, shown ? SW_SHOWNA : SW_HIDE);
    else
        SetWindowLongPtrW(h, GWL_STYLE, shown ? style | WS_VISIBLE : style & ~WS_VISIBLE);
}

HWND create_control(win_context& win, HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style,
                   DWORD ex_style = 0)
{
    HWND h = CreateWindowExW(ex_style, cls, text.c_str(), WS_CHILD | style, 0, 0, 0, 0, parent, nullptr,
                             instance(), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(win.dpi())), FALSE);
    return h;
}

// A widget backed by one HWND, laid out as a leaf.
template <class Base>
class leaf : public Base, public item, public handler
{
public:
    leaf(win_context& win)
        : win_(win)
    {
    }
    ~leaf() override
    {
        if (h_)
        {
            handlers().erase(h_);
            DestroyWindow(h_);
        }
    }

    void set_enabled(bool e) override { EnableWindow(h_, e); }
    void set_visible(bool v) override
    {
        if (visible == v) return;
        visible = v;
        win_.relayout();
    }
    void set_tooltip(const std::string& t) override { set_tooltip_text(h_, t); }
    // Before the window shows, SetFocus can't land: the window keeps the
    // request and focuses it when it activates.
    void focus() override
    {
        HWND root = GetAncestor(h_, GA_ROOT);
        if (IsWindowVisible(root))
            SetFocus(h_);
        else
            SetPropW(root, L"rapport-focus", h_);
    }

    void place(RECT r) override
    {
        move_control(h_, r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    void show_all(bool shown) override { show_control(h_, shown && visible); }

    HWND hwnd() const { return h_; }

protected:
    void adopt(HWND h)
    {
        h_ = h;
        handlers()[h] = this;
    }
    int px(float v) const { return win_.px(v); }

    win_context& win_;
    HWND h_ = nullptr;
};

// A container laid out as an Item but with no interface of its own; a
// handle on it gets this.
class plain_widget : public widget
{
public:
    plain_widget(item& item, win_context& win)
        : item_(item)
        , win_(win)
    {
    }
    void set_enabled(bool) override {}
    void set_visible(bool v) override
    {
        if (item_.visible == v) return;
        item_.visible = v;
        win_.relayout();
    }
    void set_tooltip(const std::string&) override {}
    void focus() override {}

private:
    item& item_;
    win_context& win_;
};

// -- layout containers --------------------------------------------------------

class box_item : public item
{
public:
    box_item(orientation dir, int spacing, int margin = 0)
        : dir_(dir)
        , spacing_(spacing)
        , margin_(margin)
    {
    }
    std::vector<item*> children;

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }

    SIZE measure() override
    {
        long main = 0, cross = 0;
        int n = 0;
        for (item* c : children)
        {
            if (!c->visible) continue;
            const SIZE s = size_of(c);
            main += along(s);
            cross = std::max(cross, across(s));
            ++n;
        }
        if (n > 1) main += spacing_ * (n - 1);
        main += 2 * margin_;
        cross += 2 * (margin_ / 2);
        return dir_ == orientation::horizontal ? SIZE{main, cross} : SIZE{cross, main};
    }

    void place(RECT r) override
    {
        if (dir_ == orientation::horizontal)
            InflateRect(&r, -margin_, -(margin_ / 2));
        else
            InflateRect(&r, -(margin_ / 2), -margin_);
        std::vector<item*> shown;
        for (item* c : children)
            if (c->visible) shown.push_back(c);
        if (shown.empty()) return;
        const long total = dir_ == orientation::horizontal ? r.right - r.left : r.bottom - r.top;
        const long cross_total = dir_ == orientation::horizontal ? r.bottom - r.top : r.right - r.left;
        std::vector<long> sizes;
        long used = spacing_ * long(shown.size() - 1);
        int stretch_sum = 0;
        for (item* c : shown)
        {
            sizes.push_back(along(size_of(c)));
            used += sizes.back();
            stretch_sum += c->stretch;
        }
        long spare = std::max(0L, total - used);
        if (stretch_sum > 0)
        {
            long given = 0;
            int seen = 0;
            for (size_t i = 0; i < shown.size(); ++i)
            {
                if (!shown[i]->stretch) continue;
                seen += shown[i]->stretch;
                const long target = spare * seen / stretch_sum;
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
        long pos = dir_ == orientation::horizontal ? r.left : r.top;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            item* c = shown[i];
            const SIZE nat = size_of(c);
            long cross_size = across(nat), cross_pos;
            const bool fill = c->fills() || (dir_ == orientation::vertical && c->expands_in_form());
            if (fill || cross_size > cross_total) cross_size = cross_total;
            // Single-line things sit centred across a row; in a column, left-aligned.
            if (dir_ == orientation::horizontal)
                cross_pos = r.top + (cross_total - cross_size) / 2;
            else
                cross_pos = r.left;
            RECT cr = dir_ == orientation::horizontal ? RECT{pos, cross_pos, pos + sizes[i], cross_pos + cross_size}
                                                      : RECT{cross_pos, pos, cross_pos + cross_size, pos + sizes[i]};
            c->place(cr);
            pos += sizes[i] + spacing_;
        }
    }

    void show_all(bool shown) override
    {
        for (item* c : children) c->show_all(shown && visible);
    }

private:
    SIZE size_of(item* c)
    {
        SIZE s = c->natural();
        if (c->fixed_w > 0) s.cx = c->fixed_w;
        if (c->fixed_h > 0) s.cy = c->fixed_h;
        return s;
    }
    long along(SIZE s) const { return dir_ == orientation::horizontal ? s.cx : s.cy; }
    long across(SIZE s) const { return dir_ == orientation::horizontal ? s.cy : s.cx; }

    orientation dir_;
    int spacing_;
    int margin_;
};

// Labels right-aligned in the first column, fields in the second; an empty
// label lets its field span both.
class form_item : public item
{
public:
    form_item(int gap, int row_gap)
        : gap_(gap)
        , row_gap_(row_gap)
    {
    }
    struct row
    {
        item* label = nullptr;  // null: field spans both columns
        item* field = nullptr;
    };
    std::vector<row> rows;

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }

    SIZE measure() override
    {
        long label_w = 0, field_w = 0, span_w = 0, h = 0;
        int n = 0;
        for (row& r : rows)
        {
            if (!r.field->visible) continue;
            const SIZE f = size_of(r.field);
            long row_h = f.cy;
            if (r.label)
            {
                const SIZE l = r.label->natural();
                label_w = std::max(label_w, l.cx);
                field_w = std::max(field_w, f.cx);
                row_h = std::max(row_h, l.cy);
            }
            else
                span_w = std::max(span_w, f.cx);
            h += row_h;
            ++n;
        }
        if (n > 1) h += row_gap_ * (n - 1);
        const long both = label_w ? label_w + gap_ + field_w : field_w;
        return {std::max(both, span_w), h};
    }

    void place(RECT r) override
    {
        long label_w = 0;
        for (row& row : rows)
            if (row.label && row.field->visible) label_w = std::max(label_w, row.label->natural().cx);
        long y = r.top;
        for (row& row : rows)
        {
            if (!row.field->visible)
            {
                if (row.label) row.label->place({0, 0, 0, 0});
                continue;
            }
            const SIZE f = size_of(row.field);
            const long fx = row.label ? r.left + label_w + gap_ : r.left;
            long row_h = f.cy;
            if (row.label) row_h = std::max(row_h, row.label->natural().cy);
            long fw = row.field->expands_in_form() || row.field->stretch ? r.right - fx : std::min(f.cx, r.right - fx);
            row.field->place({fx, y + (row_h - f.cy) / 2, fx + fw, y + (row_h - f.cy) / 2 + f.cy});
            if (row.label)
            {
                const SIZE l = row.label->natural();
                row.label->place({r.left + label_w - l.cx, y + (row_h - l.cy) / 2, r.left + label_w, y + (row_h - l.cy) / 2 + l.cy});
            }
            y += row_h + row_gap_;
        }
    }

    void show_all(bool shown) override
    {
        for (row& r : rows)
        {
            r.field->show_all(shown && visible);
            if (r.label) r.label->show_all(shown && visible && r.field->visible);
        }
    }

private:
    SIZE size_of(item* c)
    {
        SIZE s = c->natural();
        if (c->fixed_w > 0) s.cx = c->fixed_w;
        if (c->fixed_h > 0) s.cy = c->fixed_h;
        return s;
    }
    int gap_, row_gap_;
};

class spacer_item : public item
{
public:
    spacer_item() { stretch = 1; }
    SIZE measure() override { return {0, 0}; }
    void place(RECT) override {}
    void show_all(bool) override {}
};

// -- simple controls ----------------------------------------------------------

class label_impl : public leaf<rap::label>
{
public:
    label_impl(win_context& win, HWND parent, const spec::label& s)
        : leaf(win)
        , ellipsize_(s.ellipsize)
    {
        if (s.selectable)
            adopt(create_control(win, parent, L"EDIT", wide(s.text), ES_READONLY | ES_AUTOHSCROLL));
        else
            adopt(create_control(win, parent, L"STATIC", wide(s.text),
                                SS_LEFT | SS_NOPREFIX | (s.ellipsize ? SS_ENDELLIPSIS : 0)));
    }
    void set_text(const std::string& t) override
    {
        SetWindowTextW(h_, wide(t).c_str());
        if (!ellipsize_) win_.relayout();
    }
    SIZE measure() override
    {
        // Line breaks in the text are lines (measured single-line, the
        // label asked for one line's height and showed only the first).
        const std::wstring text = window_text(h_);
        SIZE s = text_extent(h_, text, text.find(L'\n') != std::wstring::npos);
        if (ellipsize_) s.cx = std::min<long>(s.cx, px(40));
        return s;
    }
    // Its natural width is a token one, so it fills across a column.
    bool expands_in_form() const override { return ellipsize_; }

private:
    bool ellipsize_;
};

class button_impl : public leaf<rap::button>
{
public:
    button_impl(win_context& win, HWND parent, const std::string& text, DWORD extra = 0)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"BUTTON", mnemonic(text), BS_PUSHBUTTON | WS_TABSTOP | extra));
    }
    ~button_impl() override
    {
        if (images_) g_comctl.destroy(images_);
    }
    void set_text(const std::string& t) override
    {
        SetWindowTextW(h_, mnemonic(t).c_str());
        win_.relayout();
    }
    void set_icon(const icon& icon) override
    {
        icon_ = icon;
        if (images_) g_comctl.destroy(images_);
        images_ = nullptr;
        if (!icon) return;
        const int size = px(16);
        images_ = g_comctl.create(size, size, ILC_COLOR32, 1, 0);
        if (HICON hi = render_icon_handle(icon, size, win_.scale()))
        {
            g_comctl.replace_icon(images_, -1, hi);
            DestroyIcon(hi);
        }
        BUTTON_IMAGELIST bil{images_, {px(2), 0, px(2), 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
        SendMessageW(h_, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
        win_.relayout();
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == BN_CLICKED && click_) click_();
    }
    SIZE measure() override
    {
        const SIZE t = text_extent(h_, window_text(h_));
        const bool has_text = GetWindowTextLengthW(h_) > 0;
        long w = (has_text ? t.cx : 0) + px(has_text ? 24 : 10) + (images_ ? px(has_text ? 20 : 16) : 0);
        if (has_text) w = std::max<long>(w, px(75));
        return {w, std::max<long>(px(23), t.cy + px(8))};
    }

    bool is_default = false;
    dialog_role role = dialog_role::none;

private:
    std::function<void()> click_;
    icon icon_;
    HIMAGELIST images_ = nullptr;
};

class check_impl : public leaf<rap::check>
{
public:
    check_impl(win_context& win, HWND parent, const spec::check& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"BUTTON", mnemonic(s.text), BS_AUTO3STATE | WS_TABSTOP));
        set_state(s.state);
    }
    bool checked() const override { return SendMessageW(h_, BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void set_checked(bool c) override { set_state(c ? check_state::checked : check_state::unchecked); }
    check_state state() const override
    {
        switch (SendMessageW(h_, BM_GETCHECK, 0, 0))
        {
        case BST_CHECKED: return check_state::checked;
        case BST_INDETERMINATE: return check_state::indeterminate;
        default: return check_state::unchecked;
        }
    }
    void set_state(check_state s) override
    {
        SendMessageW(h_, BM_SETCHECK,
                     s == check_state::checked         ? BST_CHECKED
                     : s == check_state::indeterminate ? BST_INDETERMINATE
                                                      : BST_UNCHECKED,
                     0);
    }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        // Clicks only toggle on/off: a mixed box goes to checked, never back to mixed.
        if (SendMessageW(h_, BM_GETCHECK, 0, 0) == BST_INDETERMINATE) SendMessageW(h_, BM_SETCHECK, BST_UNCHECKED, 0);
        if (toggled_) toggled_(checked());
    }
    SIZE measure() override
    {
        const SIZE t = text_extent(h_, window_text(h_));
        return {t.cx + px(GetWindowTextLengthW(h_) ? 22 : 14), std::max<long>(t.cy, px(17))};
    }

private:
    std::function<void(bool)> toggled_;
};

// Edit controls report Enter only to a subclass.
LRESULT CALLBACK enter_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
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

class entry_impl : public leaf<rap::entry>
{
public:
    entry_impl(win_context& win, HWND parent, const spec::entry& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"EDIT", wide(s.text), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE));
        set_placeholder(s.placeholder);
        SetWindowSubclass(h_, enter_subclass, 1, reinterpret_cast<DWORD_PTR>(&activate_));
    }
    std::string text() const override { return narrow(window_text(h_)); }
    void set_text(const std::string& t) override
    {
        quiet_ = true;
        SetWindowTextW(h_, wide(t).c_str());
        quiet_ = false;
    }
    void set_placeholder(const std::string& p) override
    {
        SendMessageW(h_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(wide(p).c_str()));
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_CHANGE && !quiet_ && changed_) changed_(text());
    }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
        return {m.cx * 20 + px(8), m.cy + px(9)};
    }

private:
    bool quiet_ = false;
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class drop_down_impl : public leaf<rap::drop_down>
{
public:
    drop_down_impl(win_context& win, HWND parent, const spec::drop_down& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP));
        current_ = s.current;
        set_items(s.items);
    }
    // Until it's opened or focused the box holds just the shown item: each
    // item added costs an accessibility event, milliseconds apiece whenever
    // an automation client listens, and a dialog has many of these.
    void set_items(std::vector<std::string> items) override
    {
        items_ = std::move(items);
        filled_ = false;
        show_current_only();
        win_.relayout();
    }
    int current() const override { return filled_ ? int(SendMessageW(h_, CB_GETCURSEL, 0, 0)) : current_; }
    void set_current(int i) override
    {
        current_ = i;
        if (filled_)
            SendMessageW(h_, CB_SETCURSEL, WPARAM(i), 0);
        else
            show_current_only();
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
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
        for (const std::string& i : items_) w = std::max(w, text_extent(h_, wide(i)).cx);
        return {w + px(32), font_metrics(win_.dpi()).cy + px(10)};
    }

private:
    void show_current_only()
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

class combo_impl : public leaf<rap::combo>
{
public:
    combo_impl(win_context& win, HWND parent, const spec::combo& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP));
        set_items(s.items);
        set_text(s.text);
        COMBOBOXINFO info{};
        info.cbSize = sizeof info;
        if (GetComboBoxInfo(h_, &info) && info.hwndItem)
            SetWindowSubclass(info.hwndItem, enter_subclass, 1, reinterpret_cast<DWORD_PTR>(&activate_));
    }
    void set_items(std::vector<std::string> items) override
    {
        const std::wstring keep = window_text(h_);
        SendMessageW(h_, CB_RESETCONTENT, 0, 0);
        for (const std::string& i : items) SendMessageW(h_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(i).c_str()));
        SetWindowTextW(h_, keep.c_str());
    }
    std::string text() const override { return narrow(window_text(h_)); }
    void set_text(const std::string& t) override
    {
        quiet_ = true;
        SetWindowTextW(h_, wide(t).c_str());
        quiet_ = false;
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }
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
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
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
class text_view_impl : public leaf<rap::text_view>
{
public:
    text_view_impl(win_context& win, HWND parent, const spec::text_view& s)
        : leaf(win)
        , mono_(s.monospace)
    {
        static HMODULE lib = LoadLibraryW(L"Msftedit.dll");
        (void)lib;
        adopt(create_control(win, parent, L"RICHEDIT50W", L"",
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP | ES_NOHIDESEL |
                                (s.wrap ? 0 : ES_AUTOHSCROLL | WS_HSCROLL) | (s.read_only ? ES_READONLY : 0),
                            WS_EX_CLIENTEDGE));
        SendMessageW(h_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        // RichEdit holds 32,767 characters unless told otherwise, and cuts
        // longer text off silently (a big diff showed only its start).
        SendMessageW(h_, EM_EXLIMITTEXT, 0, 0x7ffffffe);
        SendMessageW(h_, EM_SETBKGNDCOLOR, 0, LPARAM(GetSysColor(COLOR_WINDOW)));
        if (mono_) SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(win.dpi(), false, true)), FALSE);
        SetWindowSubclass(h_, wheel_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        // No error beep for typing into a read-only view (RichEdit allows
        // beeps by default); that it can't be edited is plain to see.
        SendMessageW(h_, EM_SETEDITSTYLE, 0, SES_ALLOWBEEPS);
        set_text(s.text, {});
    }

    // RichEdit animates wheel scrolling: each notch glides, and quick
    // wheeling queues glides, so the view lags behind the wheel. Scroll by
    // whole lines at once instead, as many per notch as the system setting
    // says (a page when it says so); high-resolution wheels send fractions
    // of a notch, which add up.
    static LRESULT CALLBACK wheel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        if (msg != WM_MOUSEWHEEL || (GET_KEYSTATE_WPARAM(wp) & (MK_CONTROL | MK_SHIFT)))
            return DefSubclassProc(h, msg, wp, lp);  // Ctrl zooms, Shift scrolls sideways
        auto* self = reinterpret_cast<text_view_impl*>(data);
        UINT per_notch = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &per_notch, 0);
        if (per_notch == WHEEL_PAGESCROLL)
        {
            SendMessageW(h, WM_VSCROLL, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            return 0;
        }
        self->wheel_ += GET_WHEEL_DELTA_WPARAM(wp) * int(per_notch);
        const int lines = self->wheel_ / WHEEL_DELTA;
        self->wheel_ -= lines * WHEEL_DELTA;
        if (lines) SendMessageW(h, EM_LINESCROLL, 0, -lines);
        return 0;
    }
    std::string text() const override
    {
        std::wstring w = window_text(h_);
        for (wchar_t& c : w)
            if (c == L'\r') c = L'\n';
        return narrow(w);
    }
    void set_text(const std::string& t, std::vector<span> spans) override
    {
        quiet_ = true;
        if (spans.empty())
            SetWindowTextW(h_, wide(t).c_str());
        else
            stream_rtf(rtf(t, std::move(spans)));
        SendMessageW(h_, EM_SETSEL, 0, 0);
        quiet_ = false;
    }
    void append(const std::string& t, const text_style& style) override
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
    void set_read_only(bool r) override { SendMessageW(h_, EM_SETREADONLY, r, 0); }
    std::pair<size_t, size_t> selection() const override
    {
        CHARRANGE cr{};
        SendMessageW(h_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        const std::string t = text();
        return {bytes_at(t, cr.cpMin), bytes_at(t, cr.cpMax)};
    }
    void select(size_t begin, size_t end) override
    {
        const std::vector<long> pos = units(text(), {begin, end});
        CHARRANGE cr{pos[0], pos[1]};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        SendMessageW(h_, EM_SCROLLCARET, 0, 0);
    }
    void on_changed(std::function<void()> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code == EN_CHANGE && !quiet_ && changed_) changed_();
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
        return {m.cx * 40, m.cy * 6 + px(8)};
    }

private:
    // RichEdit positions count UTF-16 units, with CR LF as one.
    static int units_of(const std::string& t, size_t i)
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
            for (; at < target; ++at) n += units_of(t, at);
            out[i] = n;
        }
        return out;
    }
    // A RichEdit position -> the UTF-8 byte offset where it falls.
    static size_t bytes_at(const std::string& t, long pos)
    {
        long n = 0;
        size_t at = 0;
        for (; at < t.size() && n < pos; ++at) n += units_of(t, at);
        while (at < t.size() && (uint8_t(t[at]) & 0xc0) == 0x80) ++at;  // finish the character
        return at;
    }
    // Styled text goes in as one RTF stream. Formatting each span with
    // EM_SETCHARFORMAT made RichEdit lay out again per span: seconds for a
    // diff of a few thousand coloured lines.
    std::string rtf(const std::string& t, std::vector<span> spans) const
    {
        std::sort(spans.begin(), spans.end(), [](const span& a, const span& b) { return a.begin < b.begin; });
        // The control's own font, so styled text looks like plain text.
        LOGFONTW lf{};
        GetObjectW(ui_font(win_.dpi(), false, mono_), sizeof lf, &lf);
        const int half_points = lf.lfHeight ? int(std::lround(std::abs(lf.lfHeight) * 144.0 / win_.dpi())) : 18;
        std::vector<COLORREF> colours;
        auto colour_index = [&](const paint& p)
        {
            const COLORREF c = color_ref(std::holds_alternative<color>(p) ? std::get<color>(p) : theme_color_of(std::get<theme_color>(p)));
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
        for (const span& sp : spans)
        {
            const size_t begin = std::max(sp.begin, at), end = std::min(sp.end, t.size());
            if (end <= begin) continue;
            text(at, begin);
            body += '{';
            if (sp.style.fg) body += "\\cf" + std::to_string(colour_index(*sp.style.fg));
            if (sp.style.bg) body += "\\highlight" + std::to_string(colour_index(*sp.style.bg));
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
        out += "}\\f0\\fs" + std::to_string(half_points) + ' ' + body + '}';
        return out;
    }
    void stream_rtf(const std::string& rtf)
    {
        struct rtf_reader
        {
            const std::string* data;
            size_t at = 0;
        } reader{&rtf};
        EDITSTREAM es{};
        es.dwCookie = reinterpret_cast<DWORD_PTR>(&reader);
        es.pfnCallback = [](DWORD_PTR cookie, LPBYTE buf, LONG size, LONG* got) -> DWORD
        {
            auto* r = reinterpret_cast<rtf_reader*>(cookie);
            const size_t n = std::min<size_t>(size_t(size), r->data->size() - r->at);
            std::memcpy(buf, r->data->data() + r->at, n);
            r->at += n;
            *got = LONG(n);
            return 0;
        };
        SendMessageW(h_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
    }

    void format(long a, long b, const text_style& st)
    {
        if (b <= a) return;
        CHARRANGE cr{a, b};
        SendMessageW(h_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_BOLD | CFM_ITALIC;
        cf.dwEffects = (st.bold ? CFE_BOLD : 0) | (st.italic ? CFE_ITALIC : 0);
        auto colour = [](const paint& p) { return std::holds_alternative<color>(p) ? std::get<color>(p) : theme_color_of(std::get<theme_color>(p)); };
        if (st.fg)
        {
            cf.dwMask |= CFM_COLOR;
            cf.crTextColor = color_ref(colour(*st.fg));
        }
        if (st.bg)
        {
            cf.dwMask |= CFM_BACKCOLOR;
            cf.crBackColor = color_ref(colour(*st.bg));
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

class separator_item : public item
{
public:
    separator_item(win_context& win, HWND parent, orientation line_dir)
        : win_(win)
        , dir_(line_dir)
    {
        h_ = create_control(win, parent, L"STATIC", L"",
                           line_dir == orientation::horizontal ? SS_ETCHEDHORZ : SS_ETCHEDVERT);
    }
    ~separator_item() override { DestroyWindow(h_); }
    SIZE measure() override
    {
        const int t = std::max(2, win_.px(2)), pad = win_.px(4);
        return dir_ == orientation::horizontal ? SIZE{pad, t + 2 * pad} : SIZE{t + 2 * pad, pad};
    }
    bool fills() const override { return true; }
    void place(RECT r) override
    {
        const int pad = win_.px(4);
        if (dir_ == orientation::horizontal)
            move_control(h_, r.left, (r.top + r.bottom) / 2, r.right - r.left, 2);
        else
            move_control(h_, (r.left + r.right) / 2, r.top + pad, 2, r.bottom - r.top - 2 * pad);
    }
    void show_all(bool shown) override { show_control(h_, shown && visible); }

private:
    win_context& win_;
    orientation dir_;
    HWND h_;
};

// A titled frame around one item.
class group_item : public item
{
public:
    group_item(win_context& win, HWND parent, const std::string& title)
        : win_(win)
    {
        // Created first so the controls inside sit above it in z-order.
        h_ = create_control(win, parent, L"BUTTON", wide(title), BS_GROUPBOX, WS_EX_TRANSPARENT);
    }
    ~group_item() override { DestroyWindow(h_); }
    item* content = nullptr;
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        const SIZE c = content ? content->natural() : SIZE{0, 0};
        const SIZE t = text_extent(h_, window_text(h_));
        return {std::max(c.cx, t.cx + win_.px(16)) + 2 * pad(), c.cy + top() + pad()};
    }
    void place(RECT r) override
    {
        move_control(h_, r.left, r.top, r.right - r.left, r.bottom - r.top);
        if (content) content->place({r.left + pad(), r.top + top(), r.right - pad(), r.bottom - pad()});
    }
    void show_all(bool shown) override
    {
        show_control(h_, shown && visible);
        if (content) content->show_all(shown && visible);
    }

private:
    int pad() const { return win_.px(9); }
    int top() const { return font_metrics(win_.dpi()).cy + win_.px(4); }
    win_context& win_;
    HWND h_;
};

class progress_bar_impl : public leaf<rap::progress_bar>
{
public:
    progress_bar_impl(win_context& win, HWND parent)
        : leaf(win)
    {
        adopt(create_control(win, parent, PROGRESS_CLASSW, L"", 0));
        SendMessageW(h_, PBM_SETRANGE32, 0, 1000);
    }
    void set_fraction(float f) override
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

class date_picker_impl : public leaf<rap::date_picker>
{
public:
    date_picker_impl(win_context& win, HWND parent, const spec::date_picker& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT | WS_TABSTOP));
        set_date(s.date);
    }
    rap::date date() const override
    {
        SYSTEMTIME st{};
        SendMessageW(h_, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&st));
        return rap::date{st.wYear, st.wMonth, st.wDay};
    }
    void set_date(rap::date d) override
    {
        SYSTEMTIME st{};
        st.wYear = WORD(d.year);
        st.wMonth = WORD(d.month);
        st.wDay = WORD(d.day);
        quiet_ = true;
        SendMessageW(h_, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&st));
        quiet_ = false;
    }
    void on_changed(std::function<void(rap::date)> f) override { changed_ = std::move(f); }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == DTN_DATETIMECHANGE;
        if (handled && !quiet_ && changed_) changed_(date());
        return 0;
    }
    SIZE measure() override { return {font_metrics(win_.dpi()).cx * 12 + px(30), font_metrics(win_.dpi()).cy + px(10)}; }

private:
    bool quiet_ = false;
    std::function<void(rap::date)> changed_;
};

// A number in an edit with an up/down control beside it (the edit is the
// HWND laid out; the arrows ride on its right edge).
class spin_impl : public leaf<rap::spin>
{
public:
    spin_impl(win_context& win, HWND parent, const spec::spin& s)
        : leaf(win)
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : 1)
        , digits_(std::max(0, s.digits))
        , value_(s.value)
    {
        adopt(create_control(win, parent, L"EDIT", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE));
        updown_ = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | UDS_ARROWKEYS | UDS_ALIGNRIGHT, 0, 0, 0, 0, parent,
                                  nullptr, instance(), nullptr);
        SendMessageW(updown_, UDM_SETBUDDY, reinterpret_cast<WPARAM>(h_), 0);
        SendMessageW(updown_, UDM_SETRANGE32, 0, 100);  // we step ourselves
        SendMessageW(updown_, UDM_SETPOS32, 0, 50);
        handlers()[updown_] = this;
        show();
    }
    ~spin_impl() override
    {
        handlers().erase(updown_);
        DestroyWindow(updown_);
    }
    double value() const override { return value_; }
    void set_value(double v) override
    {
        value_ = std::clamp(v, min_, max_);
        show();
    }
    void set_range(double mn, double mx) override
    {
        min_ = mn;
        max_ = mx;
        set_value(value_);
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }
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
        leaf::place({r.left, r.top, r.right - arrows, r.bottom});
        move_control(updown_, r.right - arrows, r.top, arrows, r.bottom - r.top);
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        show_control(updown_, shown && visible);
    }
    void set_enabled(bool e) override
    {
        leaf::set_enabled(e);
        EnableWindow(updown_, e);
    }
    SIZE measure() override
    {
        const SIZE m = font_metrics(win_.dpi());
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
        const std::wstring t = window_text(h_);
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
class slider_impl : public leaf<rap::slider>
{
public:
    slider_impl(win_context& win, HWND parent, const spec::slider& s)
        : leaf(win)
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : (s.max - s.min) / 1000)
        , show_value_(s.show_value)
        , digits_(s.digits)
    {
        adopt(create_control(win, parent, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP));
        marks_ = s.marks;
        apply_range();
        SendMessageW(h_, TBM_SETLINESIZE, 0, 1);
        SendMessageW(h_, TBM_SETPAGESIZE, 0, LPARAM(std::max(1.0, std::round(s.page / step_))));
        set_value(s.value);
        if (show_value_) value_label_ = create_control(win, parent, L"STATIC", L"", SS_RIGHT);
        update_label();
    }
    ~slider_impl() override
    {
        if (value_label_) DestroyWindow(value_label_);
    }
    double value() const override { return min_ + double(SendMessageW(h_, TBM_GETPOS, 0, 0)) * step_; }
    void set_value(double v) override
    {
        SendMessageW(h_, TBM_SETPOS, TRUE, LPARAM(std::lround((std::clamp(v, min_, max_) - min_) / step_)));
        update_label();
    }
    void set_range(double mn, double mx) override
    {
        const double v = value();
        min_ = mn;
        max_ = mx;
        apply_range();
        set_value(v);
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }
    void scroll(WORD) override
    {
        update_label();
        if (changed_) changed_(value());
    }
    void place(RECT r) override
    {
        const int label = value_label_ ? label_width() : 0;
        leaf::place({r.left, r.top, r.right - label, r.bottom});
        if (value_label_)
            move_control(value_label_, r.right - label, r.top + (r.bottom - r.top - font_metrics(win_.dpi()).cy) / 2, label, font_metrics(win_.dpi()).cy);
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        if (value_label_) show_control(value_label_, shown && visible);
    }
    SIZE measure() override { return {px(140) + (value_label_ ? label_width() : 0), px(marks_.empty() ? 26 : 30)}; }

private:
    int label_width() const { return font_metrics(win_.dpi()).cx * (digits_ + 5); }
    void apply_range()
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
    void update_label()
    {
        if (!value_label_) return;
        wchar_t buf[64];
        swprintf(buf, 64, L"%.*f", digits_, value());
        SetWindowTextW(value_label_, buf);
    }

    double min_, max_, step_;
    bool show_value_;
    int digits_;
    std::vector<std::pair<double, std::string>> marks_;
    HWND value_label_ = nullptr;
    std::function<void(double)> changed_;
};

// An owner-drawn button showing a swatch; clicking opens ChooseColor.
class color_button_impl : public leaf<rap::color_button>
{
public:
    color_button_impl(win_context& win, HWND parent, const spec::color_button& s)
        : leaf(win)
        , color_(s.color)
    {
        adopt(create_control(win, parent, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP));
    }
    rap::color color() const override { return color_; }
    void set_color(rap::color c) override
    {
        color_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    void on_changed(std::function<void(rap::color)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED) return;
        static COLORREF custom[16];
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = GetAncestor(h_, GA_ROOT);
        cc.rgbResult = color_ref(color_);
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (!ChooseColorW(&cc)) return;
        color_ = rap::color{GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult), color_.a};
        InvalidateRect(h_, nullptr, TRUE);
        if (changed_) changed_(color_);
    }
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        DrawFrameControl(d->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((d->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        InflateRect(&r, -px(5), -px(5));
        HBRUSH b = CreateSolidBrush(color_ref(color_));
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
    rap::color color_;
    std::function<void(rap::color)> changed_;
};

class menu_button_impl : public leaf<rap::menu_button>
{
public:
    menu_button_impl(win_context& win, HWND parent, const spec::menu_button& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, L"BUTTON", mnemonic(s.text) + L"  ▾", BS_PUSHBUTTON | WS_TABSTOP));
        text_ = s.text;
        if (s.icon) set_icon(s.icon);
        set_pushed(s.pushed);
    }
    ~menu_button_impl() override
    {
        if (images_) g_comctl.destroy(images_);
    }
    void set_text(const std::string& t) override
    {
        text_ = t;
        SetWindowTextW(h_, (mnemonic(t) + L"  ▾").c_str());
        win_.relayout();
    }
    void set_icon(const icon& icon) override
    {
        if (images_) g_comctl.destroy(images_);
        images_ = nullptr;
        if (!icon) return;
        const int size = px(16);
        images_ = g_comctl.create(size, size, ILC_COLOR32, 1, 0);
        if (HICON hi = render_icon_handle(icon, size, win_.scale()))
        {
            g_comctl.replace_icon(images_, -1, hi);
            DestroyIcon(hi);
        }
        BUTTON_IMAGELIST bil{images_, {px(2), 0, px(2), 0}, BUTTON_IMAGELIST_ALIGN_LEFT};
        SendMessageW(h_, BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&bil));
    }
    void set_pushed(bool p) override
    {
        pushed_ = p;
        SendMessageW(h_, BM_SETSTATE, p, 0);
    }
    void on_menu(std::function<menu()> f) override { menu_ = std::move(f); }
    void command(WORD code) override
    {
        if (code != BN_CLICKED || !menu_) return;
        RECT r;
        GetWindowRect(h_, &r);
        SendMessageW(h_, BM_SETSTATE, TRUE, 0);
        show_popup_menu(GetAncestor(h_, GA_ROOT), menu_(), POINT{r.left, r.bottom});
        SendMessageW(h_, BM_SETSTATE, pushed_, 0);
    }
    SIZE measure() override
    {
        const SIZE t = text_extent(h_, window_text(h_));
        return {t.cx + px(24) + (images_ ? px(20) : 0), std::max<long>(px(23), t.cy + px(8))};
    }

private:
    std::string text_;
    bool pushed_ = false;
    std::function<menu()> menu_;
    HIMAGELIST images_ = nullptr;
};

// Flat, owner-drawn toolbar button: icon only, hover and pressed/checked
// backgrounds, tooltip with the label.
class tool_button_impl : public leaf<rap::tool_button>
{
public:
    tool_button_impl(win_context& win, HWND parent, const spec::tool_button& s)
        : leaf(win)
        , icon_(s.icon)
        , toggle_(s.toggle)
        , checked_(s.checked)
    {
        adopt(create_control(win, parent, L"BUTTON", wide(s.label), BS_OWNERDRAW));
        set_tooltip_text(h_, s.label);
        SetWindowSubclass(h_, hover_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    void set_icon(const icon& icon) override
    {
        icon_ = icon;
        InvalidateRect(h_, nullptr, TRUE);
    }
    bool checked() const override { return toggle_ && checked_; }
    void set_checked(bool c) override
    {
        if (!toggle_) return;
        checked_ = c;
        InvalidateRect(h_, nullptr, TRUE);
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }
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
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        RECT r = d->rcItem;
        FillRect(d->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
        const bool pressed = d->itemState & ODS_SELECTED;
        const bool disabled = d->itemState & ODS_DISABLED;
        if (pressed || checked_ || hot_)
        {
            const color base = theme_color_of(theme_color::highlight);
            const float a = pressed ? 0.35f : checked_ ? 0.25f : 0.12f;
            const color face = theme_color_of(theme_color::window);
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
        draw_icon_on_dc(d->hDC, ir, icon_, win_.scale(),
                     disabled ? theme_color_of(theme_color::placeholder_text) : theme_color_of(theme_color::window_text));
        return true;
    }
    SIZE measure() override { return {px(28), px(28)}; }

private:
    static LRESULT CALLBACK hover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<tool_button_impl*>(data);
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

    icon icon_;
    bool toggle_, checked_;
    bool hot_ = false;
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
};

// -- list ---------------------------------------------------------------------

class list_impl;
list_impl* g_dragging = nullptr;  // the list with a row drag in progress, for Esc

// A virtual report-style list_view: the model is asked for visible cells only.
class list_impl : public leaf<rap::list>, public list_observer
{
public:
    list_impl(win_context& win, HWND parent, const spec::list& s)
        : leaf(win)
        , columns_(s.columns)
    {
        DWORD style = LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP;
        if (s.selection == selection_mode::single || s.selection == selection_mode::none) style |= LVS_SINGLESEL;
        if (!s.headers) style |= LVS_NOCOLUMNHEADER;
        adopt(create_control(win, parent, WC_LISTVIEWW, L"", style, WS_EX_CLIENTEDGE));
        ListView_SetExtendedListViewStyle(h_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        visible_.assign(columns_.size(), true);
        for (size_t i = 0; i < columns_.size(); ++i)
        {
            const column& c = columns_[i];
            std::wstring title = wide(c.title);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.pszText = title.data();
            col.fmt = c.align == align::end ? LVCFMT_RIGHT : c.align == align::center ? LVCFMT_CENTER : LVCFMT_LEFT;
            int w = extent_px(win.hwnd(), c.width, false);
            col.cx = w > 0 ? w : px(100);
            widths_.push_back(col.cx);
            ListView_InsertColumn(h_, int(i), &col);
        }
        set_model(s.model);
        soft_ = s.soft_selection;
        if (s.auto_scroll) SetWindowSubclass(h_, auto_scroll_proc, 7, reinterpret_cast<DWORD_PTR>(this));
        if (soft_) SetWindowSubclass(h_, hot_proc, 3, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(h_, selection_proc, 4, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(h_, size_proc, 8, reinterpret_cast<DWORD_PTR>(this));
    }
    ~list_impl() override
    {
        if (anchor_) DestroyWindow(anchor_);
        if (g_dragging == this) g_dragging = nullptr;
        if (model_) model_->remove_observer(this);
    }

    void set_model(list_model* m) override
    {
        if (model_) model_->remove_observer(this);
        model_ = m;
        if (model_) model_->add_observer(this);
        model_reset();
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
    void set_current(size_t row) override { ListView_SetItemState(h_, int(row), LVIS_FOCUSED, LVIS_FOCUSED); }
    void scroll_to(size_t row) override { ListView_EnsureVisible(h_, int(row), FALSE); }
    void set_sort_indicator(std::optional<size_t> column, sort_order order) override
    {
        HWND header = ListView_GetHeader(h_);
        for (int i = 0; i < int(columns_.size()); ++i)
        {
            HDITEMW hd{};
            hd.mask = HDI_FORMAT;
            Header_GetItem(header, i, &hd);
            hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (column && size_t(i) == *column) hd.fmt |= order == sort_order::ascending ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(header, i, &hd);
        }
    }
    std::vector<float> column_widths() const override
    {
        std::vector<float> out;
        for (size_t i = 0; i < columns_.size(); ++i)
            out.push_back((visible_[i] ? ListView_GetColumnWidth(h_, int(i)) : widths_[i]) / win_.scale());
        return out;
    }
    std::vector<size_t> column_order() const override
    {
        std::vector<int> order(columns_.size());
        ListView_GetColumnOrderArray(h_, int(order.size()), order.data());
        return {order.begin(), order.end()};
    }
    void set_column_order(const std::vector<size_t>& order) override
    {
        if (!is_column_order(order, columns_.size())) return;
        std::vector<int> o(order.begin(), order.end());
        ListView_SetColumnOrderArray(h_, int(o.size()), o.data());
        fill_last();
        InvalidateRect(h_, nullptr, FALSE);
    }
    void set_column_widths(const std::vector<float>& w) override
    {
        for (size_t i = 0; i < w.size() && i < columns_.size(); ++i)
        {
            widths_[i] = px(w[i]);
            if (visible_[i]) ListView_SetColumnWidth(h_, int(i), widths_[i]);
        }
        fitted_ = true;  // restored widths win over the first fit
    }
    bool column_visible(size_t c) const override { return c < visible_.size() && visible_[c]; }
    void set_column_visible(size_t c, bool v) override
    {
        if (c >= visible_.size() || visible_[c] == v) return;
        if (!v) widths_[c] = ListView_GetColumnWidth(h_, int(c));
        visible_[c] = v;
        ListView_SetColumnWidth(h_, int(c), v ? widths_[c] : 0);
        fill_last();
    }
    void on_selection_changed(std::function<void()> f) override { sel_changed_ = std::move(f); }
    void on_activate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void on_sort_requested(std::function<void(size_t, sort_order)> f) override { sort_ = std::move(f); }
    void on_context_menu(std::function<menu(std::optional<size_t>)> f) override { context_ = std::move(f); }
    void on_header_menu(std::function<menu(size_t)> f) override { header_menu_ = std::move(f); }
    void on_cell_context_menu(std::function<menu(size_t, size_t, point_f)> f) override { cell_menu_ = std::move(f); }
    void on_reorder(reorder_fn f) override
    {
        reorder_ = std::move(f);
        reorderable_ = bool(reorder_);
    }
    void set_reorderable(bool on) override { reorderable_ = on && reorder_; }
    void on_cell_drag(cell_drag d) override
    {
        if (!cell_drag_.begin) SetWindowSubclass(h_, cell_press_proc, 5, reinterpret_cast<DWORD_PTR>(this));
        cell_drag_ = std::move(d);
    }

    void model_reset() override
    {
        ListView_SetItemCountEx(h_, model_ ? int(model_->row_count()) : 0, 0);
        InvalidateRect(h_, nullptr, TRUE);
    }
    void rows_changed(size_t first, size_t count) override
    {
        ListView_RedrawItems(h_, int(first), int(first + count - 1));
    }
    void rows_inserted(size_t, size_t) override { model_reset(); }
    void rows_removed(size_t, size_t) override { model_reset(); }

    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = true;
        switch (n->code)
        {
        case HDN_ITEMCHANGEDW:
        case HDN_ITEMCHANGEDA:
            // A column before the last resized (live while dragged).
            if (reinterpret_cast<NMHEADERW*>(n)->iItem != last_visible_column()) fill_last();
            handled = false;
            return 0;
        case HDN_ENDDRAG:
            // A header dragged to a new place: the order is applied after this
            // returns, and it can change which column fills.
            PostMessageW(h_, wm_rapport_fill_last, 0, 0);
            handled = false;
            return 0;
        case LVN_GETDISPINFOW:
        {
            auto* d = reinterpret_cast<NMLVDISPINFOW*>(n);
            if ((d->item.mask & LVIF_TEXT) && model_ && size_t(d->item.iItem) < model_->row_count())
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
                selection_changed();
            return 0;
        }
        case LVN_ODSTATECHANGED:
            if (!quiet_) selection_changed();
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
                const sort_order o = sort_column_ == int(c) && sort_order_ == sort_order::ascending ? sort_order::descending
                                                                                               : sort_order::ascending;
                sort_column_ = int(c);
                sort_order_ = o;
                sort_(c, o);
            }
            return 0;
        }
        case NM_RCLICK:
            if (n->hwndFrom == ListView_GetHeader(h_))
            {
                if (!header_menu_) return 0;
                POINT pt;
                GetCursorPos(&pt);
                POINT local = pt;
                ScreenToClient(n->hwndFrom, &local);
                HDHITTESTINFO hit{};
                hit.pt = local;
                SendMessageW(n->hwndFrom, HDM_HITTEST, 0, reinterpret_cast<LPARAM>(&hit));
                if (hit.iItem >= 0) show_popup_menu(GetAncestor(h_, GA_ROOT), header_menu_(size_t(hit.iItem)), pt);
                return 1;
            }
            if (auto* a = reinterpret_cast<NMITEMACTIVATE*>(n); cell_menu_ && a->iItem >= 0)
            {
                // Which cell, and where in it (a label the menu is about).
                LVHITTESTINFO hit{};
                hit.pt = a->ptAction;
                ListView_SubItemHitTest(h_, &hit);
                const int col = std::max(hit.iSubItem, 0);
                RECT cell{};
                ListView_GetSubItemRect(h_, a->iItem, col, LVIR_BOUNDS, &cell);
                const float s = win_.scale();
                POINT pt;
                GetCursorPos(&pt);
                show_popup_menu(GetAncestor(h_, GA_ROOT),
                          cell_menu_(size_t(a->iItem), size_t(col),
                                    point_f{(a->ptAction.x - cell.left) / s, (a->ptAction.y - cell.top) / s}),
                          pt);
            }
            else if (context_)
            {
                auto* a = reinterpret_cast<NMITEMACTIVATE*>(n);
                POINT pt;
                GetCursorPos(&pt);
                show_popup_menu(GetAncestor(h_, GA_ROOT),
                          context_(a->iItem >= 0 ? std::optional<size_t>(size_t(a->iItem)) : std::nullopt), pt);
            }
            return 0;
        case NM_CUSTOMDRAW:
            return custom_draw(reinterpret_cast<NMLVCUSTOMDRAW*>(n));
        case LVN_BEGINDRAG:
            // Past the drag threshold: a press `begin` accepted drags its
            // label, any other the rows (if they reorder).
            if (cell_.pressed)
            {
                cell_.dragging = true;
                g_dragging = this;
                SetCapture(h_);
                SetWindowSubclass(h_, cell_drag_proc, 6, reinterpret_cast<DWORD_PTR>(this));
                return 0;
            }
            if (reorderable_ && reorder_)
            {
                dragging_ = true;
                g_dragging = this;
                SetCapture(h_);
                SetWindowSubclass(h_, drag_proc, 2, reinterpret_cast<DWORD_PTR>(this));
            }
            return 0;
        }
        handled = false;
        return 0;
    }

    void place(RECT r) override
    {
        leaf::place(r);
        fit_stretch();
    }
    // A stretching last column keeps the list's right edge: it takes what
    // the others leave as the list resizes (on its own WM_SIZE: placing is
    // deferred, and a scroll bar coming or going changes the client width
    // too) and as they're dragged wider.
    // The last as shown, which a dragged header order can make any of them.
    int last_visible_column() const
    {
        std::vector<int> order(columns_.size());
        if (order.empty() || !ListView_GetColumnOrderArray(h_, int(order.size()), order.data()))
            return -1;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if (*it >= 0 && size_t(*it) < visible_.size() && visible_[size_t(*it)]) return *it;
        return -1;
    }
    int filled_column() const
    {
        const int last = last_visible_column();
        return last >= 0 && columns_[size_t(last)].stretch ? last : -1;
    }
    void fill_last()
    {
        const int last = filled_column();
        if (last < 0) return;
        RECT c;
        GetClientRect(h_, &c);
        if (c.right <= 0) return;
        long others = 0;
        for (size_t i = 0; i < columns_.size(); ++i)
            if (visible_[i] && int(i) != last) others += ListView_GetColumnWidth(h_, int(i));
        const int w = int(std::max<long>(px(40), c.right - others));
        if (ListView_GetColumnWidth(h_, last) != w) ListView_SetColumnWidth(h_, last, w);
    }
    // Stretch columns share the width the others leave, once, when the list
    // first has a size. After that only the user changes column widths:
    // resizing one never resizes the others, and a list wider than its
    // window scrolls sideways.
    void fit_stretch()
    {
        RECT c;
        GetClientRect(h_, &c);
        if (fitted_ || c.right <= 0) return;
        fitted_ = true;
        long fixed = 0;
        int stretchers = 0;
        const int filled = filled_column();  // has its share, then fill_last's rest
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
            if (visible_[i] && columns_[i].stretch && int(i) != filled) ListView_SetColumnWidth(h_, int(i), each);
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        // A modest minimum, not the columns' total: that grows with every
        // column the user widens and, restored, would widen the window.
        return {px(200), font_metrics(win_.dpi()).cy * 8};
    }

private:
    // The list paints all its rows itself, in one Direct2D pass per paint:
    // a software DC target bound once to the area being repainted (binding
    // one per cell cost ~0.5 ms apiece, a hardware one ~2.5 ms for the GPU
    // readback). The list_view keeps scrolling, selection, the header,
    // keyboard and accessibility; its own item drawing is skipped. Rows look
    // the same with or without focus.
    LRESULT custom_draw(NMLVCUSTOMDRAW* cd)
    {
        if (cd->nmcd.dwDrawStage != CDDS_PREPAINT || !model_) return CDRF_DODEFAULT;
        paint_rows(cd->nmcd.hdc);
        return CDRF_SKIPDEFAULT;
    }

    struct row_colors
    {
        color background;
        std::optional<color> text;  // replaces theme_color::text (solid selection)
    };
    // `c` with its HSL lightness set to `l` (hue and saturation kept).
    static color with_lightness(color c, float l)
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
        return color{byte(rgb[0]), byte(rgb[1]), byte(rgb[2]), 255};
    }
    row_colors colors_for_row(bool selected, bool hot) const
    {
        const color base = theme_color_of(theme_color::base), accent = theme_color_of(theme_color::accent);
        const bool dark = base.r * 30 + base.g * 59 + base.b * 11 < 12800;
        auto mixed = [&](float t)
        {
            auto m = [t](uint8_t a, uint8_t b) { return uint8_t(std::lround(a + (b - a) * t)); };
            return color{m(base.r, accent.r), m(base.g, accent.g), m(base.b, accent.b), 255};
        };
        // Soft selection is the theme's highlight with its hue and
        // saturation kept and its lightness raised: Windows' default
        // #0078D7 at 90% is Explorer's selected #CCE8FF, at 95% its hover
        // #E5F3FF. (Mixing toward white also drains the saturation and
        // reads grey.) Dark themes keep a mix of the base colour.
        const color highlight = theme_color_of(theme_color::highlight);
        if (!selected)
        {
            if (!hot || !soft_) return {base, std::nullopt};
            return {dark ? mixed(0.16f) : with_lightness(highlight, 0.95f), std::nullopt};
        }
        if (soft_)
            return {dark ? mixed(hot ? 0.46f : 0.38f) : with_lightness(highlight, hot ? 0.88f : 0.90f), std::nullopt};
        return {theme_color_of(theme_color::highlight), theme_color_of(theme_color::highlight_text)};
    }

    IDWriteTextFormat* text_format(const text_style& st, align align)
    {
        const int key = (st.bold ? 1 : 0) | (st.italic ? 2 : 0) | (st.monospace ? 4 : 0) | (int(align) << 3);
        com<IDWriteTextFormat>& f = formats_[key];
        if (!f)
        {
            dwrite_factory()->CreateTextFormat(st.monospace ? L"Consolas" : ui_font_family().c_str(), nullptr,
                                              st.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                              st.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, ui_font_size_dip(), L"", f.put());
            if (!f) return nullptr;
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetTextAlignment(align == align::end      ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                : align == align::center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            com<IDWriteInlineObject> ellipsis;
            dwrite_factory()->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
        return f.get();
    }

    // The shell's small icon for a kind of file (Explorer's), as a bitmap
    // for the row target: per extension ("/" for folders), made once and
    // dropped with the target. Shell icons are straight-alpha BGRA, so WIC
    // converts them to premultiplied first.
    ID2D1Bitmap* file_icon(ID2D1RenderTarget* rt, const std::string& name)
    {
        const bool folder = !name.empty() && name.back() == '/';
        const size_t slash = name.find_last_of("/\\", folder ? name.size() - 2 : std::string::npos);
        const std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
        const size_t dot = base.rfind('.');
        const std::string key = folder ? "/" : dot == std::string::npos ? base : base.substr(dot);
        auto it = file_icons_.find(key);
        if (it != file_icons_.end()) return it->second.get();
        com<ID2D1Bitmap>& out = file_icons_[key];
        SHFILEINFOW info{};
        const std::wstring probe = folder ? L"folder" : wide(dot == std::string::npos ? base : "file" + base.substr(dot));
        if (!SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof info,
                            SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES) ||
            !info.hIcon)
            return nullptr;
        com<IWICBitmap> wic;
        com<IWICFormatConverter> pbgra;
        if (wic_factory() && SUCCEEDED(wic_factory()->CreateBitmapFromHICON(info.hIcon, wic.put())) &&
            SUCCEEDED(wic_factory()->CreateFormatConverter(pbgra.put())) &&
            SUCCEEDED(pbgra->Initialize(wic.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)))
        {
            // At the target's DPI the icon's pixels map 1:1 to device pixels.
            float dpi_x = 96, dpi_y = 96;
            rt->GetDpi(&dpi_x, &dpi_y);
            const D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_x, dpi_y);
            rt->CreateBitmapFromWicBitmap(pbgra.get(), &props, out.put());
        }
        DestroyIcon(info.hIcon);
        return out.get();
    }

    void paint_rows(HDC dc)
    {
        RECT clip{};
        if (GetClipBox(dc, &clip) == NULLREGION || IsRectEmpty(&clip)) return;
        // The column header is a child inside the client area, at the top:
        // painting over it (the background clear) blanked it until hovered.
        if (HWND header = ListView_GetHeader(h_); header && IsWindowVisible(header))
        {
            RECT hr{};
            GetWindowRect(header, &hr);
            MapWindowPoints(HWND_DESKTOP, h_, reinterpret_cast<POINT*>(&hr), 2);
            clip.top = std::max(clip.top, hr.bottom);
            if (clip.top >= clip.bottom) return;
        }
        if (!row_target_)
        {
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96, 96);
            if (FAILED(d2d_factory()->CreateDCRenderTarget(&props, row_target_.put()))) return;
            // Crisp GDI-style glyphs, like the list_view's own text.
            com<IDWriteRenderingParams> def, params;
            dwrite_factory()->CreateRenderingParams(def.put());
            if (def &&
                SUCCEEDED(dwrite_factory()->CreateCustomRenderingParams(
                    def->GetGamma(), def->GetEnhancedContrast(), def->GetClearTypeLevel(), def->GetPixelGeometry(),
                    DWRITE_RENDERING_MODE_GDI_CLASSIC, params.put())))
                row_target_->SetTextRenderingParams(params.get());
            row_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        }
        ID2D1DCRenderTarget* rt = row_target_.get();
        if (FAILED(rt->BindDC(dc, &clip))) return;
        const float s = win_.scale();
        auto dip = [s](long v) { return v / s; };

        // Column spans in client px: the header's item rects (in display
        // order, hidden ones zero-wide) shifted by the horizontal scroll.
        struct span
        {
            size_t col;
            long left, right;
        };
        std::vector<span> spans;
        HWND header = ListView_GetHeader(h_);
        const size_t count = model_->row_count();
        RECT first{};
        long scroll_x = 0;
        if (count && ListView_GetItemRect(h_, ListView_GetTopIndex(h_), &first, LVIR_BOUNDS)) scroll_x = first.left;
        for (size_t c = 0; c < columns_.size(); ++c)
        {
            RECT hr{};
            if (!visible_[c] || !Header_GetItemRect(header, int(c), &hr) || hr.right <= hr.left) continue;
            spans.push_back({c, hr.left + scroll_x, hr.right + scroll_x});
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        const color base = theme_color_of(theme_color::base);
        rt->Clear(D2D1::ColorF(base.r / 255.f, base.g / 255.f, base.b / 255.f));
        {
            d2d_painter painter(rt, s);  // sets the target's DPI: coordinates below are DIPs
            const D2D1_MATRIX_3X2_F origin = D2D1::Matrix3x2F::Translation(-dip(clip.left), -dip(clip.top));
            com<ID2D1SolidColorBrush> brush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush.put());
            auto fill = [&](RECT r, color c)
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
                const row_colors colors = colors_for_row(selected, row == hot_);
                fill(RECT{clip.left, rr.top, clip.right, rr.bottom}, colors.background);

                for (const span& sp : spans)
                {
                    if (sp.right <= clip.left || sp.left >= clip.right) continue;
                    const RECT cell{sp.left, rr.top, sp.right, rr.bottom};
                    const text_style st = model_->style(size_t(row), sp.col);
                    if (st.bg && !selected) fill(cell, painter.resolve(*st.bg));
                    if (columns_[sp.col].painted)
                    {
                        d2d_painter cell_painter(rt, s);
                        if (soft_)
                        {
                            cell_painter.override_color(theme_color::highlight, colors.background);
                            cell_painter.override_color(theme_color::highlight_text, theme_color_of(theme_color::text));
                        }
                        if (colors.text) cell_painter.override_color(theme_color::text, *colors.text);
                        rt->SetTransform(D2D1::Matrix3x2F::Translation(dip(cell.left), dip(cell.top)) * origin);
                        const size_f size{dip(cell.right - cell.left), dip(cell.bottom - cell.top)};
                        cell_painter.push_clip({0, 0, size.w, size.h});
                        model_->paint(size_t(row), sp.col, cell_painter, cell_info{size, selected, false});
                        cell_painter.pop_clip();
                        continue;
                    }
                    float icon_space = 0;
                    if (const icon icon = model_->icon(size_t(row), sp.col); !icon.file.empty())
                        if (ID2D1Bitmap* bmp = file_icon(rt, icon.file))
                        {
                            // The shell's icon for the file's type, before the text.
                            const D2D1_SIZE_F sz = bmp->GetSize();  // DIPs at the target's DPI
                            const float x = dip(cell.left) + 4, y = dip(cell.top) + (dip(cell.bottom - cell.top) - sz.height) / 2;
                            rt->SetTransform(origin);
                            rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + sz.width, y + sz.height));
                            icon_space = sz.width + 2;
                        }
                    const std::wstring text = wide(model_->text(size_t(row), sp.col));
                    if (text.empty()) continue;
                    IDWriteTextFormat* format = text_format(st, columns_[sp.col].align);
                    if (!format) continue;
                    const color fg = colors.text && !st.fg ? *colors.text
                                                           : painter.resolve(st.fg ? *st.fg : paint{theme_color::text});
                    brush->SetColor(D2D1::ColorF(fg.r / 255.f, fg.g / 255.f, fg.b / 255.f, fg.a / 255.f));
                    rt->SetTransform(origin);
                    const float pad = 6;  // the list_view's own cell padding, in DIPs
                    rt->DrawText(text.c_str(), UINT32(text.size()), format,
                                 D2D1::RectF(dip(cell.left) + pad + icon_space, dip(cell.top), dip(cell.right) - pad, dip(cell.bottom)),
                                 brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }

            // The drop line while rows are dragged (the list_view would draw
            // it, but its item painting is skipped).
            RECT mark{};
            if (dragging_ && SendMessageW(h_, LVM_GETINSERTMARKRECT, 0, reinterpret_cast<LPARAM>(&mark)) &&
                !IsRectEmpty(&mark))
                fill(mark, theme_color_of(theme_color::text));
            // A cell drag: the drop row framed, the label at the pointer.
            if (cell_.dragging)
            {
                const color hl = theme_color_of(theme_color::highlight);
                brush->SetColor(D2D1::ColorF(hl.r / 255.f, hl.g / 255.f, hl.b / 255.f));
                rt->SetTransform(origin);
                RECT tr{};
                if (cell_.target && ListView_GetItemRect(h_, int(*cell_.target), &tr, LVIR_BOUNDS))
                    rt->DrawRectangle(D2D1::RectF(dip(tr.left) + 1, dip(tr.top) + 1, dip(tr.right) - 1, dip(tr.bottom) - 1),
                                      brush.get(), 2);
                if (!cell_.start.label.empty())
                {
                    rt->SetTransform(D2D1::Matrix3x2F::Translation(dip(cell_.pt.x) + 12, dip(cell_.pt.y) + 4) * origin);
                    text_style ts;
                    ts.bold = true;
                    const size_f t = painter.measure(cell_.start.label, ts);
                    painter.rect({0, 0, t.w + 12, t.h + 4}, 4, paint{theme_color::highlight}, paint{theme_color::mid});
                    ts.fg = theme_color::highlight_text;
                    painter.text({6, 2}, cell_.start.label, ts);
                }
            }
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET)
        {
            row_target_.reset();
            file_icons_.clear();  // bitmaps belong to the target
        }
    }

    // A click changes the selection in steps (everything off, then the new
    // row on), and each step notified the app, which redrew its details
    // twice. Changes are reported once, after the list_view is done: posted
    // messages run before the next paint, so the details still land in the
    // same frame as the highlight.
    void selection_changed()
    {
        if (!sel_changed_ || sel_pending_) return;
        sel_pending_ = true;
        PostMessageW(h_, wm_rapport_selection, 0, 0);
    }
    static LRESULT CALLBACK selection_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        if (msg != wm_rapport_selection) return DefSubclassProc(h, msg, wp, lp);
        auto* self = reinterpret_cast<list_impl*>(data);
        self->sel_pending_ = false;
        if (self->sel_changed_) self->sel_changed_();
        return 0;
    }

    // The row under the pointer gets a light tint (soft-selection lists,
    // like Explorer's and TortoiseGit's).
    void set_hot(int row)
    {
        if (row == hot_) return;
        for (int r : {hot_, row})
        {
            RECT rr{};
            if (r >= 0 && ListView_GetItemRect(h_, r, &rr, LVIR_BOUNDS)) InvalidateRect(h_, &rr, FALSE);
        }
        hot_ = row;
    }
    static LRESULT CALLBACK hot_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        if (msg == WM_MOUSEMOVE)
        {
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const int row = ListView_HitTest(h, &hit);
            self->set_hot(row >= 0 && (hit.flags & LVHT_ONITEM) ? row : -1);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        else if (msg == WM_MOUSELEAVE)
            self->set_hot(-1);
        return DefSubclassProc(h, msg, wp, lp);
    }

    // Where dragged rows would land for a point in client coordinates: the
    // row they go in front of (row_count() = the end). Above the rows (over the
    // header) means the first visible row; only space below the last row
    // means the end.
    size_t drop_target(POINT pt) const
    {
        const size_t count = model_ ? model_->row_count() : 0;
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
    void show_mark(size_t target)
    {
        if (noop(target))
        {
            LVINSERTMARK none{sizeof none, 0, -1, 0};
            SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&none));
            SetCursor(LoadCursorW(nullptr, IDC_NO));
            return;
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        const size_t count = model_ ? model_->row_count() : 0;
        LVINSERTMARK mark{sizeof mark, 0, -1, 0};
        if (count)
        {
            mark.iItem = int(std::min(target, count - 1));
            mark.dwFlags = target >= count ? LVIM_AFTER : 0;
        }
        SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&mark));
    }
public:
    void cancel_drag()
    {
        stop_auto_scroll();
        end_cell_drag(false);
        end_drag(false, POINT{});
    }

private:
    void end_drag(bool drop, POINT pt)
    {
        if (!dragging_) return;
        dragging_ = false;
        if (g_dragging == this) g_dragging = nullptr;
        LVINSERTMARK none{sizeof none, 0, -1, 0};
        SendMessageW(h_, LVM_SETINSERTMARK, 0, reinterpret_cast<LPARAM>(&none));
        RemoveWindowSubclass(h_, drag_proc, 2);
        if (GetCapture() == h_) ReleaseCapture();
        if (drop && reorder_)
        {
            const std::vector<size_t> rows = selection();
            const size_t target = drop_target(pt);
            if (!rows.empty() && !noop(target)) reorder_(rows, target);
        }
    }
    // cell_drag: the press is noted before the list_view sees it (it selects
    // as usual, then reports LVN_BEGINDRAG past the threshold).
    struct cell_drag_state
    {
        bool pressed = false, dragging = false;
        size_t from = 0;
        cell_drag_start start;
        std::optional<size_t> target;
        POINT pt{};  // pointer, client coordinates
    };
    static LRESULT CALLBACK cell_press_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        if (msg == WM_LBUTTONDOWN && !(wp & (MK_SHIFT | MK_CONTROL)) && self->cell_drag_.begin && self->model_)
        {
            self->cell_ = {};
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ListView_SubItemHitTest(h, &hit) >= 0 && (hit.flags & LVHT_ONITEM) && size_t(hit.iItem) < self->model_->row_count())
            {
                RECT cell{};
                ListView_GetSubItemRect(h, hit.iItem, hit.iSubItem, LVIR_BOUNDS, &cell);
                const float s = self->win_.scale();
                if (auto start = self->cell_drag_.begin(size_t(hit.iItem), size_t(hit.iSubItem),
                                                        point_f{(hit.pt.x - cell.left) / s, (hit.pt.y - cell.top) / s}))
                    self->cell_ = {true, false, size_t(hit.iItem), std::move(*start), std::nullopt, hit.pt};
            }
        }
        else if (msg == WM_LBUTTONUP && !self->cell_.dragging)
            self->cell_ = {};
        return DefSubclassProc(h, msg, wp, lp);
    }
    static LRESULT CALLBACK cell_drag_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        switch (msg)
        {
        case WM_MOUSEMOVE:
        {
            cell_drag_state& c = self->cell_;
            c.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            LVHITTESTINFO hit{};
            hit.pt = c.pt;
            const int row = ListView_HitTest(h, &hit);
            c.target.reset();
            if (row >= 0 && (hit.flags & LVHT_ONITEM) && self->cell_drag_.can_drop &&
                self->cell_drag_.can_drop(c.from, size_t(row), c.start.data))
                c.target = size_t(row);
            SetCursor(LoadCursorW(nullptr, c.target ? IDC_HAND : IDC_NO));
            RECT client;
            GetClientRect(h, &client);
            const int edge = self->px(20);
            if (c.pt.y < edge) ListView_Scroll(h, 0, -self->px(8));
            else if (c.pt.y > client.bottom - edge) ListView_Scroll(h, 0, self->px(8));
            InvalidateRect(h, nullptr, FALSE);  // one paint pass: a few ms
            return 0;
        }
        case WM_LBUTTONUP: self->end_cell_drag(true); return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: self->end_cell_drag(false); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
            {
                self->end_cell_drag(false);
                return 0;
            }
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }
    void end_cell_drag(bool drop)
    {
        if (!cell_.dragging) return;
        cell_drag_state c = std::move(cell_);
        cell_ = {};
        if (g_dragging == this) g_dragging = nullptr;
        RemoveWindowSubclass(h_, cell_drag_proc, 6);
        if (GetCapture() == h_) ReleaseCapture();
        InvalidateRect(h_, nullptr, FALSE);
        if (drop && c.target && cell_drag_.drop) cell_drag_.drop(c.from, *c.target, c.start.data);
    }

    // Autoscroll (spec.auto_scroll): with the middle button held, a timer
    // scrolls by the pointer's offset from the anchor. The list_view scrolls
    // whole rows, so pixels gather until a row's worth.
    struct auto_scroll_state
    {
        bool on = false;
        POINT anchor{};
        int y = 0;
        double last = 0, pending = 0;  // ms; pixels not yet scrolled
    };
    static double now_ms()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
    }
    static float auto_scroll_speed(float offset)  // px/s; as gtk_common's
    {
        const float dead = 8, d = std::abs(offset) - dead;
        if (d <= 0) return 0;
        return (offset < 0 ? -1.f : 1.f) * (d * 6 + d * d * 0.05f);
    }
    void stop_auto_scroll()
    {
        if (!auto_.on) return;
        auto_ = {};
        KillTimer(h_, 0xa5c0);
        if (GetCapture() == h_) ReleaseCapture();
        if (g_dragging == this) g_dragging = nullptr;
        if (anchor_) ShowWindow(anchor_, SW_HIDE);
    }

    // The anchor floats above the list in a window of its own (layered,
    // see-through to the mouse, never active): painted into the rows, each
    // scroll's blit carried it off with them until the next repaint.
    void show_anchor(POINT at)
    {
        const float s = win_.scale();
        const int side = int(std::ceil(26 * s)) + 2;  // the ring is 11 px round, plus its stroke
        if (!anchor_)
        {
            static const ATOM cls = []
            {
                WNDCLASSEXW wc{sizeof wc};
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = instance();
                wc.lpszClassName = L"RapportAutoScrollAnchor";
                return RegisterClassExW(&wc);
            }();
            anchor_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                      MAKEINTATOM(cls), nullptr, WS_POPUP, 0, 0, side, side, GetAncestor(h_, GA_ROOT),
                                      nullptr, instance(), nullptr);
            if (!anchor_) return;
        }
        // Drawn premultiplied into a DIB, handed over with its alpha.
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96, 96);
        com<ID2D1DCRenderTarget> rt;
        RECT all{0, 0, side, side};
        if (bits && SUCCEEDED(d2d_factory()->CreateDCRenderTarget(&props, rt.put())) &&
            SUCCEEDED(rt->BindDC(mem, &all)))
        {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            {
                d2d_painter p(rt.get(), s);
                const float c = side / s / 2;
                p.circle({c, c}, 11, paint{theme_color::base}, paint{theme_color::mid});
                p.polygon({{c - 4, c - 3}, {c + 4, c - 3}, {c, c - 8}}, paint{theme_color::text}, std::nullopt, 1);
                p.polygon({{c - 4, c + 3}, {c + 4, c + 3}, {c, c + 8}}, paint{theme_color::text}, std::nullopt, 1);
                p.circle({c, c}, 1.5f, paint{theme_color::text}, std::nullopt);
            }
            rt->EndDraw();
            POINT pos = at;
            ClientToScreen(h_, &pos);
            pos.x -= side / 2;
            pos.y -= side / 2;
            SIZE size{side, side};
            POINT zero{};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(anchor_, screen, &pos, &size, mem, &zero, 0, &blend, ULW_ALPHA);
            ShowWindow(anchor_, SW_SHOWNOACTIVATE);
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    static LRESULT CALLBACK size_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (msg == WM_SIZE || msg == wm_rapport_fill_last) reinterpret_cast<list_impl*>(data)->fill_last();
        return r;
    }
    static LRESULT CALLBACK auto_scroll_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        auto_scroll_state& a = self->auto_;
        switch (msg)
        {
        case WM_MBUTTONDOWN:
            a = {true, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, GET_Y_LPARAM(lp), now_ms(), 0};
            SetCapture(h);
            g_dragging = self;  // Esc stops it, as it cancels drags
            SetTimer(h, 0xa5c0, 8, nullptr);
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            self->show_anchor(a.anchor);
            return 0;
        case WM_MOUSEMOVE:
            if (!a.on) break;
            a.y = GET_Y_LPARAM(lp);
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return 0;
        case WM_TIMER:
        {
            if (wp != 0xa5c0 || !a.on) break;
            const double now = now_ms();
            const float s = self->win_.scale();
            a.pending += auto_scroll_speed((a.y - a.anchor.y) / s) * s * (now - a.last) / 1000.0;
            a.last = now;
            RECT row{};
            if (ListView_GetItemRect(h, ListView_GetTopIndex(h), &row, LVIR_BOUNDS) && row.bottom > row.top)
            {
                const int row_px = row.bottom - row.top, rows = int(a.pending / row_px);
                if (rows)
                {
                    ListView_Scroll(h, 0, rows * row_px);
                    a.pending -= rows * row_px;
                }
            }
            return 0;
        }
        case WM_MBUTTONUP:
        case WM_CAPTURECHANGED:
            if (a.on) self->stop_auto_scroll();
            if (msg == WM_MBUTTONUP) return 0;
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    static LRESULT CALLBACK drag_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<list_impl*>(data);
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg)
        {
        case WM_MOUSEMOVE:
        {
            self->show_mark(self->drop_target(pt));
            RECT c;
            GetClientRect(h, &c);
            const int edge = self->px(20);
            if (pt.y < edge) ListView_Scroll(h, 0, -self->px(8));
            else if (pt.y > c.bottom - edge) ListView_Scroll(h, 0, self->px(8));
            return 0;
        }
        case WM_LBUTTONUP: self->end_drag(true, pt); return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: self->end_drag(false, pt); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE)
            {
                self->end_drag(false, pt);
                return 0;
            }
            break;
        }
        return DefSubclassProc(h, msg, wp, lp);
    }

    reorder_fn reorder_;
    bool reorderable_ = false, dragging_ = false;
    cell_drag cell_drag_;
    cell_drag_state cell_;
    auto_scroll_state auto_;
    HWND anchor_ = nullptr;  // the autoscroll anchor's window, made on first use
    std::vector<column> columns_;
    std::vector<bool> visible_;
    std::vector<int> widths_;
    list_model* model_ = nullptr;
    std::wstring text_;  // LVN_GETDISPINFO's buffer
    bool quiet_ = false;
    bool fitted_ = false;  // stretch columns have had their share of the width
    bool sel_pending_ = false;  // a selection change is posted, not yet reported
    com<ID2D1DCRenderTarget> row_target_;
    std::map<std::string, com<ID2D1Bitmap>> file_icons_;  // per extension, for row_target_
    std::map<int, com<IDWriteTextFormat>> formats_;  // by style and alignment
    bool soft_ = false;
    int hot_ = -1;  // the row under the pointer
    int sort_column_ = -1;
    sort_order sort_order_ = sort_order::ascending;
    std::function<void()> sel_changed_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, sort_order)> sort_;
    std::function<menu(std::optional<size_t>)> context_;
    std::function<menu(size_t)> header_menu_;
    std::function<menu(size_t, size_t, point_f)> cell_menu_;
};

// -- tabs ---------------------------------------------------------------------

class tabs_impl : public leaf<rap::tabs>
{
public:
    tabs_impl(win_context& win, HWND parent, const spec::tabs& s)
        : leaf(win)
    {
        adopt(create_control(win, parent, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP));
        for (size_t i = 0; i < s.titles.size(); ++i)
        {
            std::wstring t = mnemonic(s.titles[i]);
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = t.data();
            TabCtrl_InsertItem(h_, int(i), &item);
        }
    }
    std::vector<item*> pages;
    // Lazy pages: their builders until built, and whether each page is one.
    std::vector<std::function<node()>> lazy;
    std::vector<bool> isLazy;
    std::function<item*(const node&)> buildPage;

    int current() const override { return TabCtrl_GetCurSel(h_); }
    void set_current(int i) override
    {
        TabCtrl_SetCurSel(h_, i);
        ensure_built(i);
        win_.relayout_at_once();
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    LRESULT notify(NMHDR* n, bool& handled) override
    {
        handled = n->code == TCN_SELCHANGE;
        if (handled)
        {
            ensure_built(current());
            win_.relayout_at_once();
            if (changed_) changed_(current());
        }
        return 0;
    }
    void ensure_built(int i)
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
    bool expands_in_form() const override { return true; }
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
        leaf::place(r);
        RECT in{0, 0, r.right - r.left, r.bottom - r.top};
        TabCtrl_AdjustRect(h_, FALSE, &in);
        OffsetRect(&in, r.left, r.top);
        InflateRect(&in, -pad(), -pad());
        for (item* p : pages) p->place(in);
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        if (shown && visible) ensure_built(current());  // the first time a lazy page is the one showing
        for (size_t i = 0; i < pages.size(); ++i) pages[i]->show_all(shown && visible && int(i) == current());
    }

private:
    int pad() const { return px(6); }
    std::function<void(int)> changed_;
};

// Tabs as a list of page names down the left, the page beside it.
class side_tabs_impl : public leaf<rap::tabs>
{
public:
    side_tabs_impl(win_context& win, HWND parent, const spec::tabs& s)
        : leaf(win)
    {
        // Owner-drawn only to centre the names in rows taller than the text
        // (a plain list box draws them at the top of each row).
        adopt(create_control(win, parent, L"LISTBOX", L"",
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
        SendMessageW(h_, LB_SETITEMHEIGHT, 0, font_metrics(win.dpi()).cy + px(12));
        SendMessageW(h_, LB_SETCURSEL, 0, 0);
    }
    std::vector<item*> pages;
    std::vector<std::function<node()>> lazy;
    std::vector<bool> isLazy;
    std::function<item*(const node&)> buildPage;

    int current() const override { return std::max(0, int(SendMessageW(h_, LB_GETCURSEL, 0, 0))); }
    void set_current(int i) override
    {
        SendMessageW(h_, LB_SETCURSEL, WPARAM(i), 0);
        if (i == shown_) return;
        shown_ = i;
        ensure_built(i);
        win_.relayout_at_once();
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    void command(WORD code) override
    {
        // A click on the empty space below the names reports a change too.
        if (code != LBN_SELCHANGE || current() == shown_) return;
        shown_ = current();
        ensure_built(current());
        win_.relayout_at_once();
        if (changed_) changed_(current());
    }
    void ensure_built(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto build = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        pages[size_t(i)] = buildPage(build());
        win_.relayout();
    }
    bool draw_item(DRAWITEMSTRUCT* d) override
    {
        if (d->itemID == UINT(-1) || d->itemID >= titles_.size()) return false;
        const bool selected = d->itemState & ODS_SELECTED;
        FillRect(d->hDC, &d->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        HGDIOBJ old = SelectObject(d->hDC, ui_font(win_.dpi()));
        RECT text = d->rcItem;
        text.left += px(8);
        DrawTextW(d->hDC, titles_[d->itemID].c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(d->hDC, old);
        if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) DrawFocusRect(d->hDC, &d->rcItem);
        return true;
    }
    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
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
        return {list_width() + gap() + in.cx, std::max<long>(in.cy, long(titles_.size()) * (font_metrics(win_.dpi()).cy + px(12)))};
    }
    void place(RECT r) override
    {
        const int lw = list_width();
        leaf::place({r.left, r.top, r.left + lw, r.bottom});
        for (item* p : pages) p->place({r.left + lw + gap(), r.top, r.right, r.bottom});
    }
    void show_all(bool shown) override
    {
        leaf::show_all(shown);
        if (shown && visible) ensure_built(current());
        for (size_t i = 0; i < pages.size(); ++i) pages[i]->show_all(shown && visible && int(i) == current());
    }

private:
    int gap() const { return px(12); }
    int list_width()
    {
        long w = px(120);
        for (const std::wstring& t : titles_) w = std::max(w, text_extent(h_, t).cx + px(36));
        return int(w);
    }

    std::vector<std::wstring> titles_;
    int shown_ = 0;  // the page showing
    std::function<void(int)> changed_;
};

// -- splitter -----------------------------------------------------------------

// Panes side by side with draggable bars between them. Sizes are device px
// per pane; the window keeps the panes' proportions until a drag.
class splitter_impl : public rap::splitter, public item
{
public:
    splitter_impl(win_context& win, HWND parent, orientation dir)
        : win_(win)
        , parent_(parent)
        , dir_(dir)
    {
    }
    ~splitter_impl() override
    {
        for (HWND b : bars_) DestroyWindow(b);
    }
    std::vector<item*> panes;

    void add_bar()
    {
        register_classes();
        HWND bar = make_host(parent_);
        auto* fn = new std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>(
            [this, index = bars_.size()](HWND h, UINT msg, WPARAM, LPARAM lp, bool& handled) -> LRESULT
            { return bar_message(index, h, msg, lp, handled); });
        SetWindowLongPtrW(bar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fn));
        bars_.push_back(bar);
        fns_.emplace_back(fn);
    }

    void set_enabled(bool) override {}
    void set_visible(bool v) override
    {
        if (visible == v) return;
        visible = v;
        win_.relayout();
    }
    void set_tooltip(const std::string&) override {}
    void focus() override {}
    std::vector<float> sizes() const override
    {
        std::vector<float> out;
        for (long s : sizes_) out.push_back(s / win_.scale());
        return out;
    }
    void set_sizes(const std::vector<float>& s) override
    {
        sizes_.clear();
        for (float v : s) sizes_.push_back(long(v * win_.scale()));
        win_.relayout();
    }

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        long main = 0, cross = 0;
        for (item* p : panes)
        {
            if (!p->visible) continue;
            const SIZE s = p->natural();
            main += dir_ == orientation::horizontal ? s.cx : s.cy;
            cross = std::max(cross, dir_ == orientation::horizontal ? s.cy : s.cx);
        }
        main += bar() * long(panes.size() - 1);
        return dir_ == orientation::horizontal ? SIZE{main, cross} : SIZE{cross, main};
    }
    void place(RECT r) override
    {
        rect_ = r;
        std::vector<size_t> shown;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i]->visible) shown.push_back(i);
        const long total = dir_ == orientation::horizontal ? r.right - r.left : r.bottom - r.top;
        // Sizes: remembered ones where we have them; the first pane takes the rest.
        std::vector<long> sz(panes.size(), 0);
        long rest = total - bar() * long(std::max<size_t>(shown.size(), 1) - 1);
        if (sizes_.size() != panes.size())
        {
            int stretch_sum = 0;
            for (size_t i : shown) stretch_sum += std::max(panes[i]->stretch, 0);
            long fixed = 0;
            for (size_t i : shown)
                if (!panes[i]->stretch)
                {
                    const SIZE n = panes[i]->natural();
                    sz[i] = dir_ == orientation::horizontal ? n.cx : n.cy;
                    fixed += sz[i];
                }
            for (size_t i : shown)
                if (panes[i]->stretch) sz[i] = stretch_sum ? (rest - fixed) * panes[i]->stretch / stretch_sum : 0;
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
        long pos = dir_ == orientation::horizontal ? r.left : r.top;
        size_t b = 0;
        for (size_t k = 0; k < shown.size(); ++k)
        {
            const size_t i = shown[k];
            const long s = std::max(0L, sz[i]);
            panes[i]->place(dir_ == orientation::horizontal ? RECT{pos, r.top, pos + s, r.bottom}
                                                            : RECT{r.left, pos, r.right, pos + s});
            pos += s;
            if (k + 1 < shown.size() && b < bars_.size())
            {
                HWND bh = bars_[b++];
                if (dir_ == orientation::horizontal)
                    move_control(bh, pos, r.top, bar(), r.bottom - r.top);
                else
                    move_control(bh, r.left, pos, r.right - r.left, bar());
                show_control(bh, true);
                pos += bar();
            }
        }
        for (; b < bars_.size(); ++b) show_control(bars_[b], false);
        placed_ = sz;
    }
    void show_all(bool shown) override
    {
        for (item* p : panes) p->show_all(shown && visible);
        if (!(shown && visible))
            for (HWND b : bars_) show_control(b, false);
    }

private:
    int bar() const { return win_.px(5); }
    LRESULT bar_message(size_t index, HWND h, UINT msg, LPARAM lp, bool& handled)
    {
        handled = true;
        switch (msg)
        {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, dir_ == orientation::horizontal ? IDC_SIZEWE : IDC_SIZENS));
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
            drag_bar(index, dir_ == orientation::horizontal ? pt.x : pt.y);
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
    void drag_bar(size_t index, long at)
    {
        std::vector<size_t> shown;
        for (size_t i = 0; i < panes.size(); ++i)
            if (panes[i]->visible) shown.push_back(i);
        if (index + 1 >= shown.size() || placed_.size() != panes.size()) return;
        std::vector<long> sz(shown.size());
        for (size_t k = 0; k < shown.size(); ++k) sz[k] = std::max(0L, placed_[shown[k]]);

        long bar_start = dir_ == orientation::horizontal ? rect_.left : rect_.top;
        for (size_t k = 0; k <= index; ++k) bar_start += sz[k] + (k < index ? bar() : 0);
        long delta = at - bar() / 2 - bar_start;
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
        SendMessageW(win_.hwnd(), wm_rapport_relayout, 0, 0);
        RedrawWindow(win_.hwnd(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    win_context& win_;
    HWND parent_;
    orientation dir_;
    std::vector<HWND> bars_;
    std::vector<std::unique_ptr<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>>> fns_;
    std::vector<long> sizes_, placed_;
    RECT rect_{};
    bool dragging_ = false;
};

// -- scroll -------------------------------------------------------------------

// A child window that clips its content and scrolls it vertically (and
// horizontally if asked).
class scroll_item : public item
{
public:
    scroll_item(win_context& win, HWND parent, bool horizontal)
        : win_(win)
        , horizontal_(horizontal)
    {
        host_ = make_host(parent, WS_VSCROLL | (horizontal ? WS_HSCROLL : 0));
        fn_ = std::make_unique<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>>(
            [this](HWND, UINT msg, WPARAM wp, LPARAM, bool& handled) -> LRESULT { return message(msg, wp, handled); });
        SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fn_.get()));
        // The content sits on one panel the size of all of it, and scrolling
        // moves just that: moving each control would cost an accessibility
        // event apiece, which is slow whenever an automation client listens.
        // The panel erases under its children: group boxes are transparent
        // and never erase their insides.
        panel_ = make_host(host_);
        SetWindowLongPtrW(panel_, GWL_STYLE, GetWindowLongPtrW(panel_, GWL_STYLE) & ~WS_CLIPCHILDREN);
        show_control(panel_, true);
    }
    ~scroll_item() override { DestroyWindow(host_); }
    HWND host() const { return panel_; }  // where the content's controls go
    item* content = nullptr;

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override
    {
        SIZE c = content ? content->natural() : SIZE{0, 0};
        c.cx += GetSystemMetrics(SM_CXVSCROLL);
        c.cy = std::min<long>(c.cy, font_metrics(win_.dpi()).cy * 10);  // asks for a modest height
        return c;
    }
    void place(RECT r) override
    {
        move_now(host_, r.left, r.top, r.right - r.left, r.bottom - r.top);
        layout_content();
    }
    void show_all(bool shown) override
    {
        show_control(host_, shown && visible);
        if (content) content->show_all(shown && visible);
    }

private:
    void layout_content()
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
        move_now(panel_, -x_, -y_, w, h);
        content->place({0, 0, w, h});
        content_w_ = w, content_h_ = h;
    }
    // Scrolling moves what's already drawn: Windows shifts the pixels and the
    // panel together, and only the strip that comes into view paints.
    void scroll_to(long x, long y)
    {
        RECT client;
        GetClientRect(host_, &client);
        x = std::clamp(x, 0L, std::max(0L, content_w_ - client.right));
        y = std::clamp(y, 0L, std::max(0L, content_h_ - client.bottom));
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
        case WM_SIZE: layout_content(); return 0;
        case WM_VSCROLL:
        case WM_HSCROLL:
        {
            const bool v = msg == WM_VSCROLL;
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(host_, v ? SB_VERT : SB_HORZ, &si);
            long pos = v ? y_ : x_;
            const long line = font_metrics(win_.dpi()).cy;
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
                scroll_to(x_, pos);
            else
                scroll_to(pos, y_);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_to(x_, y_ - GET_WHEEL_DELTA_WPARAM(wp) * font_metrics(win_.dpi()).cy * 3 / WHEEL_DELTA);
            return 0;
        }
        handled = false;
        return 0;
    }

    win_context& win_;
    bool horizontal_;
    HWND host_, panel_;
    std::unique_ptr<std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool&)>> fn_;
    long x_ = 0, y_ = 0;
    long content_w_ = 0, content_h_ = 0;
};

// -- frames -------------------------------------------------------------------

// One display-paced clock for every frame subscriber: a thread waits on the
// compositor (DwmFlush) and posts a tick to the UI thread.
class frame_clock
{
public:
    explicit frame_clock(HWND target)
        : target_(target)
    {
    }
    ~frame_clock() { stop_thread(); }

    subscription subscribe(std::function<void(double)> fn)
    {
        const int id = ++next_;
        subscribers_[id] = std::move(fn);
        start_thread();
        return subscription(
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
            stop_thread();
            return;
        }
        const double t = now_seconds();
        auto copy = subscribers_;  // callbacks may unsubscribe
        for (auto& [id, fn] : copy)
            if (subscribers_.count(id)) fn(t);
    }

private:
    void start_thread()
    {
        if (running_) return;
        running_ = true;
        thread_ = std::thread(
            [this]
            {
                while (running_)
                {
                    if (FAILED(DwmFlush())) Sleep(16);
                    if (!pending_.exchange(true)) PostMessageW(target_, wm_rapport_frame, 0, 0);
                }
            });
    }
    void stop_thread()
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

frame_clock* g_frames = nullptr;

// -- canvas -------------------------------------------------------------------

class canvas_impl : public leaf<rap::canvas>
{
public:
    canvas_impl(win_context& win, HWND parent, const spec::canvas& s)
        : leaf(win)
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
            wc.lpszClassName = L"rapportCanvas";
            RegisterClassExW(&wc);
            registered = true;
        }
        adopt(CreateWindowExW(0, L"rapportCanvas", L"", WS_CHILD | (s.focusable ? WS_TABSTOP : 0), 0, 0, 0, 0, parent,
                              nullptr, instance(), this));
    }
    ~canvas_impl() override { *alive_ = false; }

    void invalidate() override { InvalidateRect(h_, nullptr, FALSE); }
    size_f size() const override
    {
        RECT r;
        GetClientRect(h_, &r);
        const float s = win_.scale();
        return {r.right / s, r.bottom / s};
    }
    float device_scale() const override { return win_.scale(); }
    void set_cursor(cursor c) override
    {
        cursor_ = c;
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == h_) apply_cursor();
    }
    void on_paint(std::function<void(rap::painter&, rap::size_f)> f) override { paint_ = std::move(f); }
    void on_resize(std::function<void(size_f)> f) override { resize_ = std::move(f); }
    void on_pointer(std::function<bool(const pointer_event&)> f) override { pointer_ = std::move(f); }
    void on_scroll(std::function<bool(const scroll_event&)> f) override { scroll_ = std::move(f); }
    void on_key(std::function<bool(const key_event&)> f) override { key_ = std::move(f); }
    subscription on_frame(std::function<void(double)> f) override
    {
        return g_frames ? g_frames->subscribe(std::move(f)) : subscription();
    }

    bool fills() const override { return true; }
    bool expands_in_form() const override { return true; }
    SIZE measure() override { return {px(64), px(64)}; }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<canvas_impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self)
        {
            bool handled = true;
            const LRESULT r = self->message(msg, wp, lp, handled);
            if (handled) return r;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    pointer_event make_pointer_event(pointer_event_type t, LPARAM lp, int button) const
    {
        const float s = win_.scale();
        return pointer_event{t, {GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s}, button, 1, key_modifiers()};
    }
    bool press(LPARAM lp, int button)
    {
        if (focusable_ && GetFocus() != h_) SetFocus(h_);
        SetCapture(h_);
        pointer_event e = make_pointer_event(pointer_event_type::press, lp, button);
        const DWORD time = GetMessageTime();
        const bool again = button == last_button_ && time - last_time_ <= GetDoubleClickTime() &&
                           std::abs(GET_X_LPARAM(lp) - last_pos_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           std::abs(GET_Y_LPARAM(lp) - last_pos_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        clicks_ = again ? clicks_ + 1 : 1;
        e.clicks = clicks_;
        last_button_ = button;
        last_time_ = time;
        last_pos_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        return pointer_ && pointer_(e);
    }
    bool release(LPARAM lp, int button)
    {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000) && !(GetKeyState(VK_RBUTTON) & 0x8000) &&
            !(GetKeyState(VK_MBUTTON) & 0x8000))
            ReleaseCapture();
        return pointer_ && pointer_(make_pointer_event(pointer_event_type::release, lp, button));
    }
    void apply_cursor()
    {
        const wchar_t* id = IDC_ARROW;
        switch (cursor_)
        {
        case cursor::default_: id = IDC_ARROW; break;
        case cursor::pointer: id = IDC_HAND; break;
        case cursor::text: id = IDC_IBEAM; break;
        case cursor::crosshair: id = IDC_CROSS; break;
        case cursor::grab:
        case cursor::grabbing:
        case cursor::move: id = IDC_SIZEALL; break;
        case cursor::zoom_in:
        case cursor::zoom_out: id = IDC_CROSS; break;
        case cursor::wait: id = IDC_WAIT; break;
        case cursor::hidden: SetCursor(nullptr); return;
        }
        SetCursor(LoadCursorW(nullptr, id));
    }

    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled)
    {
        using t = pointer_event_type;
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
                if (pointer_) pointer_(make_pointer_event(t::enter, lp, 0));
            }
            if (pointer_) pointer_(make_pointer_event(t::move, lp, 0));
            return 0;
        case WM_MOUSELEAVE:
            inside_ = false;
            if (pointer_) pointer_(pointer_event{t::leave, {}, 0, 1, key_modifiers()});
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        {
            if (!scroll_) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h_, &pt);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const float s = win_.scale();
            scroll_event e{{pt.x / s, pt.y / s}, 0, 0, delta % WHEEL_DELTA != 0, key_modifiers()};
            // Windows: positive = away from the user (up/right); rapport: +1 = down/right.
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
                apply_cursor();
                return TRUE;
            }
            break;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (key_ && key_(make_key_event(wp, lp, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN))) return 0;
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
            d2d_factory()->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(h_, D2D1::SizeU(UINT32(r.right), UINT32(r.bottom)),
                                                        D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                rt_.put());
            if (!rt_) return;
        }
        rt_->BeginDraw();
        rt_->Clear(d2d_color(theme_color_of(theme_color::window)));
        if (paint_)
        {
            d2d_painter p(rt_.get(), win_.scale());
            paint_(p, size());
        }
        if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) rt_.reset();
    }

    bool focusable_;
    com<ID2D1HwndRenderTarget> rt_;
    cursor cursor_ = cursor::default_;
    bool inside_ = false;
    int clicks_ = 0, last_button_ = 0;
    DWORD last_time_ = 0;
    POINT last_pos_{};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::function<void(rap::painter&, rap::size_f)> paint_;
    std::function<void(size_f)> resize_;
    std::function<bool(const pointer_event&)> pointer_;
    std::function<bool(const scroll_event&)> scroll_;
    std::function<bool(const key_event&)> key_;
};

// -- window -------------------------------------------------------------------

class window_impl : public rap::window, public win_context
{
public:
    window_impl(const window_spec& spec, window_impl* parent)
        : parent(parent)
        , dialog_(spec.dialog)
        , margins_(spec.margins)
        , modal_spec_(spec.modal)
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
            wc.lpszClassName = L"rapportWindow";
            wc.hIcon = app_icon(SM_CXICON);
            wc.hIconSm = app_icon(SM_CXSMICON);
            RegisterClassExW(&wc);
            registered = true;
        }
        DWORD style = dialog_ ? (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME) : WS_OVERLAPPEDWINDOW;
        if (!spec.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT | (dialog_ ? WS_EX_DLGMODALFRAME : 0), L"rapportWindow",
                                wide(spec.title).c_str(), style | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                parent ? parent->hwnd_ : nullptr, nullptr, instance(), this);
        initial_w_ = spec.width;
        initial_h_ = spec.height;
        windows().insert(this);
    }
    ~window_impl() override
    {
        windows().erase(this);
        // WM_DESTROY can't see us once GWLP_USERDATA is cleared below.
        if (last_shown() == this) last_shown() = nullptr;
        items.clear();
        widgets.clear();
        if (hwnd_)
        {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            if (modal_ && parent) EnableWindow(parent->hwnd_, TRUE);
            if (menu_bar_ && GetMenu(hwnd_) != menu_bar_) DestroyMenu(menu_bar_);  // detached: ours to free
            RemovePropW(hwnd_, L"rapport-focus");
            DestroyWindow(hwnd_);
        }
    }
    static std::set<window_impl*>& windows()
    {
        static std::set<window_impl*> w;
        return w;
    }

    HWND hwnd() const override { return hwnd_; }
    void relayout() override
    {
        ++g_layout_pass;
        if (relayout_pending_) return;
        relayout_pending_ = true;
        PostMessageW(hwnd_, wm_rapport_relayout, 0, 0);
    }
    // With drawing off, the old page goes and the new one comes (through
    // the style bit, silently) and everything then paints in one pass,
    // rather than control by control across several frames.
    void relayout_at_once() override
    {
        if (!IsWindowVisible(hwnd_))
        {
            relayout();
            return;
        }
        SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
        layout_now();
        SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    void layout_now()
    {
        relayout_pending_ = false;
        if (!root) return;
        RECT r;
        GetClientRect(hwnd_, &r);
        const int m = margin();
        InflateRect(&r, -m, -m);
        moves_in_one_pass batch;
        root->show_all(true);
        root->place(r);
    }

    // Owned parts of the window.
    item* root = nullptr;
    std::vector<item_ptr> items;
    std::vector<std::unique_ptr<widget>> widgets;  // plain handles for containers
    std::vector<button_impl*> role_buttons;

    void set_title(const std::string& t) override { SetWindowTextW(hwnd_, wide(t).c_str()); }
    void show() override
    {
        finished_ = false;
        if (!shown_once_)
        {
            shown_once_ = true;
            // First size: one set by resize() before showing (a restored
            // size) as it is; otherwise the spec's, but never smaller than
            // the content.
            const SIZE n = root ? root->natural() : SIZE{200, 100};
            const int m = margin();
            long w = std::max<long>(n.cx + 2 * m, extent_px(hwnd_, initial_w_, false));
            long h = std::max<long>(n.cy + 2 * m, extent_px(hwnd_, initial_h_, true));
            if (requested_) w = requested_->cx, h = requested_->cy;
            RECT r{0, 0, w, h};
            AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                               DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int x = 0, y = 0;
            if (parent)
            {
                // Centred over the parent.
                RECT pr;
                GetWindowRect(parent->hwnd_, &pr);
                x = (pr.left + pr.right - (r.right - r.left)) / 2;
                y = (pr.top + pr.bottom - (r.bottom - r.top)) / 2;
            }
            else
                flags |= SWP_NOMOVE;
            SetWindowPos(hwnd_, nullptr, x, y, r.right - r.left, r.bottom - r.top, flags);
            layout_now();
        }
        if (dialog_ && parent && modal_spec_)
        {
            modal_ = true;
            EnableWindow(parent->hwnd_, FALSE);
        }
        const bool snapshot = std::getenv("RAPPORT_SNAPSHOT") != nullptr;
        ShowWindow(hwnd_, snapshot ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!snapshot) SetForegroundWindow(hwnd_);
        last_shown() = this;
        snapshot_if_asked();
    }
    void close() override
    {
        if (modal_ && parent)
        {
            EnableWindow(parent->hwnd_, TRUE);
            modal_ = false;
            SetForegroundWindow(parent->hwnd_);
        }
        ShowWindow(hwnd_, SW_HIDE);
    }
    size_f size() const override
    {
        RECT r;
        GetClientRect(hwnd_, &r);
        const float s = scale();
        return {r.right / s, r.bottom / s};
    }
    void resize(size_f sz) override
    {
        if (fullscreen_ || IsZoomed(hwnd_)) return;
        RECT r{0, 0, px(sz.w), px(sz.h)};
        if (!shown_once_) requested_ = SIZE{r.right, r.bottom};
        adjust(r);
        RECT now;
        GetWindowRect(hwnd_, &now);
        place_on_screen(now.left, now.top, r.right - r.left, r.bottom - r.top);
    }
    void set_min_size(size_f s) override { min_size_ = s; }
    std::optional<point_f> position() const override
    {
        RECT r;
        GetWindowRect(hwnd_, &r);
        const float s = scale();
        return point_f{r.left / s, r.top / s};
    }
    void move(point_f p) override
    {
        RECT now;
        GetWindowRect(hwnd_, &now);
        place_on_screen(px(p.x), px(p.y), now.right - now.left, now.bottom - now.top);
    }
    // A window keeps clear of the taskbar: what would pass the edge of its
    // monitor's work area slides back inside (the visible frame, that is;
    // Windows' resize borders are invisible and may overhang).
    void place_on_screen(int x, int y, int w, int h)
    {
        RECT want{x, y, x + w, y + h};
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &mi);
        RECT inset{0, 0, 0, 0};  // invisible border on each side
        RECT outer, visible;
        using dwm_get_fn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
        static dwm_get_fn dwm_get = reinterpret_cast<dwm_get_fn>(
            reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
        if (dwm_get && GetWindowRect(hwnd_, &outer) &&
            SUCCEEDED(dwm_get(hwnd_, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &visible, sizeof visible)) && IsWindowVisible(hwnd_))
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
    rect_f work_area() const override
    {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const float s = scale();
        return {mi.rcWork.left / s, mi.rcWork.top / s, (mi.rcWork.right - mi.rcWork.left) / s,
                (mi.rcWork.bottom - mi.rcWork.top) / s};
    }
    bool fullscreen() const override { return fullscreen_; }
    void set_fullscreen(bool on) override
    {
        if (on == fullscreen_) return;
        fullscreen_ = on;
        if (on)
        {
            if (menu_bar_) SetMenu(hwnd_, nullptr);  // no chrome in fullscreen
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
            if (menu_bar_ && menu_bar_wanted_) SetMenu(hwnd_, menu_bar_);
            SetWindowPlacement(hwnd_, &placement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        if (on_fullscreen_) on_fullscreen_(on);
    }
    void on_fullscreen_changed(std::function<void(bool)> f) override { on_fullscreen_ = std::move(f); }

    // The menu bar lives in the frame (SetMenu), outside the layout. Each
    // menu is refilled as it opens (WM_INITMENUPOPUP), so it shows the
    // app's state as it is then; its commands have ids from menu_bar_ids up,
    // a block per menu.
    static constexpr UINT menu_bar_ids = 0x4000, menu_bar_block = 0x400;
    void set_menu_bar(const std::vector<menu_bar_item>& menus)
    {
        if (menus.empty()) return;
        menu_bar_ = CreateMenu();
        for (const menu_bar_item& m : menus)
        {
            bar_menu bar{m, CreatePopupMenu(), {}};
            AppendMenuW(menu_bar_, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(bar.popup), mnemonic(m.title).c_str());
            bars_.push_back(std::move(bar));
        }
        SetMenu(hwnd_, menu_bar_);
    }
    void set_menu_bar_visible(bool on) override
    {
        menu_bar_wanted_ = on;
        if (menu_bar_ && !fullscreen_) SetMenu(hwnd_, on ? menu_bar_ : nullptr);
    }
    bool menu_opening(HMENU popup)
    {
        for (size_t i = 0; i < bars_.size(); ++i)
        {
            bar_menu& bar = bars_[i];
            if (bar.popup != popup) continue;
            while (GetMenuItemCount(popup) > 0) DeleteMenu(popup, 0, MF_BYPOSITION);  // destroys submenus too
            bar.actions.clear();
            fill_menu(popup, bar.item.menu ? bar.item.menu() : menu{}, bar.actions, menu_bar_ids + UINT(i) * menu_bar_block);
            return true;
        }
        return false;
    }
    bool menu_command(UINT id)
    {
        if (id <= menu_bar_ids) return false;
        const size_t i = (id - menu_bar_ids - 1) / menu_bar_block, n = (id - menu_bar_ids) - i * menu_bar_block;
        if (i >= bars_.size() || n == 0 || n > bars_[i].actions.size()) return false;
        if (auto action = bars_[i].actions[n - 1]) action();  // a copy: the action may rebuild the menus
        return true;
    }
    void set_icon(const icon& icon) override
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
            big = render_icon_handle(icon, GetSystemMetrics(SM_CXICON), scale());
            small = render_icon_handle(icon, GetSystemMetrics(SM_CXSMICON), scale());
        }
        if (big) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        if (small) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    }
    void on_key(std::function<bool(const key_event&)> f) override { on_key_ = std::move(f); }
    void on_finished(std::function<void(dialog_role)> f) override { on_finished_ = std::move(f); }
    void on_close_request(std::function<bool()> f) override { on_close_request_ = std::move(f); }
    // Drops on any child control reach the top level as WM_DROPFILES.
    void on_files_dropped(std::function<void(const std::vector<std::string>&)> f) override
    {
        on_files_dropped_ = std::move(f);
        DragAcceptFiles(hwnd_, bool(on_files_dropped_));
    }

    // From the message loop, before the focused control sees the key.
    bool window_key(const MSG& m)
    {
        if (!on_key_) return false;
        wchar_t cls[32] = {};
        GetClassNameW(GetFocus(), cls, 32);
        const bool typing = (!_wcsicmp(cls, L"Edit") || !_wcsicmp(cls, L"RICHEDIT50W")) &&
                            !(GetWindowLongPtrW(GetFocus(), GWL_STYLE) & ES_READONLY);
        const bool not_text = m.wParam == VK_ESCAPE || (m.wParam >= VK_F1 && m.wParam <= VK_F12);
        if (typing && !not_text) return false;
        return on_key_(make_key_event(m.wParam, m.lParam, m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN));
    }

    void finish(dialog_role role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = on_finished_) f(role);  // may delete this window: last
    }

private:
    static window_impl*& last_shown()
    {
        static window_impl* w = nullptr;
        return w;
    }

    int margin() const
    {
        if (!margins_) return 0;
        const metrics m = metrics::for_scale(scale());
        return dialog_ ? m.dialog_margin : m.margin;
    }
    void adjust(RECT& r) const
    {
        AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE)), GetMenu(hwnd_) != nullptr,
                           DWORD(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    }
    void close_requested()
    {
        if (on_close_request_ && !on_close_request_()) return;
        if (dialog_)
            finish(dialog_role::cancel);
        else
            close();
    }

    // RAPPORT_SNAPSHOT=file.png: after RAPPORT_SNAPSHOT_DELAY ms (default 500),
    // save the window shown last as PNG and quit. For checking without a screen grabber.
    void snapshot_if_asked()
    {
        static bool taken = false;
        const char* path = std::getenv("RAPPORT_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("RAPPORT_SNAPSHOT_DELAY");
        SetTimer(hwnd_, 0xA11, delay ? UINT(std::atoi(delay)) : 500u, nullptr);
    }
    static void take_snapshot()
    {
        window_impl* w = last_shown();
        const char* path = std::getenv("RAPPORT_SNAPSHOT");
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
        com<IWICBitmapEncoder> enc;
        com<IWICStream> stream;
        if (wic_factory() && SUCCEEDED(wic_factory()->CreateStream(stream.put())) &&
            SUCCEEDED(stream->InitializeFromFilename(wide(path).c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic_factory()->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) &&
            SUCCEEDED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)))
        {
            com<IWICBitmapFrameEncode> frame;
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
        auto* self = reinterpret_cast<window_impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        LRESULT r = 0;
        if (self && forward_child_message(h, msg, wp, lp, r)) return r;
        if (self)
        {
            switch (msg)
            {
            case WM_SIZE:
                if (self->root) self->layout_now();
                return 0;
            case wm_rapport_relayout: self->layout_now(); return 0;
            case WM_GETMINMAXINFO:
            {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                RECT rc{0, 0, self->px(self->min_size_.w), self->px(self->min_size_.h)};
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
            case WM_CLOSE: self->close_requested(); return 0;
            case WM_DROPFILES:
            {
                auto* drop = reinterpret_cast<HDROP>(wp);
                std::vector<std::string> paths;
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i)
                {
                    std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
                    DragQueryFileW(drop, i, path.data(), UINT(path.size() + 1));
                    paths.push_back(narrow(path));
                }
                DragFinish(drop);
                if (!paths.empty() && self->on_files_dropped_) self->on_files_dropped_(paths);
                return 0;
            }
            // Keyboard focus belongs on a control: the one that had it when
            // the window was last left (or asked for before it showed), else
            // the first Tab stop (a dialog's first field).
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE)
                {
                    if (HWND f = GetFocus(); f && IsChild(h, f)) SetPropW(h, L"rapport-focus", f);
                }
                else if (!HIWORD(wp))  // not minimised
                {
                    auto f = static_cast<HWND>(GetPropW(h, L"rapport-focus"));
                    if (!f || !IsWindow(f) || !IsChild(h, f) || !IsWindowVisible(f) || !IsWindowEnabled(f))
                        f = GetNextDlgTabItem(h, nullptr, FALSE);
                    if (f) SetFocus(f);
                    return 0;
                }
                break;
            case WM_INITMENUPOPUP:
                if (self->menu_opening(reinterpret_cast<HMENU>(wp))) return 0;
                break;
            case WM_COMMAND:
                if (!lp && HIWORD(wp) == 0 && self->menu_command(LOWORD(wp))) return 0;
                if (LOWORD(wp) == IDCANCEL && !lp)
                {
                    if (self->dialog_) self->close_requested();
                    return 0;
                }
                if (LOWORD(wp) == IDOK && !lp)
                {
                    for (button_impl* b : self->role_buttons)
                        if (b->is_default) SendMessageW(b->hwnd(), BM_CLICK, 0, 0);
                    return 0;
                }
                break;
            case WM_TIMER:
                if (wp == 0xA11)
                {
                    KillTimer(h, wp);
                    take_snapshot();
                    return 0;
                }
                break;
            case WM_DESTROY:
                if (last_shown() == self) last_shown() = nullptr;
                break;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Every control gets the font for the new DPI.
    void refont(UINT dpi)
    {
        ++g_layout_pass;  // new fonts, new sizes
        EnumChildWindows(
            hwnd_,
            +[](HWND c, LPARAM d) -> BOOL
            {
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(UINT(d))), TRUE);
                return TRUE;
            },
            LPARAM(dpi));
    }

public:
    window_impl* parent;

private:
    HWND hwnd_ = nullptr;
    bool dialog_, margins_, modal_spec_;
    bool modal_ = false, finished_ = false, shown_once_ = false, fullscreen_ = false, relayout_pending_ = false;
    std::optional<SIZE> requested_;  // client size from resize() before the first show
    struct bar_menu
    {
        menu_bar_item item;
        HMENU popup;
        std::vector<std::function<void()>> actions;
    };
    HMENU menu_bar_ = nullptr;  // destroyed with the window while attached
    std::vector<bar_menu> bars_;
    bool menu_bar_wanted_ = true;
    extent initial_w_, initial_h_;
    size_f min_size_{};
    WINDOWPLACEMENT placement_{};
    LONG_PTR style_ = 0;
    std::function<void(bool)> on_fullscreen_;
    std::function<bool(const key_event&)> on_key_;
    std::function<void(dialog_role)> on_finished_;
    std::function<bool()> on_close_request_;
    std::function<void(const std::vector<std::string>&)> on_files_dropped_;
};

// -- builder ------------------------------------------------------------------

const wchar_t* stock_label(dialog_role r)
{
    switch (r)
    {
    case dialog_role::ok: return L"OK";
    case dialog_role::cancel: return L"Cancel";
    case dialog_role::apply: return L"&Apply";
    case dialog_role::close: return L"Close";
    case dialog_role::yes: return L"&Yes";
    case dialog_role::no: return L"&No";
    case dialog_role::help: return L"&Help";
    case dialog_role::reset: return L"&Reset";
    default: return L"";
    }
}

// Windows order: OK Cancel Apply, right-aligned; Help and Reset on the left.
int windows_order(dialog_role r)
{
    switch (r)
    {
    case dialog_role::help: return 0;
    case dialog_role::reset: return 1;
    case dialog_role::ok:
    case dialog_role::yes: return 10;
    case dialog_role::no: return 11;
    case dialog_role::cancel: return 12;
    case dialog_role::close: return 13;
    case dialog_role::apply: return 14;
    default: return 5;
    }
}

class builder
{
public:
    builder(window_impl& win, build_sink& sink)
        : win_(win)
        , sink_(sink)
        , metrics_(metrics::for_scale(win.scale()))
    {
    }

    item* build(const node& n, HWND parent, orientation parent_dir)
    {
        item* item = std::visit([&](const auto& s) { return make(n, s, parent, parent_dir); }, n.spec);
        item->stretch = n.common.stretch;
        item->fixed_w = extent_px(win_.hwnd(), n.common.width, false);
        item->fixed_h = extent_px(win_.hwnd(), n.common.height, true);
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
    T* widget(const node& n, Args&&... args)
    {
        T* w = own<T>(std::forward<Args>(args)...);
        sink_.created(n, *w);
        return w;
    }
    void handle(const node& n, item* item)
    {
        if (!n.common.handle) return;
        auto w = std::make_unique<plain_widget>(*item, win_);
        sink_.created(n, *w);
        win_.widgets.push_back(std::move(w));
    }

    item* make(const node& n, const spec::box& s, HWND parent, orientation)
    {
        auto* box = own<box_item>(s.dir, metrics_.spacing, s.margins ? metrics_.margin : 0);
        for (const node& c : s.items) box->children.push_back(build(c, parent, s.dir));
        handle(n, box);
        return box;
    }
    item* make(const node& n, const spec::form& s, HWND parent, orientation)
    {
        auto* form = own<form_item>(metrics_.form_column_gap, metrics_.row_gap);
        for (size_t i = 0; i < s.fields.size(); ++i)
        {
            form_item::row row;
            if (i < s.labels.size() && !s.labels[i].empty())
            {
                auto* l = own<label_impl>(win_, parent, spec::label{s.labels[i]});
                row.label = l;
            }
            row.field = build(s.fields[i], parent, orientation::horizontal);
            form->rows.push_back(row);
        }
        handle(n, form);
        return form;
    }
    item* make(const node& n, const spec::splitter& s, HWND parent, orientation)
    {
        auto* sp = own<splitter_impl>(win_, parent, s.dir);
        for (size_t i = 0; i < s.panes.size(); ++i)
        {
            sp->panes.push_back(build(s.panes[i], parent, s.dir));
            if (i + 1 < s.panes.size()) sp->add_bar();
        }
        sink_.created(n, *sp);
        return sp;
    }
    item* make(const node& n, const spec::group& s, HWND parent, orientation)
    {
        auto* g = own<group_item>(win_, parent, s.title);
        if (!s.content.empty()) g->content = build(s.content.front(), parent, orientation::vertical);
        handle(n, g);
        return g;
    }
    item* make(const node& n, const spec::scroll& s, HWND parent, orientation)
    {
        auto* sc = own<scroll_item>(win_, parent, s.horizontal);
        if (!s.content.empty()) sc->content = build(s.content.front(), sc->host(), orientation::vertical);
        handle(n, sc);
        return sc;
    }
    item* make(const node& n, const spec::tabs& s, HWND parent, orientation)
    {
        if (s.sidebar) return pages_for(widget<side_tabs_impl>(n, win_, parent, s), s, parent);
        auto* t = pages_for(widget<tabs_impl>(n, win_, parent, s), s, parent);
        // The tab control was created first; pages must paint above it.
        SetWindowPos(t->hwnd(), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return t;
    }
    template <class T>
    T* pages_for(T* t, const spec::tabs& s, HWND parent)
    {
        for (const node& p : s.pages) t->pages.push_back(build(p, parent, orientation::vertical));
        t->lazy = s.lazy;
        for (const auto& l : s.lazy) t->isLazy.push_back(bool(l));
        t->buildPage = [&win = win_, &sink = sink_, parent](const node& page)
        { return builder(win, sink).build(page, parent, orientation::vertical); };
        return t;
    }
    item* make(const node&, const spec::spacer&, HWND, orientation)
    {
        auto* sp = own<spacer_item>();
        sp->is_spacer = true;
        return sp;
    }
    item* make(const node&, const spec::separator&, HWND parent, orientation parent_dir)
    {
        return own<separator_item>(win_, parent,
                                  parent_dir == orientation::vertical ? orientation::horizontal : orientation::vertical);
    }
    item* make(const node& n, const spec::label& s, HWND parent, orientation) { return widget<label_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::button& s, HWND parent, orientation)
    {
        auto* b = widget<button_impl>(n, win_, parent, s.icon_only ? std::string() : s.text, s.flat ? BS_FLAT : 0);
        if (s.icon) b->set_icon(s.icon);
        if (s.icon_only && n.common.tooltip.empty()) b->set_tooltip(s.text);
        return b;
    }
    item* make(const node& n, const spec::toolbar& s, HWND parent, orientation)
    {
        auto* box = own<box_item>(orientation::horizontal, win_.px(2));
        for (const node& c : s.items)
        {
            item* it = build(c, parent, orientation::horizontal);
            if (std::holds_alternative<spec::spacer>(c.spec)) it->stretch = 1;
            box->children.push_back(it);
        }
        handle(n, box);
        return box;
    }
    item* make(const node& n, const spec::tool_button& s, HWND parent, orientation)
    {
        return widget<tool_button_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::slider& s, HWND parent, orientation) { return widget<slider_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::spin& s, HWND parent, orientation) { return widget<spin_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::color_button& s, HWND parent, orientation)
    {
        return widget<color_button_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::check& s, HWND parent, orientation) { return widget<check_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::entry& s, HWND parent, orientation) { return widget<entry_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::drop_down& s, HWND parent, orientation) { return widget<drop_down_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::combo& s, HWND parent, orientation) { return widget<combo_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::text_view& s, HWND parent, orientation)
    {
        return widget<text_view_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::list& s, HWND parent, orientation) { return widget<list_impl>(n, win_, parent, s); }
    item* make(const node& n, const spec::menu_button& s, HWND parent, orientation)
    {
        return widget<menu_button_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::progress_bar&, HWND parent, orientation)
    {
        return widget<progress_bar_impl>(n, win_, parent);
    }
    item* make(const node& n, const spec::date_picker& s, HWND parent, orientation)
    {
        return widget<date_picker_impl>(n, win_, parent, s);
    }
    item* make(const node& n, const spec::canvas& s, HWND parent, orientation) { return widget<canvas_impl>(n, win_, parent, s); }
    item* make(const node&, const spec::buttons& s, HWND parent, orientation)
    {
        auto* row = own<box_item>(orientation::horizontal, metrics_.spacing);
        std::vector<const spec::dialog_button*> order;
        for (const auto& b : s.buttons) order.push_back(&b);
        std::stable_sort(order.begin(), order.end(),
                         [](auto* a, auto* b) { return windows_order(a->role) < windows_order(b->role); });
        bool spaced = false;
        for (const spec::dialog_button* b : order)
        {
            if (!spaced && windows_order(b->role) >= 5)
            {
                row->children.push_back(own<spacer_item>());
                spaced = true;
            }
            const std::string text = b->text.empty() ? narrow(stock_label(b->role)) : b->text;
            const bool is_default = b->role == dialog_role::ok || b->role == dialog_role::yes;
            auto* w = own<button_impl>(win_, parent, text, is_default ? BS_DEFPUSHBUTTON : 0);
            w->is_default = is_default;
            w->role = b->role;
            if (b->on_click)
                w->on_click(b->on_click);
            else
            {
                const dialog_role role = b->role;
                window_impl* win = &win_;
                w->on_click([win, role] { win->finish(role); });
            }
            sink_.created(*b, *w);
            win_.role_buttons.push_back(w);
            row->children.push_back(w);
        }
        if (!spaced) row->children.insert(row->children.begin(), own<spacer_item>());
        return row;
    }

    window_impl& win_;
    build_sink& sink_;
    metrics metrics_;
};

// -- backend ------------------------------------------------------------------

class win32_backend : public backend
{
public:
    static backend* create(int*, char**)
    {
        // Per-monitor DPI awareness (v2) where the system has it.
        using fn_t = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<fn_t>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
            fn(reinterpret_cast<HANDLE>(-4) /* PER_MONITOR_AWARE_V2 */);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        enable_visual_styles();
        g_comctl.resolve();
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_PROGRESS_CLASS |
                                                 ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
                                                 ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        if (!d2d_factory() || !dwrite_factory()) return nullptr;
        return new win32_backend;
    }

    win32_backend()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.hInstance = instance();
        wc.lpfnWndProc = message_proc;
        wc.lpszClassName = L"rapportMessages";
        RegisterClassExW(&wc);
        messages_ = CreateWindowExW(0, L"rapportMessages", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance(), this);
        frames_ = std::make_unique<frame_clock>(messages_);
        g_frames = frames_.get();
    }
    ~win32_backend() override
    {
        g_frames = nullptr;
        frames_.reset();
        DestroyWindow(messages_);
    }

    std::string_view name() const override { return "win32"; }

    std::unique_ptr<rap::window> create_window(const window_spec& spec, rap::window* parent, build_sink& sink) override
    {
        auto win = std::make_unique<window_impl>(spec, static_cast<window_impl*>(parent));
        win->set_menu_bar(spec.menu_bar);
        win->root = builder(*win, sink).build(spec.content, win->hwnd(), orientation::vertical);
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
                g_dragging->cancel_drag();
                continue;
            }
            if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && m.message != WM_CHAR)
            {
                HWND top = GetAncestor(m.hwnd, GA_ROOT);
                bool consumed = false;
                for (window_impl* w : window_impl::windows())
                    if (w->hwnd() == top)
                    {
                        consumed = w->window_key(m);
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
        PostMessageW(messages_, wm_rapport_post, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(f))));
    }

    void message(rap::window* parent, const message_spec& m, std::function<void(dialog_role)> done) override
    {
        UINT type = MB_OK;
        auto has = [&](dialog_role r) { return std::find(m.buttons.begin(), m.buttons.end(), r) != m.buttons.end(); };
        if (has(dialog_role::yes) && has(dialog_role::no))
            type = has(dialog_role::cancel) ? MB_YESNOCANCEL : MB_YESNO;
        else if (has(dialog_role::ok) && has(dialog_role::cancel))
            type = MB_OKCANCEL;
        switch (m.icon)
        {
        case message_spec::icon::info: type |= MB_ICONINFORMATION; break;
        case message_spec::icon::warning: type |= MB_ICONWARNING; break;
        case message_spec::icon::error: type |= MB_ICONERROR; break;
        case message_spec::icon::question: type |= MB_ICONQUESTION; break;
        }
        const std::wstring text = wide(m.text + (m.detail.empty() ? "" : "\n\n" + m.detail));
        const int r = MessageBoxW(parent ? static_cast<window_impl*>(parent)->hwnd() : nullptr, text.c_str(), wide(m.title).c_str(), type);
        if (!done) return;
        switch (r)
        {
        case IDOK: done(dialog_role::ok); break;
        case IDYES: done(dialog_role::yes); break;
        case IDNO: done(dialog_role::no); break;
        default: done(dialog_role::cancel); break;
        }
    }

    void choose_files(rap::window* parent, const file_dialog_spec& f, std::function<void(std::vector<std::string>)> done) override
    {
        std::vector<std::string> out;
        const bool save = f.mode == file_dialog_spec::mode::save;
        com<IFileDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       save ? IID_IFileSaveDialog : IID_IFileOpenDialog,
                                       reinterpret_cast<void**>(dlg.put()))))
        {
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            opts |= FOS_FORCEFILESYSTEM;
            if (f.mode == file_dialog_spec::mode::open_many) opts |= FOS_ALLOWMULTISELECT;
            if (f.mode == file_dialog_spec::mode::folder) opts |= FOS_PICKFOLDERS;
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
            if (f.mode == file_dialog_spec::mode::save && !f.filters.empty())
            {
                const std::string& first = f.filters.front().pattern;
                const size_t dot = first.find("*.");
                const std::string ext = dot == std::string::npos ? "" : first.substr(dot + 2, first.find(';', dot) - dot - 2);
                if (!ext.empty() && ext.find_first_of("*?") == std::string::npos) dlg->SetDefaultExtension(wide(ext).c_str());
            }
            if (!f.initial_path.empty())
            {
                const std::wstring path = wide(f.initial_path);
                const DWORD attrs = GetFileAttributesW(path.c_str());
                std::wstring folder = path, file;
                if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                {
                    const size_t slash = path.find_last_of(L"\\/");
                    folder = slash == std::wstring::npos ? L"" : path.substr(0, slash);
                    file = slash == std::wstring::npos ? path : path.substr(slash + 1);
                }
                com<IShellItem> item;
                if (!folder.empty() &&
                    SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem,
                                                          reinterpret_cast<void**>(item.put()))))
                    dlg->SetFolder(item.get());
                if (!file.empty()) dlg->SetFileName(file.c_str());
            }
            HWND owner = parent ? static_cast<window_impl*>(parent)->hwnd() : nullptr;
            if (SUCCEEDED(dlg->Show(owner)))
            {
                auto path_of = [](IShellItem* item)
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
                if (f.mode == file_dialog_spec::mode::open_many)
                {
                    com<IShellItemArray> items;
                    if (SUCCEEDED(static_cast<IFileOpenDialog*>(dlg.get())->GetResults(items.put())))
                    {
                        DWORD n = 0;
                        items->GetCount(&n);
                        for (DWORD i = 0; i < n; ++i)
                        {
                            com<IShellItem> it;
                            if (SUCCEEDED(items->GetItemAt(i, it.put()))) out.push_back(path_of(it.get()));
                        }
                    }
                }
                else
                {
                    com<IShellItem> it;
                    if (SUCCEEDED(dlg->GetResult(it.put()))) out.push_back(path_of(it.get()));
                }
            }
        }
        if (done) done(std::move(out));
    }

    void set_clipboard(const std::string& s) override
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

    void set_clipboard_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride) override
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
                if (format == pixel_format::rgba8)
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

    std::unique_ptr<rap::image> create_image(int w, int h, pixel_format format, const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<d2d_image>(w, h, format, pixels, stride);
    }

    subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        const UINT_PTR id = ++next_timer_;
        timers_[id] = {std::move(fn), repeat};
        SetTimer(messages_, id, UINT(std::max(ms, 1)), nullptr);
        return subscription(
            [this, id, alive = alive_]
            {
                if (!*alive) return;
                if (timers_.erase(id)) KillTimer(messages_, id);
            });
    }

private:
    // Common controls v6 (themed controls) without an application manifest:
    // activate the one shell32 carries for itself.
    static void enable_visual_styles()
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

    static LRESULT CALLBACK message_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<win32_backend*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (msg)
        {
        case wm_rapport_post:
        {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
            (*fn)();
            return 0;
        }
        case wm_rapport_frame:
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

    struct timer_entry
    {
        std::function<void()> fn;
        bool repeat;
    };
    HWND messages_ = nullptr;
    std::unique_ptr<frame_clock> frames_;
    std::map<UINT_PTR, timer_entry> timers_;
    UINT_PTR next_timer_ = 0x100;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace rap::win32

RAPPORT_BACKEND_ENTRY(win32, rap::win32::win32_backend)
