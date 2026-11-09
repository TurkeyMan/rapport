// Explorer's icon overlays: one IShellIconOverlayIdentifier COM class per
// badge_kind, asking the provider's badge() for each icon Explorer draws.
//
// Explorer shows at most 15 overlays across every installed app, taking
// the first by registry name, so the names start with spaces, as other
// version control and sync apps' do. It reads them only from HKLM and only
// as it starts: a machine install, and the badges appear after the next
// sign-in (or Explorer restart).
#include "explorer_badges.h"
#include "overlay_names.h"

#include <rapport/shell.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rap::shell::detail
{

namespace
{

const wchar_t k_overlays[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\";

// The provider the overlays share, made on first use: badge() is asked from
// Explorer's icon threads at once, which the provider is told to allow.
provider& shared_provider()
{
    static std::unique_ptr<provider> p = make_provider();
    return *p;
}

// Its badge kinds, highest priority first: Explorer's priority 0 is the
// strongest, and the registry names sort in this order too.
const std::vector<badge_kind>& kinds()
{
    static const std::vector<badge_kind> k = []
    {
        std::vector<badge_kind> v = shared_provider().badge_kinds();
        std::ranges::stable_sort(v, std::ranges::greater{}, &badge_kind::priority);
        if (v.size() > 15) v.resize(15);
        return v;
    }();
    return k;
}

// The extension's class ID with its last byte moved on by 1 + the kind's
// place: stable, and one per kind.
CLSID kind_clsid(size_t i)
{
    CLSID id{};
    CLSIDFromString(wide(this_extension.windows_clsid).c_str(), &id);
    id.Data4[7] = BYTE(id.Data4[7] + 1 + i);
    return id;
}

std::wstring clsid_string(const CLSID& id)
{
    wchar_t s[64];
    StringFromGUID2(id, s, 64);
    return s;
}

// "     Tortilla1Conflicted": five spaces, sorting ahead of the four OneDrive
// uses for its seven overlays, which otherwise leave no room for an app's
// last badge; then the extension, and the kind in priority order.
std::wstring registry_name(size_t i)
{
    std::wstring id = wide(kinds()[i].id);
    if (!id.empty()) id[0] = wchar_t(towupper(id[0]));
    return L"     " + wide(this_extension.name) + std::to_wstring(i + 1) + id;
}

// The provider's badge for `path`. Explorer asks each overlay class in turn
// about an icon, on one thread, so the last answer on this thread is kept a
// moment for the others rather than asked again.
std::string badge_of(LPCWSTR path)
{
    struct last
    {
        std::wstring path;
        std::string id;
        ULONGLONG at = 0;
    };
    thread_local last kept;
    const ULONGLONG now = GetTickCount64();
    if (kept.path == path && now - kept.at < 250) return kept.id;
    kept = {path, shared_provider().badge(std::filesystem::path(path)), now};
    return kept.id;
}

class overlay final : public IShellIconOverlayIdentifier
{
public:
    explicit overlay(size_t kind)
        : kind_(kind)
    {
        ++objects();
    }
    ~overlay() { --objects(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellIconOverlayIdentifier)
        {
            *out = static_cast<IShellIconOverlayIdentifier*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP IsMemberOf(LPCWSTR path, DWORD) override
    try
    {
        if (!path) return S_FALSE;
        return badge_of(path) == kinds()[kind_].id ? S_OK : S_FALSE;
    }
    catch (...)
    {
        return S_FALSE;
    }

    STDMETHODIMP GetOverlayInfo(LPWSTR file, int size, int* index, DWORD* flags) override
    try
    {
        if (!file || !index || !flags) return E_POINTER;
        const std::wstring icon = (resource_dir() / wide(kinds()[kind_].icon)).wstring();
        if (icon.size() + 1 > size_t(size)) return E_FAIL;
        std::copy(icon.c_str(), icon.c_str() + icon.size() + 1, file);
        *index = 0;
        *flags = ISIOI_ICONFILE | ISIOI_ICONINDEX;
        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }

    STDMETHODIMP GetPriority(int* priority) override
    {
        if (!priority) return E_POINTER;
        *priority = int(std::min<size_t>(kind_, 100));
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    size_t kind_;
};

class overlay_factory final : public IClassFactory
{
public:
    explicit overlay_factory(size_t kind)
        : kind_(kind)
    {
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *out = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override
    try
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* o = new overlay(kind_);
        const HRESULT hr = o->QueryInterface(riid, out);
        o->Release();
        return hr;
    }
    catch (...)
    {
        return E_OUTOFMEMORY;
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        lock ? ++locks() : --locks();
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    size_t kind_;
};

}  // namespace

HRESULT badge_class_object(REFCLSID clsid, REFIID riid, void** out)
{
    for (size_t i = 0; i < kinds().size(); ++i)
        if (clsid == kind_clsid(i))
        {
            auto* f = new overlay_factory(i);
            const HRESULT hr = f->QueryInterface(riid, out);
            f->Release();
            return hr;
        }
    return CLASS_E_CLASSNOTAVAILABLE;
}

namespace
{

// install_overlays' Ops on the registry. Overlays are only ever under HKLM;
// elsewhere there's a class to write and no name.
struct overlay_registry
{
    HKEY root;
    std::wstring module;

    bool machine() const { return root == HKEY_LOCAL_MACHINE; }

    static std::wstring parent()
    {
        std::wstring p(k_overlays);
        p.pop_back();
        return p;
    }

    bool write(size_t i)
    {
        const std::wstring clsid = clsid_string(kind_clsid(i));
        const std::wstring server = L"Software\\Classes\\CLSID\\" + clsid;
        return set_value(root, server, nullptr,
                         wide(this_extension.name) + L" " + wide(kinds()[i].id) + L" badge") &&
               set_value(root, server + L"\\InprocServer32", nullptr, module) &&
               set_value(root, server + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
               (!machine() || set_value(root, k_overlays + registry_name(i), nullptr, clsid));
    }

    std::optional<std::vector<overlay_entry>> list()
    {
        std::vector<overlay_entry> out;
        if (!machine()) return out;
        HKEY overlays = nullptr;
        const LSTATUS opened =
            RegOpenKeyExW(root, parent().c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &overlays);
        if (opened == ERROR_FILE_NOT_FOUND) return out;
        if (opened != ERROR_SUCCESS) return std::nullopt;
        wchar_t name[256];
        LSTATUS step;
        DWORD i = 0, n = 256;
        while ((step = RegEnumKeyExW(overlays, i, name, &n, nullptr, nullptr, nullptr, nullptr)) ==
               ERROR_SUCCESS)
        {
            wchar_t value[64];
            DWORD bytes = sizeof value;
            const LSTATUS got =
                RegGetValueW(overlays, name, nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes);
            if (got == ERROR_SUCCESS)
                out.push_back({name, value});
            else if (got != ERROR_FILE_NOT_FOUND && got != ERROR_MORE_DATA &&
                     got != ERROR_UNSUPPORTED_TYPE)  // those can't be a class ID of ours
            {
                RegCloseKey(overlays);
                return std::nullopt;
            }
            ++i;
            n = 256;
        }
        RegCloseKey(overlays);
        if (step != ERROR_NO_MORE_ITEMS) return std::nullopt;
        return out;
    }

    bool ours(const std::wstring& clsid)
    {
        // Every kind ever registered: up to 15, whatever the provider says now.
        for (size_t k = 0; k < 15; ++k)
            if (_wcsicmp(clsid.c_str(), clsid_string(kind_clsid(k)).c_str()) == 0) return true;
        return false;
    }

    bool remove(const std::wstring& name)
    {
        HKEY overlays = nullptr;
        const LSTATUS opened =
            RegOpenKeyExW(root, parent().c_str(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &overlays);
        if (opened == ERROR_FILE_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS) return false;
        const LSTATUS deleted = RegDeleteTreeW(overlays, name.c_str());
        RegCloseKey(overlays);
        return deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
    }
};

}  // namespace

bool register_badges(HKEY root, const std::wstring& module)
{
    overlay_registry registry{root, module};
    std::vector<std::wstring> names;
    for (size_t i = 0; i < kinds().size(); ++i) names.push_back(registry_name(i));
    return install_overlays(registry, names);
}

void unregister_badges(HKEY root)
{
    HKEY classes = nullptr;
    const bool have_classes =
        RegOpenKeyExW(root, L"Software\\Classes\\CLSID", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                      &classes) == ERROR_SUCCESS;
    // Every kind ever registered: up to 15, whatever the provider says now.
    for (size_t i = 0; i < 15; ++i)
    {
        const std::wstring clsid = clsid_string(kind_clsid(i));
        if (have_classes) RegDeleteTreeW(classes, clsid.c_str());
    }
    if (have_classes) RegCloseKey(classes);
    overlay_registry registry{root, {}};
    prune_overlays(registry, {});
}

}  // namespace rap::shell::detail
