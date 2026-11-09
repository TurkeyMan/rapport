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

        // PageUp moves by a page less one, so this row lands on the placeholder.
        t->select("tail" + std::to_string(visible - 3));
        press(tree_hwnd, VK_PRIOR);
        expect(t->selected() == "lazy", "PageUp landing on one goes on to the row before");

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

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
