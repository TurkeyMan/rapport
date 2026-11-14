// rapport-document-tabs: a window with a browser's tabs, a folder each, as a
// file manager has them, for trying the tab bar on each backend:
//   - Ctrl+T opens a tab (at the end, made current); Ctrl+W, a middle click
//     or a tab's close button (GTK, Qt) closes one; the last tab closing
//     closes the window, as file managers do.
//   - Ctrl+Tab and Ctrl+Shift+Tab go round them, Ctrl+1..8 to one, Ctrl+9 to
//     the last; all of them with the path field focused too.
//   - Open a dozen: the tabs narrow as they're added, then the row scrolls.
//   - Drag a tab along the row to reorder them (Escape, on Windows, puts it
//     back), or Ctrl+Shift+PgUp and Ctrl+Shift+PgDn; each move's printed.
//   - The tab's folder is shown under the tabs: the app swaps the content,
//     the tabs only say which one is current. "Rename" retitles the current
//     tab; the path field's Enter takes it to that folder.
//   - The row above the content holds one of each kind of control: on
//     Windows 11 it's the toolbar band under the tabs (Explorer's address
//     row), each control there the height of a WinUI text box, in WinUI's
//     look. RAPPORT_DENSITY=compact shows it in WinUI's compact density.
//   rapport-document-tabs --many    opens with fourteen tabs
#include <rapport/rapport.h>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>

namespace
{

constexpr const char* folders[] = {"Documents", "Downloads", "Music",
                                   "Pictures",  "Videos",    "Projects"};

std::string base_name(const std::string& path)
{
    const size_t slash = path.find_last_of('/', path.size() > 1 ? path.size() - 2 : 0);
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (!name.empty() && name.back() == '/') name.pop_back();
    return name.empty() ? path : name;
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
    rap::document_tabs* tabs = nullptr;
    rap::entry* path_field = nullptr;
    rap::label* shown = nullptr;
    std::map<std::string, std::string> paths;  // tab id -> its folder
    int next = 0;
    std::unique_ptr<rap::window> win;

    auto tab_for = [&](const std::string& id)
    {
        const std::string& path = paths[id];
        return rap::document_tab{id, base_name(path), rap::icon::for_file(path), path};
    };
    // The content shows the current tab's folder.
    auto show = [&](const std::string& id)
    {
        if (path_field) path_field->set_text(paths[id]);
        if (shown) shown->set_text("The files in " + paths[id] + " would be listed here.");
    };
    auto open = [&](const std::string& path)
    {
        const std::string id = "tab" + std::to_string(next++);
        paths[id] = path;
        tabs->add(tab_for(id));
        tabs->select(id);
        show(id);
    };

    using namespace rap::layout;
    win = (*ui)->create(window{"Document tabs",
                               {vbox{
                                   hbox{tool_button{rap::icon::named("go-previous"), "Back"},
                                        tool_button{rap::icon::named("go-up"), "Up"},
                                        menu_button{"View"}.on_menu(
                                            []
                                            {
                                                return rap::menu{{.label = "Details"},
                                                                 {.label = "List"}};
                                            }),
                                        separator{},
                                        entry{}
                                            .placeholder("Folder")
                                            .on_activate(
                                                [&]
                                                {
                                                    const std::string id = tabs->current();
                                                    if (id.empty()) return;
                                                    std::string path = path_field->text();
                                                    if (path.empty()) return;
                                                    if (path.back() != '/') path += '/';
                                                    paths[id] = path;
                                                    tabs->update(tab_for(id));
                                                    show(id);
                                                })
                                            .bind(path_field)
                                            .stretch(),
                                        drop_down{{"Name", "Date", "Size"}, 0},
                                        combo{{"*", "*.txt", "*.png"}, "*"},
                                        label{"Hidden"},
                                        check{"Show"},
                                        tool_button{rap::icon::named("view-list"), "Preview"}
                                            .toggle(true),
                                        button{"Refresh"}.icon(rap::icon::named("view-refresh")),
                                        button{"_Rename"}.on_click(
                                            [&]
                                            {
                                                const std::string id = tabs->current();
                                                if (id.empty()) return;
                                                rap::document_tab t = tab_for(id);
                                                t.title += " (renamed)";
                                                tabs->update(t);
                                            })},
                                   label{}.bind(shown).stretch(),
                               }}}
                            .size(70_ch, 20_lines)
                            .base_surface()  // a file manager's, as Expedition's (Windows 11)
                            .tabs(document_tabs{}
                                      .on_select(show)
                                      .on_reorder(
                                          [&](const std::string& id, int index)
                                          { std::printf("%s moved to %d\n", paths[id].c_str(), index); })
                                      .on_new(
                                          [&]
                                          {
                                              open("/home/me/" +
                                                   std::string(folders[next % std::size(folders)]) +
                                                   "/");
                                          })
                                      .on_close(
                                          [&](const std::string& id)
                                          {
                                              paths.erase(id);
                                              tabs->remove(id);
                                              if (!tabs->current().empty())
                                                  show(tabs->current());
                                              else
                                                  (*ui)->quit();
                                          })
                                      .bind(tabs)));
    open("/home/me/");
    open("/home/me/Documents/");
    if (argc > 1 && std::string_view(argv[1]) == "--many")
        for (int i = 0; i < 12; ++i)
            open("/home/me/" + std::string(folders[i % std::size(folders)]) + "/");
    tabs->select("tab0");
    show("tab0");
    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
