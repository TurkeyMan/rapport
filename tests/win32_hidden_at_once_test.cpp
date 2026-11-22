// A relayout_at_once while its window is hidden (#108): the window comes
// back as it would have been had it been shown when its content changed.
// A tab switched to a lazy page (built the first time it's shown) larger
// than the window grows a shown window to fit (grow_to_content); hidden
// (close()), switched, then shown again, it's grown to the same size as
// it's shown, whether the relayout the switch posted ran before the show or
// not. By grow_to_content's own rules: never shrunk (a window larger than
// its content stays so), and a maximised one is left as it is. Switched
// from a callback within a layout of its own, then shown later or from
// that callback itself: grown on the content as it is then.
//
// Shown without activating (RAPPORT_NO_ACTIVATE). With `maximised`, the
// maximised case alone (win32-hidden-at-once-maximised): it maximises its
// window, which activates it, and shows it as an app does, so it runs
// without RAPPORT_NO_ACTIVATE (whose SW_SHOWNOACTIVATE brings a hidden
// maximised window back restored, whatever's owed); only on a CI runner
// (GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1), else skipped (77).
#include <rapport/rapport.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
int fails = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}
bool env_is(const char* name, const char* value)
{
    const char* v = std::getenv(name);
    return v && std::strcmp(v, value) == 0;
}
void pump()
{
    for (int round = 0; round < 10; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}
std::string size_str(const RECT& r)
{
    return std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top);
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool maximised_only = argc > 1 && std::strcmp(argv[1], "maximised") == 0;
    if (maximised_only &&
        !(env_is("GITHUB_ACTIONS", "true") && env_is("RAPPORT_TEST_SEND_INPUT", "1")))
    {
        std::printf(
            "skipped: maximising activates the window: only on a CI runner (GITHUB_ACTIONS=true, "
            "RAPPORT_TEST_SEND_INPUT=1)\n");
        return 77;
    }
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    using namespace rap::layout;
    using namespace rap::literals;

    enum class how
    {
        shown,               // switched while shown: the size the others must come back at
        hidden_run_first,    // hidden, switched, the posted relayout run, then shown
        hidden_shown_first,  // hidden, switched, shown before the posted relayout's run
        hidden_nested,  // hidden, switched from within a layout of its own (a callback), then shown
        larger,         // already larger than the page: hidden, switched, shown, not shrunk
        maximised,      // maximised, hidden, switched, shown: as it was
    };
    RECT grown{};
    for (const how h : {how::shown, how::hidden_run_first, how::hidden_shown_first,
                        how::hidden_nested, how::larger, how::maximised})
    {
        const std::string what =
            h == how::shown                ? "switched while shown"
            : h == how::hidden_run_first   ? "hidden, switched, its relayout run, then shown"
            : h == how::hidden_shown_first ? "hidden, switched, shown before its relayout ran"
            : h == how::hidden_nested
                ? "hidden, switched from a canvas's on_resize in its layout, then shown"
            : h == how::larger ? "larger than the page, hidden, switched, shown"
                               : "maximised, hidden, switched, shown";
        if ((h == how::maximised) != maximised_only) continue;  // that case on its own run
        rap::tabs* pages = nullptr;
        bool switch_in_layout = false;  // once, from the first page's canvas as it's sized
        auto win = (*ui)->create(window{
            "Hidden at once",
            {tabs{{"One",
                   vbox{label{"Small"}, canvas{}
                                            .on_resize(
                                                [&](rap::size_f)
                                                {
                                                    if (!std::exchange(switch_in_layout, false))
                                                        return;
                                                    pages->set_current(1);  // within this layout
                                                })
                                            .stretch()}},
                  {"Two", lazy{[] { return rap::node(canvas{}.width(520_px).height(420_px)); }}}}
                 .bind(pages)
                 .stretch()}}.size(h == how::larger ? 800_px : 260_px,
                                   h == how::larger ? 640_px : 180_px));
        HWND top = win ? FindWindowW(nullptr, L"Hidden at once") : nullptr;
        expect(top && pages, what + ": the window and its tabs");
        if (!top || !pages) return 1;
        win->show();
        pump();
        if (h == how::maximised)
        {
            ShowWindow(top, SW_MAXIMIZE);
            pump();
            expect(IsZoomed(top) != FALSE, what + ": maximised");
        }
        RECT before;
        GetWindowRect(top, &before);
        if (h == how::shown)
            pages->set_current(1);
        else
        {
            win->close();
            pump();
            expect(!IsWindowVisible(top), what + ": hidden");
            if (h == how::hidden_nested)
            {
                // Its layout (resized while hidden), the first page's canvas
                // sized in it, which switches: an at-once within a layout.
                switch_in_layout = true;
                win->resize({300, 220});
                expect(!switch_in_layout && pages->current() == 1,
                       what + ": switched from the canvas's on_resize, within the layout");
            }
            else
                pages->set_current(1);
            if (h != how::hidden_shown_first) pump();
            win->show();
        }
        pump();
        RECT after;
        GetWindowRect(top, &after);
        expect(IsWindowVisible(top) != FALSE, what + ": shown");
        switch (h)
        {
        case how::shown:
            grown = after;
            expect(after.right - after.left > before.right - before.left &&
                       after.bottom - after.top > before.bottom - before.top,
                   what + ": grown to the page, " + size_str(before) + " to " + size_str(after));
            break;
        case how::hidden_run_first:
        case how::hidden_shown_first:
        case how::hidden_nested:
            expect(after.right - after.left == grown.right - grown.left &&
                       after.bottom - after.top == grown.bottom - grown.top,
                   what + ": grown as it is shown, " + size_str(after) + ", shown " + size_str(grown));
            break;
        case how::larger:
        case how::maximised:
            expect(EqualRect(&before, &after) != FALSE,
                   what + ": as it was, " + size_str(after) + ", before " + size_str(before));
            if (h == how::maximised) expect(IsZoomed(top) != FALSE, what + ": still maximised");
            break;
        }
        win.reset();
        pump();
    }
    // Shown from within that layout's callback itself, after switching (not
    // the maximised run): the grow it owes is measured on the content as it
    // is then, not on what was measured before the switch (the at-once's own
    // re-pass, which measures anew, hasn't run yet). A stack already built,
    // measuring only its shown page, so nothing on the way (a lazy page's
    // build) measures anew for it.
    if (!maximised_only)
    {
        const auto make = [&](rap::stack*& stk, std::function<void()>& in_layout)
        {
            return (*ui)->create(window{
                "Hidden stack",
                {vbox{
                    canvas{}
                        .on_resize(
                            [&in_layout](rap::size_f)
                            {
                                if (auto f = std::exchange(in_layout, nullptr)) f();
                            })
                        .stretch(),
                    stack{{"small", label{"Small"}}, {"big", canvas{}.width(520_px).height(420_px)}}
                        .homogeneous(false)
                        .bind(stk)}}}.size(260_px, 180_px));
        };
        // The size it grows to, shown.
        RECT grown_stack{};
        {
            rap::stack* stk = nullptr;
            std::function<void()> in_layout;
            auto win = make(stk, in_layout);
            HWND top = win ? FindWindowW(nullptr, L"Hidden stack") : nullptr;
            expect(top && stk, "a stack, shown: the window and its stack");
            if (!top || !stk) return 1;
            win->show();
            pump();
            RECT before;
            GetWindowRect(top, &before);
            stk->show("big");
            pump();
            GetWindowRect(top, &grown_stack);
            expect(grown_stack.right - grown_stack.left > before.right - before.left,
                   "a stack, shown: grown to its larger page, " + size_str(before) + " to " +
                       size_str(grown_stack));
            win.reset();
            pump();
        }
        {
            const std::string what =
                "hidden, a stack switched and the window shown from a callback within its layout";
            rap::stack* stk = nullptr;
            std::function<void()> in_layout;
            auto win = make(stk, in_layout);
            HWND top = win ? FindWindowW(nullptr, L"Hidden stack") : nullptr;
            expect(top && stk, what + ": the window and its stack");
            if (!top || !stk) return 1;
            win->show();
            pump();
            win->close();
            pump();
            in_layout = [&]
            {
                stk->show("big");
                win->show();
            };
            win->resize({300, 220});  // its layout (hidden): the canvas sized, the callback run
            expect(!in_layout, what + ": the callback run within the layout");
            pump();
            RECT after;
            GetWindowRect(top, &after);
            expect(IsWindowVisible(top) != FALSE, what + ": shown");
            expect(after.right - after.left == grown_stack.right - grown_stack.left &&
                       after.bottom - after.top == grown_stack.bottom - grown_stack.top,
                   what + ": grown as it is shown, " + size_str(after) + ", shown " +
                       size_str(grown_stack));
            win.reset();
            pump();
        }
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
