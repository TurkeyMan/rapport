// Registering an extension, for the app that ships it. Kept apart from the
// adapters so an app can link it without a provider of its own.
#include <rapport/shell.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rap::shell
{

#ifdef _WIN32

namespace
{

// The module registers itself, through the DllInstall that COM servers
// export for regsvr32 /i.
bool call(const std::filesystem::path& module, bool add, scope where)
{
    HMODULE m = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return false;
    using fn = HRESULT(STDAPICALLTYPE*)(BOOL, PCWSTR);
    auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(m, "DllInstall")));
    const bool ok = f && SUCCEEDED(f(add, where == scope::machine ? L"machine" : L"user"));
    FreeLibrary(m);
    return ok;
}

}  // namespace

bool install(const std::filesystem::path& module, scope where)
{
    return call(module, true, where);
}

bool uninstall(const std::filesystem::path& module, scope where)
{
    return call(module, false, where);
}

#else

bool install(const std::filesystem::path&, scope)
{
    return false;
}

bool uninstall(const std::filesystem::path&, scope)
{
    return false;
}

#endif

}  // namespace rap::shell
