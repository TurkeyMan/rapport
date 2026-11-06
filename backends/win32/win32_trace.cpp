#define RAPPORT_TRACE_IMPLEMENTATION
#include "win32_trace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace rap::win32::trace
{
std::atomic<bool> enabled{false};

namespace
{
// One writer per ring. A concurrent dump checks the sequence on both sides
// of its copy. Atomic payloads make even a rejected, overwritten copy safe.
using word = std::atomic<uint64_t>;
static_assert(word::is_always_lock_free);
constexpr size_t capacity = 65536;
struct slot
{
    word sequence{}, ticks{}, kind{}, arg{}, cause{}, details{};
};
struct ring
{
    std::unique_ptr<slot[]> slots = std::make_unique<slot[]>(capacity);
    word count{};
    std::atomic<DWORD> thread{};
};
struct session
{
    std::array<ring, 32> rings{};
    word threads{}, next_id{1};
    LARGE_INTEGER frequency{};
    std::wstring path;
    std::mutex dump_lock;  // only dumpers take this
};
session* recording;
thread_local uint64_t current;
thread_local ring* local;
thread_local bool registered;
thread_local unsigned moving, sizing;

uint64_t window_details(HWND h)
{
    wchar_t name[64]{};
    GetClassNameW(h, name, 64);
    const wchar_t* classes[] = {L"STATIC",          L"BUTTON",        L"msctls_trackbar32", L"EDIT",
                                L"SysTabControl32", L"RapportCanvas", L"RapportHost"};
    uint64_t cls = 0;
    for (size_t i = 0; i < std::size(classes); ++i)
        if (!_wcsicmp(name, classes[i])) cls = i + 1;
    return (cls << 32) | (uint64_t(IsWindowVisible(h) != FALSE) << 40) |
           (uint64_t(WORD(GetClassLongPtrW(h, GCW_ATOM))) << 48);
}

void write(phase p, bool finish, uint64_t arg, uint64_t cause, uint64_t details)
{
    if (!registered)
    {
        registered = true;
        auto index = recording->threads.fetch_add(1, std::memory_order_relaxed);
        if (index < recording->rings.size())
        {
            local = &recording->rings[index];
            local->thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        }
    }
    if (!local) return;
    const auto index = local->count.load(std::memory_order_relaxed);
    slot& s = local->slots[index % capacity];
    s.sequence.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s.ticks.store(now.QuadPart, std::memory_order_relaxed);
    s.kind.store(uint64_t(p) * 2 + finish, std::memory_order_relaxed);
    s.arg.store(arg, std::memory_order_relaxed);
    s.cause.store(cause, std::memory_order_relaxed);
    s.details.store(details, std::memory_order_relaxed);
    s.sequence.store(index + 1, std::memory_order_release);
    local->count.store(index + 1, std::memory_order_release);
}

constexpr const char* names[] = {"window.resize",
                                 "layout",
                                 "splitter.step",
                                 "child.moves",
                                 "EndDeferWindowPos",
                                 "WM_SIZE",
                                 "RDW_UPDATENOW",
                                 "pane.paint",
                                 "pane.WM_PRINT",
                                 "pane.BitBlt",
                                 "canvas.paint",
                                 "readback.submit",
                                 "readback.complete",
                                 "readback.publish",
                                 "Present",
                                 "gpu.wait",
                                 "fence.wait",
                                 "query.wait",
                                 "pane.invalidate",
                                 "pane.printed_pixels",
                                 "pane.visible_pixels",
                                 "move.pass",
                                 "window.move",
                                 "move.decision",
                                 "message",
                                 "dispatch",
                                 "input.message_time",
                                 "input.age_ms",
                                 "tab.cache_miss",
                                 "TCM_ADJUSTRECT",
                                 "WM_CTLCOLORSTATIC"};
static_assert(std::size(names) == size_t(phase::control_colour) + 1);
struct event
{
    uint64_t ticks, kind, arg, cause, details;
    bool paired = false;
};

bool dump()
{
    if (!enabled.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(recording->dump_lock);
    FILE* f = _wfopen(recording->path.c_str(), L"wb");
    if (!f) return false;
    std::fputs("{\"traceEvents\":[", f);
    bool comma = false;
    uint64_t omitted = 0;
    for (const ring& r : recording->rings)
    {
        const auto count = r.count.load(std::memory_order_acquire);
        const auto start = count > capacity ? count - capacity : 0;
        omitted += start;
        std::vector<event> events;
        std::vector<size_t> stack;
        for (auto i = start; i < count; ++i)
        {
            const auto& s = r.slots[i % capacity];
            if (s.sequence.load(std::memory_order_acquire) != i + 1)
            {
                stack.clear();
                ++omitted;
                continue;
            }
            event e{s.ticks.load(std::memory_order_relaxed), s.kind.load(std::memory_order_relaxed),
                    s.arg.load(std::memory_order_relaxed), s.cause.load(std::memory_order_relaxed),
                    s.details.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.sequence.load(std::memory_order_relaxed) != i + 1)
            {
                stack.clear();
                ++omitted;
                continue;
            }
            if (!(e.kind & 1))
                stack.push_back(events.size());
            else if (!stack.empty())
            {
                events[stack.back()].paired = e.paired = true;
                stack.pop_back();
            }
            events.push_back(e);
        }
        for (const auto& e : events)
        {
            if (!e.paired)
            {
                ++omitted;
                continue;
            }
            const double us = double(e.ticks) * 1000000.0 / double(recording->frequency.QuadPart);
            // Integer microseconds avoid locale-dependent JSON number formatting.
            std::fprintf(
                f,
                "%s{\"name\":\"%s\",\"cat\":\"rapport\",\"ph\":\"%c\",\"pid\":%lu,"
                "\"tid\":%lu,\"ts\":%llu,\"args\":{\"burst\":%llu,\"arg\":\"0x%llx\","
                "\"qpc\":%llu,\"message\":%llu,\"class\":\"%s\",\"visible\":%llu,"
                "\"inBatch\":%llu,\"inSize\":%llu,\"sizeChanged\":%llu,\"classAtom\":%llu}}",
                comma ? "," : "", names[e.kind / 2], (e.kind & 1) ? 'E' : 'B',
                GetCurrentProcessId(), r.thread.load(), static_cast<unsigned long long>(us),
                e.cause, e.arg, e.ticks, e.details & 0xffffffffull,
                std::array{"other", "STATIC", "BUTTON", "msctls_trackbar32", "EDIT",
                           "SysTabControl32", "RapportCanvas",
                           "RapportHost"}[(e.details >> 32) & 255],
                (e.details >> 40) & 1, (e.details >> 41) & 1, (e.details >> 42) & 1,
                (e.details >> 43) & 1, e.details >> 48);
            comma = true;
        }
    }
    const auto threads = recording->threads.load();
    std::fprintf(f, "],\"qpcFrequency\":%lld,\"omittedEvents\":%llu,\"unrecordedThreads\":%llu}\n",
                 recording->frequency.QuadPart, omitted,
                 threads > recording->rings.size() ? threads - recording->rings.size() : 0ull);
    const bool ok = std::ferror(f) == 0;
    return std::fclose(f) == 0 && ok;
}
}  // namespace

void initialize()
{
    static const bool initialized = []
    {
        const wchar_t* path = _wgetenv(L"RAPPORT_TRACE");
        if (!path || !*path) return false;
        auto s = std::make_unique<session>();
        s->path = path;
        if (!QueryPerformanceFrequency(&s->frequency) ||
            std::atexit([] { rapport_win32_trace_dump(); }) != 0)
            return false;
        recording = s.release();  // worker TLS and exit dump share process lifetime
        enabled.store(true, std::memory_order_release);
        return true;
    }();
    (void)initialized;
}

uint64_t current_id()
{
    return current;
}

void scope::begin(phase p, uint64_t arg, uint64_t cause, uint64_t details)
{
    std::atomic_thread_fence(std::memory_order_acquire);  // session published at initialize()
    parent_ = current;
    id_ = cause     ? cause
          : current ? current
                    : recording->next_id.fetch_add(1, std::memory_order_relaxed);
    current = id_;
    phase_ = p;
    details_ = details;
    if (p == phase::window_move) details_ = window_details(reinterpret_cast<HWND>(arg));
    if (p == phase::move_pass) ++moving;
    // Multiple canvases can submit within one layout, even within one burst.
    arg_ = p == phase::readback_submit ? recording->next_id.fetch_add(1, std::memory_order_relaxed)
                                       : arg;
    write(p, false, arg_, id_, details_);
}
void scope::end()
{
    write(phase_, true, arg_, id_, details_);
    if (phase_ == phase::move_pass) --moving;
    current = parent_;
}
void mark(phase p, uint64_t arg)
{
    scope point(p, arg);
}

namespace
{
// Hooks exist only on traced UI threads. Fixed storage avoids allocation in dispatch.
thread_local std::array<std::optional<scope>, 128> messages;
thread_local size_t depth;
thread_local bool inspecting;
thread_local HHOOK before_hook, after_hook;
LRESULT CALLBACK before_message(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && !inspecting)
    {
        const auto& m = *reinterpret_cast<CWPSTRUCT*>(lp);
        inspecting = true;
        if (depth < messages.size())
        {
            auto details = window_details(m.hwnd) | m.message | (uint64_t(moving != 0) << 41) |
                           (uint64_t(sizing != 0) << 42);
            if (m.message == WM_WINDOWPOSCHANGING && m.lParam)
            {
                auto& pos = *reinterpret_cast<WINDOWPOS*>(m.lParam);
                RECT old{};
                GetWindowRect(m.hwnd, &old);
                if (!(pos.flags & SWP_NOSIZE) &&
                    (pos.cx != old.right - old.left || pos.cy != old.bottom - old.top))
                    details |= 1ull << 43;
            }
            messages[depth].emplace(phase::message, uintptr_t(m.hwnd), 0, details);
        }
        ++depth;
        if (m.message == WM_SIZE) ++sizing;
        inspecting = false;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK after_message(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && !inspecting)
    {
        const auto& m = *reinterpret_cast<CWPRETSTRUCT*>(lp);
        inspecting = true;
        if (m.message == WM_SIZE && sizing) --sizing;
        if (depth && --depth < messages.size()) messages[depth].reset();
        inspecting = false;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}  // namespace
void install_message_hooks()
{
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (before_hook) return;
    HHOOK before = SetWindowsHookExW(WH_CALLWNDPROC, before_message, nullptr, GetCurrentThreadId());
    HHOOK after =
        SetWindowsHookExW(WH_CALLWNDPROCRET, after_message, nullptr, GetCurrentThreadId());
    if (!before || !after)
    {
        if (before) UnhookWindowsHookEx(before);
        if (after) UnhookWindowsHookEx(after);
        return;
    }
    before_hook = before;
    after_hook = after;
}
void remove_message_hooks()
{
    if (before_hook) UnhookWindowsHookEx(before_hook);
    if (after_hook) UnhookWindowsHookEx(after_hook);
    before_hook = after_hook = nullptr;
}
}  // namespace rap::win32::trace

extern "C" __declspec(dllexport) bool rapport_win32_trace_dump()
{
    return rap::win32::trace::dump();
}
