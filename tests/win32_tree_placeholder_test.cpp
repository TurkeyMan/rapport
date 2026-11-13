// A Win32 tree's placeholder row: the keyboard goes past it rather than
// getting stuck on it, and nothing the app does reports a selection change.
//   rapport-win32-tree-placeholder-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

HWND find_tree()
{
    HWND found = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM out) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND c, LPARAM o) -> BOOL
                {
                    wchar_t cls[32] = {};
                    GetClassNameW(c, cls, 32);
                    if (wcscmp(cls, WC_TREEVIEWW)) return TRUE;
                    *reinterpret_cast<HWND*>(o) = c;
                    return FALSE;
                },
                out);
            return *reinterpret_cast<HWND*>(out) == nullptr;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

void press(HWND tree, WPARAM key)
{
    // A tree reads modifiers from this thread's key state, which can pick up
    // keys really held on the machine: Ctrl turns Up and Down into scrolling.
    const BYTE keys[256] = {};
    SetKeyboardState(const_cast<BYTE*>(keys));
    SendMessageW(tree, WM_KEYDOWN, key, 1);
    SendMessageW(tree, WM_KEYUP, key, 0xC0000001);
}

void press_with_ctrl(HWND tree, WPARAM key)
{
    BYTE keys[256] = {};
    keys[VK_CONTROL] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(tree, WM_KEYDOWN, key, 1);
    SendMessageW(tree, WM_KEYUP, key, 0xC0000001);
    keys[VK_CONTROL] = 0;
    SetKeyboardState(keys);
}

DWORD windows_build()
{
    using get_version_fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto get_version = reinterpret_cast<get_version_fn>(GetProcAddress(GetModuleHandleW(L"ntdll"), "RtlGetVersion"));
    OSVERSIONINFOW info{sizeof info};
    return get_version && get_version(&info) == 0 ? info.dwBuildNumber : 0;
}

// Where the tree stands, for a failed paging check: what's selected, how
// many rows show, which is first, and comctl32's version.
void where(HWND tree, rap::tree* t)
{
    wchar_t first[64] = {};
    TVITEMW it{TVIF_TEXT};
    it.hItem = TreeView_GetFirstVisible(tree);
    it.pszText = first;
    it.cchTextMax = 64;
    if (it.hItem) TreeView_GetItem(tree, &it);
    RECT client{};
    GetClientRect(tree, &client);
    std::printf("     selected %s, %d rows visible of height %d in %ld px, first visible %ls, comctl32 %d\n",
                t->selected().value_or("(none)").c_str(), int(SendMessageW(tree, TVM_GETVISIBLECOUNT, 0, 0)),
                int(SendMessageW(tree, TVM_GETITEMHEIGHT, 0, 0)), client.bottom, first,
                int(SendMessageW(tree, CCM_GETVERSION, 0, 0)));
}

rap::tree_item item(std::string id, bool lazy)
{
    rap::tree_item t;
    t.label = id;
    t.id = std::move(id);
    t.has_children = lazy;
    return t;
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    rap::tree* t = nullptr;
    int changes = 0;
    using namespace rap::layout;
    auto win = (*ui)->create(window{"t", {tree{{item("a", true), item("b", false), item("c", true)}}
                                              .bind(t)
                                              .on_selection_changed([&] { ++changes; })
                                              .on_expanding([](const std::string&) {})}});
    win->show();
    HWND tree_hwnd = find_tree();
    expect(tree_hwnd != nullptr, "the tree is there");

    t->set_expanded("a", true);
    t->select("a");
    changes = 0;

    press(tree_hwnd, VK_DOWN);
    expect(t->selected() == "b", "Down goes past a placeholder");
    press(tree_hwnd, VK_UP);
    expect(t->selected() == "a", "Up goes past it too");
    press(tree_hwnd, VK_RIGHT);
    expect(t->selected() == "a", "Right on an open node with only a placeholder stays");
    press(tree_hwnd, VK_LEFT);
    expect(t->selected() == "a", "Left closes it");
    press(tree_hwnd, VK_RIGHT);
    expect(t->selected() == "a", "Right opens it again");

    t->set_expanded("c", true);
    t->select("a");
    press(tree_hwnd, VK_END);
    expect(t->selected() == "c", "End stops short of a last placeholder");
    press(tree_hwnd, VK_DOWN);
    expect(t->selected() == "c", "Down at the end stays, without getting stuck");
    press(tree_hwnd, VK_UP);
    expect(t->selected() == "b", "and Up still moves");

    press(tree_hwnd, 'L');  // type-ahead towards "Loading…"
    expect(t->selected().has_value(), "type-ahead can't select a placeholder");

    t->select("a");
    changes = 0;
    t->set_children("a", {item("x", false)});
    t->set_children("c", {});
    t->update_item("b", "bee", {}, true);
    expect(changes == 0, "set_children and update_item report no selection change");
    press(tree_hwnd, VK_DOWN);
    expect(t->selected() == "x", "the answer is there to step onto");

    // Ctrl with the arrows scrolls and leaves the selection where it was.
    {
        t->set_items({item("p", true), item("q", false)});
        t->set_expanded("p", true);
        t->select("p");
        press_with_ctrl(tree_hwnd, VK_DOWN);
        expect(t->selected() == "p", "Ctrl+Down over a placeholder doesn't select");
        press_with_ctrl(tree_hwnd, VK_END);
        expect(t->selected() == "p", "and nor does Ctrl+End");
    }

    // Boxes that first arrive in a lazy answer, nested too.
    {
        t->set_items({item("r", true)});
        rap::tree_item parent = item("s", false);
        rap::tree_item boxed = item("t", false);
        boxed.check = rap::check_state::unchecked;
        parent.children = {boxed};
        t->set_children("r", {parent});
        expect(TreeView_GetImageList(tree_hwnd, TVSIL_STATE) != nullptr, "a nested box in an answer has its images");
    }

    // The paging keys, landing exactly on a placeholder.
    {
        std::vector<rap::tree_item> items;
        for (int i = 0; i < 200; ++i) items.push_back(item("i" + std::to_string(i), false));
        t->set_items(items);
        const int visible = int(SendMessageW(tree_hwnd, TVM_GETVISIBLECOUNT, 0, 0));
        items.clear();
        for (int i = 0; i < visible - 2; ++i) items.push_back(item("i" + std::to_string(i), false));
        items.push_back(item("lazy", true));
        items.push_back(item("after", false));
        for (int i = 0; i < 100; ++i) items.push_back(item("tail" + std::to_string(i), false));
        t->set_items(items);
        t->set_expanded("lazy", true);
        t->select("i0");
        press(tree_hwnd, VK_NEXT);
        expect(t->selected() == "after", "PageDown landing on a placeholder goes on to the next row");
        if (t->selected() != "after") where(tree_hwnd, t);

        // PageUp moves by a page less one, so this row lands on the placeholder.
        t->select("tail" + std::to_string(visible - 3));
        press(tree_hwnd, VK_PRIOR);
        expect(t->selected() == "lazy", "PageUp landing on one goes on to the row before");
        if (t->selected() != "lazy") where(tree_hwnd, t);

        // The placeholder the very last row: nothing beyond, so back the other way.
        items.resize(size_t(visible - 1));
        t->set_items(items);
        t->set_expanded("lazy", true);
        t->select("i0");
        press(tree_hwnd, VK_NEXT);
        expect(t->selected() == "lazy", "PageDown onto a last placeholder stops on the row before it");
        press(tree_hwnd, VK_NEXT);
        expect(t->selected() == "lazy", "and pressing it again isn't stuck anywhere new");
    }

    const LONG plain_style = GetWindowLongW(tree_hwnd, GWL_STYLE);
    expect(!(plain_style & TVS_NOHSCROLL), "a plain tree keeps its horizontal scroll bar");
    win->close();
    win.reset();

    // A navigator: no horizontal scroll bar from Windows 11, tooltips on.
    {
        rap::tree* nav = nullptr;
        auto nav_win = (*ui)->create(window{"n", {tree{{item("a", false)}}.navigator().bind(nav)}});
        nav_win->show();
        HWND nav_hwnd = find_tree();
        const LONG style = GetWindowLongW(nav_hwnd, GWL_STYLE);
        expect(((style & TVS_NOHSCROLL) != 0) == (windows_build() >= 22000), "a navigator's scroll bar follows the build");
        expect(!(style & TVS_NOTOOLTIPS), "and it keeps its tooltips, for names that clip");
        nav_win->close();
    }

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
