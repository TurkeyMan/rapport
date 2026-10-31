// Between the Explorer extension's parts: its icon overlays
// (explorer_badges.cpp) beside its context menu (explorer_menu.cpp), one
// COM server.
#pragma once

#include <windows.h>

#include <atomic>
#include <string>

namespace rap::shell::detail
{

// Live COM objects and server locks, for DllCanUnloadNow.
std::atomic<long>& objects();
std::atomic<long>& locks();

// The class factory for a badge kind's overlay, if `clsid` is one.
HRESULT badge_class_object(REFCLSID clsid, REFIID riid, void** out);

// The overlays' COM classes under `root`, and in HKLM Explorer's list of
// overlays too.
bool register_badges(HKEY root, const std::wstring& module);
void unregister_badges(HKEY root);

}  // namespace rap::shell::detail
