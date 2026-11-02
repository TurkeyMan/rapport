// rap::sys network: whether the connection is metered, and online. Windows' network list
// manager; elsewhere NetworkManager over D-Bus through GLib's gio, loaded on
// first use (gio_internal.h).
#include "rapport/sys.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <netlistmgr.h>  // after windows.h, whose types it uses
// clang-format on
#else
#include "gio_internal.h"
#endif

namespace rap::sys
{

#if defined(_WIN32)

namespace
{

// `fn` with the network list manager's interface `iid`, false when there's
// none. A worker's thread may not have COM yet: in the apartment it has, or
// one of its own for the call.
template <class Interface, class Fn>
bool with_network_list(const GUID& iid, Fn fn)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Spelt out rather than from uuid.lib, which MinGW's lacks them in.
    constexpr GUID clsid = {0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    Interface* i = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, iid, reinterpret_cast<void**>(&i))))
    {
        ok = fn(i);
        i->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

}  // namespace

bool network_metered()
{
    constexpr GUID iid = {0xDCB00008, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    bool metered = false;
    with_network_list<INetworkCostManager>(iid, [&](INetworkCostManager* costs)
    {
        DWORD cost = 0;
        if (FAILED(costs->GetCost(&cost, nullptr))) return false;
        metered = cost & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE |
                          NLM_CONNECTION_COST_OVERDATALIMIT | NLM_CONNECTION_COST_ROAMING);
        return true;
    });
    return metered;
}

bool network_online()
{
    constexpr GUID iid = {0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    bool online = true;
    with_network_list<INetworkListManager>(iid, [&](INetworkListManager* list)
    {
        NLM_CONNECTIVITY c{};
        if (FAILED(list->GetConnectivity(&c))) return false;
        online = c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET);
        return true;
    });
    return online;
}

#else  // NetworkManager

namespace
{

// A uint32 property of NetworkManager itself, asked fresh each time (a
// monitor's cached copy needs a running GLib loop to stay current, which a
// Qt or Win32 program doesn't have); none without NetworkManager.
std::optional<uint32_t> network_manager(const char* property)
{
    const detail::gio_api* g = detail::gio();
    static void* const bus = g ? g->bus(detail::system_bus) : nullptr;
    if (!bus) return std::nullopt;
    const detail::gio_api& a = *g;
    void* reply = a.call(bus, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                         "org.freedesktop.DBus.Properties", "Get",
                         a.variant_new("(ss)", "org.freedesktop.NetworkManager", property), "(v)",
                         0 /* G_DBUS_CALL_FLAGS_NONE */, 1000, nullptr, nullptr);
    if (!reply) return std::nullopt;
    void* boxed = a.child(reply, 0);
    void* value = a.inner(boxed);
    std::optional<uint32_t> v;
    if (a.is_of_type(value, "u")) v = a.uint32(value);
    a.unref(value);
    a.unref(boxed);
    a.unref(reply);
    return v;
}

}  // namespace

bool network_metered()
{
    // NMMetered: 1 yes, 3 guessed yes; 0 unknown, 2 no, 4 guessed no.
    const auto m = network_manager("Metered");
    return m == 1u || m == 3u;
}

bool network_online()
{
    // NMConnectivityState: 4 full; 1 none, 2 behind a portal, 3 limited;
    // 0 unknown (counted as online, as is no NetworkManager).
    const auto c = network_manager("Connectivity");
    return !c || *c == 0 || *c == 4;
}

#endif

}  // namespace rap::sys
