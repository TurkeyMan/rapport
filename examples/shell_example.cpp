// A file-manager extension: a submenu that opens rapport-demo, built with
// rapport_add_shell_extension().
#include <rapport/shell.h>

namespace
{

class example_provider : public rap::shell::provider
{
public:
    std::vector<rap::shell::menu_item> menu(const rap::shell::selection& sel) override
    {
        const std::string what = sel.background ? "this folder"
                                 : sel.paths.size() == 1
                                     ? sel.paths.front().filename().string()
                                     : std::to_string(sel.paths.size()) + " items";
        rap::shell::menu_item sub{.label = "rapport example"};
        sub.submenu = {
            {.id = "demo", .label = "Open the &demo"},
            {.label = "Selected: " + what, .enabled = false},
        };
        if (sel.extended)
        {
            sub.submenu.push_back(rap::shell::menu_item::separator());
            sub.submenu.push_back({.id = "extended", .label = "Only with Shift held"});
        }
        return {rap::shell::menu_item::separator(), sub, rap::shell::menu_item::separator()};
    }

    void invoke(std::string_view id, const rap::shell::selection& sel) override
    {
        if (id != "demo") return;
#ifdef _WIN32
        const char* demo = "rapport-demo.exe";
#else
        const char* demo = "rapport-demo";
#endif
        rap::shell::launch(rap::shell::module_dir() / demo, {}, sel.folder);
    }
};

}  // namespace

RAPPORT_SHELL_PROVIDER(example_provider)
