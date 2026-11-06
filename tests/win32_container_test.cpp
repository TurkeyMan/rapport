#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>
#include <cstdio>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
int failures;
void expect(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "failed: %s\n", why); ++failures; }
}

std::wstring tip_for(HWND tip, HWND root, HWND control)
{
    TOOLINFOW ti{TTTOOLINFOW_V2_SIZE};
    ti.hwnd = root;
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    wchar_t buf[256]{};
    ti.lpszText = buf;
    SendMessageW(tip, TTM_GETTEXTW, 256, reinterpret_cast<LPARAM>(&ti));
    return buf;
}

int gap_tools(HWND tip, HWND root, POINT point, UINT_PTR& smallest)
{
    int matches = 0;
    long height = LONG_MAX;
    for (int i = 0; i < SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0); ++i)
    {
        TOOLINFOW ti{TTTOOLINFOW_V2_SIZE};
        if (SendMessageW(tip, TTM_ENUMTOOLSW, i, reinterpret_cast<LPARAM>(&ti)) &&
            ti.hwnd == root && !(ti.uFlags & TTF_IDISHWND) && PtInRect(&ti.rect, point))
        {
            ++matches;
            if (ti.rect.bottom - ti.rect.top < height)
            {
                height = ti.rect.bottom - ti.rect.top;
                smallest = ti.uId;
            }
        }
    }
    return matches;
}
}

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using namespace rap::layout;
    rap::widget *outer = nullptr, *inner = nullptr, *late = nullptr;
    rap::button *a = nullptr, *b = nullptr;
    auto win = (*ui)->create(window{"Rapport container properties", {vbox{
        form{{"Field", button{"unbound"}}}.enabled(false).tooltip("unbound tip"),
        vbox{button{"parent"},
             vbox{button{"a"}.bind(a), button{"b"}.enabled(false).tooltip("own").bind(b)}
                 .margins().tooltip("inner").handle(inner),
             deferred{[] { return button{"deferred"}; }}.visible(false).handle(late)}
            .enabled(false).tooltip("outer").handle(outer)}}});
    late->set_visible(true);
    win->show();
    HWND root = FindWindowW(nullptr, L"Rapport container properties");
    HWND tip = static_cast<HWND>(GetPropW(root, L"rapport-tooltip"));
    const auto button_hwnd = [&](const wchar_t* text) { return FindWindowExW(root, nullptr, L"Button", text); };
    HWND parent = button_hwnd(L"parent"), ah = button_hwnd(L"a"), bh = button_hwnd(L"b"),
         dh = button_hwnd(L"deferred"), uh = button_hwnd(L"unbound");
    if (!root || !tip || !parent || !ah || !bh || !dh || !uh) return 2;
    expect(!IsWindowEnabled(uh) && !IsWindowEnabled(parent) && !IsWindowEnabled(dh),
          "unbound, bound and deferred descendants inherit disabled state");
    expect(!IsWindowEnabled(ah) && !IsWindowEnabled(bh), "nested children inherit disabled state");
    outer->set_enabled(true);
    expect(IsWindowEnabled(ah) && !IsWindowEnabled(bh), "an initially disabled child stays disabled");
    outer->set_enabled(false);
    a->set_enabled(false);
    b->set_enabled(true);
    expect(!IsWindowEnabled(bh), "enabling a child while its parent is disabled");
    inner->set_enabled(false);
    outer->set_enabled(true);
    expect(IsWindowEnabled(parent) && IsWindowEnabled(dh) && !IsWindowEnabled(bh), "nested disabled state");
    inner->set_enabled(true);
    expect(!IsWindowEnabled(ah) && IsWindowEnabled(bh), "local child state restored");
    outer->set_enabled(false);
    outer->set_enabled(true);
    expect(!IsWindowEnabled(ah) && IsWindowEnabled(bh), "round-trip");
    expect(tip_for(tip, root, uh) == L"unbound tip" && tip_for(tip, root, dh) == L"outer", "inherited tips");
    expect(tip_for(tip, root, ah) == L"inner" && tip_for(tip, root, bh) == L"own", "nearest and own tips");
    b->set_tooltip("");
    inner->set_tooltip("");
    expect(tip_for(tip, root, bh) == L"outer", "removing overrides reveals ancestor");
    inner->set_tooltip("inner");
    RECT r{};
    GetWindowRect(ah, &r);
    POINT gap{r.left - 1, r.top + 1};
    ScreenToClient(root, &gap);
    UINT_PTR inner_tool = 0;
    expect(gap_tools(tip, root, gap, inner_tool) >= 2, "nested container margin has a rectangle tool");
    // Put the margin under the current cursor without moving the pointer.
    POINT cursor{}, screen_gap = gap;
    GetCursorPos(&cursor);
    ClientToScreen(root, &screen_gap);
    RECT window_rect;
    GetWindowRect(root, &window_rect);
    SetWindowPos(root, nullptr, window_rect.left + cursor.x - screen_gap.x,
                 window_rect.top + cursor.y - screen_gap.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    TTHITTESTINFOW hit{};
    hit.hwnd = root;
    hit.pt = gap;
    hit.ti.cbSize = TTTOOLINFOW_V2_SIZE;
    expect(SendMessageW(tip, TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit)) &&
               !(hit.ti.uFlags & TTF_IDISHWND), "nested margin hit test");
    wchar_t text[256]{};
    hit.ti.lpszText = text;
    SendMessageW(tip, TTM_GETTEXTW, 256, reinterpret_cast<LPARAM>(&hit.ti));
    expect(std::wstring_view(text) == L"inner", "nearest container text over its gap");
    inner->set_visible(false);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    TOOLINFOW hidden{TTTOOLINFOW_V2_SIZE};
    hidden.hwnd = root;
    hidden.uId = inner_tool;
    expect(!SendMessageW(tip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&hidden)),
           "hidden container drops its gap tool");
    return failures ? 1 : 0;
}
