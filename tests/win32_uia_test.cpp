// UI Automation for drawn elements (win32_uia): a window serving a caption
// tab strip's elements, as the Windows 11 strip will, read and driven by a
// UIA client in the same process. The strip is a Tab (Selection) of
// TabItems (SelectionItem, named by their titles), each with a "Close tab"
// Button (Invoke), and a "New tab" Button (Invoke) after them.
//
// The client runs on a thread of its own, in the multithreaded apartment,
// while the window's thread pumps messages, as UIA asks of a client reading
// its own process's windows: the provider's calls are marshalled onto the
// window's thread, where each of the owner's handlers must run, once.
// Bounds are the elements' client rects on the screen. Elements added and
// removed are seen to be, a removed one held by the client is no longer
// available, and so is every one once the window is gone. The window is
// shown without activating and is never given input.
//   rapport-win32-uia-test
#include "win32_uia.h"

#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace rap::win32;

std::mutex out_lock;
std::atomic<int> fails{0};

void expect(bool ok, const char* what)
{
    std::lock_guard g(out_lock);
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++fails;
}

void note(const char* what, long value)
{
    std::lock_guard g(out_lock);
    std::printf("     %s: 0x%08lx\n", what, value);
    std::fflush(stdout);
}

// The strip's elements. Ids: the Tab 1, "New tab" 2, the n-th TabItem
// 10 * (n + 1) and its close button one more.
constexpr int tabs_id = 1;
constexpr int new_tab_id = 2;
constexpr int item_id(int n) { return 10 * (n + 1); }
constexpr int close_id(int n) { return item_id(n) + 1; }
constexpr int item_width = 160;
constexpr int strip_height = 40;

std::vector<uia_element> strip(const std::vector<std::wstring>& titles, int selected)
{
    std::vector<uia_element> out;
    const int n = int(titles.size());
    uia_element tabs;
    tabs.id = tabs_id;
    tabs.control_type = UIA_TabControlTypeId;
    tabs.name = L"Tabs";
    tabs.bounds = RECT{0, 0, n * item_width + 48, strip_height};
    tabs.patterns = uia_selection;
    out.push_back(tabs);
    for (int i = 0; i < n; ++i)
    {
        uia_element item;
        item.id = item_id(i);
        item.parent = tabs_id;
        item.control_type = UIA_TabItemControlTypeId;
        item.name = titles[size_t(i)];
        item.bounds = RECT{i * item_width, 0, (i + 1) * item_width, strip_height};
        item.patterns = uia_selection_item;
        item.focusable = true;
        item.selected = i == selected;
        out.push_back(item);
        uia_element close;
        close.id = close_id(i);
        close.parent = item.id;
        close.control_type = UIA_ButtonControlTypeId;
        close.name = L"Close tab";
        close.bounds = RECT{item.bounds.right - 32, 8, item.bounds.right - 8, 32};
        close.patterns = uia_invoke;
        out.push_back(close);
    }
    uia_element add;
    add.id = new_tab_id;
    add.parent = tabs_id;
    add.control_type = UIA_ButtonControlTypeId;
    add.name = L"New tab";
    add.bounds = RECT{n * item_width + 8, 4, n * item_width + 40, 36};
    add.patterns = uia_invoke;
    out.push_back(add);
    return out;
}

// The strip's side of it: counts what UIA asks of it, and selects as asked.
struct strip_owner final : uia_owner
{
    uia_provider* provider = nullptr;
    DWORD ui_thread = 0;
    std::array<std::atomic<int>, 64> invoked{};
    std::array<std::atomic<int>, 64> selects{};
    std::atomic<int> other_calls{0};
    std::atomic<bool> off_thread{false};

    void here() { if (GetCurrentThreadId() != ui_thread) off_thread = true; }
    void uia_invoke(int id) override
    {
        here();
        if (id >= 0 && id < 64) ++invoked[size_t(id)];
    }
    void uia_select(int id) override
    {
        here();
        if (id >= 0 && id < 64) ++selects[size_t(id)];
        for (const uia_element& e : provider->elements())
            if (uia_element* m = provider->element(e.id); m && (m->patterns & uia_selection_item))
                m->selected = m->id == id;
        provider->raise_event(id, UIA_SelectionItem_ElementSelectedEventId);
    }
    bool uia_add_to_selection(int) override { ++other_calls; return true; }
    bool uia_remove_from_selection(int) override { ++other_calls; return true; }
};

// What the client asks of the window's thread, posted: each sets `done`.
constexpr UINT raise_events = WM_APP + 1;
constexpr UINT add_tab = WM_APP + 2;
constexpr UINT remove_tab = WM_APP + 3;
constexpr UINT destroy = WM_APP + 4;

struct shared
{
    HWND hwnd = nullptr;
    strip_owner* owner = nullptr;
    RECT screen[64]{};  // each element's expected screen rect, by id
    // A tab element as the window's thread has it, handed to the client
    // unmarshalled, to see a call from the wrong thread refused.
    IRawElementProviderSimple* raw_tab = nullptr;
    HANDLE done = nullptr;      // the window's thread did what was posted
    HANDLE finished = nullptr;  // the client is through
};
shared S;

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* p = reinterpret_cast<uia_provider*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    LRESULT result = 0;
    if (p && p->handle_message(msg, wp, lp, result)) return result;
    switch (msg)
    {
    case raise_events:
        p->raise_focus_changed(item_id(1));
        p->raise_structure_changed(StructureChangeType_ChildrenInvalidated, tabs_id);
        p->raise_focus_changed(0);
        SetEvent(S.done);
        return 0;
    case add_tab:
        p->set_elements(strip({L"First", L"Second", L"Third", L"Fourth"}, 1));
        p->raise_structure_changed(StructureChangeType_ChildAdded, item_id(3));
        SetEvent(S.done);
        return 0;
    case remove_tab:
        p->set_elements(strip({L"First", L"Second", L"Third"}, 1));
        p->raise_child_removed(tabs_id, item_id(3));
        SetEvent(S.done);
        return 0;
    case destroy:
        DestroyWindow(hwnd);
        SetEvent(S.done);
        return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Posts to the window's thread and waits for it to have done it.
bool ask(UINT msg)
{
    ResetEvent(S.done);
    return PostMessageW(S.hwnd, msg, 0, 0) && WaitForSingleObject(S.done, 10000) == WAIT_OBJECT_0;
}

// Waits up to `ms` for `n` to reach `want`.
bool wait_for(const std::atomic<int>& n, int want, DWORD ms = 5000)
{
    for (DWORD waited = 0; n < want && waited < ms; waited += 10) Sleep(10);
    return n >= want;
}

template <class T>
void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}

std::wstring name_of(IUIAutomationElement* e)
{
    BSTR b = nullptr;
    std::wstring out;
    if (e && SUCCEEDED(e->get_CurrentName(&b)) && b) out = b;
    SysFreeString(b);
    return out;
}

bool rect_is(IUIAutomationElement* e, int id)
{
    RECT r{};
    if (!e || FAILED(e->get_CurrentBoundingRectangle(&r))) return false;
    const RECT& want = S.screen[id];
    return r.left == want.left && r.top == want.top && r.right == want.right && r.bottom == want.bottom;
}

// A UIA event handler that counts.
template <class Interface>
struct counter : Interface
{
    std::atomic<ULONG> refs{1};
    std::atomic<int> calls{0};
    virtual ~counter() = default;  // deleted as this, being the handler
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(Interface))
        {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs;
        if (n == 0) delete this;
        return n;
    }
};
struct structure_counter final : counter<IUIAutomationStructureChangedEventHandler>
{
    IFACEMETHODIMP HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType, SAFEARRAY*) override
    {
        ++calls;
        return S_OK;
    }
};
struct focus_counter final : counter<IUIAutomationFocusChangedEventHandler>
{
    IFACEMETHODIMP HandleFocusChangedEvent(IUIAutomationElement*) override
    {
        ++calls;
        return S_OK;
    }
};

IUIAutomationElement* find(IUIAutomation* uia, IUIAutomationElement* in, TreeScope scope, CONTROLTYPEID type,
                           const wchar_t* name)
{
    VARIANT t;
    VariantInit(&t);
    t.vt = VT_I4;
    t.lVal = type;
    IUIAutomationCondition* by_type = nullptr;
    IUIAutomationCondition* by_name = nullptr;
    IUIAutomationCondition* both = nullptr;
    IUIAutomationElement* found = nullptr;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, t, &by_type);
    if (name)
    {
        VARIANT n;
        VariantInit(&n);
        n.vt = VT_BSTR;
        n.bstrVal = SysAllocString(name);
        uia->CreatePropertyCondition(UIA_NamePropertyId, n, &by_name);
        VariantClear(&n);
        if (by_type && by_name) uia->CreateAndCondition(by_type, by_name, &both);
    }
    IUIAutomationCondition* c = name ? both : by_type;
    if (in && c) in->FindFirst(scope, c, &found);
    release(both);
    release(by_name);
    release(by_type);
    return found;
}

int count(IUIAutomation* uia, IUIAutomationElement* in, CONTROLTYPEID type)
{
    VARIANT t;
    VariantInit(&t);
    t.vt = VT_I4;
    t.lVal = type;
    IUIAutomationCondition* c = nullptr;
    IUIAutomationElementArray* all = nullptr;
    int n = -1;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, t, &c);
    if (in && c && SUCCEEDED(in->FindAll(TreeScope_Children, c, &all)) && all) all->get_Length(&n);
    release(all);
    release(c);
    return n;
}

template <class Pattern>
Pattern* pattern(IUIAutomationElement* e, PATTERNID id)
{
    Pattern* p = nullptr;
    if (e) e->GetCurrentPatternAs(id, __uuidof(Pattern), reinterpret_cast<void**>(&p));
    return p;
}

bool same(IUIAutomation* uia, IUIAutomationElement* a, IUIAutomationElement* b)
{
    BOOL is = FALSE;
    return a && b && SUCCEEDED(uia->CompareElements(a, b, &is)) && is;
}

void client()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    strip_owner& owner = *S.owner;
    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                     reinterpret_cast<void**>(&uia));
    expect(uia != nullptr, "a UIA client");
    if (!uia)
    {
        CoUninitialize();
        SetEvent(S.finished);
        return;
    }

    // A call into an element off the window's thread, not marshalled: refused.
    {
        VARIANT v;
        VariantInit(&v);
        ProviderOptions options{};
        expect(S.raw_tab->GetPropertyValue(UIA_NamePropertyId, &v) == RPC_E_WRONG_THREAD && v.vt == VT_EMPTY,
               "an element called off the window's thread refuses");
        expect(SUCCEEDED(S.raw_tab->get_ProviderOptions(&options)) && (options & ProviderOptions_UseComThreading) &&
                   (options & ProviderOptions_ServerSideProvider),
               "and says it's server-side and uses COM threading, from any thread");
    }

    IUIAutomationElement* window = nullptr;
    uia->ElementFromHandle(S.hwnd, &window);
    expect(window != nullptr, "the window's element");

    IUIAutomationElement* tabs = find(uia, window, TreeScope_Descendants, UIA_TabControlTypeId, nullptr);
    expect(tabs != nullptr, "the strip: a Tab in the window");
    expect(name_of(tabs) == L"Tabs", "named");
    expect(rect_is(tabs, tabs_id), "its bounds, on the screen");
    expect(count(uia, tabs, UIA_TabItemControlTypeId) == 3, "three TabItems in it");
    IUIAutomationTreeWalker* walker = nullptr;
    uia->get_RawViewWalker(&walker);
    {
        IUIAutomationElement* up = nullptr;
        if (walker && tabs) walker->GetParentElement(tabs, &up);
        expect(same(uia, up, window), "the Tab's parent is the window");
        release(up);
    }

    IUIAutomationElement* first = find(uia, tabs, TreeScope_Children, UIA_TabItemControlTypeId, L"First");
    IUIAutomationElement* second = find(uia, tabs, TreeScope_Children, UIA_TabItemControlTypeId, L"Second");
    IUIAutomationElement* third = find(uia, tabs, TreeScope_Children, UIA_TabItemControlTypeId, L"Third");
    expect(first && second && third, "the TabItems by their titles");
    expect(rect_is(second, item_id(1)), "a TabItem's bounds, on the screen");
    {
        IUIAutomationElement* next = nullptr;
        if (walker && first) walker->GetNextSiblingElement(first, &next);
        expect(same(uia, next, second), "the first TabItem's next sibling is the second");
        release(next);
    }

    // Selection: one, required, the first; then the second, as UIA asks.
    IUIAutomationSelectionPattern* selection = pattern<IUIAutomationSelectionPattern>(tabs, UIA_SelectionPatternId);
    expect(selection != nullptr, "the Tab has Selection");
    if (selection)
    {
        BOOL multiple = TRUE, required = FALSE;
        selection->get_CurrentCanSelectMultiple(&multiple);
        selection->get_CurrentIsSelectionRequired(&required);
        expect(!multiple && required, "of one, required");
        IUIAutomationElementArray* picked = nullptr;
        int n = 0;
        IUIAutomationElement* one = nullptr;
        if (SUCCEEDED(selection->GetCurrentSelection(&picked)) && picked)
        {
            picked->get_Length(&n);
            if (n == 1) picked->GetElement(0, &one);
        }
        expect(n == 1 && same(uia, one, first), "the first tab selected");
        release(one);
        release(picked);
    }
    IUIAutomationSelectionItemPattern* second_item =
        pattern<IUIAutomationSelectionItemPattern>(second, UIA_SelectionItemPatternId);
    expect(second_item != nullptr, "a TabItem has SelectionItem");
    if (second_item)
    {
        expect(SUCCEEDED(second_item->Select()), "selecting the second tab");
        expect(owner.selects[size_t(item_id(1))] == 1, "the strip was asked once");
        int others = 0;
        for (int i = 0; i < 64; ++i)
            if (i != item_id(1)) others += owner.selects[size_t(i)];
        expect(others == 0, "and for no other tab");
        BOOL is = FALSE;
        second_item->get_CurrentIsSelected(&is);
        expect(is, "the second tab is selected");
        IUIAutomationElement* container = nullptr;
        second_item->get_CurrentSelectionContainer(&container);
        expect(same(uia, container, tabs), "in the Tab");
        release(container);
        // One required: the only selected can't be removed, nor another added.
        const HRESULT removed = second_item->RemoveFromSelection();
        note("RemoveFromSelection of the only selected", removed);
        expect(FAILED(removed), "the only selected tab can't be taken out of the selection");
    }
    {
        IUIAutomationSelectionItemPattern* third_item =
            pattern<IUIAutomationSelectionItemPattern>(third, UIA_SelectionItemPatternId);
        const HRESULT added = third_item ? third_item->AddToSelection() : E_FAIL;
        note("AddToSelection in a one-tab selection", added);
        expect(FAILED(added), "nor another added to it");
        expect(owner.other_calls == 0, "and the strip isn't asked either");
        release(third_item);
    }

    // Invoke: the second tab's close button, then New tab, each once.
    IUIAutomationElement* close = find(uia, second, TreeScope_Children, UIA_ButtonControlTypeId, L"Close tab");
    expect(close != nullptr, "the second tab's Close tab button");
    expect(rect_is(close, close_id(1)), "its bounds, on the screen");
    {
        IUIAutomationElement* up = nullptr;
        if (walker && close) walker->GetParentElement(close, &up);
        expect(same(uia, up, second), "in its tab");
        release(up);
    }
    IUIAutomationInvokePattern* close_invoke = pattern<IUIAutomationInvokePattern>(close, UIA_InvokePatternId);
    expect(close_invoke && SUCCEEDED(close_invoke->Invoke()), "invoking it");
    expect(wait_for(owner.invoked[size_t(close_id(1))], 1), "the strip hears it");
    IUIAutomationElement* add = find(uia, tabs, TreeScope_Children, UIA_ButtonControlTypeId, L"New tab");
    expect(add != nullptr, "the New tab button");
    expect(rect_is(add, new_tab_id), "its bounds, on the screen");
    {
        IUIAutomationInvokePattern* add_invoke = pattern<IUIAutomationInvokePattern>(add, UIA_InvokePatternId);
        expect(add_invoke && SUCCEEDED(add_invoke->Invoke()), "invoking it");
        expect(wait_for(owner.invoked[size_t(new_tab_id)], 1), "the strip hears it");
        release(add_invoke);
    }
    Sleep(200);
    {
        int others = 0;
        for (int i = 0; i < 64; ++i)
            if (i != close_id(1) && i != new_tab_id) others += owner.invoked[size_t(i)];
        expect(owner.invoked[size_t(close_id(1))] == 1 && owner.invoked[size_t(new_tab_id)] == 1 && others == 0,
               "each heard once, and nothing else invoked");
    }

    // Events: raised while a client listens, received or not, harmlessly.
    auto* structure = new structure_counter;
    auto* focus = new focus_counter;
    const bool listening = tabs && SUCCEEDED(uia->AddStructureChangedEventHandler(tabs, TreeScope_Subtree, nullptr,
                                                                                    structure)) &&
                           SUCCEEDED(uia->AddFocusChangedEventHandler(nullptr, focus));
    expect(listening, "listening for structure and focus changes");
    expect(ask(raise_events), "focus and structure changes raised");

    // A tab added: seen, then removed while the client holds it.
    expect(ask(add_tab), "a fourth tab added");
    expect(count(uia, tabs, UIA_TabItemControlTypeId) == 4, "four TabItems");
    IUIAutomationElement* fourth = find(uia, tabs, TreeScope_Children, UIA_TabItemControlTypeId, L"Fourth");
    expect(fourth != nullptr, "the fourth by its title");
    expect(ask(remove_tab), "and removed");
    {
        BSTR b = nullptr;
        const HRESULT hr = fourth ? fourth->get_CurrentName(&b) : E_FAIL;
        SysFreeString(b);
        note("a removed tab's name", hr);
        expect(hr == UIA_E_ELEMENTNOTAVAILABLE, "the removed tab, held, is no longer available");
    }
    expect(count(uia, tabs, UIA_TabItemControlTypeId) == 3, "three TabItems again");
    expect(name_of(second) == L"Second", "the tabs kept are the same elements");
    Sleep(300);
    {
        std::lock_guard g(out_lock);
        std::printf("     structure changes heard: %d, focus changes heard: %d\n", structure->calls.load(),
                    focus->calls.load());
    }
    uia->RemoveAllEventHandlers();
    release(structure);
    release(focus);

    // The window gone: what the client holds is no longer available.
    expect(ask(destroy), "the window destroyed");
    {
        BSTR b = nullptr;
        const HRESULT hr = second ? second->get_CurrentName(&b) : E_FAIL;
        SysFreeString(b);
        note("a tab's name, the window gone", hr);
        expect(hr == UIA_E_ELEMENTNOTAVAILABLE, "a held TabItem is no longer available");
        RECT r{};
        const HRESULT bounds = close ? close->get_CurrentBoundingRectangle(&r) : E_FAIL;
        note("a button's bounds, the window gone", bounds);
        expect(bounds == UIA_E_ELEMENTNOTAVAILABLE, "nor a held button");
        const HRESULT invoked = close_invoke ? close_invoke->Invoke() : E_FAIL;
        note("invoking a held button, the window gone", invoked);
        expect(FAILED(invoked), "which can't be invoked");
        const HRESULT selected = second_item ? second_item->Select() : E_FAIL;
        note("selecting a held tab, the window gone", selected);
        expect(FAILED(selected), "nor a held tab selected");
    }

    release(close_invoke);
    release(second_item);
    release(selection);
    release(fourth);
    release(add);
    release(close);
    release(third);
    release(second);
    release(first);
    release(walker);
    release(tabs);
    release(window);
    release(uia);
    CoUninitialize();
    SetEvent(S.finished);
}

}  // namespace

int main()
{
    OleInitialize(nullptr);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"rapport-uia-test";
    RegisterClassExW(&wc);
    // Shown, so UIA sees it as any window, but never activated.
    S.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"rapport UIA test", WS_POPUP,
                             64, 64, 640, 200, nullptr, nullptr, instance, nullptr);
    expect(S.hwnd != nullptr, "a window");
    if (!S.hwnd) return 1;

    strip_owner owner;
    owner.ui_thread = GetCurrentThreadId();
    auto* provider = new uia_provider(S.hwnd, owner);
    owner.provider = provider;
    provider->set_elements(strip({L"First", L"Second", L"Third"}, 0));
    SetWindowLongPtrW(S.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(provider));
    ShowWindow(S.hwnd, SW_SHOWNOACTIVATE);
    for (const uia_element& e : provider->elements())
    {
        POINT p[2] = {{e.bounds.left, e.bounds.top}, {e.bounds.right, e.bounds.bottom}};
        MapWindowPoints(S.hwnd, nullptr, p, 2);
        S.screen[e.id] = RECT{p[0].x, p[0].y, p[1].x, p[1].y};
    }
    S.owner = &owner;

    // The root and a tab as the window's thread has them, held past the
    // window, as a client in another process might.
    IRawElementProviderFragment* root = nullptr;
    IRawElementProviderFragment* tab = nullptr;
    if (IRawElementProviderSimple* r = provider->root())
        r->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(&root));
    if (root) root->Navigate(NavigateDirection_FirstChild, &tab);
    if (tab) tab->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&S.raw_tab));
    expect(root && tab && S.raw_tab, "the root and its first child, directly");

    S.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    S.finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker(client);
    // Pump until the client's through: UIA's calls into the provider arrive
    // through this thread's messages.
    const ULONGLONG deadline = GetTickCount64() + 60000;
    bool through = false;
    while (!through)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD r = MsgWaitForMultipleObjects(1, &S.finished, FALSE, DWORD(deadline - now), QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
            through = true;
        else if (r == WAIT_OBJECT_0 + 1)
        {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        else
            break;
    }
    if (!through)
    {
        expect(false, "the client finished within a minute");
        std::fflush(stdout);
        ExitProcess(1);
    }
    worker.join();

    expect(!owner.off_thread, "the strip was only ever called on its window's thread");
    // The window's gone and the provider disconnected: what's held is
    // unavailable, and calling it touches nothing freed.
    {
        VARIANT v;
        VariantInit(&v);
        expect(S.raw_tab->GetPropertyValue(UIA_NamePropertyId, &v) == UIA_E_ELEMENTNOTAVAILABLE,
               "a held element, the window gone, is unavailable");
        IRawElementProviderFragment* child = nullptr;
        expect(root->Navigate(NavigateDirection_FirstChild, &child) == UIA_E_ELEMENTNOTAVAILABLE && !child,
               "and the root too");
        expect(provider->root() == nullptr && provider->element(tabs_id) == nullptr, "the provider has none");
    }
    // The owner's state gone as well: still nothing touched.
    delete provider;
    {
        UiaRect r{};
        expect(tab->get_BoundingRectangle(&r) == UIA_E_ELEMENTNOTAVAILABLE, "nor once the provider's gone");
    }
    release(S.raw_tab);
    release(tab);
    release(root);
    CloseHandle(S.done);
    CloseHandle(S.finished);
    OleUninitialize();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails.load());
    return fails ? 1 : 0;
}
