// A toolbar's buttons from the keyboard (Win32): the strip they share is
// one Tab stop while a button's reachable; focused, the arrow keys, Home and
// End move the current button over the shown, enabled ones (wrapping),
// Space and Enter press it (a toggle flipped), and focus() / has_focus()
// work on each button. Its dialog code claims those keys from the dialog.
// The window is shown (focus needs it), so only on a CI runner
// (GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1); else skipped (77).
//   rapport-win32-toolbar-keyboard-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

HWND find_strip()
{
    HWND found = nullptr;
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
                    if (!std::wcscmp(cls, TOOLBARCLASSNAMEW)) *reinterpret_cast<HWND*>(p) = h;
                    return TRUE;
                },
                p);
            return TRUE;
        },
        LPARAM(&found));
    return found;
}

void key(HWND h, UINT vk)
{
    SendMessageW(h, WM_KEYDOWN, vk, 1);
    SendMessageW(h, WM_KEYUP, vk, 0xC0000001);
}

bool tab_stop(HWND h) { return (GetWindowLongPtrW(h, GWL_STYLE) & WS_TABSTOP) != 0; }

LRESULT dlg_code(HWND h, UINT vk)
{
    MSG m{h, WM_KEYDOWN, vk, 1, 0, {}};
    return SendMessageW(h, WM_GETDLGCODE, vk, reinterpret_cast<LPARAM>(&m));
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
    int a_clicks = 0, c_clicks = 0, d_toggles = 0;
    bool d_state = false;
    using namespace rap::layout;
    auto win = (*ui)->create(rap::layout::window{
        "toolbar keyboard",
        {vbox{toolbar{tool_button{rap::icon::named("go-previous"), "A"}.on_click([&] { ++a_clicks; }).bind(a),
                      tool_button{rap::icon::named("go-next"), "B"}.bind(b),
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
              rap::layout::entry{"after"}.bind(after)}}});
    b->set_enabled(false);
    win->show();

    (*ui)->post(
        [&]
        {
            HWND strip = find_strip();
            expect(strip != nullptr, "the toolbar's strip found");
            if (!strip)
            {
                (*ui)->quit(1);
                return;
            }
            expect(tab_stop(strip), "the strip is a Tab stop");
            expect((dlg_code(strip, VK_RETURN) & DLGC_WANTMESSAGE) && (dlg_code(strip, VK_SPACE) & DLGC_WANTMESSAGE) &&
                       (dlg_code(strip, VK_RIGHT) & DLGC_WANTMESSAGE),
                   "Enter, Space and the arrows are the strip's, not the dialog's");

            c->focus();
            expect(GetFocus() == strip, "focus() on a button focuses the strip");
            expect(c->has_focus() && !a->has_focus(), "and that button has the focus, the others don't");
            expect(SendMessageW(strip, TB_GETHOTITEM, 0, 0) == 2, "it's the hot item");

            key(strip, VK_RIGHT);
            expect(d->has_focus(), "Right: the next button");
            key(strip, VK_RIGHT);
            expect(a->has_focus(), "Right from the last: wraps to the first");
            key(strip, VK_RIGHT);
            expect(c->has_focus(), "Right over a disabled button: skipped");
            key(strip, VK_LEFT);
            expect(a->has_focus(), "Left: back over it");
            key(strip, VK_END);
            expect(d->has_focus(), "End: the last");
            key(strip, VK_HOME);
            expect(a->has_focus(), "Home: the first");

            key(strip, VK_SPACE);
            expect(a_clicks == 1, "Space presses the current button");
            key(strip, VK_RETURN);
            expect(a_clicks == 2, "Enter presses it too");
            key(strip, VK_END);
            key(strip, VK_SPACE);
            expect(d_toggles == 1 && d_state && d->checked(), "Space on a toggle: checked, on_toggled(true)");
            key(strip, VK_RETURN);
            expect(d_toggles == 2 && !d_state && !d->checked(), "Enter again: unchecked, on_toggled(false)");

            c->set_visible(false);
            key(strip, VK_HOME);
            key(strip, VK_RIGHT);
            expect(d->has_focus(), "a hidden button's skipped too");

            after->focus();
            expect(!d->has_focus() && GetFocus() != strip, "the focus elsewhere: no button has it");
            SetFocus(strip);
            expect(d->has_focus(), "back to the strip: the button it had");

            a->set_enabled(false);
            d->set_enabled(false);
            expect(!tab_stop(strip), "no button reachable: not a Tab stop");
            a->set_enabled(true);
            expect(tab_stop(strip), "one enabled again: a Tab stop again");
            expect(c_clicks == 0, "nothing pressed that wasn't asked for");

            win.reset();
            (*ui)->quit(fails ? 1 : 0);
        });
    return (*ui)->run();
}
