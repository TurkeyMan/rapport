// gio, loaded at run time (gio_internal.h).
#include "gio_internal.h"

#include "rapport/sys.h"

namespace rap::sys::detail
{

const gio_api* gio()
{
    static const gio_api a = []
    {
        gio_api a{};
        auto lib = library::open("libgio-2.0.so.0");
        if (!lib) return a;
        a.bus_get = lib->function<bus_get_fn>("g_bus_get_sync");
        a.call = lib->function<call_sync_fn>("g_dbus_connection_call_sync");
        a.call_with_fds =
            lib->function<call_fd_sync_fn>("g_dbus_connection_call_with_unix_fd_list_sync");
        a.variant_new = lib->function<variant_new_fn>("g_variant_new");
        a.variant_new_parsed = lib->function<variant_new_fn>("g_variant_new_parsed");
        a.child = lib->function<variant_child_fn>("g_variant_get_child_value");
        a.inner = lib->function<variant_variant_fn>("g_variant_get_variant");
        a.is_of_type = lib->function<variant_is_fn>("g_variant_is_of_type");
        a.uint32 = lib->function<variant_uint32_fn>("g_variant_get_uint32");
        a.unref = lib->function<variant_unref_fn>("g_variant_unref");
        a.fd_list_new = lib->function<fd_list_new_fn>("g_unix_fd_list_new");
        a.fd_list_append = lib->function<fd_list_append_fn>("g_unix_fd_list_append");
        a.object_unref = lib->function<object_unref_fn>("g_object_unref");
        a.error_free = lib->function<error_free_fn>("g_error_free");
        if (!a.bus_get || !a.call || !a.call_with_fds || !a.variant_new || !a.variant_new_parsed ||
            !a.child || !a.inner || !a.is_of_type || !a.uint32 || !a.unref || !a.fd_list_new ||
            !a.fd_list_append || !a.object_unref || !a.error_free)
            return gio_api{};
        lib->release();
        return a;
    }();
    return a.bus_get ? &a : nullptr;
}

void* gio_api::bus(int type) const
{
    void* error = nullptr;
    void* b = bus_get(type, nullptr, &error);
    if (error) error_free(error);
    return b;
}

}  // namespace rap::sys::detail
