// <extension>-probe: a provider's menu for some paths, printed, without a
// file manager. Built by rapport_add_shell_extension() for testing.
//   probe [--background] [--extended] [--invoke ID] PATH...
// With --background, the one PATH is the folder clicked in.
#include <rapport/shell.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace
{

void print(const std::vector<rap::shell::menu_item>& items, int depth)
{
    for (const rap::shell::menu_item& item : items)
    {
        if (item.is_separator())
        {
            std::printf("%*s----\n", depth * 2, "");
            continue;
        }
        std::printf("%*s%s%s", depth * 2, "", item.label.c_str(),
                    item.enabled ? "" : "  (disabled)");
        if (!item.id.empty()) std::printf("  [%s]", item.id.c_str());
        if (!item.icon.empty())
        {
            const bool found = std::filesystem::exists(rap::shell::module_dir() / item.icon);
            std::printf("  icon %s%s", item.icon.c_str(), found ? "" : " MISSING");
        }
        std::printf("\n");
        print(item.submenu, depth + 1);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    rap::shell::selection sel;
    std::string invoke;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--background") == 0)
            sel.background = true;
        else if (std::strcmp(argv[i], "--extended") == 0)
            sel.extended = true;
        else if (std::strcmp(argv[i], "--invoke") == 0 && i + 1 < argc)
            invoke = argv[++i];
        else
            sel.paths.push_back(std::filesystem::absolute(argv[i]));
    }
    if (sel.paths.empty())
    {
        std::fprintf(stderr, "usage: %s [--background] [--extended] [--invoke ID] PATH...\n",
                     argv[0]);
        return 2;
    }
    if (sel.background)
    {
        sel.folder = sel.paths.front();
        sel.paths.clear();
    }
    else
        sel.folder = sel.paths.front().parent_path();

    std::printf("%s (%s)\n", rap::shell::this_extension.name, rap::shell::this_extension.id);
    auto provider = rap::shell::make_provider();
    const auto start = std::chrono::steady_clock::now();
    const auto items = provider->menu(sel);
    const auto took = std::chrono::steady_clock::now() - start;
    print(items, 1);
    std::printf("menu() took %.2f ms\n", std::chrono::duration<double, std::milli>(took).count());
    if (!invoke.empty())
    {
        std::printf("invoking %s\n", invoke.c_str());
        provider->invoke(invoke, sel);
    }
    return 0;
}
