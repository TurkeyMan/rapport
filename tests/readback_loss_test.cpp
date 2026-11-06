#include "gpu_internal.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <future>

using namespace rap::gpu;

int main()
{
    for (bool texture : {false, true})
    {
        auto dev = device::headless();
        if (!dev) return 77;
        std::array<std::byte, 4> out;
        out.fill(std::byte{0xac});
        const auto before = out;
        std::promise<void> retired;
        auto called = retired.get_future();
        auto done = [&] { retired.set_value(); };
        ticket copy;
        if (texture)
        {
            auto t = dev->create({1, 1, format::rgba8});
            if (!t) return 1;
            fail_next_readback_map();
            copy = dev->readback(t, out, 4, 0, done);
        }
        else
        {
            auto b = dev->create_buffer(out.size());
            if (!b) return 1;
            fail_next_readback_map();
            copy = dev->readback(b, out, done);
        }
        if (called.wait_for(std::chrono::seconds(10)) != std::future_status::ready ||
            !copy.done() || !dev->lost() || out != before)
        {
            std::fprintf(stderr, "failed %s readback contract\n", texture ? "texture" : "buffer");
            return 1;
        }
    }
}
