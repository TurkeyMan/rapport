// A relayout_at_once while its window is hidden (#108): the window comes
// back as it would have been had it been shown when its content changed.
// A tab switched to a lazy page (built the first time it's shown) larger
// than the window grows a shown window to fit (grow_to_content); hidden
// (close()), switched, then shown again, it's grown to the same size as
// it's shown, whether the relayout the switch posted ran before the show or
// not. By grow_to_content's own rules: never shrunk (a window larger than
// its content stays so), and a maximised one is left as it is.
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
#include <memory>
#include <string>

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
        shown,              // switched while shown: the size the others must come back at
        hidden_run_first,   // hidden, switched, the posted relayout run, then shown
        hidden_shown_first, // hidden, switched, shown before the posted relayout's run
        larger,             // already larger than the page: hidden, switched, shown, not shrunk
        maximised,          // maximised, hidden, switched, shown: as it was
    };
    RECT grown{};
    for (const how h : {how::shown, how::hidden_run_first, how::hidden_shown_first, how::larger, how::maximised})
    {
        const std::string what = h == how::shown                ? "switched while shown"
                                 : h == how::hidden_run_first   ? "hidden, switched, its relayout run, then shown"
                                 : h == how::hidden_shown_first ? "hidden, switched, shown before its relayout ran"
                                 : h == how::larger             ? "larger than the page, hidden, switched, shown"
                                                                : "maximised, hidden, switched, shown";
        if ((h == how::maximised) != maximised_only) continue;  // that case on its own run
        rap::tabs* pages = nullptr;
        auto win = (*ui)->create(
            window{"Hidden at once",
                   {tabs{{"One", label{"Small"}},
                         {"Two", lazy{[] { return rap::node(canvas{}.width(520_px).height(420_px)); }}}}
                        .bind(pages)
                        .stretch()}}
                .size(h == how::larger ? 800_px : 260_px, h == how::larger ? 640_px : 180_px));
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
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
