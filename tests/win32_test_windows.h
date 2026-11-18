// For Windows tests: a top-level window of this thread's, by its title.
// FindWindowW looks through every window on the desktop, and the variants of
// a test (light and dark, compact...) run side by side under ctest -j with
// their windows titled alike: one would find another's. A test's windows are
// made on its own thread, so they're looked for there only.
//
// Include after windows.h.
#pragma once

#include <cwchar>

inline HWND test_window(const wchar_t* title)
{
    struct find
    {
        const wchar_t* title;
        HWND found = nullptr;
    } f{title};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(p);
            wchar_t text[256]{};
            GetWindowTextW(h, text, 256);
            if (std::wcscmp(text, f->title)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
