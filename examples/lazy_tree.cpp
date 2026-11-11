// rapport-lazy-tree: a tree that loads its children when they're opened, for
// checking how it looks and behaves:
//   - Documents, Music and Empty have chevrons with nothing loaded.
//   - Opening Documents shows "Loading…", then its folders, after a second.
//   - Music is open from the start and never answered: "Loading…" stays, can't
//     be selected, and the arrow keys step past it.
//   - Opening Empty shows "Loading…", then the chevron goes.
//   - A line above Empty sets it and Loaded apart (tree_item::separator_before).
//   rapport-lazy-tree --snapshot file.png    saves the window and exits
#include <rapport/rapport.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

rap::tree_item folder(std::string name, bool lazy, bool expanded = false, bool line = false)
{
    rap::tree_item t;
    t.separator_before = line;
    t.label = name;
    t.id = std::move(name);
    t.has_children = lazy;
    t.expanded = expanded;
    t.icon = rap::icon::for_file("x/");
    return t;
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    const std::string snapshot =
        argc > 2 && std::string_view(argv[1]) == "--snapshot" ? argv[2] : std::string();

    rap::tree* tree = nullptr;
    std::vector<rap::subscription> answers;
    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "Lazy tree",
        {rap::layout::tree{{folder("Documents", true), folder("Music", true, true), folder("Empty", true, false, true),
                            folder("Loaded", false)}}
             .bind(tree)
             .on_expanding(
                 [&](const std::string& id)
                 {
                     if (id == "Music") return;  // never answered
                     answers.push_back((*ui)->timer(
                         1000, false,
                         [&, id]
                         {
                             if (id == "Empty")
                                 tree->set_children(id, {});
                             else
                                 tree->set_children(id, {folder(id + "/one", false), folder(id + "/two", true)});
                         }));
                 })
             .stretch()}}
                                 .size(40_ch, 16_lines));
    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();

    rap::subscription snap;
    if (!snapshot.empty())
        snap = (*ui)->timer(600, false,
                            [&]
                            {
                                (*ui)->save_snapshot(snapshot);
                                (*ui)->quit();
                            });
    return (*ui)->run();
}
