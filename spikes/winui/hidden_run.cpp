// Runs a program on a desktop of its own and waits for it, so whatever windows
// it makes are never on the screen. For timing the spike's start-up unattended.
//
//   hidden_run [--timeout ms] program [args...]
//
// Prints the program's exit code and the time from starting it to its exit.
#include <windows.h>

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    int first = 1;
    DWORD timeout = 60000;
    if (argc > 2 && std::wstring(argv[1]) == L"--timeout")
    {
        timeout = DWORD(_wtoi(argv[2]));
        first = 3;
    }
    if (first >= argc)
    {
        std::fprintf(stderr, "usage: hidden_run [--timeout ms] program [args...]\n");
        return 2;
    }

    std::wstring command;
    for (int i = first; i < argc; ++i)
    {
        if (i > first) command += L' ';
        command += L'"';
        command += argv[i];
        command += L'"';
    }

    std::wstring name = L"rapport-hidden-" + std::to_wstring(GetCurrentProcessId());
    HDESK desk = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desk)
    {
        std::fprintf(stderr, "CreateDesktop failed: %lu\n", GetLastError());
        return 2;
    }

    STARTUPINFOW si{sizeof si};
    si.lpDesktop = name.data();
    PROCESS_INFORMATION pi{};
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        CloseDesktop(desk);
        return 2;
    }
    DWORD waited = WaitForSingleObject(pi.hProcess, timeout);
    QueryPerformanceCounter(&end);
    if (waited != WAIT_OBJECT_0)
    {
        std::fprintf(stderr, "timed out; killed\n");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    std::printf("exit 0x%08lx after %.1f ms\n", code, double(end.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseDesktop(desk);
    return 0;
}
