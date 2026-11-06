#include "gpu_internal.h"

#include <cstdio>
#include <limits>

int main()
{
    using namespace rap::gpu;
    const auto huge = std::numeric_limits<size_t>::max();
    const bool ok =
        upload_fits(16, {}, 2, 2) && !upload_fits(15, {}, 2, 2) &&
        !upload_fits(0, {}, 1, 1) && upload_fits(20, {.stride = 12}, 2, 2) &&
        !upload_fits(16, {.stride = 7}, 2, 2) &&
        rows_fit(4, 4, huge, 1) && !rows_fit(4, 4, huge, 2);
    if (!ok) std::fputs("unexpected GPU transfer layout result\n", stderr);
    return ok ? 0 : 1;
}
