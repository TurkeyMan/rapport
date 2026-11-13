// UI Automation fragments for drawn elements: see win32_uia.h.
//
// Each element a client has seen is one COM object (a node), kept by the
// provider's state for as long as the element exists, so that UIA can be
// told to let go of exactly those (UiaDisconnectProvider) when the element
// or the window goes. A node knows only its id and the state; every call
// looks the element up afresh, so a node outliving its element, or the
// whole window, answers UIA_E_ELEMENTNOTAVAILABLE instead of reading freed
// memory. The state holds the nodes and each node the state: the cycle is
// broken when the provider disconnects.
#include "win32_uia.h"

#include <oleauto.h>

#include <atomic>
#include <unordered_map>
#include <utility>

namespace rap::win32
{

namespace
{

class node;

}  // namespace

struct uia_provider::state : std::enable_shared_from_this<uia_provider::state>
{
    HWND hwnd = nullptr;
    DWORD thread = 0;
    UINT invoke_message = 0;
    // Null once disconnected: no element is available after that, and the
    // owner is never called again.
    uia_owner* owner = nullptr;
    // While disconnecting, UIA may still read the elements (it finds a
    // node's clients by its runtime id), but nothing may act on them.
    bool closing = false;
    std::vector<uia_element> elements;
    // Each holds a reference. Id 0 is the fragment root.
    std::unordered_map<int, node*> nodes;

    bool connected() const { return owner != nullptr; }
    const uia_element* find(int id) const
    {
        for (const uia_element& e : elements)
            if (e.id == id) return &e;
        return nullptr;
    }
    // The nearest ancestor of `id` that's a selection container.
    const uia_element* container_of(int id) const
    {
        const uia_element* e = find(id);
        while (e && e->parent)
        {
            e = find(e->parent);
            if (e && (e->patterns & uia_selection)) return e;
        }
        return nullptr;
    }
    // The node for `id` (borrowed), made the first time it's asked for;
    // null for an id with no element, or once disconnected.
    node* get(int id);
    void release(int id);
    void disconnect();
};

namespace
{

// A client-coordinates rect as UIA's screen one.
UiaRect to_screen(HWND hwnd, const RECT& r)
{
    POINT p[2] = {{r.left, r.top}, {r.right, r.bottom}};
    // MapWindowPoints, unlike ClientToScreen, mirrors a right-to-left
    // window's x: the corners may swap.
    MapWindowPoints(hwnd, nullptr, p, 2);
    const LONG left = p[0].x < p[1].x ? p[0].x : p[1].x;
    const LONG right = p[0].x < p[1].x ? p[1].x : p[0].x;
    return UiaRect{double(left), double(p[0].y), double(right - left), double(p[1].y - p[0].y)};
}

void set_bool(VARIANT* v, bool b)
{
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

void set_string(VARIANT* v, const std::wstring& s)
{
    if (s.empty()) return;
    v->bstrVal = SysAllocStringLen(s.data(), UINT(s.size()));
    if (v->bstrVal) v->vt = VT_BSTR;
}

// One element, or (id 0) the root: the window as UIA sees it, whose own
// properties come from the HWND (the host provider), with the elements whose
// parent is 0 as its children.
class node final : public IRawElementProviderSimple,
                   public IRawElementProviderFragment,
                   public IRawElementProviderFragmentRoot,
                   public IInvokeProvider,
                   public ISelectionItemProvider,
                   public ISelectionProvider
{
public:
    node(std::shared_ptr<uia_provider::state> s, int id) : s_(std::move(s)), id_(id) {}

    // IUnknown. AddRef and Release may come from any thread: the count is
    // atomic, and the last release touches only this node and its share of
    // the state, never the window.
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment))
            *out = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderFragmentRoot) && id_ == 0)
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == __uuidof(IInvokeProvider) && id_ != 0)
            *out = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(ISelectionItemProvider) && id_ != 0)
            *out = static_cast<ISelectionItemProvider*>(this);
        else if (iid == __uuidof(ISelectionProvider) && id_ != 0)
            *out = static_cast<ISelectionProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override
    {
        if (!out) return E_POINTER;
        // Asked before UIA knows our threading, so from any thread: it reads
        // nothing of the state.
        *out = ProviderOptions(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        if (!e) return S_OK;
        if (pattern == UIA_InvokePatternId && (e->patterns & uia_invoke))
            *out = static_cast<IInvokeProvider*>(this);
        else if (pattern == UIA_SelectionItemPatternId && (e->patterns & uia_selection_item))
            *out = static_cast<ISelectionItemProvider*>(this);
        else if (pattern == UIA_SelectionPatternId && (e->patterns & uia_selection))
            *out = static_cast<ISelectionProvider*>(this);
        if (*out) AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override
    {
        if (!out) return E_POINTER;
        VariantInit(out);
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        // The root's are the window's own, from the host provider.
        if (!e) return S_OK;
        switch (property)
        {
        case UIA_ControlTypePropertyId:
            out->vt = VT_I4;
            out->lVal = e->control_type;
            break;
        case UIA_NamePropertyId: set_string(out, e->name); break;
        case UIA_AutomationIdPropertyId: set_string(out, e->automation_id); break;
        case UIA_IsEnabledPropertyId: set_bool(out, e->enabled); break;
        case UIA_IsKeyboardFocusablePropertyId: set_bool(out, e->focusable); break;
        case UIA_HasKeyboardFocusPropertyId: set_bool(out, e->focused); break;
        case UIA_IsOffscreenPropertyId: set_bool(out, e->offscreen); break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId: set_bool(out, true); break;
        default: break;
        }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (const HRESULT hr = check(); FAILED(hr)) return hr;
        return id_ == 0 ? UiaHostProviderFromHwnd(s_->hwnd, out) : S_OK;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        const auto& all = s_->elements;
        int to = -1;
        switch (direction)
        {
        case NavigateDirection_Parent:
            // The root's parent, and siblings, are the window's: UIA's.
            if (e) to = e->parent;
            break;
        case NavigateDirection_FirstChild:
            for (const uia_element& c : all)
                if (c.parent == id_)
                {
                    to = c.id;
                    break;
                }
            break;
        case NavigateDirection_LastChild:
            for (const uia_element& c : all)
                if (c.parent == id_) to = c.id;
            break;
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling:
        {
            if (!e) break;
            std::vector<int> siblings;
            for (const uia_element& c : all)
                if (c.parent == e->parent) siblings.push_back(c.id);
            for (size_t i = 0; i < siblings.size(); ++i)
                if (siblings[i] == id_)
                {
                    if (direction == NavigateDirection_NextSibling && i + 1 < siblings.size()) to = siblings[i + 1];
                    if (direction == NavigateDirection_PreviousSibling && i > 0) to = siblings[i - 1];
                    break;
                }
            break;
        }
        }
        if (to < 0) return S_OK;
        if (node* n = s_->get(to))
        {
            *out = static_cast<IRawElementProviderFragment*>(n);
            n->AddRef();
        }
        return S_OK;
    }
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (GetCurrentThreadId() != s_->thread) return RPC_E_WRONG_THREAD;
        // The root's is the window's. An element's depends on its id alone,
        // so UIA can still find a node by it while disconnecting it.
        if (id_ == 0) return S_OK;
        SAFEARRAY* a = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!a) return E_OUTOFMEMORY;
        int* v = nullptr;
        SafeArrayAccessData(a, reinterpret_cast<void**>(&v));
        v[0] = UiaAppendRuntimeId;
        v[1] = id_;
        SafeArrayUnaccessData(a);
        *out = a;
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override
    {
        if (!out) return E_POINTER;
        *out = UiaRect{};
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        if (e) *out = to_screen(s_->hwnd, e->bounds);
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        return check();
    }
    IFACEMETHODIMP SetFocus() override
    {
        const uia_element* e = nullptr;
        if (const HRESULT hr = acting(&e); FAILED(hr)) return hr;
        if (!e) return S_OK;
        if (!e->enabled) return UIA_E_ELEMENTNOTENABLED;
        if (!e->focusable) return UIA_E_INVALIDOPERATION;
        s_->owner->uia_focus(id_);
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (const HRESULT hr = check(); FAILED(hr)) return hr;
        if (node* root = s_->get(0))
        {
            *out = static_cast<IRawElementProviderFragmentRoot*>(root);
            root->AddRef();
        }
        return S_OK;
    }

    // IRawElementProviderFragmentRoot (the root only)
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (const HRESULT hr = check(); FAILED(hr)) return hr;
        POINT p{LONG(x), LONG(y)};
        MapWindowPoints(nullptr, s_->hwnd, &p, 1);
        // The deepest element under the point; of overlapping siblings, the
        // one listed last, as drawn last.
        int at = 0;
        for (bool deeper = true; deeper;)
        {
            deeper = false;
            int hit = 0;
            for (const uia_element& c : s_->elements)
                if (c.parent == at && !c.offscreen && PtInRect(&c.bounds, p)) hit = c.id;
            if (hit)
            {
                at = hit;
                deeper = true;
            }
        }
        if (at == 0) return S_OK;  // the window itself: UIA's
        if (node* n = s_->get(at))
        {
            *out = static_cast<IRawElementProviderFragment*>(n);
            n->AddRef();
        }
        return S_OK;
    }
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (const HRESULT hr = check(); FAILED(hr)) return hr;
        for (const uia_element& c : s_->elements)
            if (c.focused)
            {
                if (node* n = s_->get(c.id))
                {
                    *out = static_cast<IRawElementProviderFragment*>(n);
                    n->AddRef();
                }
                break;
            }
        return S_OK;
    }

    // IInvokeProvider: posted, as Invoke mustn't wait for what it does (a
    // tab closed, a dialog shown). The owner hears it from handle_message,
    // if the element is still there, enabled and invokable then.
    IFACEMETHODIMP Invoke() override
    {
        const uia_element* e = nullptr;
        if (const HRESULT hr = acting(&e); FAILED(hr)) return hr;
        if (!e || !(e->patterns & uia_invoke)) return UIA_E_INVALIDOPERATION;
        if (!e->enabled) return UIA_E_ELEMENTNOTENABLED;
        if (!PostMessageW(s_->hwnd, s_->invoke_message, WPARAM(id_), 0)) return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }

    // ISelectionItemProvider
    IFACEMETHODIMP Select() override
    {
        const uia_element* e = nullptr;
        if (const HRESULT hr = selectable(&e); FAILED(hr)) return hr;
        s_->owner->uia_select(id_);
        return S_OK;
    }
    IFACEMETHODIMP AddToSelection() override
    {
        const uia_element* e = nullptr;
        if (const HRESULT hr = selectable(&e); FAILED(hr)) return hr;
        if (e->selected) return S_OK;
        const uia_element* c = s_->container_of(id_);
        if (!c || !c->multiple_selection) return UIA_E_INVALIDOPERATION;
        return s_->owner->uia_add_to_selection(id_) ? S_OK : UIA_E_INVALIDOPERATION;
    }
    IFACEMETHODIMP RemoveFromSelection() override
    {
        const uia_element* e = nullptr;
        if (const HRESULT hr = selectable(&e); FAILED(hr)) return hr;
        if (!e->selected) return S_OK;
        if (const uia_element* c = s_->container_of(id_); c && c->selection_required)
        {
            int selected = 0;
            for (const uia_element& o : s_->elements)
                if (o.selected && (o.patterns & uia_selection_item) && s_->container_of(o.id) == c) ++selected;
            if (selected <= 1) return UIA_E_INVALIDOPERATION;
        }
        return s_->owner->uia_remove_from_selection(id_) ? S_OK : UIA_E_INVALIDOPERATION;
    }
    IFACEMETHODIMP get_IsSelected(BOOL* out) override
    {
        if (!out) return E_POINTER;
        *out = FALSE;
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        *out = e && e->selected;
        return S_OK;
    }
    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (const HRESULT hr = check(); FAILED(hr)) return hr;
        if (const uia_element* c = s_->container_of(id_))
            if (node* n = s_->get(c->id))
            {
                *out = static_cast<IRawElementProviderSimple*>(n);
                n->AddRef();
            }
        return S_OK;
    }

    // ISelectionProvider: the selected items whose nearest container this is.
    IFACEMETHODIMP GetSelection(SAFEARRAY** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (const HRESULT hr = check(); FAILED(hr)) return hr;
        std::vector<node*> picked;
        for (const uia_element& o : s_->elements)
            if (o.selected && (o.patterns & uia_selection_item) && s_->container_of(o.id) &&
                s_->container_of(o.id)->id == id_)
                if (node* n = s_->get(o.id)) picked.push_back(n);
        SAFEARRAY* a = SafeArrayCreateVector(VT_UNKNOWN, 0, ULONG(picked.size()));
        if (!a) return E_OUTOFMEMORY;
        for (LONG i = 0; i < LONG(picked.size()); ++i)
        {
            // SafeArrayPutElement takes its own reference.
            IUnknown* u = static_cast<IRawElementProviderSimple*>(picked[size_t(i)]);
            SafeArrayPutElement(a, &i, u);
        }
        *out = a;
        return S_OK;
    }
    IFACEMETHODIMP get_CanSelectMultiple(BOOL* out) override
    {
        if (!out) return E_POINTER;
        *out = FALSE;
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        *out = e && e->multiple_selection;
        return S_OK;
    }
    IFACEMETHODIMP get_IsSelectionRequired(BOOL* out) override
    {
        if (!out) return E_POINTER;
        *out = FALSE;
        const uia_element* e = nullptr;
        if (const HRESULT hr = check(&e); FAILED(hr)) return hr;
        *out = e && e->selection_required;
        return S_OK;
    }

private:
    ~node() = default;

    // Whether this node may be read now, with its element (null for the
    // root). The thread first: nothing of the state is read off it.
    HRESULT check(const uia_element** e = nullptr) const
    {
        if (GetCurrentThreadId() != s_->thread) return RPC_E_WRONG_THREAD;
        if (!s_->connected()) return UIA_E_ELEMENTNOTAVAILABLE;
        const uia_element* found = id_ != 0 ? s_->find(id_) : nullptr;
        if (id_ != 0 && !found) return UIA_E_ELEMENTNOTAVAILABLE;
        if (e) *e = found;
        return S_OK;
    }
    // And acted on, which it can't be while disconnecting.
    HRESULT acting(const uia_element** e) const
    {
        if (const HRESULT hr = check(e); FAILED(hr)) return hr;
        return s_->closing ? UIA_E_ELEMENTNOTAVAILABLE : S_OK;
    }
    HRESULT selectable(const uia_element** e) const
    {
        if (const HRESULT hr = acting(e); FAILED(hr)) return hr;
        if (!*e || !((*e)->patterns & uia_selection_item)) return UIA_E_INVALIDOPERATION;
        if (!(*e)->enabled) return UIA_E_ELEMENTNOTENABLED;
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    std::shared_ptr<uia_provider::state> s_;
    int id_;
};

}  // namespace


node* uia_provider::state::get(int id)
{
    // None new while disconnecting: what's handed out now would outlive it.
    if (!connected() || closing) return nullptr;
    if (id != 0 && !find(id)) return nullptr;
    if (auto it = nodes.find(id); it != nodes.end()) return it->second;
    // Made with one reference, which is the state's.
    node* n = new node(shared_from_this(), id);
    nodes.emplace(id, n);
    return n;
}

void uia_provider::state::release(int id)
{
    auto it = nodes.find(id);
    if (it == nodes.end()) return;
    node* n = it->second;
    nodes.erase(it);
    // UIA lets go of what it holds of the node, and refuses its clients
    // from then on; the node itself goes when the last of them releases it.
    UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(n));
    static_cast<IRawElementProviderSimple*>(n)->Release();
}

void uia_provider::state::disconnect()
{
    if (!connected() || closing) return;
    closing = true;
    // UIA forgets the window's root first, so no new client reaches it.
    if (IsWindow(hwnd)) UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
    std::vector<int> ids;
    for (const auto& [id, _] : nodes) ids.push_back(id);
    for (const int id : ids) release(id);
    owner = nullptr;
    elements.clear();
    closing = false;
}

uia_provider::uia_provider(HWND hwnd, uia_owner& owner) : state_(std::make_shared<state>())
{
    state_->hwnd = hwnd;
    state_->thread = GetWindowThreadProcessId(hwnd, nullptr);
    state_->invoke_message = RegisterWindowMessageW(L"rapport-uia-invoke");
    state_->owner = &owner;
}

uia_provider::~uia_provider()
{
    state_->disconnect();
}

bool uia_provider::handle_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    state& s = *state_;
    if (msg == WM_GETOBJECT)
    {
        // UIA asks with UiaRootObjectId; MSAA's own ids are the window's.
        if (!s.connected() || s.closing || static_cast<long>(lp) != static_cast<long>(UiaRootObjectId)) return false;
        node* root = s.get(0);
        if (!root) return false;
        result = UiaReturnRawElementProvider(s.hwnd, wp, lp, static_cast<IRawElementProviderSimple*>(root));
        return true;
    }
    if (msg == WM_DESTROY)
    {
        s.disconnect();
        return false;
    }
    if (msg == s.invoke_message && s.invoke_message != 0)
    {
        result = 0;
        const int id = int(wp);
        const uia_element* e = s.connected() && !s.closing ? s.find(id) : nullptr;
        if (!e || !(e->patterns & uia_invoke) || !e->enabled) return true;
        // Raised before the owner acts: what it does may remove the element.
        raise_event(id, UIA_Invoke_InvokedEventId);
        if (s.owner) s.owner->uia_invoke(id);
        return true;
    }
    return false;
}

void uia_provider::set_elements(std::vector<uia_element> elements)
{
    state& s = *state_;
    if (!s.connected()) return;
    s.elements = std::move(elements);
    std::vector<int> gone;
    for (const auto& [id, _] : s.nodes)
        if (id != 0 && !s.find(id)) gone.push_back(id);
    for (const int id : gone) s.release(id);
}

uia_element* uia_provider::element(int id)
{
    state& s = *state_;
    if (!s.connected()) return nullptr;
    for (uia_element& e : s.elements)
        if (e.id == id) return &e;
    return nullptr;
}

const std::vector<uia_element>& uia_provider::elements() const
{
    return state_->elements;
}

void uia_provider::raise_focus_changed(int id)
{
    raise_event(id, UIA_AutomationFocusChangedEventId);
}

void uia_provider::raise_event(int id, EVENTID event)
{
    if (!state_->connected() || !UiaClientsAreListening()) return;
    if (node* n = state_->get(id)) UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(n), event);
}

void uia_provider::raise_property_changed(int id, PROPERTYID property, const VARIANT& before, const VARIANT& after)
{
    if (!state_->connected() || !UiaClientsAreListening()) return;
    if (node* n = state_->get(id))
        UiaRaiseAutomationPropertyChangedEvent(static_cast<IRawElementProviderSimple*>(n), property, before, after);
}

void uia_provider::raise_structure_changed(StructureChangeType change, int id)
{
    if (!state_->connected() || !UiaClientsAreListening()) return;
    node* n = state_->get(id);
    if (!n) return;
    // The root's runtime id is the window's, which UIA knows: none given.
    int runtime_id[2] = {UiaAppendRuntimeId, id};
    UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(n), change, id ? runtime_id : nullptr,
                                  id ? 2 : 0);
}

void uia_provider::raise_child_removed(int parent, int child)
{
    if (!state_->connected() || !UiaClientsAreListening()) return;
    node* n = state_->get(parent);
    if (!n) return;
    int runtime_id[2] = {UiaAppendRuntimeId, child};
    UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(n), StructureChangeType_ChildRemoved,
                                  runtime_id, 2);
}

IRawElementProviderSimple* uia_provider::root() const
{
    return static_cast<IRawElementProviderSimple*>(state_->get(0));
}

}  // namespace rap::win32
