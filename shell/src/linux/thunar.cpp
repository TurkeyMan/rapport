// The provider as a Thunar extension (thunarx-3), in <libdir>/thunarx-3.
// Thunar asks for a folder's items for its background and for folders in
// the side pane, which both act on that folder.
#include "gobject_menu.h"

// thunarx-provider-plugin.h lacks the C++ guards the other headers have.
extern "C"
{
#include <thunarx/thunarx.h>
}

namespace
{

using namespace rap::shell::adapter;

struct fm
{
    using file_info = ThunarxFileInfo;
    using item = ThunarxMenuItem;
    using menu = ThunarxMenu;
    static constexpr bool separators = false;
    static GFile* location(file_info* f) { return thunarx_file_info_get_location(f); }
    static item* new_item(const char* name, const char* label, const char* icon)
    {
        return thunarx_menu_item_new(name, label, "", icon);
    }
    static menu* new_menu() { return thunarx_menu_new(); }
    static void append(menu* m, item* i) { thunarx_menu_append_item(m, i); }
    static void set_submenu(item* i, menu* m)
    {
        thunarx_menu_item_set_menu(i, m);
        g_object_unref(m);
    }
};

GList* file_items(ThunarxMenuProvider*, GtkWidget*, GList* files)
{
    return menu_items<fm>(files_selection<fm>(files));
}

GList* folder_items(ThunarxMenuProvider*, GtkWidget*, ThunarxFileInfo* folder)
{
    return menu_items<fm>(background_selection<fm>(folder));
}

void menu_provider_init(gpointer iface, gpointer)
{
    auto* i = static_cast<ThunarxMenuProviderIface*>(iface);
    i->get_file_menu_items = file_items;
    i->get_folder_menu_items = folder_items;
}

GType provider_type = 0;

}  // namespace

extern "C" G_MODULE_EXPORT void thunar_extension_initialize(ThunarxProviderPlugin* plugin)
{
    if (thunarx_check_version(THUNARX_MAJOR_VERSION, THUNARX_MINOR_VERSION, THUNARX_MICRO_VERSION))
        return;
    // Unloading would leave the type registered over code that's gone.
    thunarx_provider_plugin_set_resident(plugin, TRUE);
    static const GTypeInfo info{.class_size = sizeof(GObjectClass),
                                .instance_size = sizeof(GObject)};
    provider_type = thunarx_provider_plugin_register_type(
        plugin, G_TYPE_OBJECT, type_name("Thunar"), &info, GTypeFlags(0));
    static const GInterfaceInfo menu{.interface_init = menu_provider_init};
    thunarx_provider_plugin_add_interface(plugin, provider_type, THUNARX_TYPE_MENU_PROVIDER, &menu);
}

extern "C" G_MODULE_EXPORT void thunar_extension_shutdown() {}

extern "C" G_MODULE_EXPORT void thunar_extension_list_types(const GType** types, gint* count)
{
    *types = &provider_type;
    *count = provider_type ? 1 : 0;
}
