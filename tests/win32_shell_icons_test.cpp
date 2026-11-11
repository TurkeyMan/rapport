// Windows' own icons by freedesktop name, at 100%, 125%, 150% and 200%'s
// small-icon sizes: each there, that size, and not blank.
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
    using rap::win32::shell_icon_pixels;
    for (const char* name : {"drive-harddisk", "drive-removable-media", "drive-optical", "folder-remote", "user-home",
                             "user-desktop", "folder-download", "folder-documents", "folder-pictures", "folder-music",
                             "folder-videos"})
        for (const int px : {16, 20, 24, 32})
        {
            const std::string what = std::string(name) + " at " + std::to_string(px);
            const auto* p = shell_icon_pixels(name, px);
            expect(p != nullptr, what + ": there");
            if (!p) continue;
            expect(p->size() == size_t(px) * size_t(px), what + ": that size");
            size_t opaque = 0;
            for (const uint32_t c : *p) opaque += (c >> 24) != 0;
            expect(opaque > p->size() / 8, what + ": not blank");
            expect(shell_icon_pixels(name, px) == p, what + ": kept");
        }
    expect(!shell_icon_pixels("no-such-icon", 16), "an unknown name has none");
    expect(!shell_icon_pixels("drive-harddisk-symbolic", 16), "nor a symbolic one");
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
