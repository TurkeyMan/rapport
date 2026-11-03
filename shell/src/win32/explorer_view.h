// What Explorer's view said about a right-click, carried from the entry's
// first GetState (on Explorer's UI thread, where the view's objects live) to
// its second (on a background thread, which mustn't touch them), and what
// was right-clicked, worked out from it.
//
// A snapshot is kept for the items it was taken with, and taken once by the
// slow call for the same items. A slow call says nothing else about which
// right-click it's for, so whenever one might be another's, no view:
//  - Two right-clicks about the same items (two background clicks with none,
//    or a folder and its background) make those items ambiguous.
//  - A new site poisons what the last one said: its slow calls may still
//    come, and mustn't take the new site's snapshot for the same items.
//  - Past `most` items tracked at once, it gives up: no view from then on.
// The site, its generation and the snapshots share one lock, so a new site
// takes effect in one step, and an ask of the last one that's under way
// doesn't land. With no view, the click is taken as its items: a background
// click then counts as its folder, which is wrong but harmless.
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
    // SetSite: `swap` changes the site, under the same lock as what the
    // last site said is poisoned, so no slow call can take any of it after,
    // nor the new site's snapshot for items the last one's calls were about.
    template <class F>
    void change_site(F&& swap)
    {
        std::lock_guard<std::mutex> lock(lock_);
        swap();
        ++site_;
        for (snapshot& s : pending_) s.view.reset();
    }

    // Before asking the view: `read` reads the site, under the same lock,
    // and what's returned, handed back to publish, says which site it was.
    template <class F>
    std::uint64_t begin(F&& read) const
    {
        std::lock_guard<std::mutex> lock(lock_);
        read();
        return site_;
    }
    std::uint64_t begin() const
    {
        return begin([] {});
    }

    // After: kept for the slow call about the same items, unless the site
    // changed meanwhile. A second about items already waiting makes them
    // ambiguous: no view for any of their slow calls.
    bool publish(std::uint64_t site, paths items, view_info view)
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (lost_ || site != site_) return false;
        for (snapshot& s : pending_)
            if (s.items == items)
            {
                s.view.reset();
                ++s.calls;
                return true;
            }
        if (pending_.size() == most)
        {
            // Dropping any would let its slow call take a later snapshot.
            lost_ = true;
            pending_.clear();
            return false;
        }
        pending_.push_back({std::move(items), std::move(view), 1});
        return true;
    }

    // The slow call: what the view said about these items, once, or
    // nothing when there's none, they were ambiguous or poisoned, or
    // tracking was lost.
    std::optional<view_info> take(const paths& items)
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (lost_) return std::nullopt;
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if (it->items == items)
            {
                std::optional<view_info> view = std::move(it->view);
                it->view.reset();
                if (--it->calls == 0) pending_.erase(it);
                return view;
            }
        return std::nullopt;
    }

    static constexpr std::size_t most = 8;

private:
    struct snapshot
    {
        paths items;
        std::optional<view_info> view;  // none once ambiguous or poisoned
        std::size_t calls;              // slow calls still to come about these items
    };

    mutable std::mutex lock_;
    std::uint64_t site_ = 0;
    std::vector<snapshot> pending_;
    bool lost_ = false;  // gave up tracking: no view from then on
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
