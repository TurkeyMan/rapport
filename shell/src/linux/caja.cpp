// The provider as a Caja extension (libcaja-extension), in
// <libdir>/caja/extensions-2.0. Caja lists it under Edit > Preferences >
// Extensions.
#include "gobject_menu.h"

#include <libcaja-extension/caja-extension-types.h>
#include <libcaja-extension/caja-file-info.h>
#include <libcaja-extension/caja-menu-provider.h>

namespace
{

using namespace rap::shell::adapter;

struct fm
{
    using file_info = CajaFileInfo;
    using item = CajaMenuItem;
    using menu = CajaMenu;
    static constexpr bool separators = false;
    static GFile* location(file_info* f) { return caja_file_info_get_location(f); }
    static item* new_item(const char* name, const char* label, const char* icon)
    {
        return caja_menu_item_new(name, label, "", icon);
    }
    static menu* new_menu() { return caja_menu_new(); }
    static void append(menu* m, item* i) { caja_menu_append_item(m, i); }
    static void set_submenu(item* i, menu* m)
    {
        caja_menu_item_set_submenu(i, m);
        g_object_unref(m);
    }
};

GList* file_items(CajaMenuProvider*, GtkWidget*, GList* files)
{
    return menu_items<fm>(files_selection<fm>(files));
}

GList* background_items(CajaMenuProvider*, GtkWidget*, CajaFileInfo* folder)
{
    return menu_items<fm>(background_selection<fm>(folder));
}

void menu_provider_init(gpointer iface, gpointer)
{
    auto* i = static_cast<CajaMenuProviderIface*>(iface);
    i->get_file_items = file_items;
    i->get_background_items = background_items;
}

GType provider_type = 0;

}  // namespace

extern "C" G_MODULE_EXPORT void caja_module_initialize(GTypeModule* module)
{
    static const GTypeInfo info{.class_size = sizeof(GObjectClass),
                                .instance_size = sizeof(GObject)};
    provider_type =
        g_type_module_register_type(module, G_TYPE_OBJECT, type_name("Caja"), &info, GTypeFlags(0));
    static const GInterfaceInfo menu{.interface_init = menu_provider_init};
    g_type_module_add_interface(module, provider_type, CAJA_TYPE_MENU_PROVIDER, &menu);
}

extern "C" G_MODULE_EXPORT void caja_module_shutdown() {}

extern "C" G_MODULE_EXPORT void caja_module_list_types(const GType** types, int* count)
{
    *types = &provider_type;
    *count = 1;
}
