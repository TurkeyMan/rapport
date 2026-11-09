// A Win32 tree's placeholder row: the keyboard goes past it rather than
// getting stuck on it, and nothing the app does reports a selection change.
//   rapport-win32-tree-placeholder-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <string>

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

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
