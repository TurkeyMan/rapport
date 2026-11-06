#include <rapport/sys.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    using namespace std::chrono_literals;
    const bool self = std::string_view(argv[1]) == "self";
    const auto dir = rap::sys::path_from_utf8(argv[2]);
    std::unique_ptr<rap::sys::directory_watch> watch;
    std::atomic<bool> armed = false, called = false;
    auto result = rap::sys::directory_watch::start(dir, {}, [&](auto)
    {
        if (!armed.load()) return;
        if (self) watch.reset();
        called = true;
    });
    if (!result) return 2;
    watch = std::move(*result);
    armed = true;
    for (int i = 0; i < 100 && !called; ++i)
    {
        std::ofstream(dir / "wake") << i;
        std::this_thread::sleep_for(30ms);
    }
    if (self) std::_Exit(2);  // the contract check should already have terminated this child
    watch.reset();
    return called ? 0 : 1;
}
