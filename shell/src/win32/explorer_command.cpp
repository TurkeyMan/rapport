// Windows 11's own context menu: an IExplorerCommand, the one entry Explorer
// shows at the top level for an app with package identity (a sparse package
// is enough). It needs no registry keys of ours: the package's manifest
// names its CLSID, with the file types it's for.
//
// The entry is named for the extension and opens the provider's menu. An
// item of that menu that is itself a submenu named for the extension (an
// app's "&Tortilla" under its own top items) is laid out in place, after a
// separator, since the entry already carries the name.
//
// Explorer asks for the entry's state off its UI thread (E_PENDING sends it
// there), so the provider's menu is built there. Like the classic menu, this
// runs inside Explorer's process (or its COM surrogate): every entry point
// catches everything.
#include "explorer_badges.h"

#include <rapport/shell.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#ifndef ECF_ISSEPARATOR
#define ECF_ISSEPARATOR 0x8  // Windows SDK 10.0.18362 and later
#endif

namespace
{

using rap::shell::menu_item;
using rap::shell::detail::wide;

std::atomic<long>& g_objects = rap::shell::detail::objects();
std::atomic<long>& g_locks = rap::shell::detail::locks();

// A string Explorer frees with CoTaskMemFree.
HRESULT dup(const std::wstring& s, LPWSTR* out)
{
    if (!out) return E_POINTER;
    return SHStrDupW(s.c_str(), out);
}

// Without mnemonics, which this menu doesn't show: "&&" is a plain '&'.
std::wstring plain(std::string_view label)
{
    std::wstring in = wide(label), out;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == L'&' && i + 1 < in.size())
            ++i;
        else if (in[i] == L'&')
            continue;
        out += in[i];
    }
    return out;
}

// The menu for one right-click: the provider, the selection, and the items
// it gave, shared by the entry and every item under it.
struct menu_state
{
    std::unique_ptr<rap::shell::provider> provider;
    rap::shell::selection sel;
    std::vector<menu_item> items;
    std::string icon;  // the entry's: that of the submenu laid out in place
    bool built = false;
};

// The provider's menu with the extension's own submenu laid out in place,
// and no separator at either end or twice in a row. `icon` becomes that
// submenu's.
std::vector<menu_item> flatten(std::vector<menu_item> items, std::string& icon)
{
    const std::wstring own = plain(rap::shell::this_extension.name);
    std::vector<menu_item> out;
    auto add = [&](menu_item item)
    {
        if (item.is_separator() && (out.empty() || out.back().is_separator())) return;
        out.push_back(std::move(item));
    };
    for (menu_item& item : items)
    {
        if (!item.submenu.empty() && plain(item.label) == own)
        {
            icon = item.icon;
            add(menu_item::separator());
            for (menu_item& sub : item.submenu) add(std::move(sub));
            add(menu_item::separator());
        }
        else
            add(std::move(item));
    }
    while (!out.empty() && out.back().is_separator()) out.pop_back();
    return out;
}

std::wstring file_path(IShellItem* item)
{
    PWSTR p = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return {};
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

// The folder a background click was in: the site is Explorer's view.
std::wstring site_folder(IUnknown* site, bool& has_selection)
{
    has_selection = false;
    if (!site) return {};
    IServiceProvider* sp = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IFolderView* fv = nullptr;
    IShellItem* folder = nullptr;
    std::wstring path;
    if (SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&sp))) &&
        SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) &&
        SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv))))
    {
        int n = 0;
        has_selection = SUCCEEDED(fv->ItemCount(SVGIO_SELECTION, &n)) && n > 0;
        if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&folder)))) path = file_path(folder);
    }
    for (IUnknown* p :
         {static_cast<IUnknown*>(folder), static_cast<IUnknown*>(fv), static_cast<IUnknown*>(view),
          static_cast<IUnknown*>(browser), static_cast<IUnknown*>(sp)})
        if (p) p->Release();
    return path;
}

// What was right-clicked. Explorer gives a background click its folder as
// the one item, so that's told apart by the view having nothing selected.
rap::shell::selection selection_of(IShellItemArray* items, IUnknown* site)
{
    rap::shell::selection sel;
    DWORD n = 0;
    if (items) items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i)
    {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        std::wstring p = file_path(item);
        item->Release();
        if (!p.empty()) sel.paths.emplace_back(std::move(p));
    }
    bool has_selection = false;
    const std::wstring folder = site_folder(site, has_selection);
    if (!has_selection && !folder.empty() &&
        (sel.paths.empty() || (sel.paths.size() == 1 && sel.paths.front() == folder)))
    {
        sel.paths.clear();
        sel.folder = folder;
        sel.background = true;
    }
    else if (!sel.paths.empty())
        sel.folder = sel.paths.front().parent_path();
    return sel;
}

class command_list;

// The entry (no item) or one of the items under it.
class command final : public IExplorerCommand, public IObjectWithSite
{
public:
    command()
        : state_(std::make_shared<menu_state>())
    {
        ++g_objects;
    }
    command(std::shared_ptr<menu_state> state, const menu_item* item)
        : state_(std::move(state))
        , item_(item)
    {
        ++g_objects;
    }
    ~command()
    {
        if (site_) site_->Release();
        --g_objects;
    }

    // -- IUnknown -------------------------------------------------------------

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerCommand)
            *out = static_cast<IExplorerCommand*>(this);
        else if (riid == IID_IObjectWithSite)
            *out = static_cast<IObjectWithSite*>(this);
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

    // -- IObjectWithSite --------------------------------------------------------

    STDMETHODIMP SetSite(IUnknown* site) override
    {
        if (site) site->AddRef();
        if (site_) site_->Release();
        site_ = site;
        return S_OK;
    }
    STDMETHODIMP GetSite(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return site_ ? site_->QueryInterface(riid, out) : E_FAIL;
    }

    // -- IExplorerCommand -------------------------------------------------------

    STDMETHODIMP GetTitle(IShellItemArray*, LPWSTR* out) override
    try
    {
        return dup(item_ ? plain(item_->label) : plain(rap::shell::this_extension.name), out);
    }
    catch (...)
    {
        return E_FAIL;
    }

    // "path,index", or an .ico's path, beside the extension.
    STDMETHODIMP GetIcon(IShellItemArray*, LPWSTR* out) override
    try
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        const std::string& icon = item_ ? item_->icon : state_->icon;
        if (icon.empty()) return E_NOTIMPL;
        return dup((rap::shell::resource_dir() / std::filesystem::path(wide(icon))).wstring(), out);
    }
    catch (...)
    {
        return E_FAIL;
    }

    STDMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* out) override
    {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }

    STDMETHODIMP GetCanonicalName(GUID* out) override
    {
        if (out) *out = GUID_NULL;
        return E_NOTIMPL;
    }

    // The entry builds the menu, which may be slow (a repository's state),
    // so off Explorer's UI thread: it asks again there after E_PENDING.
    STDMETHODIMP GetState(IShellItemArray* items, BOOL ok_to_be_slow, EXPCMDSTATE* out) override
    try
    {
        if (!out) return E_POINTER;
        *out = ECS_HIDDEN;
        if (item_)
        {
            *out = item_->enabled ? ECS_ENABLED : ECS_DISABLED;
            return S_OK;
        }
        if (!ok_to_be_slow) return E_PENDING;
        build(items);
        if (!state_->items.empty()) *out = ECS_ENABLED;
        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }

    STDMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override
    try
    {
        if (!item_ || item_->id.empty() || !state_->provider) return E_FAIL;
        state_->provider->invoke(item_->id, state_->sel);
        return S_OK;
    }
    catch (...)
    {
        return E_FAIL;
    }

    STDMETHODIMP GetFlags(EXPCMDFLAGS* out) override
    {
        if (!out) return E_POINTER;
        if (!item_ || !item_->submenu.empty())
            *out = ECF_HASSUBCOMMANDS;
        else if (item_->is_separator())
            *out = ECF_ISSEPARATOR;
        else
            *out = ECF_DEFAULT;
        return S_OK;
    }

    STDMETHODIMP EnumSubCommands(IEnumExplorerCommand** out) override;

private:
    void build(IShellItemArray* items)
    {
        state_->provider = rap::shell::make_provider();
        state_->sel = selection_of(items, site_);
        state_->items.clear();
        state_->icon.clear();
        if (state_->provider && (!state_->sel.paths.empty() || state_->sel.background))
            state_->items = flatten(state_->provider->menu(state_->sel), state_->icon);
        state_->built = true;
    }

    std::atomic<ULONG> refs_{1};
    std::shared_ptr<menu_state> state_;
    const menu_item* item_ = nullptr;  // in state_->items; null for the entry
    IUnknown* site_ = nullptr;
};

// The items under the entry, or under one of its submenus.
class command_list final : public IEnumExplorerCommand
{
public:
    command_list(std::shared_ptr<menu_state> state, const std::vector<menu_item>* items,
                 size_t at = 0)
        : state_(std::move(state))
        , items_(items)
        , at_(at)
    {
        ++g_objects;
    }
    ~command_list() { --g_objects; }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumExplorerCommand)
        {
            *out = static_cast<IEnumExplorerCommand*>(this);
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

    STDMETHODIMP Next(ULONG n, IExplorerCommand** out, ULONG* fetched) override
    try
    {
        if (!out) return E_POINTER;
        ULONG got = 0;
        for (; got < n && at_ < items_->size(); ++got, ++at_)
            out[got] = new command(state_, &(*items_)[at_]);
        if (fetched) *fetched = got;
        return got == n ? S_OK : S_FALSE;
    }
    catch (...)
    {
        return E_OUTOFMEMORY;
    }
    STDMETHODIMP Skip(ULONG n) override
    {
        at_ = std::min(items_->size(), at_ + n);
        return at_ < items_->size() ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        at_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumExplorerCommand** out) override
    try
    {
        if (!out) return E_POINTER;
        *out = new command_list(state_, items_, at_);
        return S_OK;
    }
    catch (...)
    {
        return E_OUTOFMEMORY;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<menu_state> state_;
    const std::vector<menu_item>* items_;
    size_t at_;
};

STDMETHODIMP command::EnumSubCommands(IEnumExplorerCommand** out)
try
{
    if (!out) return E_POINTER;
    *out = nullptr;
    const std::vector<menu_item>* items = item_ ? &item_->submenu : &state_->items;
    if (!item_ && !state_->built) return E_FAIL;
    *out = new command_list(state_, items);
    return S_OK;
}
catch (...)
{
    return E_OUTOFMEMORY;
}

class command_factory final : public IClassFactory
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
        auto* c = new command;
        const HRESULT hr = c->QueryInterface(riid, out);
        c->Release();
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

command_factory g_command_factory;

}  // namespace

namespace rap::shell::detail
{

HRESULT command_class_object(REFCLSID clsid, REFIID riid, void** out)
{
    const char* id = this_extension.windows_command_clsid;
    CLSID ours{};
    if (!id || !*id || FAILED(CLSIDFromString(wide(id).c_str(), &ours)) || clsid != ours)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_command_factory.QueryInterface(riid, out);
}

}  // namespace rap::shell::detail
