// ui::post's functions are destroyed, never leaked (backend.h, post). Each
// posted function captures a tracker that counts its own destruction, and
// whether it ran.
// - Posted past the thread's queue limit (10000 messages, Windows'
//   default, the UI thread not pumping meanwhile): the ones that couldn't
//   be delivered are destroyed there and then, without running; once
//   pumped, every one is either run or destroyed so.
// - Posted, then the backend torn down before they ran (the ui destroyed,
//   nothing pumped): every one destroyed, none run.
// Never shown; no windows.
#include <rapport/rapport.h>
#include <windows.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
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
int destroyed = 0, ran = 0;
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
        constexpr int posts = 10500;
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
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
