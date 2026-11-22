// ui::post's functions are destroyed, never leaked (backend.h, post). Each
// posted function captures a tracker that counts its own destruction, and
// whether it ran.
// - Posted past the thread's queue limit (USERPostMessageLimit, read from
//   the registry, 10000 by default; the UI thread not pumping meanwhile):
//   the ones that couldn't be delivered are destroyed there and then,
//   without running; once pumped, every one is either run or destroyed so.
// - Posted, then the backend torn down before they ran (the ui destroyed,
//   nothing pumped): every one destroyed, none run.
// - Posted from another thread during the teardown, after what was queued
//   is taken out and before the message window goes (a worker held at a
//   barrier until then, through the backend's private seam
//   rapport_win32_set_teardown_hook), through a copy of the backend's own
//   way of posting (rapport_win32_post_handle, as its path icon finders
//   hold one: the backend itself mustn't be called into as it's destroyed):
//   destroyed, not run, not left in the queue to be dropped with the
//   window.
// Never shown; no windows.
#include <rapport/rapport.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" void rapport_win32_set_teardown_hook(void (*)(void*), void*);
extern "C" void* rapport_win32_post_handle();
#endif

namespace
{
int fails = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}
void pump()
{
    for (int round = 0; round < 50; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
}
std::atomic<int> destroyed = 0, ran = 0;
// What a posted function owns: counted as it goes.
struct tracker
{
    ~tracker() { ++destroyed; }
};
std::function<void()> job()
{
    auto t = std::make_shared<tracker>();
    return [t] { ++ran; };
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
        if (!ui)
        {
            std::printf("no win32 backend: %s\n", ui.error().c_str());
            return 77;
        }
        // Past the queue's limit, nothing pumped.
        destroyed = ran = 0;
        // 500 past the thread's queue limit (USERPostMessageLimit, 10000
        // unless set otherwise).
        DWORD limit = 10000, size = sizeof limit;
        RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                     L"USERPostMessageLimit", RRF_RT_REG_DWORD, nullptr, &limit, &size);
        const int posts = int(limit) + 500;
        for (int i = 0; i < posts; ++i) (*ui)->post(job());
        const int at_once = destroyed;
        expect(at_once > 0 && ran == 0, "posts past the queue's limit: those not delivered destroyed at once, not run (" +
                                            std::to_string(at_once) + " of " + std::to_string(posts) + ")");
        pump();
        expect(ran + at_once == posts && destroyed == posts,
               "pumped: every one run or destroyed undelivered, every one destroyed (" + std::to_string(ran) + " run, " +
                   std::to_string(destroyed) + " destroyed, of " + std::to_string(posts) + ")");
    }
    {
        auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
        if (!ui) return 77;
        destroyed = ran = 0;
        for (int i = 0; i < 20; ++i) (*ui)->post(job());
        expect(destroyed == 0 && ran == 0, "twenty posted, nothing pumped: none run or destroyed yet");
        ui->reset();  // the backend torn down
        expect(destroyed == 20 && ran == 0,
               "the backend torn down: every post still queued destroyed, none run (" + std::to_string(destroyed) +
                   " destroyed, " + std::to_string(ran) + " run)");
        pump();  // what's left in the queue, if anything, goes nowhere
        expect(destroyed == 20 && ran == 0, "after, pumped: still none run (" + std::to_string(ran) + ")");
    }
    {
        auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
        if (!ui) return 77;
        void (*set_hook)(void (*)(void*), void*) = nullptr;
        void* (*post_handle)() = nullptr;
#ifdef RAPPORT_TEST_BUILTIN_WIN32
        set_hook = rapport_win32_set_teardown_hook;
        post_handle = rapport_win32_post_handle;
#else
        if (HMODULE m = GetModuleHandleW(L"rapport_win32.dll"))
        {
            set_hook = reinterpret_cast<void (*)(void (*)(void*), void*)>(
                GetProcAddress(m, "rapport_win32_set_teardown_hook"));
            post_handle =
                reinterpret_cast<void* (*)()>(GetProcAddress(m, "rapport_win32_post_handle"));
        }
#endif
        expect(set_hook && post_handle,
               "the backend's rapport_win32_set_teardown_hook and rapport_win32_post_handle seams");
        if (!set_hook || !post_handle) return 1;
        destroyed = ran = 0;
        // The worker's way of posting: a copy of the backend's own, as its
        // path icon finders hold one, which outlives the backend (not the
        // backend itself, which mustn't be called into as it's destroyed).
        using post_fn = std::function<void(std::function<void()>)>;
        std::unique_ptr<post_fn> handle(static_cast<post_fn*>(post_handle()));
        expect(handle != nullptr, "a copy of the backend's way of posting");
        if (!handle) return 1;
        // The worker, held until the teardown's taken what was queued out.
        struct barrier
        {
            std::mutex m;
            std::condition_variable cv;
            bool go = false, done = false;
        } b;
        std::thread worker(
            [&]
            {
                std::unique_lock lock(b.m);
                b.cv.wait(lock, [&] { return b.go; });
                lock.unlock();
                (*handle)(job());  // the backend's being torn down: a copied handle's post
                lock.lock();
                b.done = true;
                b.cv.notify_all();
            });
        set_hook(
            [](void* p)
            {
                auto& b = *static_cast<barrier*>(p);
                std::unique_lock lock(b.m);
                b.go = true;
                b.cv.notify_all();
                b.cv.wait(lock, [&] { return b.done; });
            },
            &b);
        ui->reset();  // torn down: the worker posts between the drain and the window going
        set_hook(nullptr, nullptr);
        worker.join();
        pump();
        expect(destroyed == 1 && ran == 0, "posted during the teardown, after the queue's emptied: "
                                           "destroyed, not run, not left queued (" +
                                               std::to_string(destroyed) + " destroyed, " +
                                               std::to_string(ran) + " run)");
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
