// A rapport app gets common controls 6 (themed) whatever its manifest
// (#41): built with none (rapport-win32-comctl6-test-no-manifest) and with
// MSVC's default one, which asks only for an execution level and so is the
// process's default activation context without common controls 6
// (rapport-win32-comctl6-test). A list's class, and the image lists rapport
// makes for v6 controls, must come from comctl32 6. Its window is never
// shown.
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <cstdio>
#include <cwchar>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
int failures;
void expect(bool ok, const char* why)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", why);
    if (!ok) ++failures;
}

// comctl32's major version, and its path, for the module that holds `m`.
DWORD version_of(HMODULE m, wchar_t (&path)[MAX_PATH])
{
    path[0] = 0;
    if (!m) return 0;
    GetModuleFileNameW(m, path, MAX_PATH);
    auto get = reinterpret_cast<DLLGETVERSIONPROC>(reinterpret_cast<void*>(GetProcAddress(m, "DllGetVersion")));
    DLLVERSIONINFO v{sizeof v};
    return get && SUCCEEDED(get(&v)) ? v.dwMajorVersion : 0;
}

HWND find_class(HWND root, const wchar_t* cls)
{
    struct search { const wchar_t* cls; HWND found; } s{cls, nullptr};
    EnumChildWindows(root, [](HWND h, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<search*>(lp);
        wchar_t name[64]{};
        GetClassNameW(h, name, 64);
        if (_wcsicmp(name, s->cls)) return TRUE;
        s->found = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

struct rows : rap::list_model
{
    size_t row_count() const override { return 2; }
    std::string text(size_t, size_t) const override { return "row"; }
};
}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using namespace rap::layout;
    rows model;
    // Never shown: nothing reaches the screen.
    auto win = (*ui)->create(window{"Common controls 6", {vbox{list{{{"Name", {}, true}}, &model}, entry{}}}});
    HWND root = FindWindowW(nullptr, L"Common controls 6");
    if (!root) return 2;
    expect(!IsWindowVisible(root), "the test window is never shown");

    HWND lv = find_class(root, WC_LISTVIEWW);
    expect(lv != nullptr, "the list is a list view");
    wchar_t path[MAX_PATH];
    const DWORD v = version_of(lv ? reinterpret_cast<HMODULE>(GetClassLongPtrW(lv, GCLP_HMODULE)) : nullptr, path);
    std::wprintf(L"     the list view's class is comctl32 %lu: %s\n", v, path);
    expect(v >= 6, "the list view is common controls 6's");

    // What a v6 control's image list is made by: comctl32 6 too (the one a
    // LoadLibrary sees now, as rapport's own does).
    const DWORD loaded = version_of(LoadLibraryW(L"comctl32.dll"), path);
    std::wprintf(L"     comctl32 as loaded now is %lu: %s\n", loaded, path);
    expect(loaded >= 6, "comctl32 as loaded after ui::load is 6");

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
