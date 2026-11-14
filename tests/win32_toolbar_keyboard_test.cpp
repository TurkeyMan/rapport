// A toolbar's buttons from the keyboard (Win32), its keys posted through the
// app's own message loop (IsDialogMessage, the window's on_key) as typed
// keys come: the toolbar is one Tab stop though a spacer parts its buttons
// into two strips; Tab and Shift+Tab go to and from it; focused, the arrow
// keys, Home and End move the current button over the shown, enabled ones
// of both strips (wrapping), and Space and Enter press it (a toggle
// flipped) without the dialog's default button pressed; a window on_key
// that takes those keys doesn't get them from the toolbar, and still gets
// them elsewhere. focus() and has_focus() work on each button.
// The dialog is shown (focus needs it), so only on a CI runner
// (GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1); else skipped (77).
//   rapport-win32-toolbar-keyboard-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(RAPPORT_TEST_BUILTIN_WIN32)
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

bool env_is(const char* name, const char* value)
{
    const char* v = std::getenv(name);
    return v && !std::strcmp(v, value);
}

std::vector<HWND> strips()
{
    std::vector<HWND> found;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND h, LPARAM p) -> BOOL
                {
                    wchar_t cls[32] = {};
                    GetClassNameW(h, cls, 32);
                    if (!std::wcscmp(cls, TOOLBARCLASSNAMEW)) reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
                    return TRUE;
                },
                p);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    // Left to right, as laid out.
    std::sort(found.begin(), found.end(),
              [](HWND a, HWND b)
              {
                  RECT ra, rb;
                  GetWindowRect(a, &ra);
                  GetWindowRect(b, &rb);
                  return ra.left < rb.left;
              });
    return found;
}

bool tab_stop(HWND h) { return (GetWindowLongPtrW(h, GWL_STYLE) & WS_TABSTOP) != 0; }

// Shift held (for Shift+Tab) or not, as the loop's GetKeyState sees it
// while it handles the keys posted after.
void hold_shift(bool on)
{
    BYTE state[256] = {};
    GetKeyboardState(state);
    state[VK_SHIFT] = on ? 0x80 : 0;
    SetKeyboardState(state);
}

// A key as typed: down (with its character, as TranslateMessage would make
// it, for Space and Enter) and up, posted to the focus, through the loop.
// Shift is let go before the next step (each step starts so).
void type_key(UINT vk, bool shift = false)
{
    HWND f = GetFocus();
    hold_shift(shift);
    const LPARAM scan = LPARAM(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
    PostMessageW(f, WM_KEYDOWN, vk, scan | 1);
    if (vk == VK_SPACE) PostMessageW(f, WM_CHAR, L' ', scan | 1);
    if (vk == VK_RETURN) PostMessageW(f, WM_CHAR, L'\r', scan | 1);
    PostMessageW(f, WM_KEYUP, vk, scan | 0xC0000001);
}

}  // namespace

int main(int argc, char** argv)
{
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SEND_INPUT", "1"))
    {
        std::printf("skipped: shows a window and moves the focus, only on a CI runner "
                    "(GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1)\n");
        return 77;
    }
    rap::load_options opts;
    opts.backend = "win32";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 2;
    }
    rap::tool_button *a = nullptr, *b = nullptr, *c = nullptr, *d = nullptr;
    rap::entry* after = nullptr;
    int a_clicks = 0, c_clicks = 0, d_toggles = 0, finished = 0, window_keys = 0;
    bool d_state = false;
    using namespace rap::layout;
    auto win = (*ui)->create(rap::layout::dialog{
        "toolbar keyboard",
        {vbox{toolbar{tool_button{rap::icon::named("go-previous"), "A"}.on_click([&] { ++a_clicks; }).bind(a),
                      tool_button{rap::icon::named("go-next"), "B"}.bind(b),
                      spacer{},
                      tool_button{rap::icon::named("go-up"), "C"}.on_click([&] { ++c_clicks; }).bind(c),
                      tool_button{rap::icon::named("view-refresh"), "D"}
                          .toggle()
                          .on_toggled(
                              [&](bool on)
                              {
                                  ++d_toggles;
                                  d_state = on;
                              })
                          .bind(d)},
              rap::layout::entry{"after"}.bind(after), buttons{{{rap::dialog_role::ok}}}}}});
    win->on_finished([&](rap::dialog_role) { ++finished; });
    // An app that takes the arrows, Space and Enter for itself (IfritView's
    // image keys) wherever the focus doesn't keep them.
    win->on_key(
        [&](const rap::key_event& e)
        {
            if (!e.press) return false;
            switch (e.key)
            {
            case rap::key::left:
            case rap::key::right:
            case rap::key::up:
            case rap::key::down:
            case rap::key::home:
            case rap::key::end:
            case rap::key::space:
            case rap::key::enter: ++window_keys; return true;
            default: return false;
            }
        });
    b->set_enabled(false);
    win->show();

    // Steps, each after the keys the one before posted have been handled.
    std::vector<std::function<void()>> steps;
    size_t at = 0;
    std::function<void()> next = [&]
    {
        if (at == steps.size())
        {
            win.reset();
            (*ui)->quit(fails ? 1 : 0);
            return;
        }
        hold_shift(false);
        steps[at++]();
        (*ui)->post([&] { next(); });
    };
    std::vector<HWND> s;
    auto focused = [&] { return GetFocus(); };

    steps.push_back(
        [&]
        {
            s = strips();
            expect(s.size() == 2, "a spacer parts the toolbar into two strips (" + std::to_string(s.size()) + ")");
            if (s.size() != 2) return;
            expect(tab_stop(s[0]) != tab_stop(s[1]), "one Tab stop between them");
            after->focus();
            type_key(VK_TAB, true);
        });
    steps.push_back(
        [&]
        {
            expect(focused() == s[0] && a->has_focus(), "Shift+Tab from the entry: the toolbar, its first button");
            type_key(VK_RIGHT);
        });
    steps.push_back(
        [&]
        {
            expect(c->has_focus() && focused() == s[1], "Right over a disabled button: into the next strip, its first");
            type_key(VK_RIGHT);
        });
    steps.push_back(
        [&]
        {
            expect(d->has_focus(), "Right: the next");
            type_key(VK_RIGHT);
        });
    steps.push_back(
        [&]
        {
            expect(a->has_focus() && focused() == s[0], "Right from the last: wraps to the first strip's first");
            type_key(VK_LEFT);
        });
    steps.push_back(
        [&]
        {
            expect(d->has_focus(), "Left from the first: the last");
            type_key(VK_HOME);
        });
    steps.push_back(
        [&]
        {
            expect(a->has_focus(), "Home: the first");
            type_key(VK_SPACE);
        });
    steps.push_back(
        [&]
        {
            expect(a_clicks == 1, "Space presses the current button");
            type_key(VK_RETURN);
        });
    steps.push_back(
        [&]
        {
            expect(a_clicks == 2, "Enter presses it too");
            expect(finished == 0, "and not the dialog's default button");
            expect(window_keys == 0, "the window's on_key got none of the toolbar's keys");
            type_key(VK_END);
        });
    steps.push_back(
        [&]
        {
            type_key(VK_SPACE);
        });
    steps.push_back(
        [&]
        {
            expect(d_toggles == 1 && d_state && d->checked(), "Space on a toggle: checked, on_toggled(true)");
            type_key(VK_RETURN);
        });
    steps.push_back(
        [&]
        {
            expect(d_toggles == 2 && !d_state && !d->checked(), "Enter again: unchecked, on_toggled(false)");
            expect(tab_stop(s[1]) && !tab_stop(s[0]), "the Tab stop is the strip the focus was last in");
            type_key(VK_TAB);
        });
    steps.push_back(
        [&]
        {
            expect(focused() != s[0] && focused() != s[1], "Tab: out of the toolbar, not into its other strip");
            expect(!d->has_focus(), "no button has the focus then");
            type_key(VK_TAB, true);
        });
    steps.push_back(
        [&]
        {
            // Shift+Tab may land on the entry first, the toolbar before it.
            if (focused() != s[1]) type_key(VK_TAB, true);
        });
    steps.push_back(
        [&]
        {
            expect(focused() == s[1] && d->has_focus(), "back to the toolbar: the button it had");
            // To the dialog's OK button (past the entry): not a toolbar, so
            // there the window's on_key has its keys first.
            type_key(VK_TAB);
        });
    steps.push_back([&] { type_key(VK_TAB); });
    steps.push_back(
        [&]
        {
            wchar_t cls[16] = {};
            GetClassNameW(focused(), cls, 16);
            expect(!_wcsicmp(cls, L"Button"), "Tab, Tab: the OK button");
            type_key(VK_RIGHT);
        });
    steps.push_back(
        [&]
        {
            expect(window_keys == 1, "elsewhere the window's on_key still gets its keys (" +
                                         std::to_string(window_keys) + ")");
            c->set_visible(false);
        });
    steps.push_back(
        [&]
        {
            c->focus();
            expect(!c->has_focus(), "focus() on a hidden button: not the current one");
            a->focus();
            expect(a->has_focus() && focused() == s[0], "focus() on a button: it, its strip focused");
            // The focused button disabled, its strip left with none to
            // reach: the next strip's, the focus there.
            a->set_enabled(false);
            expect(d->has_focus() && focused() == s[1],
                   "the focused button disabled, none left in its strip: the next strip's");
            type_key(VK_SPACE);
        });
    steps.push_back(
        [&]
        {
            expect(d_toggles == 3, "and Space presses it");
            a->set_enabled(true);
            a->focus();
            // Hidden: its strip's laid out (and finds it gone) on the loop.
            a->set_visible(false);
        });
    steps.push_back(
        [&]
        {
            expect(d->has_focus() && focused() == s[1],
                   "the focused button hidden, none left in its strip: the next strip's");
            a->set_visible(true);
        });
    steps.push_back(
        [&]
        {
            // D first, so the focus is left on A's strip with nothing in it.
            a->focus();
            d->set_enabled(false);
            a->set_enabled(false);
            expect(!tab_stop(s[0]) && !tab_stop(s[1]), "no button reachable: no Tab stop");
            expect(focused() == s[0] && !a->has_focus() && !d->has_focus(),
                   "the focus left on A's strip, no button current");
            // One enabled again in the other strip than the focus's: it's
            // the current button, the focus there.
            d->set_enabled(true);
            expect(tab_stop(s[1]) && !tab_stop(s[0]), "one enabled again: its strip the Tab stop");
            expect(d->has_focus() && focused() == s[1], "and the current button, the focus moved to it");
            type_key(VK_SPACE);
        });
    steps.push_back(
        [&]
        {
            expect(d_toggles == 4, "Space presses it");
            expect(c_clicks == 0, "nothing pressed that wasn't asked for");
        });

    (*ui)->post([&] { next(); });
    return (*ui)->run();
}
