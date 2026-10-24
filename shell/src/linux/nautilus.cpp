// The provider as a Nautilus 4 extension (libnautilus-extension-4), in
// <libdir>/nautilus/extensions-4. Nautilus 4's menus show no icons and no
// separators.
#include "gobject_menu.h"

#include <nautilus-extension.h>

namespace
{

using namespace rap::shell::adapter;

struct fm
{
    using file_info = NautilusFileInfo;
    using item = NautilusMenuItem;
    using menu = NautilusMenu;
    static constexpr bool separators = false;
    static GFile* location(file_info* f) { return nautilus_file_info_get_location(f); }
    static item* new_item(const char* name, const char* label, const char* icon)
    {
        return nautilus_menu_item_new(name, label, "", icon);
    }
    static menu* new_menu() { return nautilus_menu_new(); }
    static void append(menu* m, item* i) { nautilus_menu_append_item(m, i); }
    static void set_submenu(item* i, menu* m)
    {
        nautilus_menu_item_set_submenu(i, m);
        g_object_unref(m);
    }
};

GList* file_items(NautilusMenuProvider*, GList* files)
{
    return menu_items<fm>(files_selection<fm>(files));
}

GList* background_items(NautilusMenuProvider*, NautilusFileInfo* folder)
{
    return menu_items<fm>(background_selection<fm>(folder));
}

void menu_provider_init(gpointer iface, gpointer)
{
    auto* i = static_cast<NautilusMenuProviderInterface*>(iface);
    i->get_file_items = file_items;
    i->get_background_items = background_items;
}

GType provider_type = 0;

}  // namespace

extern "C" G_MODULE_EXPORT void nautilus_module_initialize(GTypeModule* module)
{
    static const GTypeInfo info{.class_size = sizeof(GObjectClass),
                                .instance_size = sizeof(GObject)};
    provider_type = g_type_module_register_type(module, G_TYPE_OBJECT, type_name("Nautilus"), &info,
                                                GTypeFlags(0));
    static const GInterfaceInfo menu{.interface_init = menu_provider_init};
    g_type_module_add_interface(module, provider_type, NAUTILUS_TYPE_MENU_PROVIDER, &menu);
}

extern "C" G_MODULE_EXPORT void nautilus_module_shutdown() {}

extern "C" G_MODULE_EXPORT void nautilus_module_list_types(const GType** types, int* count)
{
    *types = &provider_type;
    *count = 1;
}
