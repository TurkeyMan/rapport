// For tests that capture the screen twice and compare (win32-caption-
// glass): the caret hidden while they do, and each hide balanced.
//
// caret_hider::watch() hooks this thread's window messages for its life,
// counting the focus and activation messages (WM_SETFOCUS, WM_KILLFOCUS,
// WM_ACTIVATE, WM_NCACTIVATE). Around each attempt, hide() hides whichever
// window owns the caret now (GetGUIThreadInfo's hwndCaret) and show() shows
// it again: every hide balanced unless the window it was hidden on lost the
// focus meanwhile (its caret destroyed then, and one it has after is new,
// visible, never hidden). Activation alone never stops the balance. moved()
// says whether anything moved during the attempt (a retry), lost() whether
// the hidden caret was destroyed.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace caret_hider
{

struct state
{
    int moves = 0;
    HWND hidden_on = nullptr;
    bool lost = false;
    HHOOK hook = nullptr;
};
inline state& s()
{
    static state st;
    return st;
}

inline LRESULT CALLBACK hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const CWPSTRUCT* m = reinterpret_cast<const CWPSTRUCT*>(lp);
        const UINT msg = m->message;
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ACTIVATE || msg == WM_NCACTIVATE) ++s().moves;
        if (msg == WM_KILLFOCUS && s().hidden_on && m->hwnd == s().hidden_on) s().lost = true;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Watch this thread's focus and activation messages (until unwatch()).
inline bool watch()
{
    if (!s().hook) s().hook = SetWindowsHookExW(WH_CALLWNDPROC, hook_proc, nullptr, GetCurrentThreadId());
    return s().hook != nullptr;
}
inline void unwatch()
{
    if (s().hook) UnhookWindowsHookEx(s().hook);
    s().hook = nullptr;
}

// The window owning this thread's caret now, or null.
inline HWND owner()
{
    GUITHREADINFO gui{sizeof gui};
    return GetGUIThreadInfo(GetCurrentThreadId(), &gui) ? gui.hwndCaret : nullptr;
}
// This thread's caret shown (GUI_CARETBLINKING), if there is one.
inline bool caret_shown()
{
    GUITHREADINFO gui{sizeof gui};
    return GetGUIThreadInfo(GetCurrentThreadId(), &gui) && gui.hwndCaret && (gui.flags & GUI_CARETBLINKING);
}

// An attempt's start: the moves counted from here, the caret's owner's
// caret hidden. Its owner (null: none), and whether it was hidden.
inline HWND hide(bool& hid)
{
    s().moves = 0;
    s().lost = false;
    const HWND h = owner();
    hid = h && HideCaret(h);
    s().hidden_on = hid ? h : nullptr;
    return h;
}
// An attempt's end: what hide() hid shown again on the same window, unless
// it lost the focus meanwhile (that caret's gone).
inline void show()
{
    if (s().hidden_on && !s().lost) ShowCaret(s().hidden_on);
    s().hidden_on = nullptr;
}
// Since hide(): focus or activation messages; the hidden caret destroyed.
inline int moved()
{
    return s().moves;
}
inline bool lost()
{
    return s().lost;
}

}  // namespace caret_hider
