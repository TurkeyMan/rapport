// rap::sys network: whether the connection is metered. Windows' network list
// manager; elsewhere NetworkManager over D-Bus through GLib's gio, loaded on
// first use with the few functions it needs declared here, as secrets.cpp
// does libsecret.
#include "rapport/sys.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <netlistmgr.h>  // after windows.h, whose types it uses
// clang-format on
#endif

namespace rap::sys
{

#if defined(_WIN32)

bool network_metered()
{
    // A worker's thread may not have COM yet: in the apartment it has, or
    // one of its own for the call.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Spelt out rather than from uuid.lib, which MinGW's lacks them in.
    constexpr GUID clsid = {0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    constexpr GUID iid = {0xDCB00008, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    bool metered = false;
    INetworkCostManager* costs = nullptr;
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, iid, reinterpret_cast<void**>(&costs))))
    {
        DWORD cost = 0;
        if (SUCCEEDED(costs->GetCost(&cost, nullptr)))
            metered = cost & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE |
                              NLM_CONNECTION_COST_OVERDATALIMIT | NLM_CONNECTION_COST_ROAMING);
        costs->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return metered;
}

#else  // NetworkManager

namespace
{

// gio's and GLib's, as gio.h has them (gio pulls GLib in).
using bus_get_fn = void*(int bus_type, void* cancellable, void** error);
using call_sync_fn = void*(void* connection, const char* bus_name, const char* path, const char* interface,
                           const char* method, void* parameters, const void* reply_type, int flags, int timeout_ms,
                           void* cancellable, void** error);
using variant_new_fn = void*(const char* format, ...);
using variant_child_fn = void*(void* variant, size_t index);
using variant_variant_fn = void*(void* variant);
using variant_is_fn = int(void* variant, const void* type);
using variant_uint32_fn = uint32_t(void* variant);
using variant_unref_fn = void(void* variant);
using error_free_fn = void(void* error);

struct gio_api
{
    void* system_bus = nullptr;  // kept for the life of the process
    call_sync_fn* call = nullptr;
    variant_new_fn* variant_new = nullptr;
    variant_child_fn* child = nullptr;
    variant_variant_fn* inner = nullptr;
    variant_is_fn* is_of_type = nullptr;
    variant_uint32_fn* uint32 = nullptr;
    variant_unref_fn* unref = nullptr;
};

const gio_api& api()
{
    static const gio_api a = []
    {
        gio_api a{};
        auto lib = library::open("libgio-2.0.so.0");
        if (!lib) return a;
        auto* bus_get = lib->function<bus_get_fn>("g_bus_get_sync");
        auto* error_free = lib->function<error_free_fn>("g_error_free");
        a.call = lib->function<call_sync_fn>("g_dbus_connection_call_sync");
        a.variant_new = lib->function<variant_new_fn>("g_variant_new");
        a.child = lib->function<variant_child_fn>("g_variant_get_child_value");
        a.inner = lib->function<variant_variant_fn>("g_variant_get_variant");
        a.is_of_type = lib->function<variant_is_fn>("g_variant_is_of_type");
        a.uint32 = lib->function<variant_uint32_fn>("g_variant_get_uint32");
        a.unref = lib->function<variant_unref_fn>("g_variant_unref");
        if (!bus_get || !error_free || !a.call || !a.variant_new || !a.child || !a.inner || !a.is_of_type ||
            !a.uint32 || !a.unref)
            return gio_api{};
        void* error = nullptr;
        a.system_bus = bus_get(2 /* G_BUS_TYPE_SYSTEM */, nullptr, &error);
        if (error) error_free(error);
        if (!a.system_bus) return gio_api{};
        lib->release();
        return a;
    }();
    return a;
}

}  // namespace

bool network_metered()
{
    const gio_api& a = api();
    if (!a.system_bus) return false;
    // NetworkManager's own answer for the primary connection, asked fresh
    // each time (a monitor's cached copy needs a running GLib loop to stay
    // current, which a Qt or Win32 program doesn't have).
    void* reply = a.call(a.system_bus, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                         "org.freedesktop.DBus.Properties", "Get",
                         a.variant_new("(ss)", "org.freedesktop.NetworkManager", "Metered"), "(v)",
                         0 /* G_DBUS_CALL_FLAGS_NONE */, 1000, nullptr, nullptr);
    if (!reply) return false;  // no NetworkManager: can't tell
    void* boxed = a.child(reply, 0);
    void* value = a.inner(boxed);
    // NMMetered: 1 yes, 3 guessed yes; 0 unknown, 2 no, 4 guessed no.
    const uint32_t m = a.is_of_type(value, "u") ? a.uint32(value) : 0;
    a.unref(value);
    a.unref(boxed);
    a.unref(reply);
    return m == 1 || m == 3;
}

#endif

}  // namespace rap::sys
