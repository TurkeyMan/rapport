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

// The module registers itself (per user), as COM servers do.
bool call(const std::filesystem::path& module, const char* entry)
{
    HMODULE m = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return false;
    using fn = HRESULT(STDAPICALLTYPE*)();
    auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(m, entry)));
    const bool ok = f && SUCCEEDED(f());
    FreeLibrary(m);
    return ok;
}

}  // namespace

bool install(const std::filesystem::path& module)
{
    return call(module, "DllRegisterServer");
}

bool uninstall(const std::filesystem::path& module)
{
    return call(module, "DllUnregisterServer");
}

#else

bool install(const std::filesystem::path&)
{
    return false;
}

bool uninstall(const std::filesystem::path&)
{
    return false;
}

#endif

}  // namespace rap::shell
