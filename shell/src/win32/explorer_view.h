// What Explorer's view said about a right-click, carried from the entry's
// first GetState (on Explorer's UI thread, where the view's objects live) to
// its second (on a background thread, which mustn't touch them), and what
// was right-clicked, worked out from it.
//
// A snapshot is kept for the items it was taken with, and taken once by the
// slow call for the same items. A new site drops them all, and an ask that
// was under way when the site changed doesn't land. With no snapshot to
// match, the click is taken as its items, with no view: a background click
// then counts as its folder, which is wrong but harmless.
//
// No COM here, so tests/explorer_view_test.cpp can check it.
#pragma once

#include <rapport/shell.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace rap::shell::detail
{

using paths = std::vector<std::filesystem::path>;

struct view_info
{
    std::filesystem::path folder;  // the view's; empty when it had none
    bool has_selection = false;
};

class view_snapshots
{
public:
    // SetSite: what the last site said is gone, and an ask of it under way
    // won't land.
    void site_changed()
    {
        std::lock_guard<std::mutex> lock(lock_);
        ++site_;
        pending_.clear();
    }

    // Before asking the view: handed back to publish.
    std::uint64_t begin() const
    {
        std::lock_guard<std::mutex> lock(lock_);
        return site_;
    }

    // After: kept for the slow call about the same items, unless the site
    // changed meanwhile. A right-click about the same items replaces its
    // last; past `most` the oldest go.
    bool publish(std::uint64_t site, paths items, view_info view)
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (site != site_) return false;
        std::erase_if(pending_, [&](const snapshot& s) { return s.items == items; });
        pending_.push_back({std::move(items), std::move(view)});
        if (pending_.size() > most) pending_.erase(pending_.begin());
        return true;
    }

    // The slow call: what the view said about these items, once.
    std::optional<view_info> take(const paths& items)
    {
        std::lock_guard<std::mutex> lock(lock_);
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if (it->items == items)
            {
                view_info view = std::move(it->view);
                pending_.erase(it);
                return view;
            }
        return std::nullopt;
    }

    static constexpr std::size_t most = 4;

private:
    struct snapshot
    {
        paths items;
        view_info view;
    };

    mutable std::mutex lock_;
    std::uint64_t site_ = 0;
    std::vector<snapshot> pending_;
};

// What was right-clicked. Explorer gives a background click its folder as
// the one item (or none), so that's told apart by the view having nothing
// selected. Without the view, it's the items as they are.
inline selection selection_of(paths items, const std::optional<view_info>& view)
{
    selection sel;
    if (view && !view->has_selection && !view->folder.empty() &&
        (items.empty() || (items.size() == 1 && items.front() == view->folder)))
    {
        sel.folder = view->folder;
        sel.background = true;
        return sel;
    }
    sel.paths = std::move(items);
    if (!sel.paths.empty()) sel.folder = sel.paths.front().parent_path();
    return sel;
}

}  // namespace rap::shell::detail
