// Runs a test (argv[1..]) under high contrast, for CI: a runner is a
// throwaway VM, where turning it on for a while is fine. It changes a system
// setting, so it refuses to unless both GITHUB_ACTIONS=true and
// RAPPORT_TEST_SET_HIGH_CONTRAST=1 say it's such a runner (skipped, 77,
// otherwise). It turns on "High Contrast Black", confirms high contrast is
// reported on and the system colours have changed (Server 2025 gives a
// Windows 11 contrast theme's, #202020 windows with white text, for it),
// runs the test (ended, and failed, if it doesn't finish in 120 s: inside
// CTest's 180, with room for the 30 s the colours may take to come back),
// and turns it off again whatever the test did. `--sleep S` only sleeps: a
// hung test, for win32-high-contrast-run-hung. It fails unless the flag is off and the window
// and text colours are back as they were within 30 seconds.
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

// An inheritable copy of one of this process's standard handles, for the
// test's (null if it has none).
HANDLE inheritable(DWORD which)
{
    const HANDLE h = GetStdHandle(which);
    HANDLE copy = nullptr;
    if (h && h != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return copy;
}
}

int main(int argc, char** argv)
{
    // Unbuffered, so stdout and stderr reach CTest in the order they happen.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // A test that hangs, for the -hung self-test: only sleeps.
    if (argc == 3 && !std::strcmp(argv[1], "--sleep"))
    {
        Sleep(DWORD(std::atoi(argv[2])) * 1000);
        return 0;
    }
    // The test's bound: 120 s (RAPPORT_TEST_RUN_TIMEOUT_S, if set), well
    // inside CTest's 180 s, so the setting's put back here, never left to
    // CTest's kill of this process.
    DWORD timeout_s = 120;
    if (const char* t = std::getenv("RAPPORT_TEST_RUN_TIMEOUT_S"); t && std::atoi(t) > 0)
        timeout_s = DWORD(std::atoi(t));
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
        // The test's output and failures this process's (CTest's log), passed
        // to it explicitly.
        STARTUPINFOA si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inheritable(STD_INPUT_HANDLE);
        si.hStdOutput = inheritable(STD_OUTPUT_HANDLE);
        si.hStdError = inheritable(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL started =
            CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        for (HANDLE h : {si.hStdInput, si.hStdOutput, si.hStdError})
            if (h) CloseHandle(h);
        if (started)
        {
            const DWORD waited = WaitForSingleObject(pi.hProcess, timeout_s * 1000);
            DWORD exit = 1;
            if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit))
                code = int(exit);
            else
            {
                // Timed out, or the wait failed: ended, and a failure.
                std::fprintf(stderr, "%s didn't finish in %lu s (wait %lu, error %lu): ended\n", argv[1],
                             timeout_s, waited, GetLastError());
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 10 * 1000);
                code = 1;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        else
            std::fprintf(stderr, "couldn't run %s (%lu)\n", argv[1], GetLastError());
    }
    // Put back, or the tests after this one run in high contrast: a failure
    // to, whatever the test did (keeping its failure if it failed).
    const bool reset = set_high_contrast(false);
    if (!reset) std::fprintf(stderr, "SPI_SETHIGHCONTRAST (off) failed (%lu)\n", GetLastError());
    auto restored = [&] {
        return !high_contrast_on() && GetSysColor(COLOR_WINDOW) == window && GetSysColor(COLOR_WINDOWTEXT) == text;
    };
    for (int i = 0; i < 300 && reset && !restored(); ++i) Sleep(100);
    print_colours("turned off");
    if (reset && !restored())
        std::fprintf(stderr, "high contrast wasn't put back: the flag is %s, the window and text colours %s\n",
                     high_contrast_on() ? "still on" : "off",
                     GetSysColor(COLOR_WINDOW) == window && GetSysColor(COLOR_WINDOWTEXT) == text ? "as they were"
                                                                                                    : "not as they were");
    if ((!reset || !restored()) && code == 0) code = 1;
    return code;
}
