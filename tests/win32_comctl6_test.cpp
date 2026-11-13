// A rapport app gets common controls 6 (themed) whatever its manifest
// (#41): built with none (rapport-win32-comctl6-test-no-manifest) and with
// MSVC's default one, which asks only for an execution level and so is the
// process's default activation context without common controls 6
// (rapport-win32-comctl6-test). A list's class must come from comctl32 6,
// with a context of the caller's own active around it all, and none of
// rapport's is left active over the caller's (#43). Its window is never
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
// A context of the caller's own, with no common controls in it, active
// around everything rapport does: what a host that activates its own
// manifest does (#43).
HANDLE callers_context()
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"rap", 0, path)) return INVALID_HANDLE_VALUE;
    const char manifest[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                            "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
                            "<assemblyIdentity type=\"win32\" name=\"Rapport.Test.Caller\" version=\"1.0.0.0\"/>"
                            "</assembly>";
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD wrote = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, manifest, DWORD(sizeof manifest - 1), &wrote, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    ACTCTXW ctx{sizeof ctx};
    ctx.lpSource = path;
    HANDLE h = ok ? CreateActCtxW(&ctx) : INVALID_HANDLE_VALUE;
    DeleteFileW(path);
    return h;
}

// The caller pops its context: false if that raised (one of ours was left
// above it, STATUS_SXS_EARLY_DEACTIVATION).
bool caller_pops(ULONG_PTR cookie)
{
#ifdef _MSC_VER
    __try
    {
        return DeactivateActCtx(0, cookie) != FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    return DeactivateActCtx(0, cookie) != FALSE;
#endif
}
}  // namespace

int main(int argc, char** argv)
{
    HANDLE caller = callers_context();
    expect(caller != INVALID_HANDLE_VALUE, "the caller's own context is made");
    ULONG_PTR cookie = 0;
    expect(caller != INVALID_HANDLE_VALUE && ActivateActCtx(caller, &cookie), "and active");

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
    expect(v >= 6, "the list view is common controls 6's, with the caller's context active");

    // Nothing of rapport's left active over the caller's.
    HANDLE now = nullptr;
    expect(GetCurrentActCtx(&now) && now == caller, "after ui::load and a window, the caller's context is current");
    if (now) ReleaseActCtx(now);
    expect(caller_pops(cookie), "the caller pops its context without an early-deactivation fault");
    if (caller != INVALID_HANDLE_VALUE) ReleaseActCtx(caller);

    std::printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
