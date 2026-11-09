// Replacing Explorer overlay names: nothing that worked goes until all its
// replacements are written, a failure is reported, and a retry finishes.
#include "overlay_names.h"

#include <cstdio>
#include <map>

using namespace rap::shell::detail;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) return;
    std::printf("FAILED: %s\n", what);
    ++failures;
}

const std::vector<std::wstring> k_old{L"    App1Modified", L"    App2Clean"};
const std::vector<std::wstring> k_new{L"     App1Modified", L"     App2Clean"};
const std::vector<std::wstring> k_ids{L"{A}", L"{B}"};
const std::wstring k_foreign = L"    OneDrive1";

struct fake
{
    std::map<std::wstring, std::wstring> keys;
    int fail_write = -1;
    bool fail_list = false;
    std::wstring fail_remove;

    fake()
    {
        for (size_t i = 0; i < k_old.size(); ++i) keys[k_old[i]] = k_ids[i];
        keys[k_foreign] = L"{ONEDRIVE}";
    }

    bool write(size_t i)
    {
        if (int(i) == fail_write) return false;
        keys[k_new[i]] = k_ids[i];
        return true;
    }
    std::optional<std::vector<overlay_entry>> list()
    {
        if (fail_list) return std::nullopt;
        std::vector<overlay_entry> out;
        for (const auto& [name, id] : keys) out.push_back({name, id});
        return out;
    }
    bool ours(const std::wstring& id) { return id == k_ids[0] || id == k_ids[1]; }
    bool remove(const std::wstring& name)
    {
        if (name == fail_remove) return false;
        keys.erase(name);
        return true;
    }

    bool has(const std::vector<std::wstring>& names) const
    {
        for (const std::wstring& n : names)
            if (!keys.contains(n)) return false;
        return true;
    }
    bool finished() const
    {
        return has(k_new) && keys.contains(k_foreign) && keys.size() == k_new.size() + 1;
    }
    // A retry with nothing failing.
    bool retry()
    {
        fail_write = -1;
        fail_list = false;
        fail_remove.clear();
        return install_overlays(*this, k_new) && finished();
    }
};

void write_fails_at(int at, const char* retried)
{
    fake f;
    f.fail_write = at;
    check(!install_overlays(f, k_new), "a failed write is reported");
    check(f.has(k_old), "a failed write keeps every old name");
    check(f.retry(), retried);
}

}  // namespace

int main()
{
    write_fails_at(0, "a retry finishes after the first write failed");
    write_fails_at(1, "a retry finishes after a later write failed");

    {
        fake f;
        check(install_overlays(f, k_new) && f.finished(), "old names are replaced");
    }
    {
        fake f;
        f.fail_list = true;
        check(!install_overlays(f, k_new), "not being able to list is reported");
        check(f.has(k_new) && f.has(k_old), "unlisted, both sets of names stay");
        check(f.retry(), "a retry finishes after the list failed");
    }
    {
        fake f;
        f.fail_remove = k_old[0];
        check(!install_overlays(f, k_new), "a stale name left behind is reported");
        check(f.has(k_new), "a failed removal keeps the new names");
        check(f.retry(), "a retry finishes after a removal failed");
    }
    {
        fake f;
        check(install_overlays(f, k_new) && install_overlays(f, k_new) && f.finished(),
              "registering again changes nothing");
        f.keys.clear();
        f.keys[L"     APP1MODIFIED"] = k_ids[0];
        check(prune_overlays(f, k_new) && f.keys.size() == 1,
              "a name differing only in case is the same name");
        check(prune_overlays(f, {}) && f.keys.empty(), "unregistering removes every name of ours");
    }

    if (failures == 0) std::printf("all passed\n");
    return failures == 0 ? 0 : 1;
}
