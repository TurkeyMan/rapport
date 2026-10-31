// Explorer's context menu: a classic IShellExtInit + IContextMenu COM server,
// registered per user. On Windows 11 its entries sit under "Show more
// options"; the top-level menu needs IExplorerCommand and a signed package.
//
// This runs inside explorer.exe: every entry point catches everything, and
// the module links its C++ runtime statically (see rapport_add_shell_extension).
#include "explorer_badges.h"

#include <rapport/shell.h>

#include <olectl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace
{

using rap::shell::menu_item;

std::atomic<long>& g_objects = rap::shell::detail::objects();
std::atomic<long>& g_locks = rap::shell::detail::locks();

std::wstring wide(std::string_view s)
{
    if (s.empty()) return {};
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string narrow(std::wstring_view w)
{
    if (w.empty()) return {};
    std::string s(
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr),
        '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr,
                        nullptr);
    return s;
}

CLSID our_clsid()
{
    CLSID id{};
    CLSIDFromString(wide(rap::shell::this_extension.windows_clsid).c_str(), &id);
    return id;
}

template <class T>
struct com_ptr
{
    T* p = nullptr;
    ~com_ptr()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p; }
};

// A menu bitmap from an .ico or .png: 32bpp premultiplied, as menus want for
// alpha. The icon frame nearest the size is picked, then scaled to it.
HBITMAP load_menu_bitmap(const std::filesystem::path& file)
{
    const UINT size = UINT(GetSystemMetrics(SM_CXSMICON));
    com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic.p))))
        return nullptr;
    com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;
    UINT frames = 0, best = 0, best_w = 0;
    decoder->GetFrameCount(&frames);
    for (UINT i = 0; i < frames; ++i)
    {
        com_ptr<IWICBitmapFrameDecode> f;
        UINT w = 0, h = 0;
        if (FAILED(decoder->GetFrame(i, &f)) || FAILED(f->GetSize(&w, &h))) continue;
        // The smallest frame at least the size, else the largest.
        const bool better = best_w == 0 || (w >= size && (best_w < size || w < best_w)) ||
                            (w < size && best_w < size && w > best_w);
        if (better)
        {
            best = i;
            best_w = w;
        }
    }
    com_ptr<IWICBitmapFrameDecode> frame;
    com_ptr<IWICBitmapScaler> scaler;
    com_ptr<IWICFormatConverter> converter;
    if (FAILED(decoder->GetFrame(best, &frame)) || FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.p, size, size, WICBitmapInterpolationModeFant)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.p, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                    .biWidth = LONG(size),
                    .biHeight = -LONG(size),  // top-down
                    .biPlanes = 1,
                    .biBitCount = 32,
                    .biCompression = BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    if (FAILED(converter->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(bits))))
    {
        DeleteObject(bmp);
        return nullptr;
    }
    return bmp;
}

class context_menu final : public IShellExtInit, public IContextMenu
{
public:
    context_menu() { ++g_objects; }
    ~context_menu()
    {
        for (HBITMAP b : bitmaps_) DeleteObject(b);
        --g_objects;
    }

    // -- IUnknown -------------------------------------------------------------

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit)
            *out = static_cast<IShellExtInit*>(this);
        else if (riid == IID_IContextMenu)
            *out = static_cast<IContextMenu*>(this);
        else
        {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // -- IShellExtInit ----------------------------------------------------------

    // Files arrive as the data object's CF_HDROP; a background click gives
    // only the folder.
    STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE folder, IDataObject* data, HKEY) override
    try
    {
        sel_ = {};
        if (data)
        {
            FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM m{};
            if (SUCCEEDED(data->GetData(&fe, &m)))
            {
                if (auto drop = static_cast<HDROP>(GlobalLock(m.hGlobal)))
                {
                    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < n; ++i)
                    {
                        std::wstring p(DragQueryFileW(drop, i, nullptr, 0), L'\0');
                        DragQueryFileW(drop, i, p.data(), UINT(p.size() + 1));
                        sel_.paths.emplace_back(p);
                    }
                    GlobalUnlock(m.hGlobal);
                }
                ReleaseStgMedium(&m);
            }
        }
        if (folder)
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(folder, SIGDN_FILESYSPATH, &path)))
            {
                sel_.folder = path;
                CoTaskMemFree(path);
            }
        }
        sel_.background = !data && !sel_.folder.empty();
        if (sel_.folder.empty() && !sel_.paths.empty())
            sel_.folder = sel_.paths.front().parent_path();
        return sel_.paths.empty() && !sel_.background ? E_FAIL : S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }

    // -- IContextMenu -----------------------------------------------------------

    STDMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last,
                                  UINT flags) override
    try
    {
        if (flags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        sel_.extended = (flags & CMF_EXTENDEDVERBS) != 0;
        provider_ = rap::shell::make_provider();
        ids_.clear();
        if (!provider_) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        UINT next = first;
        insert(menu, index, provider_->menu(sel_), first, last, next);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, next - first);
    }
    catch (...)
    {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }

    // By offset from QueryContextMenu's first id, or by verb (an item's id).
    STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ci) override
    try
    {
        if (!ci || !provider_) return E_FAIL;
        std::string id;
        const bool unicode =
            ci->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (ci->fMask & CMIC_MASK_UNICODE);
        const auto* ex = reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(ci);
        if (unicode && HIWORD(ex->lpVerbW))
            id = narrow(ex->lpVerbW);
        else if (HIWORD(ci->lpVerb))
            id = ci->lpVerb;
        else
        {
            const size_t offset = LOWORD(ci->lpVerb);
            if (offset >= ids_.size()) return E_FAIL;
            id = ids_[offset];
        }
        if (id.empty() || std::find(ids_.begin(), ids_.end(), id) == ids_.end()) return E_FAIL;
        provider_->invoke(id, sel_);
        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }

    STDMETHODIMP GetCommandString(UINT_PTR offset, UINT type, UINT*, CHAR* name, UINT size) override
    try
    {
        if (offset >= ids_.size() || ids_[offset].empty()) return E_INVALIDARG;
        switch (type)
        {
        case GCS_VERBW:
            if (!name || size == 0) return E_INVALIDARG;
            lstrcpynW(reinterpret_cast<LPWSTR>(name), wide(ids_[offset]).c_str(), int(size));
            return S_OK;
        case GCS_VERBA:
            if (!name || size == 0) return E_INVALIDARG;
            lstrcpynA(name, ids_[offset].c_str(), int(size));
            return S_OK;
        case GCS_VALIDATEW:
        case GCS_VALIDATEA: return S_OK;
        default: return E_NOTIMPL;
        }
    }
    catch (...)
    {
        return E_FAIL;
    }

private:
    // Each item takes the next id; submenus take one too, as Explorer expects,
    // mapped to no command.
    void insert(HMENU menu, UINT pos, const std::vector<menu_item>& items, UINT first, UINT last,
                UINT& next)
    {
        for (const menu_item& item : items)
        {
            if (next > last) return;
            MENUITEMINFOW mii{.cbSize = sizeof mii};
            std::wstring text;
            if (item.is_separator())
            {
                mii.fMask = MIIM_FTYPE;
                mii.fType = MFT_SEPARATOR;
            }
            else
            {
                text = wide(item.label);
                mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE;
                mii.dwTypeData = text.data();
                mii.fState = item.enabled ? MFS_ENABLED : MFS_DISABLED;
                mii.wID = next++;
                ids_.push_back(item.submenu.empty() ? item.id : std::string());
                if (!item.submenu.empty())
                {
                    HMENU sub = CreatePopupMenu();
                    insert(sub, 0, item.submenu, first, last, next);
                    mii.fMask |= MIIM_SUBMENU;
                    mii.hSubMenu = sub;
                }
                if (!item.icon.empty())
                    if (HBITMAP b = load_menu_bitmap(rap::shell::module_dir() /
                                                     std::filesystem::path(wide(item.icon))))
                    {
                        mii.fMask |= MIIM_BITMAP;
                        mii.hbmpItem = b;
                        bitmaps_.push_back(b);
                    }
            }
            InsertMenuItemW(menu, pos++, TRUE, &mii);
        }
    }

    std::atomic<ULONG> refs_{1};
    rap::shell::selection sel_;
    std::unique_ptr<rap::shell::provider> provider_;
    std::vector<std::string> ids_;  // by offset from the first id; empty for a submenu
    std::vector<HBITMAP> bitmaps_;
};

class factory final : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *out = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // A static object: references don't own it.
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override
    try
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* menu = new context_menu;
        const HRESULT hr = menu->QueryInterface(riid, out);
        menu->Release();
        return hr;
    }
    catch (...)
    {
        return E_OUTOFMEMORY;
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};

factory g_factory;

// -- registration ---------------------------------------------------------------

// Under Software\Classes of HKCU (one user, no elevation) or HKLM (every
// user; an installer's choice, elevated). Files and folders, folder
// backgrounds, and drives.
const wchar_t* const k_handler_parents[] = {
    L"Software\\Classes\\AllFilesystemObjects\\shellex\\ContextMenuHandlers\\",
    L"Software\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\",
    L"Software\\Classes\\Drive\\shellex\\ContextMenuHandlers\\",
};
const wchar_t k_approved[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

bool set_value(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                        &k, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                  DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

std::wstring module_path()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&g_factory), &self);
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(self, path.data(), DWORD(path.size()));
        if (n == 0) return {};
        if (n < path.size()) return path.substr(0, n);
        path.resize(path.size() * 2);
    }
}

}  // namespace

namespace rap::shell::detail
{

std::atomic<long>& objects()
{
    static std::atomic<long> n{0};
    return n;
}

std::atomic<long>& locks()
{
    static std::atomic<long> n{0};
    return n;
}

}  // namespace rap::shell::detail

// -- exports (named in explorer.def) ----------------------------------------------

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
try
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != our_clsid()) return rap::shell::detail::badge_class_object(clsid, riid, out);
    return g_factory.QueryInterface(riid, out);
}
catch (...)
{
    return E_UNEXPECTED;
}

STDAPI DllCanUnloadNow()
{
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}

namespace
{

HRESULT register_in(HKEY root)
{
    const std::wstring clsid = wide(rap::shell::this_extension.windows_clsid);
    const std::wstring name = wide(rap::shell::this_extension.name);
    const std::wstring path = module_path();
    const std::wstring server = L"Software\\Classes\\CLSID\\" + clsid;
    bool ok = !path.empty() && set_value(root, server, nullptr, name) &&
              set_value(root, server + L"\\InprocServer32", nullptr, path) &&
              set_value(root, server + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    for (const wchar_t* parent : k_handler_parents)
        ok = ok && set_value(root, parent + name, nullptr, clsid);
    ok = ok && set_value(root, k_approved, clsid.c_str(), name);
    ok = ok && rap::shell::detail::register_badges(root, path);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : SELFREG_E_CLASS;
}

HRESULT unregister_in(HKEY root)
{
    const std::wstring clsid = wide(rap::shell::this_extension.windows_clsid);
    const std::wstring name = wide(rap::shell::this_extension.name);
    for (const wchar_t* parent : k_handler_parents)
        RegDeleteKeyExW(root, (parent + name).c_str(), KEY_WOW64_64KEY, 0);
    HKEY classes = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Classes\\CLSID", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                      &classes) == ERROR_SUCCESS)
    {
        RegDeleteTreeW(classes, clsid.c_str());
        RegCloseKey(classes);
    }
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, k_approved, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS)
    {
        RegDeleteValueW(k, clsid.c_str());
        RegCloseKey(k);
    }
    rap::shell::detail::unregister_badges(root);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace

// For this user: regsvr32 tortilla-shell.dll, or rap::shell::install().
STDAPI DllRegisterServer()
try
{
    return register_in(HKEY_CURRENT_USER);
}
catch (...)
{
    return E_UNEXPECTED;
}

STDAPI DllUnregisterServer()
try
{
    return unregister_in(HKEY_CURRENT_USER);
}
catch (...)
{
    return E_UNEXPECTED;
}

// "user" or "machine": regsvr32 /n /i:machine <dll>, or rap::shell::install()
// with scope::machine (elevated).
STDAPI DllInstall(BOOL install, PCWSTR how)
try
{
    const HKEY root =
        how && lstrcmpiW(how, L"machine") == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    return install ? register_in(root) : unregister_in(root);
}
catch (...)
{
    return E_UNEXPECTED;
}
