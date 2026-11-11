// The click that closes a popup menu dropped from a button, kept from
// opening the menu again. A click outside a popup menu closes it and is left
// in the queue for the window under it, so a second click on the button that
// opened the menu would open it again rather than close it, as a combo box
// does. The menu's own message loop is watched for the button press that
// ends it; once the menu returns, that press, and only that one, is taken
// from the button's queue. Anything else queued stays: a click made after
// the menu closed, and a pending WM_QUIT.
#pragma once

#include <windows.h>

namespace rap::win32
{

// A button press the menu's loop saw: its time and screen position, which
// the same message keeps when it reaches the window under it.
struct menu_press
{
    DWORD time = 0;
    POINT pt{};
    bool seen = false;
};

// Takes `press` from `anchor`'s queue if it's the next button press there.
// True if it did. A WM_QUIT met on the way is put back.
inline bool drop_menu_press(HWND anchor, const menu_press& press)
{
    if (!anchor || !press.seen) return false;
    MSG msg;
    // PM_NOREMOVE first: PeekMessage hands back WM_QUIT whatever the filter.
    if (!PeekMessageW(&msg, anchor, WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, PM_NOREMOVE | PM_NOYIELD))
        return false;
    if (msg.message != WM_LBUTTONDOWN && msg.message != WM_LBUTTONDBLCLK) return false;
    if (msg.time != press.time || msg.pt.x != press.pt.x || msg.pt.y != press.pt.y) return false;
    const UINT which = msg.message;
    if (!PeekMessageW(&msg, anchor, which, which, PM_REMOVE | PM_NOYIELD)) return false;
    if (msg.message == WM_QUIT)
    {
        PostQuitMessage(int(msg.wParam));
        return false;
    }
    return true;
}

// Watches the menu loops of this thread while it lives, keeping the last
// left-button press any of them saw (in the menu or outside it).
class menu_press_watch
{
public:
    menu_press_watch()
    {
        previous_ = current();
        current() = this;
        hook_ = SetWindowsHookExW(WH_MSGFILTER, filter, nullptr, GetCurrentThreadId());
    }
    ~menu_press_watch()
    {
        if (hook_) UnhookWindowsHookEx(hook_);
        current() = previous_;
    }
    menu_press_watch(const menu_press_watch&) = delete;
    menu_press_watch& operator=(const menu_press_watch&) = delete;

    const menu_press& last() const { return last_; }

private:
    static menu_press_watch*& current()
    {
        thread_local menu_press_watch* w = nullptr;
        return w;
    }
    static LRESULT CALLBACK filter(int code, WPARAM w, LPARAM l)
    {
        if (code == MSGF_MENU && current())
        {
            const MSG* m = reinterpret_cast<const MSG*>(l);
            if (m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONDBLCLK ||
                m->message == WM_NCLBUTTONDOWN || m->message == WM_NCLBUTTONDBLCLK)
                current()->last_ = {m->time, m->pt, true};
        }
        return CallNextHookEx(nullptr, code, w, l);
    }

    HHOOK hook_ = nullptr;
    menu_press_watch* previous_ = nullptr;
    menu_press last_;
};

}  // namespace rap::win32
