// GLib's gio, loaded on first use with the few functions rap::sys needs
// declared here, as secrets.cpp does libsecret: D-Bus for network.cpp
// (NetworkManager) and sys.cpp (the desktop portal). Not on Windows.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rap::sys::detail
{

// gio's and GLib's, as gio.h has them (gio pulls GLib in).
using bus_get_fn = void*(int bus_type, void* cancellable, void** error);
using call_sync_fn = void*(void* connection, const char* bus_name, const char* path,
                           const char* interface, const char* method, void* parameters,
                           const void* reply_type, int flags, int timeout_ms, void* cancellable,
                           void** error);
using call_fd_sync_fn = void*(void* connection, const char* bus_name, const char* path,
                              const char* interface, const char* method, void* parameters,
                              const void* reply_type, int flags, int timeout_ms, void* fd_list,
                              void** out_fd_list, void* cancellable, void** error);
using variant_new_fn = void*(const char* format, ...);
using variant_child_fn = void*(void* variant, size_t index);
using variant_variant_fn = void*(void* variant);
using variant_is_fn = int(void* variant, const void* type);
using variant_uint32_fn = uint32_t(void* variant);
using variant_unref_fn = void(void* variant);
using fd_list_new_fn = void*();
using fd_list_append_fn = int(void* list, int fd, void** error);
using object_unref_fn = void(void* object);
using error_free_fn = void(void* error);

// G_BUS_TYPE_SYSTEM, G_BUS_TYPE_SESSION.
inline constexpr int system_bus = 1;
inline constexpr int session_bus = 2;

struct gio_api
{
    bus_get_fn* bus_get = nullptr;
    call_sync_fn* call = nullptr;
    call_fd_sync_fn* call_with_fds = nullptr;
    variant_new_fn* variant_new = nullptr;
    variant_new_fn* variant_new_parsed = nullptr;
    variant_child_fn* child = nullptr;
    variant_variant_fn* inner = nullptr;
    variant_is_fn* is_of_type = nullptr;
    variant_uint32_fn* uint32 = nullptr;
    variant_unref_fn* unref = nullptr;
    fd_list_new_fn* fd_list_new = nullptr;
    fd_list_append_fn* fd_list_append = nullptr;
    object_unref_fn* object_unref = nullptr;
    error_free_fn* error_free = nullptr;

    // A bus connection, gio's shared one (kept for the life of the
    // process); nullptr without one.
    void* bus(int type) const;
};

// The library, loaded once and kept; nullptr without gio.
const gio_api* gio();

}  // namespace rap::sys::detail
