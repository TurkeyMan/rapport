#include "win32_trace.h"

#include <filesystem>
#include <thread>
#include <windows.h>

int main(int argc, char** argv)
{
    namespace t = rap::win32::trace;
    t::initialize();
    if (argc == 4)
    {
        t::install_message_hooks();
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"RapportTraceTest";
        cls.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT
        {
            if (msg == WM_SIZE) SendMessageW(h, WM_PAINT, 0, 0);
            if (msg == WM_PAINT || msg == WM_PRINT) return 0;
            return DefWindowProcW(h, msg, wp, lp);
        };
        if (!RegisterClassW(&cls)) return 1;
        HWND h = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                               cls.hInstance, nullptr);
        if (!h) return 1;
        {
            t::scope pass(t::phase::move_pass);
            SendMessageW(h, WM_SIZE, 0, 0);
            SendMessageW(h, WM_PRINT, 0, 0);
            t::mark(t::phase::pane_invalidated, uintptr_t(h));
        }
        DestroyWindow(h);
        t::remove_message_hooks();
        return rapport_win32_trace_dump() ? 0 : 1;
    }
    if (argc == 2)
    {
        for (int i = 0; i < 40000; ++i)
        {
            t::scope outer(t::phase::layout, i);
            t::scope inner(t::phase::window_size, i);
        }
        return std::filesystem::exists(argv[1]) ? 1 : 0;  // only the exit dump writes
    }
    uint64_t burst;
    {
        t::scope outer(t::phase::resize, 11);
        burst = outer.id();
        t::scope inner(t::phase::layout, 12);
        std::thread worker(
            [burst]
            {
                t::scope outer(t::phase::readback_submit, 21, burst);
                t::scope inner(t::phase::readback_complete, 22);
            });
        worker.join();
    }
    if (argc == 1) return rapport_win32_trace_dump() ? 1 : 0;
    if (argc != 3 || !rapport_win32_trace_dump()) return 1;
    std::filesystem::copy_file(argv[1], argv[2], std::filesystem::copy_options::overwrite_existing);
    t::scope final(t::phase::present, 31);
}
