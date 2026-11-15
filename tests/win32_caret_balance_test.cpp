// win32_caret_hider.h's balance, in a never-shown window (no focus, no
// input, nothing on the screen): its caret made and shown, then
//  - activation alone during an attempt (WM_NCACTIVATE, the caret still
//    there): the attempt unsteady, and the hide balanced all the same, so a
//    retry's hide and show leave the caret shown (#87: skipping the show on
//    any activation left it hidden);
//  - the caret's window losing the focus during an attempt (WM_KILLFOCUS, its
//    caret destroyed and a new one made and shown): no show for the gone
//    caret, the new one shown;
//  - a steady attempt: nothing moved, the caret shown after;
//  - a caret hidden before an attempt (the hides cumulative): with
//    activation during it, still hidden after, balanced (one ShowCaret
//    shows it).
//   rapport-win32-caret-balance-test
#include "win32_caret_hider.h"

#include <cstdio>
#include <string>

namespace
{
int fails = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

int main()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"rapportCaretBalanceTest";
    RegisterClassW(&wc);
    // Never shown (no WS_VISIBLE, no ShowWindow).
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Caret balance", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr,
                             nullptr, wc.hInstance, nullptr);
    expect(h != nullptr, "a never-shown window");
    if (!h) return 1;
    expect(CreateCaret(h, nullptr, 2, 20) && ShowCaret(h), "its caret made and shown");
    expect(caret_hider::owner() == h && caret_hider::caret_shown(), "the caret its, shown");
    expect(caret_hider::watch(), "this thread's focus and activation watched");

    // Activation alone: unsteady, the hide balanced.
    {
        bool hid = false;
        expect(caret_hider::hide(hid) == h && hid, "attempt 1: the caret hidden");
        expect(!caret_hider::caret_shown(), "attempt 1: hidden");
        SendMessageW(h, WM_NCACTIVATE, TRUE, 0);
        expect(caret_hider::moved() > 0 && !caret_hider::lost(),
               "attempt 1: activation seen, the caret not lost");
        caret_hider::show();
        expect(caret_hider::caret_shown(), "attempt 1: shown again, though activation moved");
    }
    // The retry: steady, and the caret shown after (not left hidden by an
    // unbalanced first hide).
    {
        bool hid = false;
        caret_hider::hide(hid);
        expect(hid && caret_hider::moved() == 0, "attempt 2: hidden, nothing moved");
        caret_hider::show();
        expect(caret_hider::caret_shown(), "attempt 2: the caret shown after the retry, every hide balanced");
    }
    // The caret's window losing the focus: its caret destroyed, a new one
    // made and shown (as an edit does when the focus comes back); no show
    // for the gone one, the new one shown.
    {
        bool hid = false;
        caret_hider::hide(hid);
        SendMessageW(h, WM_KILLFOCUS, 0, 0);
        DestroyCaret();
        CreateCaret(h, nullptr, 2, 20);
        ShowCaret(h);
        expect(caret_hider::lost(), "attempt 3: the hidden caret's window lost the focus");
        caret_hider::show();
        expect(caret_hider::caret_shown(), "attempt 3: the new caret shown");
        // One hide and show of the new caret: shown after, so nothing was
        // owed to it (a show to the gone caret would have been).
        HideCaret(h);
        ShowCaret(h);
        expect(caret_hider::caret_shown(), "attempt 3: the new caret's count balanced");
    }
    // Hidden before the attempt: hidden after it too, activation and all,
    // and one ShowCaret (the one owed for the hide before) shows it.
    {
        HideCaret(h);
        expect(!caret_hider::caret_shown(), "attempt 4: the caret hidden beforehand");
        bool hid = false;
        caret_hider::hide(hid);
        SendMessageW(h, WM_NCACTIVATE, FALSE, 0);
        caret_hider::show();
        expect(hid && !caret_hider::caret_shown(), "attempt 4: still hidden after, as it was");
        ShowCaret(h);
        expect(caret_hider::caret_shown(), "attempt 4: one ShowCaret shows it: balanced");
    }
    caret_hider::unwatch();
    DestroyCaret();
    DestroyWindow(h);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
