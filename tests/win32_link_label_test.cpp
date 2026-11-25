// Link labels (label::link) on Windows, in a window that's never shown: an
// ellipsized link measures as an ellipsized label does, shows its text cut
// short ("..." at its end, as rapport puts it in the SysLink) when it's given
// less, and its whole text again when there's room; it still clicks, and
// its whole text is its tooltip and accessible name. focusable(false): no
// WS_TABSTOP, so the dialog's Tab order passes it by, and it still clicks.
// A link that's only an icon is the icon's static alone (no SysLink), no
// taller than a line of text, clicked with the hand over it, its tooltip
// on it and its accessible name, a link to screen readers.
#include <rapport/rapport.h>

#include <windows.h>
#include <commctrl.h>
#include <oleacc.h>

#include "win32_test_windows.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

const std::string long_text =
    "C:\\Users\\someone\\Pictures\\holiday\\the picture with a long name.png";
const std::wstring long_wide(long_text.begin(), long_text.end());
const std::wstring copy_tip = L"Copy the path";

std::vector<HWND> descendants(HWND root, const wchar_t* cls)
{
    struct search
    {
        const wchar_t* cls;
        std::vector<HWND> found;
    } s{cls, {}};
    EnumChildWindows(
        root,
        [](HWND h, LPARAM lp) -> BOOL
        {
            auto* s = reinterpret_cast<search*>(lp);
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (!_wcsicmp(c, s->cls)) s->found.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

std::wstring text_of(HWND h)
{
    std::wstring s(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
    GetWindowTextW(h, s.data(), int(s.size()));
    // (Up to its terminator: SysLink's count of what it copied takes that in.)
    s.resize(std::wcslen(s.c_str()));
    return s;
}

// A SysLink's text without its markup's tags, however it gives it back.
std::wstring link_text(HWND h)
{
    std::wstring s = text_of(h);
    for (const wchar_t* tag : {L"<a>", L"</a>"})
        for (size_t at; (at = s.find(tag)) != std::wstring::npos;) s.erase(at, std::wcslen(tag));
    return s;
}

SIZE size_of(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return {r.right - r.left, r.bottom - r.top};
}

// The link's text as it is and its width, for a failure's log.
void note(HWND h)
{
    std::printf("     link %ld px across: \"%ls\" (%zu characters untagged; the whole text %zu)\n",
                size_of(h).cx, text_of(h).c_str(), link_text(h).size(), long_wide.size());
    std::fflush(stdout);
}

std::wstring tip_for(HWND root, HWND control)
{
    HWND tip = static_cast<HWND>(GetPropW(root, L"rapport-tooltip"));
    if (!tip) return {};
    TOOLINFOW ti{TTTOOLINFOW_V2_SIZE};
    ti.hwnd = root;
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    std::vector<wchar_t> buf(1024);
    ti.lpszText = buf.data();
    SendMessageW(tip, TTM_GETTEXTW, buf.size(), reinterpret_cast<LPARAM>(&ti));
    return buf.data();
}

// What a screen reader is told of `h` (MSAA, through its annotations).
std::wstring acc_name(HWND h, long child = CHILDID_SELF)
{
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(h, DWORD(OBJID_CLIENT), IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc)
        return L"(none)";
    VARIANT self;
    VariantInit(&self);
    self.vt = VT_I4;
    self.lVal = child;
    BSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(acc->get_accName(self, &name)) && name) out = name;
    SysFreeString(name);
    acc->Release();
    return out;
}
long acc_role(HWND h)
{
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(h, DWORD(OBJID_CLIENT), IID_IAccessible,
                                          reinterpret_cast<void**>(&acc))) || !acc)
        return -1;
    VARIANT self, role;
    VariantInit(&self);
    VariantInit(&role);
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    long out = -1;
    if (SUCCEEDED(acc->get_accRole(self, &role)) && role.vt == VT_I4) out = role.lVal;
    acc->Release();
    return out;
}

// The pointer's left button pressed and let go a few pixels into `h`.
void click(HWND h)
{
    const LPARAM at = MAKELPARAM(3, size_of(h).cy / 2);
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, at);
    SendMessageW(h, WM_LBUTTONUP, 0, at);
}

void pump()
{
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "win32";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL no win32 backend: %s\n", ui.error().c_str());
        return 1;
    }
    using namespace rap::layout;

    int long_clicks = 0, plain_clicks = 0, icon_clicks = 0;
    auto win = (*ui)->create(
        window{"rapport link label test",
               {vbox{button{"Before"},
                     label{long_text}.ellipsize().link([&] { ++long_clicks; }),
                     label{"Plain link"}.link([&] { ++plain_clicks; }).focusable(false),
                     label{}.icon(rap::icon::named("edit-copy")).link([&] { ++icon_clicks; }).tooltip(
                         std::string(copy_tip.begin(), copy_tip.end())),
                     label{"Ag"}}}});
    HWND root = test_window(L"rapport link label test");
    if (!root)
    {
        std::printf("FAIL the window\n");
        return 1;
    }
    expect(!IsWindowVisible(root), "the window is never shown");

    HWND lng = nullptr, plain = nullptr, before = nullptr, line = nullptr, icon = nullptr;
    const std::vector<HWND> links = descendants(root, L"SysLink");
    for (HWND h : links)
    {
        const std::wstring t = text_of(h);
        if (t.find(L"Plain link") != std::wstring::npos) plain = h;
        else if (t.find(L"C:\\Users") != std::wstring::npos) lng = h;
    }
    for (HWND h : descendants(root, L"Button"))
        if (text_of(h) == L"Before") before = h;
    for (HWND h : descendants(root, L"Static"))
    {
        if (text_of(h) == L"Ag") line = h;
        else if ((GetWindowLongPtrW(h, GWL_STYLE) & SS_TYPEMASK) == SS_ICON) icon = h;
    }
    expect(lng && plain && before && line && icon, "the controls are there");
    if (!lng || !plain || !before || !line || !icon) return 1;

    // -- an ellipsized link
    // (Room for it on a small screen: CI's is 1024 across.)
    win->resize({900, 400});
    pump();
    note(lng);
    expect(link_text(lng) == long_wide, "given room: its whole text");
    win->resize({220, 400});
    pump();
    note(lng);
    const std::wstring narrow = link_text(lng);
    expect(narrow.size() < long_wide.size() &&
               (narrow.ends_with(L"...") || narrow.ends_with(L"\u2026")) && narrow.starts_with(L"C:\\"),
           "given less: its text cut short, \"...\" at its end");
    LITEM item{};
    item.mask = LIF_ITEMINDEX | LIF_STATE;
    item.iLink = 0;
    item.stateMask = LIS_ENABLED;
    expect(SendMessageW(lng, LM_GETITEM, 0, reinterpret_cast<LPARAM>(&item)) != 0,
           "given less: still a link");
    expect(size_of(lng).cx < 220, "given less: the link laid out narrower than its text");
    click(lng);
    expect(long_clicks == 1, "cut short, it still runs its click");
    expect(tip_for(root, lng) == long_wide, "its tooltip: the whole text");
    expect(acc_name(lng) == long_wide, "its accessible name: the whole text");
    expect((GetWindowLongPtrW(lng, GWL_STYLE) & WS_TABSTOP) != 0, "a link: a Tab stop by default");
    win->resize({900, 400});
    pump();
    note(lng);
    expect(link_text(lng) == long_wide, "given room again: its whole text again");

    // -- focusable(false)
    expect((GetWindowLongPtrW(plain, GWL_STYLE) & WS_TABSTOP) == 0, "focusable(false): no WS_TABSTOP");
    bool passed_by = true;
    HWND first = GetNextDlgTabItem(root, nullptr, FALSE);
    for (HWND h = first; h;)
    {
        if (h == plain) passed_by = false;
        h = GetNextDlgTabItem(root, h, FALSE);
        if (h == first) break;
    }
    expect(passed_by, "focusable(false): the Tab order passes it by");
    click(plain);
    expect(plain_clicks == 1, "focusable(false): it still runs its click");

    // -- an icon-only link
    expect(links.size() == 2, "icon-only link: no SysLink of its own");
    expect(size_of(icon).cy <= size_of(line).cy, "icon-only link: no taller than a line of text");
    expect(size_of(icon).cx <= size_of(line).cy, "icon-only link: the icon alone, no gap after it");
    click(icon);
    expect(icon_clicks == 1, "icon-only link: clicking the icon runs it");
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */));
    SendMessageW(icon, WM_SETCURSOR, reinterpret_cast<WPARAM>(icon), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    expect(GetCursor() == LoadCursorW(nullptr, MAKEINTRESOURCEW(32649) /* IDC_HAND */), "icon-only link: the pointer's hand over it");
    expect(tip_for(root, icon) == copy_tip, "icon-only link: its tooltip on the icon");
    expect(acc_name(icon) == copy_tip, "icon-only link: named by its tooltip");
    expect(acc_role(icon) == ROLE_SYSTEM_LINK, "icon-only link: a link to screen readers");
    expect((GetWindowLongPtrW(icon, GWL_STYLE) & WS_TABSTOP) == 0, "icon-only link: no Tab stop");

    win.reset();
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
