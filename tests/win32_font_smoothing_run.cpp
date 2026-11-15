// Runs a test (argv[2..]) with the font smoothing `argv[1]` ("off" or
// "cleartype"), for CI: a runner is a throwaway VM, where changing it for a
// while is fine. It changes a system setting, so it refuses to unless both
// GITHUB_ACTIONS=true and RAPPORT_TEST_SET_FONT_SMOOTHING=1 say it's such a
// runner (skipped, 77, otherwise). It sets the smoothing before the test's
// process starts (SPI_SETFONTSMOOTHING and ...TYPE, written to the user's
// profile and broadcast: a process's GDI was seen drawing ClearType still
// when the process turned it off itself), confirms it reads back so, runs the
// test with RAPPORT_TEST_FONT_SMOOTHING_SET naming what was set (ended, and
// failed, if it doesn't finish in 120 s: inside CTest's 180), and puts the
// smoothing back as it was whatever the test did, failing unless it reads
// back as it was. `--sleep S` only sleeps: a hung test, for
// win32-font-smoothing-run-hung.
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

struct smoothing
{
    BOOL on = FALSE;
    UINT type = 0;
};
smoothing read()
{
    smoothing s;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &s.on, 0);
    SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &s.type, 0);
    return s;
}
const char* name(const smoothing& s)
{
    return !s.on ? "off" : s.type == FE_FONTSMOOTHINGCLEARTYPE ? "ClearType" : "standard";
}
bool same(const smoothing& a, const smoothing& b)
{
    return (a.on != FALSE) == (b.on != FALSE) && a.type == b.type;
}
// Each call's result, and the setting read back.
bool set(const smoothing& s)
{
    const UINT how = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    bool ok = SystemParametersInfoW(SPI_SETFONTSMOOTHINGTYPE, 0, reinterpret_cast<PVOID>(UINT_PTR(s.type)), how) !=
              FALSE;
    if (!ok) std::fprintf(stderr, "SPI_SETFONTSMOOTHINGTYPE failed (%lu)\n", GetLastError());
    if (!SystemParametersInfoW(SPI_SETFONTSMOOTHING, s.on, nullptr, how))
    {
        std::fprintf(stderr, "SPI_SETFONTSMOOTHING failed (%lu)\n", GetLastError());
        ok = false;
    }
    return ok && same(read(), s);
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
}  // namespace

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
    if (argc < 3 || (std::strcmp(argv[1], "off") && std::strcmp(argv[1], "cleartype")))
    {
        std::fprintf(stderr, "usage: %s off|cleartype test [args...]\n", argv[0]);
        return 2;
    }
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SET_FONT_SMOOTHING", "1"))
    {
        std::printf("skipped: changes a system setting; only on a CI runner with RAPPORT_TEST_SET_FONT_SMOOTHING=1\n");
        std::printf("fringe check: not run, font smoothing %s needs the CI opt-in\n", argv[1]);
        return 77;
    }
    const smoothing was = read();
    const smoothing want = !std::strcmp(argv[1], "off") ? smoothing{FALSE, was.type}
                                                        : smoothing{TRUE, FE_FONTSMOOTHINGCLEARTYPE};
    std::printf("font smoothing before: %s (type %u)\n", name(was), was.type);
    int code = 1;
    if (!set(want))
        std::fprintf(stderr, "FAIL font smoothing not set to %s: reads %s\n", name(want), name(read()));
    else
    {
        std::printf("font smoothing set: %s\n", name(read()));
        SetEnvironmentVariableA("RAPPORT_TEST_FONT_SMOOTHING_SET", argv[1]);
        std::string command;
        for (int i = 2; i < argc; ++i)
        {
            if (i > 2) command += ' ';
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
                std::fprintf(stderr, "%s didn't finish in %lu s (wait %lu, error %lu): ended\n", argv[2],
                             timeout_s, waited, GetLastError());
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 10 * 1000);
                code = 1;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        else
            std::fprintf(stderr, "couldn't run %s (%lu)\n", argv[2], GetLastError());
    }
    // Put back, or the tests after this one run with it: a failure not to,
    // whatever the test did (keeping its failure if it failed).
    if (!set(was))
    {
        const smoothing now = read();
        std::fprintf(stderr, "FAIL font smoothing NOT put back: was %s (type %u), now %s (type %u)\n", name(was),
                     was.type, name(now), now.type);
        if (code == 0 || code == 77) code = 1;
    }
    else
        std::printf("font smoothing put back: %s\n", name(read()));
    return code;
}
