// Windows' own icons by freedesktop name, at 100%, 125%, 150% and 200%'s
// small-icon sizes: none until prepared, then each there, that size, not
// blank, and the shell asked once for it.
//   rapport-win32-shell-icons-test
#include "win32_shell_icons.h"

#include <windows.h>

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

}  // namespace

int main()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    using namespace rap::win32;
    for (const char* name : {"drive-harddisk", "drive-removable-media", "drive-optical", "folder-remote", "user-home",
                             "user-desktop", "folder-download", "folder-documents", "folder-pictures", "folder-music",
                             "folder-videos"})
        for (const int px : {16, 20, 24, 32})
        {
            const std::string what = std::string(name) + " at " + std::to_string(px);
            expect(!shell_icon_pixels(name, px), what + ": none before it's prepared");
            const size_t asked = shell_icon_extractions();
            prepare_shell_icon(name, px);
            expect(shell_icon_extractions() == asked + 1, what + ": the shell asked once");
            const auto* p = shell_icon_pixels(name, px);
            expect(p != nullptr, what + ": there");
            if (!p) continue;
            expect(p->size() == size_t(px) * size_t(px), what + ": that size");
            size_t opaque = 0;
            for (const uint32_t c : *p) opaque += (c >> 24) != 0;
            expect(opaque > p->size() / 8, what + ": not blank");
            prepare_shell_icon(name, px);
            expect(shell_icon_extractions() == asked + 1 && shell_icon_pixels(name, px) == p,
                   what + ": kept, not asked again");
        }
    const size_t asked = shell_icon_extractions();
    prepare_shell_icon("no-such-icon", 16);
    prepare_shell_icon("drive-harddisk-symbolic", 16);
    expect(shell_icon_extractions() == asked, "an unknown or symbolic name doesn't ask the shell");
    expect(!shell_icon_pixels("no-such-icon", 16) && !shell_icon_pixels("drive-harddisk-symbolic", 16),
           "and has none");
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
