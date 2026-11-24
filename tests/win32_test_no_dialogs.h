// For Windows tests: nothing of a failure on the desktop. A Debug build's
// CRT reports (asserts, heap corruption, invalid parameters) and an abort or
// a crash would each open a modal dialog, a window on whoever's desktop runs
// the test; with this they go to stderr and the process just ends, failed.
//
// Installed as the test starts, by including this: tests/CMakeLists.txt
// force-includes it into every test built with MSVC (/FI), so no test needs
// to call anything. It includes no windows.h, so a test's own macros before
// its windows.h (NOMINMAX, WIN32_LEAN_AND_MEAN, versions) still apply.
#pragma once
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>

extern "C" __declspec(dllimport) unsigned int __stdcall SetErrorMode(unsigned int mode);

namespace test
{

inline void no_dialogs()
{
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
    SetErrorMode(0x0001u | 0x0002u | 0x8000u);
}

// As the test starts (static initialisation, before main()).
inline const bool no_dialogs_installed = (no_dialogs(), true);

}  // namespace test
#else
namespace test
{
inline void no_dialogs() {}
}  // namespace test
#endif
