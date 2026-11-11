// Where a side button's press goes (backends/common/side_buttons.h): to
// what's under the pointer when nothing is held, else to whatever has the
// pointer's grab, as GTK delivers it.
//   rapport-side-buttons-test
#include "side_buttons.h"

#include <cstdio>

namespace
{

int fails = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

constexpr int left = 1, back = 8, forward = 9;

}  // namespace

int main()
{
    namespace sb = rap::side_buttons;
    bool under = false;  // whether the pointer is over a canvas now
    int asked = 0;
    auto pick = [&]
    {
        ++asked;
        return under;
    };

    {
        sb::router r;
        under = true;
        check(r.press(back, false, pick), "nothing held, over a canvas: the canvas's");
        r.release(back);
        under = false;
        check(!r.press(back, false, pick), "nothing held, over a list: not a canvas's");
        r.release(back);
    }
    {
        // A drag from a canvas over a list: the canvas still has the pointer.
        sb::router r;
        under = true;
        r.press(left, false, pick);
        under = false;
        asked = 0;
        check(r.press(back, true, pick), "dragging from a canvas over a list: still the canvas's");
        check(asked == 0, "and what's under the pointer isn't asked");
        r.release(back);
        r.release(left);
        check(!r.press(back, false, pick), "let go, the list's again");
        r.release(back);
    }
    {
        // The other way: a drag from a list over a canvas.
        sb::router r;
        under = false;
        r.press(left, false, pick);
        under = true;
        check(!r.press(forward, true, pick), "dragging from a list over a canvas: not the canvas's");
        r.release(forward);
        r.release(left);
        check(r.press(forward, false, pick), "let go, the canvas's again");
        r.release(forward);
    }
    {
        // Back held from a canvas, the pointer moves off, Forward pressed:
        // the grab is the canvas's (no ordinary button in the state).
        sb::router r;
        under = true;
        r.press(back, false, pick);
        under = false;
        check(r.press(forward, false, pick), "a side button held from a canvas keeps the grab");
        // Either let go first: the other still holds it.
        r.release(back);
        check(r.press(back, false, pick), "with one still held, in either order");
        r.release(forward);
        r.release(back);
        check(!r.press(back, false, pick), "all let go: what's under the pointer");
        r.release(back);
    }
    {
        // A release lost to another window: the same button pressed again
        // starts afresh rather than keeping a stale grab.
        sb::router r;
        under = true;
        r.press(back, false, pick);
        under = false;
        check(!r.press(back, false, pick), "a side button's release lost: its next press starts afresh");
    }
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
