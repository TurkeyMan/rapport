// UI Automation for what a control draws itself: a small tree of elements
// that aren't windows (a caption's tabs and their buttons, a canvas's
// shapes), served to screen readers and other UIA clients as fragments of
// the control's window.
//
// The control (the owner) keeps a uia_provider beside its HWND, describes
// its drawn elements to it, forwards its window's messages to it, and raises
// the events UIA clients expect when what it draws changes. Nothing here
// knows what the elements are: their shape is the owner's.
//
// Threading: everything here, owner callbacks included, runs on the thread
// that made the window, which must be in a single-threaded apartment (the
// backend's UI thread is: OleInitialize). The elements say so to UIA
// (ProviderOptions_UseComThreading), so a client in another process or
// another apartment is marshalled onto that thread by COM, through the
// window's message loop. A call that arrives on any other thread anyway is
// refused (RPC_E_WRONG_THREAD) rather than reading the owner's state
// unsynchronised.
//
// Lifetime: the window's WM_DESTROY (through handle_message) or the
// provider's destruction, whichever is first, disconnects every element
// handed out. A client still holding one gets UIA_E_ELEMENTNOTAVAILABLE from
// then on, and the owner is never called again.
#pragma once

#include <windows.h>
// COM and OLE Automation first: WIN32_LEAN_AND_MEAN leaves them out of
// windows.h, and UIA's headers need them.
#include <ole2.h>
#include <uiautomation.h>

#include <memory>
#include <string>
#include <vector>

namespace rap::win32
{

// Which control patterns an element supports (bit flags).
enum uia_patterns : unsigned
{
    uia_invoke = 1u << 0,          // IInvokeProvider: the owner's uia_invoke
    uia_selection_item = 1u << 1,  // ISelectionItemProvider: in the nearest ancestor with uia_selection
    uia_selection = 1u << 2,       // ISelectionProvider: a container of selection items
};

// One drawn element. Ids are the owner's, unique within the provider and
// above zero: 0 is the window itself, the fragment root, which UIA describes
// from the HWND. Elements are listed in reading order; an element's children
// are those naming it as their parent, in the order they're listed.
struct uia_element
{
    int id = 0;
    int parent = 0;
    CONTROLTYPEID control_type = UIA_CustomControlTypeId;
    std::wstring name;
    std::wstring automation_id;  // stable for test tools; none when empty
    RECT bounds{};               // client px of the window; converted to screen for UIA
    unsigned patterns = 0;
    bool enabled = true;
    bool focusable = false;
    bool focused = false;    // has the keyboard focus (at most one should)
    bool selected = false;   // for uia_selection_item
    bool offscreen = false;  // scrolled or clipped out of sight
    // For uia_selection: more than one item may be selected, and whether
    // there must always be one.
    bool multiple_selection = false;
    bool selection_required = true;
};

// What the elements do, called on the window's thread. Invoke arrives
// posted (UIA's Invoke must return without waiting for what it does), the
// others while the client waits.
class uia_owner
{
public:
    virtual void uia_invoke(int id) = 0;
    // SelectionItem.Select: make `id` the only selected item of its container.
    virtual void uia_select(int id) = 0;
    // AddToSelection and RemoveFromSelection where the container allows
    // them: false refuses (UIA_E_INVALIDOPERATION). Adding is asked only of
    // a container with multiple_selection; removing, never of the last
    // selected item of one with selection_required.
    virtual bool uia_add_to_selection(int id) { (void)id; return false; }
    virtual bool uia_remove_from_selection(int id) { (void)id; return false; }
    // SetFocus on a focusable element; UIA has already focused the window.
    virtual void uia_focus(int id) { (void)id; }

protected:
    ~uia_owner() = default;
};

class uia_provider
{
public:
    uia_provider(HWND hwnd, uia_owner& owner);
    ~uia_provider();  // disconnects, as WM_DESTROY does
    uia_provider(const uia_provider&) = delete;
    uia_provider& operator=(const uia_provider&) = delete;

    // The window's WM_GETOBJECT (UIA's root object) and WM_DESTROY, and the
    // posted invokes: true when it was one of those, with what to return in
    // `result`. A WM_DESTROY is the owner's too: it returns false, having
    // disconnected.
    bool handle_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);

    // Replaces the tree. An element whose id is gone is disconnected from any
    // client holding it; one whose id stays is the same element to clients.
    // Raise structure (and property) events for what changed.
    void set_elements(std::vector<uia_element> elements);
    // The element by id, to change in place (then raise what changed); null
    // when there's none or the window is gone.
    uia_element* element(int id);
    const std::vector<uia_element>& elements() const;

    // Events, each only when a client is listening. `id` 0 is the window.
    void raise_focus_changed(int id);
    void raise_event(int id, EVENTID event);  // e.g. UIA_SelectionItem_ElementSelectedEventId
    void raise_property_changed(int id, PROPERTYID property, const VARIANT& before, const VARIANT& after);
    // ChildAdded on the new child; ChildrenInvalidated, ChildrenBulkAdded,
    // ChildrenBulkRemoved and ChildrenReordered on the parent whose children
    // changed. ChildRemoved has its own, as the child is gone by then.
    void raise_structure_changed(StructureChangeType change, int id);
    void raise_child_removed(int parent, int child);

    // The fragment root, as WM_GETOBJECT hands it to UIA (not add-refed);
    // null once disconnected. For tests.
    IRawElementProviderSimple* root() const;

    struct state;

private:
    std::shared_ptr<state> state_;
};

}  // namespace rap::win32
