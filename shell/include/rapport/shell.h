// rapport shell: an app's entries in the file manager's context menu, and
// (later) badges on its icons, through each platform's own extension point.
//
// An app writes a provider, built with rapport_add_shell_extension() into an
// extension the file manager loads: a COM DLL for Explorer, a plugin for
// Dolphin, Nautilus, Nemo, Caja or Thunar. It runs inside the file manager's
// process, so it stays small: no toolkit, nothing slow on the UI thread, and
// nothing that throws past it (the adapters catch, but a crash takes the file
// manager down). Choosing an item usually launches the app with launch().
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rap::shell
{

// What was right-clicked.
struct selection
{
    std::vector<std::filesystem::path> paths;  // the items; empty on a background
    std::filesystem::path folder;              // the folder they're in, or the background's
    bool background = false;                   // a right-click on a folder's empty space
    bool extended = false;                     // Shift held: room for the rarer entries
};

struct menu_item
{
    std::string id;     // handed back to invoke(); unique within one menu
    std::string label;  // UTF-8; '&' marks the mnemonic; empty for a separator
    std::string icon;   // an image file (.ico, .png), relative to the extension's directory
    bool enabled = true;
    std::vector<menu_item> submenu;

    static menu_item separator() { return {}; }
    bool is_separator() const { return label.empty() && submenu.empty(); }
};

// A badge an extension can put on file icons, declared once.
struct badge_kind
{
    std::string id;
    std::string icon;  // as menu_item::icon
    int priority = 0;  // the higher wins where a platform shows only one
};

// One provider serves one request (a menu, then the chosen item), so it needs
// no locking; anything shared between requests must be thread-safe, since
// file managers ask from more than one thread.
class provider
{
public:
    virtual ~provider() = default;

    virtual std::vector<menu_item> menu(const selection&) = 0;
    virtual void invoke(std::string_view id, const selection&) = 0;

    virtual std::vector<badge_kind> badge_kinds() const { return {}; }
    // A badge_kind id, or empty. Asked for every icon drawn, so answer from a
    // cache, never by doing the work.
    virtual std::string badge(const std::filesystem::path&) { return {}; }
};

// The extension's own directory: where its icons and its app's executable are.
std::filesystem::path module_dir();

// Starts a program and doesn't wait for it; args don't include the program.
bool launch(const std::filesystem::path& program, const std::vector<std::string>& args,
            const std::filesystem::path& working_dir = {});

// Defined by the app, with RAPPORT_SHELL_PROVIDER(type).
std::unique_ptr<provider> make_provider();

#define RAPPORT_SHELL_PROVIDER(type)                                                               \
    std::unique_ptr<::rap::shell::provider> rap::shell::make_provider()                            \
    {                                                                                              \
        return std::make_unique<type>();                                                           \
    }

// The extension's identity, from rapport_add_shell_extension().
struct extension_info
{
    const char* name;           // shown where the platform names extensions
    const char* id;             // reverse-DNS, e.g. org.example.App
    const char* windows_clsid;  // "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
};
extern const extension_info this_extension;

// For the app (or its installer): registers the extension module at `module`
// with the file manager, per user where the platform allows (Windows: HKCU,
// no elevation). On Linux the plugins load only from system directories,
// so there it's the package's job, and these return false.
bool install(const std::filesystem::path& module);
bool uninstall(const std::filesystem::path& module);

}  // namespace rap::shell
