// The GObject file managers (Nautilus, Nemo, Caja, Thunar) share one menu
// shape: items with a unique name, a label, a tip and an icon, an "activate"
// signal and a "sensitive" property, and submenus. Each adapter describes
// its own types to the code here with an Fm:
//
//   struct Fm
//   {
//       using file_info = NautilusFileInfo;
//       using item = NautilusMenuItem;
//       using menu = NautilusMenu;
//       static constexpr bool separators = false;
//       static GFile* location(file_info*);      // a new reference
//       static item* new_item(const char* name, const char* label, const char* icon);
//                                                // (a tip of "", never null: Nemo
//                                                // and Caja refuse a null one)
//       static item* new_separator(const char* name);  // where separators
//       static menu* new_menu();
//       static void append(menu*, item*);
//       static void set_submenu(item*, menu*);   // takes the menu
//   };
#pragma once

#include "adapter.h"

#include <gio/gio.h>

#include <optional>
#include <string>

namespace rap::shell::adapter
{

inline std::optional<std::filesystem::path> local_path(GFile* file)
{
    if (!file) return std::nullopt;
    char* p = g_file_get_path(file);
    g_object_unref(file);
    if (!p) return std::nullopt;  // not on a local filesystem
    std::filesystem::path out(p);
    g_free(p);
    return out;
}

// What was right-clicked, from the file manager's list of file infos; none
// if any of them isn't a local file.
template <class Fm>
std::optional<selection> files_selection(GList* files)
{
    selection sel;
    for (GList* l = files; l; l = l->next)
    {
        auto p = local_path(Fm::location(static_cast<typename Fm::file_info*>(l->data)));
        if (!p) return std::nullopt;
        sel.paths.push_back(std::move(*p));
    }
    if (sel.paths.empty()) return std::nullopt;
    sel.folder = sel.paths.front().parent_path();
    sel.extended = shift_held();
    return sel;
}

template <class Fm>
std::optional<selection> background_selection(typename Fm::file_info* folder)
{
    auto p = local_path(folder ? Fm::location(folder) : nullptr);
    if (!p) return std::nullopt;
    selection sel;
    sel.folder = std::move(*p);
    sel.background = true;
    sel.extended = shift_held();
    return sel;
}

namespace detail
{

inline const char request_key[] = "rapport-shell-request";
inline const char id_key[] = "rapport-shell-id";

inline void on_activate(GObject* item, gpointer)
{
    auto* r = static_cast<std::shared_ptr<request>*>(g_object_get_data(item, request_key));
    auto* id = static_cast<const char*>(g_object_get_data(item, id_key));
    if (r && *r && id) choose(**r, id);
}

template <class Fm>
typename Fm::menu* to_menu(const std::vector<menu_item>& items, const std::shared_ptr<request>& r,
                           const std::string& name);

template <class Fm>
typename Fm::item* to_item(const menu_item& mi, const std::shared_ptr<request>& r,
                           const std::string& name)
{
    if constexpr (Fm::separators)
        if (mi.is_separator()) return Fm::new_separator(name.c_str());
    const std::string label = gtk_label(mi.label), icon = icon_spec(mi.icon);
    typename Fm::item* item =
        Fm::new_item(name.c_str(), label.c_str(), icon.empty() ? nullptr : icon.c_str());
    if (!item) return nullptr;
    if (!mi.enabled) g_object_set(item, "sensitive", FALSE, nullptr);
    if (!mi.submenu.empty())
        Fm::set_submenu(item, to_menu<Fm>(mi.submenu, r, name));
    else
    {
        // The item holds the request: the provider lives as long as the menu.
        g_object_set_data_full(
            G_OBJECT(item), request_key, new std::shared_ptr<request>(r),
            +[](gpointer p) { delete static_cast<std::shared_ptr<request>*>(p); });
        g_object_set_data_full(G_OBJECT(item), id_key, g_strdup(mi.id.c_str()), g_free);
        g_signal_connect(item, "activate", G_CALLBACK(on_activate), nullptr);
    }
    return item;
}

template <class Fm>
typename Fm::menu* to_menu(const std::vector<menu_item>& items, const std::shared_ptr<request>& r,
                           const std::string& name)
{
    typename Fm::menu* menu = Fm::new_menu();
    for (size_t i = 0; i < items.size(); ++i)
    {
        typename Fm::item* item = to_item<Fm>(items[i], r, name + "." + std::to_string(i));
        if (!item) continue;
        Fm::append(menu, item);
        g_object_unref(item);
    }
    return menu;
}

}  // namespace detail

// The file manager's list of menu items for the selection, owned by the
// caller, as get_file_items and get_background_items return them.
template <class Fm>
GList* menu_items(std::optional<selection> sel) noexcept
{
    if (!sel) return nullptr;
    try
    {
        std::vector<menu_item> items;
        auto r = ask(std::move(*sel), items, Fm::separators);
        if (!r) return nullptr;
        // Items need names unique across every extension.
        const std::string name = std::string(this_extension.id) + ".menu";
        GList* out = nullptr;
        for (size_t i = 0; i < items.size(); ++i)
            if (auto* item = detail::to_item<Fm>(items[i], r, name + "." + std::to_string(i)))
                out = g_list_append(out, item);
        return out;
    }
    catch (...)
    {
        return nullptr;
    }
}

// A GType name of the extension's own, for one file manager's provider:
// "RapportShell_org_example_App_Nautilus".
inline const char* type_name(const char* fm)
{
    static std::string name = [&]
    {
        std::string n = "RapportShell_";
        for (const char* c = this_extension.id; *c; ++c) n += g_ascii_isalnum(*c) ? *c : '_';
        return n + "_" + fm;
    }();
    return name.c_str();
}

}  // namespace rap::shell::adapter
