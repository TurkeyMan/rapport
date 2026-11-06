// Private Win32 instrumentation for scripted layout and presentation measurements.
#pragma once

#include <atomic>
#include <cstdint>

namespace rap::win32::trace
{
enum class phase
{
    resize,
    layout,
    drag_step,
    child_moves,
    end_defer,
    window_size,
    redraw,
    pane_paint,
    pane_print,
    pane_blit,
    canvas_paint,
    readback_submit,
    readback_complete,
    readback_publish,
    present,
    gpu_wait,
    fence_wait,
    query_wait,
    pane_invalidated,
    pane_area,
    pane_visible_area,
    move_pass,
    window_move,
    move_decision,
    message,
    dispatch,
    input_time,
    input_age,
    tab_cache_miss,
    tab_adjust,
    control_colour
};

extern std::atomic<bool> enabled;
void initialize();
uint64_t current_id();
void mark(phase, uint64_t arg = 0);
void install_message_hooks();
void remove_message_hooks();

class scope
{
public:
    scope(phase p, uint64_t arg = 0, uint64_t cause = 0, uint64_t details = 0)
    {
        if (enabled.load(std::memory_order_relaxed)) begin(p, arg, cause, details);
    }
    ~scope()
    {
        if (id_) end();
    }
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
    uint64_t arg() const { return id_ ? arg_ : 0; }
    uint64_t id() const { return id_; }

private:
    void begin(phase, uint64_t, uint64_t, uint64_t);
    void end();
    uint64_t id_ = 0, parent_;
    uint64_t arg_, details_;
    phase phase_;
};
}  // namespace rap::win32::trace

// Private harness entry point, also available by name from rapport_win32.dll.
#ifdef RAPPORT_TRACE_IMPLEMENTATION
extern "C" __declspec(dllexport) bool rapport_win32_trace_dump();
#else
extern "C" bool rapport_win32_trace_dump();
#endif

namespace rap
{
class splitter;
}
// Returns the bar HWND as void*, or nullptr for a wrong backend, index or UI thread.
// The caller must keep the splitter alive. The returned HWND is borrowed.
#ifdef RAPPORT_TRACE_SPLITTER_IMPLEMENTATION
extern "C" __declspec(dllexport) void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned index);
#else
extern "C" void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned index);
#endif
