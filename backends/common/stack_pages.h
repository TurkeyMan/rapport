// A stack's pages (layout::stack, rap::stack): their ids, which is shown,
// and which shows once the shown one goes. Shared by the backends, which
// keep their own page (its widgets) alongside each id.
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rap::stack_pages
{

template <class Page>
class pages
{
public:
    struct entry
    {
        std::string id;
        Page page;
    };

    int index_of(const std::string& id) const
    {
        for (size_t i = 0; i < list_.size(); ++i)
            if (list_[i].id == id) return int(i);
        return -1;
    }
    bool contains(const std::string& id) const { return index_of(id) >= 0; }
    size_t size() const { return list_.size(); }
    bool empty() const { return list_.empty(); }
    entry& operator[](size_t i) { return list_[i]; }
    const entry& operator[](size_t i) const { return list_[i]; }
    auto begin() { return list_.begin(); }
    auto end() { return list_.end(); }

    // The shown page's index, or -1 with none.
    int shown() const { return shown_; }
    entry* shown_entry() { return shown_ >= 0 ? &list_[size_t(shown_)] : nullptr; }
    std::string shown_id() const { return shown_ >= 0 ? list_[size_t(shown_)].id : std::string(); }
    void set_shown(int i) { shown_ = i; }

    std::vector<std::string> ids() const
    {
        std::vector<std::string> out;
        out.reserve(list_.size());
        for (const entry& e : list_) out.push_back(e.id);
        return out;
    }

    // Last; shown if it's the only one. False (and nothing added) for an id
    // already there.
    bool add(std::string id, Page page)
    {
        if (contains(id)) return false;
        list_.push_back({std::move(id), std::move(page)});
        if (shown_ < 0) shown_ = 0;
        return true;
    }

    // Takes page `i` out. The one shown after: the same page if another went,
    // else the next, or the previous if it was last (-1 when none is left).
    entry take(size_t i)
    {
        entry e = std::move(list_[i]);
        list_.erase(list_.begin() + std::ptrdiff_t(i));
        if (shown_ > int(i))
            --shown_;
        else if (shown_ == int(i))
            shown_ = std::min(int(i), int(list_.size()) - 1);
        return e;
    }

private:
    std::vector<entry> list_;
    int shown_ = -1;
};

// Pages removed whose widgets wait to be destroyed until the event loop is
// next reached (a page may be removed from one of its own callbacks).
// `post` runs a function on the UI thread later; the graveyard may be gone
// by then.
template <class Page>
class graveyard
{
public:
    graveyard() = default;
    graveyard(const graveyard&) = delete;
    graveyard& operator=(const graveyard&) = delete;

    template <class Post>
    void bury(Page page, Post&& post)
    {
        dead_.push_back(std::move(page));
        if (dead_.size() > 1) return;  // a clearing is on its way
        std::weak_ptr<graveyard*> alive = self_;
        post(
            [alive]
            {
                if (auto self = alive.lock()) (*self)->clear();
            });
    }
    void clear()
    {
        // Destroying one may remove (and bury) another.
        while (!dead_.empty())
        {
            auto dead = std::move(dead_);
            dead_.clear();
            dead.clear();
        }
    }
    size_t size() const { return dead_.size(); }
    template <class F>
    void each(F&& f)
    {
        for (Page& p : dead_) f(p);
    }

private:
    std::vector<Page> dead_;
    std::shared_ptr<graveyard*> self_ = std::make_shared<graveyard*>(this);
};

}  // namespace rap::stack_pages
