// window::show() of a window shown before, hidden since (close()), under
// RAPPORT_NO_ACTIVATE: it comes back as it was, restored, minimised or
// maximised. NO_ACTIVATE changes only the activation (SW_SHOWNA, not
// SW_SHOWNOACTIVATE, which restores a minimised or maximised window).
//
// Shown without activating. Minimised without activating too
// (SW_SHOWMINNOACTIVE); the maximised case maximises its window, which
// activates it: only on a CI runner (GITHUB_ACTIONS=true,
// RAPPORT_TEST_SEND_INPUT=1), else skipped (that case alone).
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
std::string rect_str(const RECT& r)
{
    return std::to_string(r.left) + "," + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + "x" +
           std::to_string(r.bottom - r.top);
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    using namespace rap::layout;
    using namespace rap::literals;
    const bool can_activate = env_is("GITHUB_ACTIONS", "true") && env_is("RAPPORT_TEST_SEND_INPUT", "1");

    enum class state
    {
        restored,
        minimised,
        maximised,
    };
    for (const state st : {state::restored, state::minimised, state::maximised})
    {
        const std::string what = st == state::restored    ? "restored"
                                 : st == state::minimised ? "minimised"
                                                          : "maximised";
        if (st == state::maximised && !can_activate)
        {
            std::printf("skipped: maximised (maximising activates the window): only on a CI runner "
                        "(GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1)\n");
            continue;
        }
        auto win = (*ui)->create(window{"Show again", {label{"Content"}}}.size(300_px, 200_px));
        HWND top = win ? FindWindowW(nullptr, L"Show again") : nullptr;
        expect(top != nullptr, what + ": the window");
        if (!top) return 1;
        win->show();
        pump();
        if (st == state::minimised) ShowWindow(top, SW_SHOWMINNOACTIVE);
        if (st == state::maximised) ShowWindow(top, SW_MAXIMIZE);
        pump();
        const bool iconic = IsIconic(top) != FALSE, zoomed = IsZoomed(top) != FALSE;
        expect(iconic == (st == state::minimised) && zoomed == (st == state::maximised), what + ": so before");
        WINDOWPLACEMENT was{sizeof was};
        GetWindowPlacement(top, &was);
        RECT before;
        GetWindowRect(top, &before);
        win->close();
        pump();
        expect(!IsWindowVisible(top), what + ": hidden");
        win->show();
        pump();
        RECT after;
        GetWindowRect(top, &after);
        WINDOWPLACEMENT now{sizeof now};
        GetWindowPlacement(top, &now);
        expect(IsWindowVisible(top) != FALSE, what + ": shown again");
        expect(bool(IsIconic(top)) == iconic && bool(IsZoomed(top)) == zoomed,
               what + ": shown again as it was (" + (IsIconic(top) ? "minimised" : IsZoomed(top) ? "maximised" : "restored") +
                   ")");
        expect(EqualRect(&before, &after) && EqualRect(&was.rcNormalPosition, &now.rcNormalPosition),
               what + ": where it was, " + rect_str(after) + ", before " + rect_str(before));
        win.reset();
        pump();
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
