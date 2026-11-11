// Runs a test (argv[1..]) under high contrast, for CI: a runner is a
// throwaway VM, where turning it on for a while is fine. It changes a system
// setting, so it refuses to unless both GITHUB_ACTIONS=true and
// RAPPORT_TEST_SET_HIGH_CONTRAST=1 say it's such a runner (skipped, 77,
// otherwise). It turns on "High Contrast Black", confirms high contrast is
// reported on and the system colours have changed (Server 2025 gives a
// Windows 11 contrast theme's, #202020 windows with white text, for it),
// runs the test, and turns it off again whatever the test did, waiting for
// the colours to come back.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
bool env_is(const char* name, const char* value)
{
    const char* v = std::getenv(name);
    return v && !std::strcmp(v, value);
}

bool high_contrast_on()
{
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool set_high_contrast(bool on)
{
    wchar_t scheme[] = L"High Contrast Black";
    HIGHCONTRASTW hc{sizeof hc};
    hc.dwFlags = on ? HCF_HIGHCONTRASTON : 0;
    hc.lpszDefaultScheme = on ? scheme : nullptr;
    return SystemParametersInfoW(SPI_SETHIGHCONTRAST, sizeof hc, &hc, SPIF_SENDCHANGE) != FALSE;
}

void print_colours(const char* when)
{
    std::printf("%s: high contrast %s, COLOR_WINDOW %06lx, COLOR_WINDOWTEXT %06lx, COLOR_BTNFACE %06lx\n", when,
                high_contrast_on() ? "on" : "off", GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_BTNFACE));
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s test [args...]\n", argv[0]);
        return 2;
    }
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SET_HIGH_CONTRAST", "1"))
    {
        std::printf("skipped: changes a system setting; only on a CI runner with RAPPORT_TEST_SET_HIGH_CONTRAST=1\n");
        return 77;
    }
    print_colours("before");
    const COLORREF window = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
    if (high_contrast_on())
    {
        std::fprintf(stderr, "high contrast is already on: not touched\n");
        return 2;
    }
    if (!set_high_contrast(true))
    {
        std::fprintf(stderr, "SPI_SETHIGHCONTRAST failed (%lu)\n", GetLastError());
        return 1;
    }
    // The scheme's colours, which may take a moment to arrive.
    bool applied = false;
    for (int i = 0; i < 100 && !applied; ++i)
    {
        applied = high_contrast_on() && GetSysColor(COLOR_WINDOW) != window && GetSysColor(COLOR_WINDOWTEXT) != text;
        if (!applied) Sleep(100);
    }
    print_colours("turned on");
    int code = 1;
    if (!applied)
        std::fprintf(stderr, "high contrast didn't take: the flag or the system colours aren't the scheme's\n");
    else
    {
        std::string command;
        for (int i = 1; i < argc; ++i)
        {
            if (i > 1) command += ' ';
            command += '"';
            command += argv[i];
            command += '"';
        }
        STARTUPINFOA si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
            DWORD exit = 1;
            GetExitCodeProcess(pi.hProcess, &exit);
            code = int(exit);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        else
            std::fprintf(stderr, "couldn't run %s (%lu)\n", argv[1], GetLastError());
    }
    set_high_contrast(false);
    for (int i = 0; i < 300 && (high_contrast_on() || GetSysColor(COLOR_WINDOW) != window); ++i) Sleep(100);
    print_colours("turned off");
    if (high_contrast_on() || GetSysColor(COLOR_WINDOW) != window)
        std::fprintf(stderr, "note: the desktop's colours didn't come back as they were\n");
    return code;
}
