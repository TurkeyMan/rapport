// Owner-drawn buttons with Windows' own icons, from a cold cache: the icons
// are prepared when the buttons take them (created, set_icon, a DPI change)
// and painting never asks the shell.
//   rapport-win32-shell-icons-paint-test (static builds: it reads the cache)
#include <rapport/rapport.h>

#include "win32_shell_icons.h"  // after rapport: windows.h's min and max

#include <cstdio>
#include <string>

RAPPORT_BUILTIN_BACKEND(win32);

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

void paint_all(HWND top)
{
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

}  // namespace

int main(int argc, char** argv)
{
    using rap::win32::shell_icon_extractions;
    using rap::win32::shell_icon_pixels;
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    expect(shell_icon_extractions() == 0, "the cache starts cold");

    rap::tool_button* button = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "t",
        {hbox{tool_button{rap::icon::named("drive-optical"), "Disc"}.bind(button),
              entry{}.leading_action(rap::icon::named("folder-music"), "Music", [] {}).stretch()}}});
    HWND top = nullptr;  // this thread's window
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            if (!GetParent(h) && IsWindow(h)) *reinterpret_cast<HWND*>(p) = h;
            return *reinterpret_cast<HWND*>(p) == nullptr;
        },
        reinterpret_cast<LPARAM>(&top));
    expect(top != nullptr, "the window is there");
    if (!top) return 1;
    const UINT dpi = top ? GetDpiForWindow(top) : 96;
    const int px = MulDiv(16, int(dpi), 96);
    expect(shell_icon_pixels("drive-optical", px) && shell_icon_pixels("folder-music", px),
           "the tool button's and the entry button's icons are prepared when they're made");

    win->show();
    size_t asked = shell_icon_extractions();
    paint_all(top);
    expect(shell_icon_extractions() == asked, "painting them asks the shell nothing");

    button->set_icon(rap::icon::named("folder-videos"));
    expect(shell_icon_pixels("folder-videos", px) != nullptr, "set_icon prepares the new one");
    asked = shell_icon_extractions();
    paint_all(top);
    expect(shell_icon_extractions() == asked, "and painting it asks nothing");

    // A new DPI: everything wanted is prepared at its size before a repaint.
    const UINT to = dpi == 144 ? 120 : 144;
    const int to_px = MulDiv(16, int(to), 96);
    RECT r{};
    GetWindowRect(top, &r);
    SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&r));
    expect(shell_icon_pixels("folder-videos", to_px) && shell_icon_pixels("folder-music", to_px) &&
               shell_icon_pixels("drive-optical", to_px),
           "WM_DPICHANGED prepares them at the new DPI's size");
    asked = shell_icon_extractions();
    paint_all(top);
    expect(shell_icon_extractions() == asked, "and the paint after asks nothing");

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
