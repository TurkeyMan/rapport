// Replacing an extension's names among Explorer's icon overlays, apart from
// the registry so its failure paths can be tested. The new names are all
// written before any old one goes: a registration that fails part way leaves
// the badges that worked, and running it again finishes the job.
//
// Ops provides:
//   bool write(size_t i);          // the i'th badge's class and name
//   std::optional<std::vector<overlay_entry>> list();  // nullopt: can't tell
//   bool ours(const std::wstring& clsid);
//   bool remove(const std::wstring& name);  // true if it's already gone
#pragma once

#include <cstddef>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace rap::shell::detail
{

struct overlay_entry
{
    std::wstring name;
    std::wstring clsid;
};

// Removes every name of ours but those in `keep` (registry names ignore
// case). False if any may be left.
template <class Ops>
bool prune_overlays(Ops& ops, const std::vector<std::wstring>& keep)
{
    const std::optional<std::vector<overlay_entry>> entries = ops.list();
    if (!entries) return false;
    bool ok = true;
    for (const overlay_entry& e : *entries)
    {
        if (!ops.ours(e.clsid)) continue;
        bool kept = false;
        for (const std::wstring& k : keep) kept = kept || _wcsicmp(k.c_str(), e.name.c_str()) == 0;
        if (!kept) ok = ops.remove(e.name) && ok;
    }
    return ok;
}

// Writes `names` (one per badge, in order), then removes our others. False,
// with the old names still there, if a write fails; false if a stale name
// may remain and still be taking a slot.
template <class Ops>
bool install_overlays(Ops& ops, const std::vector<std::wstring>& names)
{
    for (size_t i = 0; i < names.size(); ++i)
        if (!ops.write(i)) return false;
    return prune_overlays(ops, names);
}

}  // namespace rap::shell::detail
