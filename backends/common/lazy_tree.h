// When a tree asks the app for a node's children: the rule the backends
// share. A node marked here "has children, not loaded yet" asks once when it
// becomes open, whoever opened it, and not again until it's been answered.
// Asks raised while the tree is changing wait until the change is finished, so
// the app can answer from inside its handler. No toolkit here, so
// tests/tree_lazy_test.cpp can check it.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rap::lazy_tree
{

class tracker
{
public:
    // Asks raised before there's a handler wait for it.
    void on_request(std::function<void(const std::string&)> f)
    {
        request_ = std::move(f);
        flush();
    }

    // Around anything that changes the tree.
    class batch
    {
    public:
        explicit batch(tracker& t)
            : t_(t)
        {
            ++t_.depth_;
        }
        ~batch()
        {
            if (--t_.depth_ == 0) t_.flush();
        }
        batch(const batch&) = delete;
        batch& operator=(const batch&) = delete;

    private:
        tracker& t_;
    };

    void clear() { nodes_.clear(); }
    void mark(const std::string& id) { nodes_[id] = state::waiting; }
    // Answered, or gone.
    void forget(const std::string& id) { nodes_.erase(id); }
    // Showing the placeholder: marked and not yet answered.
    bool unloaded(const std::string& id) const { return nodes_.contains(id); }

    void opened(const std::string& id)
    {
        const auto it = nodes_.find(id);
        if (it == nodes_.end() || it->second != state::waiting) return;
        it->second = state::queued;
        queue_.push_back(id);
        flush();
    }

private:
    enum class state
    {
        waiting,
        queued,
        requested,
    };

    void flush()
    {
        if (depth_ || flushing_ || !request_) return;
        flushing_ = true;
        const std::weak_ptr<void> alive = alive_;
        // By index: a handler that answers can queue more. A handler that
        // removes itself leaves the rest waiting for the next one.
        size_t i = 0;
        for (; i < queue_.size() && request_; ++i)
        {
            const std::string id = queue_[i];
            const auto it = nodes_.find(id);
            if (it == nodes_.end() || it->second != state::queued) continue;
            it->second = state::requested;
            {
                const auto request = request_;  // it may replace itself
                request(id);
            }  // the copy may hold the last owner of the tree: gone before the check
            if (alive.expired()) return;  // it destroyed the tree, and this with it
        }
        queue_.erase(queue_.begin(), queue_.begin() + std::ptrdiff_t(i));
        flushing_ = false;
    }

    std::unordered_map<std::string, state> nodes_;
    std::vector<std::string> queue_;
    std::function<void(const std::string&)> request_;
    int depth_ = 0;
    bool flushing_ = false;
    std::shared_ptr<void> alive_ = std::make_shared<char>();
};

}  // namespace rap::lazy_tree
