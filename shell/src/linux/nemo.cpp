// The provider as a Nemo extension (libnemo-extension), in
// <libdir>/nemo/extensions-3.0. Nemo lists it in Plugins, where it can be
// turned off. Badges are emblems, drawn over the icon.
#include "gobject_badges.h"

#include <libnemo-extension/nemo-file-info.h>
#include <libnemo-extension/nemo-info-provider.h>
#include <libnemo-extension/nemo-menu-provider.h>
#include <libnemo-extension/nemo-name-and-desc-provider.h>

namespace
{

using namespace rap::shell::adapter;

struct fm
{
    using file_info = NemoFileInfo;
    using item = NemoMenuItem;
    using menu = NemoMenu;
    static constexpr bool separators = true;
    static GFile* location(file_info* f) { return nemo_file_info_get_location(f); }
    static item* new_item(const char* name, const char* label, const char* icon)
    {
        return nemo_menu_item_new(name, label, "", icon);
    }
    static item* new_separator(const char* name) { return nemo_menu_item_new_separator(name); }
    static menu* new_menu() { return nemo_menu_new(); }
    static void append(menu* m, item* i) { nemo_menu_append_item(m, i); }
    static void set_submenu(item* i, menu* m)
    {
        nemo_menu_item_set_submenu(i, m);
        g_object_unref(m);
    }
};

GList* file_items(NemoMenuProvider*, GtkWidget*, GList* files)
{
    return menu_items<fm>(files_selection<fm>(files));
}

GList* background_items(NemoMenuProvider*, GtkWidget*, NemoFileInfo* folder)
{
    return menu_items<fm>(background_selection<fm>(folder));
}

void menu_provider_init(gpointer iface, gpointer)
{
    auto* i = static_cast<NemoMenuProviderInterface*>(iface);
    i->get_file_items = file_items;
    i->get_background_items = background_items;
}

// "Name:::Description", as Nemo's Plugins list shows it.
GList* name_and_desc(NemoNameAndDescProvider*)
{
    const std::string s = std::string(rap::shell::this_extension.name) +
                          ":::Entries in the context menu, and badges on icons";
    return g_list_append(nullptr, g_strdup(s.c_str()));
}

void name_and_desc_init(gpointer iface, gpointer)
{
    static_cast<NemoNameAndDescProviderInterface*>(iface)->get_name_and_desc = name_and_desc;
}

struct badge_fm
{
    using file_info = NemoFileInfo;
    using info_provider = NemoInfoProvider;
    using handle = NemoOperationHandle;
    static GFile* location(file_info* f) { return nemo_file_info_get_location(f); }
    static void add_emblem(file_info* f, const char* name) { nemo_file_info_add_emblem(f, name); }
    static void invalidate(file_info* f) { nemo_file_info_invalidate_extension_info(f); }
    static void complete(GClosure* done, info_provider* p, handle* h)
    {
        nemo_info_provider_update_complete_invoke(done, p, h, NEMO_OPERATION_COMPLETE);
    }
};

NemoOperationResult update_file_info(NemoInfoProvider* provider, NemoFileInfo* file, GClosure* done,
                                     NemoOperationHandle** handle)
{
    return start_badge<badge_fm>(provider, file, done, handle) ? NEMO_OPERATION_IN_PROGRESS
                                                               : NEMO_OPERATION_COMPLETE;
}

void cancel_update(NemoInfoProvider*, NemoOperationHandle* handle)
{
    cancel_badge(handle);
}

void info_provider_init(gpointer iface, gpointer)
{
    auto* i = static_cast<NemoInfoProviderInterface*>(iface);
    i->update_file_info = update_file_info;
    i->cancel_update = cancel_update;
}

GType provider_type = 0;

}  // namespace

extern "C" G_MODULE_EXPORT void nemo_module_initialize(GTypeModule* module)
{
    static const GTypeInfo info{.class_size = sizeof(GObjectClass),
                                .instance_size = sizeof(GObject)};
    provider_type =
        g_type_module_register_type(module, G_TYPE_OBJECT, type_name("Nemo"), &info, GTypeFlags(0));
    static const GInterfaceInfo menu{.interface_init = menu_provider_init};
    g_type_module_add_interface(module, provider_type, NEMO_TYPE_MENU_PROVIDER, &menu);
    static const GInterfaceInfo badges{.interface_init = info_provider_init};
    g_type_module_add_interface(module, provider_type, NEMO_TYPE_INFO_PROVIDER, &badges);
    static const GInterfaceInfo desc{.interface_init = name_and_desc_init};
    g_type_module_add_interface(module, provider_type, NEMO_TYPE_NAME_AND_DESC_PROVIDER, &desc);
}

extern "C" G_MODULE_EXPORT void nemo_module_shutdown() {}

extern "C" G_MODULE_EXPORT void nemo_module_list_types(const GType** types, int* count)
{
    *types = &provider_type;
    *count = 1;
}
