// The click that closed a button's popup menu is dropped from the button's
// queue, and nothing else is: a later click, a click after the menu closed
// some other way, and a pending WM_QUIT all stay. Messages are posted to a
// message-only window, so nothing on the desktop is touched.
#include "menu_dismiss.h"

#include <cstdio>
#include <vector>

using rap::win32::drop_menu_press;
using rap::win32::menu_press;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) return;
    std::printf("FAILED: %s\n", what);
    ++failures;
}

struct queued
{
    UINT message;
    WPARAM wparam;
};

// Everything queued for `h`, and a WM_QUIT if one is pending, taken in order.
std::vector<queued> drain(HWND h)
{
    std::vector<queued> out;
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE | PM_NOYIELD))
        if (m.hwnd == h || m.message == WM_QUIT) out.push_back({m.message, m.wParam});
    return out;
}

bool same(const std::vector<queued>& got, std::vector<UINT> want)
{
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (got[i].message != want[i]) return false;
    return true;
}

void post_click(HWND h)
{
    PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
    PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(2, 2));
}

// The press the menu's loop would have seen: the next button-down's own.
menu_press next_press(HWND h)
{
    MSG m;
    if (!PeekMessageW(&m, h, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_NOREMOVE | PM_NOYIELD)) return {};
    return {m.time, m.pt, true};
}

}  // namespace

int main()
{
    HWND h = CreateWindowExW(0, L"BUTTON", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h)
    {
        std::printf("FAILED: no message-only window\n");
        return 1;
    }
    drain(h);

    {
        // The dismissing click, then a fresh one: only the first press goes,
        // so the fresh click opens the menu again.
        post_click(h);
        post_click(h);
        const menu_press p = next_press(h);
        check(drop_menu_press(h, p), "the dismissing press is dropped");
        check(same(drain(h), {WM_LBUTTONUP, WM_LBUTTONDOWN, WM_LBUTTONUP}),
              "a click after the dismissing one stays whole");
    }
    {
        // Closed with Escape: the menu's loop saw no press, so a click
        // queued since stays.
        post_click(h);
        check(!drop_menu_press(h, menu_press{}), "nothing is dropped after Escape");
        check(same(drain(h), {WM_LBUTTONDOWN, WM_LBUTTONUP}), "a click after Escape stays");
    }
    {
        // A press inside the menu (a separator, say) before Escape: it's
        // not the button's next press, which stays.
        post_click(h);
        menu_press p = next_press(h);
        p.time -= 5000;
        check(!drop_menu_press(h, p), "a press the button didn't get isn't dropped");
        p.time += 5000;
        p.pt.x += 7;
        check(!drop_menu_press(h, p), "a press elsewhere isn't dropped");
        check(same(drain(h), {WM_LBUTTONDOWN, WM_LBUTTONUP}), "the button's own click stays");
    }
    {
        // The button's next mouse message is a release: its press has gone
        // already, so nothing later is taken for it.
        PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(2, 2));
        post_click(h);
        const menu_press p = next_press(h);
        check(!drop_menu_press(h, p), "nothing is dropped past a release");
        check(same(drain(h), {WM_LBUTTONUP, WM_LBUTTONDOWN, WM_LBUTTONUP}), "the queue is untouched");
    }
    {
        // A pending quit with nothing to drop survives.
        PostQuitMessage(37);
        menu_press p{GetTickCount(), {}, true};
        check(!drop_menu_press(h, p), "nothing is dropped with only a quit pending");
        const std::vector<queued> q = drain(h);
        check(q.size() == 1 && q[0].message == WM_QUIT && q[0].wparam == 37,
              "a pending WM_QUIT survives, with its code");
    }
    {
        // A pending quit with the dismissing press queued: the press goes,
        // the quit stays.
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
        const menu_press p = next_press(h);
        PostQuitMessage(41);
        check(drop_menu_press(h, p), "the dismissing press is dropped with a quit pending");
        const std::vector<queued> q = drain(h);
        check(q.size() == 1 && q[0].message == WM_QUIT && q[0].wparam == 41,
              "the quit survives dropping the press");
    }

    DestroyWindow(h);
    if (failures == 0) std::printf("all passed\n");
    return failures == 0 ? 0 : 1;
}
