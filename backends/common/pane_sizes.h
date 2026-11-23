// A splitter's panes told their sizes once all are placed. Shared by the
// GTK and Qt backends (Win32 records a splitter's sizes before placing its
// panes instead).
//
// A toolkit's splitter places its panes one after another, so a pane told
// its size as it's placed (a canvas's on_resize) would read the others'
// sizes, splitter::sizes(), as they were. While a splitter places its panes
// (to the outermost one being placed: a splitter in a pane is placed within
// its outer one's placing), what its panes are told waits, and is told once
// the outermost's placing is done, in the order they were placed: sizes()
// there is the whole new layout (#105). (Qt tells it there; GTK once its
// own layout is done, as it's still allocating the window then: see
// gtk_common.h.) Each waits with its pane's alive
// token, so a callback that destroys another pane, or the window, leaves
// the rest untold; one placed again before it's told is told once, its
// latest. A callback that lays the window out again (a set_sizes) takes the
// backend's usual path: what that places waits for the delivery already
// under way, never a new one inside it.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace rap::pane_sizes
{

struct waiting
{
    const void* pane;
    std::weak_ptr<bool> alive;  // the pane's: gone, or false, and it's left untold
    std::function<void()> tell;
};
inline int placing = 0;          // splitters placing their panes, nested
inline bool delivering = false;  // what waited being told
inline std::size_t next = 0;     // the first in the queue still to be told
inline std::vector<waiting> queue;

inline void deliver()
{
    if (delivering) return;  // the delivery under way tells it
    delivering = true;
    // By index: a callback may make more wait (a splitter placed again).
    for (next = 0; next < queue.size();)
    {
        waiting w = std::move(queue[next++]);
        const std::shared_ptr<bool> alive = w.alive.lock();
        if (alive && *alive && w.tell) w.tell();
    }
    queue.clear();
    next = 0;
    delivering = false;
}

// A splitter placing its panes, for as long as this lives; the outermost's
// end tells what waited.
struct placing_scope
{
    placing_scope() { ++placing; }
    ~placing_scope()
    {
        if (--placing == 0) deliver();
    }
    placing_scope(const placing_scope&) = delete;
    placing_scope& operator=(const placing_scope&) = delete;
};

// A splitter placing its panes within something that will tell what waited
// once it's done (Qt: an event filter on the splitter, ahead of the event
// its own placing_scope brackets): no delivery when this ends.
struct placing_held
{
    placing_held() { ++placing; }
    ~placing_held() { --placing; }
    placing_held(const placing_held&) = delete;
    placing_held& operator=(const placing_held&) = delete;
};

// `tell`, now, or once the splitters being placed are (with `pane`'s alive
// token: untold if it's gone by then). With some still waiting to be told
// (GTK tells them once its layout is done; or a delivery under way), it
// waits behind them, so no pane is told out of order, or its older size
// after its newer one.
inline void now_or_after(const void* pane, std::weak_ptr<bool> alive, std::function<void()> tell)
{
    if (!placing && !delivering && queue.empty())
    {
        tell();
        return;
    }
    for (std::size_t i = queue.size(); i-- > next;)
        if (queue[i].pane == pane && !queue[i].alive.expired())
        {
            queue[i].tell = std::move(tell);  // placed again before it's told: its latest
            return;
        }
    queue.push_back({pane, std::move(alive), std::move(tell)});
}

}  // namespace rap::pane_sizes
