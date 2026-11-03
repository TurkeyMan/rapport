// rapport-explorer-probe: drives an extension DLL's context menu the way
// Explorer does (IShellExtInit with a shell data object, then IContextMenu),
// without registering it. With --command, Windows 11's entry instead
// (IExplorerCommand: its state, then its items), with no view as its site,
// so a background can't be told from its folder; --invoke then takes an
// item's title.
//   rapport-explorer-probe DLL {CLSID} [--command] [--background] [--extended]
//                          [--invoke VERB] PATH...
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    std::string s(
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr),
        '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr,
                        nullptr);
    return s;
}

void dump(IContextMenu* cm, HMENU menu, UINT first, int depth)
{
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i)
    {
        wchar_t text[256] = {};
        MENUITEMINFOW mii{.cbSize = sizeof mii};
        mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_BITMAP;
        mii.dwTypeData = text;
        mii.cch = 255;
        GetMenuItemInfoW(menu, UINT(i), TRUE, &mii);
        if (mii.fType & MFT_SEPARATOR)
        {
            std::printf("%*s----\n", depth * 2, "");
            continue;
        }
        wchar_t verb[128] = {};
        const bool has_verb =
            !mii.hSubMenu && SUCCEEDED(cm->GetCommandString(mii.wID - first, GCS_VERBW, nullptr,
                                                            reinterpret_cast<LPSTR>(verb), 127));
        std::printf("%*s%s%s%s%s%s\n", depth * 2, "", narrow(text).c_str(),
                    mii.fState & MFS_DISABLED ? "  (disabled)" : "", has_verb ? "  [" : "",
                    has_verb ? narrow(verb).c_str() : "", has_verb ? "]" : "");
        if (mii.hbmpItem) std::printf("%*s  (bitmap)\n", depth * 2, "");
        if (mii.hSubMenu) dump(cm, mii.hSubMenu, first, depth + 1);
    }
}

std::string title(IExplorerCommand* c, IShellItemArray* items)
{
    PWSTR t = nullptr;
    if (FAILED(c->GetTitle(items, &t))) return {};
    std::string s = narrow(t);
    CoTaskMemFree(t);
    return s;
}

// Prints the items under `c`, and invokes the one titled `invoke`.
void dump_command(IExplorerCommand* c, IShellItemArray* items, const std::string& invoke, int depth)
{
    IEnumExplorerCommand* list = nullptr;
    if (FAILED(c->EnumSubCommands(&list))) return;
    IExplorerCommand* sub = nullptr;
    while (list->Next(1, &sub, nullptr) == S_OK)
    {
        EXPCMDFLAGS flags = 0;
        EXPCMDSTATE state = ECS_ENABLED;
        sub->GetFlags(&flags);
        sub->GetState(items, TRUE, &state);
        PWSTR icon = nullptr;
        const bool has_icon = SUCCEEDED(sub->GetIcon(items, &icon));
        if (flags & 0x8)  // ECF_ISSEPARATOR
            std::printf("%*s----\n", depth * 2, "");
        else
        {
            const std::string t = title(sub, items);
            std::printf("%*s%s%s%s%s\n", depth * 2, "", t.c_str(),
                        state & ECS_DISABLED ? "  (disabled)" : "", has_icon ? "  icon: " : "",
                        has_icon ? narrow(icon).c_str() : "");
            if (flags & ECF_HASSUBCOMMANDS)
                dump_command(sub, items, invoke, depth + 1);
            else if (t == invoke)
                std::printf("Invoke(%s): 0x%08lx\n", t.c_str(), sub->Invoke(items, nullptr));
        }
        CoTaskMemFree(icon);
        sub->Release();
    }
    list->Release();
}

int probe_command(IClassFactory* factory, const std::vector<PIDLIST_ABSOLUTE>& pidls,
                  const std::string& invoke)
{
    IExplorerCommand* c = nullptr;
    IShellItemArray* items = nullptr;
    if (FAILED(
            factory->CreateInstance(nullptr, IID_IExplorerCommand, reinterpret_cast<void**>(&c))))
    {
        std::fprintf(stderr, "no IExplorerCommand for that CLSID\n");
        return 1;
    }
    if (FAILED(SHCreateShellItemArrayFromIDLists(
            UINT(pidls.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items)))
        return 1;
    EXPCMDSTATE state = ECS_HIDDEN;
    std::printf("GetState (UI thread): 0x%08lx\n", c->GetState(items, FALSE, &state));
    const DWORD start = GetTickCount();
    const HRESULT hr = c->GetState(items, TRUE, &state);
    std::printf("GetState: 0x%08lx, %s, %lu ms\n", hr,
                state == ECS_HIDDEN    ? "hidden"
                : state & ECS_DISABLED ? "disabled"
                                       : "enabled",
                GetTickCount() - start);
    PWSTR icon = nullptr;
    if (SUCCEEDED(c->GetIcon(items, &icon)))
    {
        std::printf("icon: %s\n", narrow(icon).c_str());
        CoTaskMemFree(icon);
    }
    std::printf("%s\n", title(c, items).c_str());
    if (state != ECS_HIDDEN) dump_command(c, items, invoke, 1);
    items->Release();
    c->Release();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4)
    {
        std::fprintf(
            stderr,
            "usage: rapport-explorer-probe DLL {CLSID} [--command] [--background] [--extended] "
            "[--invoke VERB] PATH...\n");
        return 2;
    }
    bool background = false, extended = false, command = false;
    std::wstring invoke;
    std::vector<std::wstring> paths;
    for (int i = 3; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--background")
            background = true;
        else if (a == L"--command")
            command = true;
        else if (a == L"--extended")
            extended = true;
        else if (a == L"--invoke" && i + 1 < argc)
            invoke = argv[++i];
        else
            paths.push_back(a);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll)
    {
        std::fprintf(stderr, "can't load the DLL (%lu)\n", GetLastError());
        return 1;
    }
    using get_class_object = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto get = reinterpret_cast<get_class_object>(
        reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    CLSID clsid{};
    IClassFactory* factory = nullptr;
    IShellExtInit* init = nullptr;
    IContextMenu* cm = nullptr;
    if (!get || FAILED(CLSIDFromString(argv[2], &clsid)) ||
        FAILED(get(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))))
    {
        std::fprintf(stderr, "no class object for that CLSID\n");
        return 1;
    }
    if (command)
    {
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const std::wstring& p : paths)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)))
            {
                std::fprintf(stderr, "no such item: %s\n", narrow(p).c_str());
                return 1;
            }
            pidls.push_back(pidl);
        }
        const int r = probe_command(factory, pidls, narrow(invoke));
        for (auto p : pidls) CoTaskMemFree(p);
        factory->Release();
        return r;
    }
    if (FAILED(
            factory->CreateInstance(nullptr, IID_IShellExtInit, reinterpret_cast<void**>(&init))) ||
        FAILED(init->QueryInterface(IID_IContextMenu, reinterpret_cast<void**>(&cm))))
    {
        std::fprintf(stderr, "no context menu object for that CLSID\n");
        return 1;
    }

    HRESULT hr;
    if (background)
    {
        PIDLIST_ABSOLUTE folder = nullptr;
        if (FAILED(SHParseDisplayName(paths.at(0).c_str(), nullptr, &folder, 0, nullptr))) return 1;
        hr = init->Initialize(folder, nullptr, nullptr);
        CoTaskMemFree(folder);
    }
    else
    {
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const std::wstring& p : paths)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)))
            {
                std::fprintf(stderr, "no such item: %s\n", narrow(p).c_str());
                return 1;
            }
            pidls.push_back(pidl);
        }
        IShellItemArray* items = nullptr;
        IDataObject* data = nullptr;
        if (FAILED(SHCreateShellItemArrayFromIDLists(
                UINT(pidls.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items)) ||
            FAILED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))))
            return 1;
        hr = init->Initialize(nullptr, data, nullptr);
        data->Release();
        items->Release();
        for (auto p : pidls) CoTaskMemFree(p);
    }
    std::printf("Initialize: 0x%08lx\n", hr);
    if (FAILED(hr)) return 0;

    const UINT first = 100;
    HMENU menu = CreatePopupMenu();
    const DWORD start = GetTickCount();
    hr = cm->QueryContextMenu(menu, 0, first, 0x7FFF,
                              CMF_NORMAL | (extended ? CMF_EXTENDEDVERBS : 0));
    std::printf("QueryContextMenu: 0x%08lx, %u ids, %lu ms\n", hr, HRESULT_CODE(hr),
                GetTickCount() - start);
    dump(cm, menu, first, 1);

    if (!invoke.empty())
    {
        CMINVOKECOMMANDINFOEX ci{.cbSize = sizeof ci};
        const std::string verb = narrow(invoke);
        ci.fMask = CMIC_MASK_UNICODE;
        ci.lpVerb = verb.c_str();
        ci.lpVerbW = invoke.c_str();
        ci.nShow = SW_SHOWNORMAL;
        hr = cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ci));
        std::printf("InvokeCommand(%s): 0x%08lx\n", verb.c_str(), hr);
    }
    DestroyMenu(menu);
    cm->Release();
    init->Release();
    factory->Release();
    using can_unload = HRESULT(STDAPICALLTYPE*)();
    auto unload = reinterpret_cast<can_unload>(
        reinterpret_cast<void*>(GetProcAddress(dll, "DllCanUnloadNow")));
    std::printf("DllCanUnloadNow: %s\n", unload && unload() == S_OK ? "yes" : "no");
    return 0;
}
