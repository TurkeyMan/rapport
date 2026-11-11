// A splitter's sizes() are the panes' sizes as laid out, as GTK's and Qt's
// are (#29): after set_sizes and a resize, the pane that takes what the
// window gains reports its new size, and the sizes still add up to the
// splitter. Before any layout, and between set_sizes and the layout it
// asks for, sizes() is what was asked for. A size of 0 to set_sizes leaves
// that pane as it is now, not as it was last asked for. A hidden pane
// reports the size it comes back at. The window is never shown: Win32 lays
// out a hidden window as a shown one.
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
int failures;
void expect(bool ok, const char* why)
{
    if (!ok)
    {
        std::fprintf(stderr, "failed: %s\n", why);
        ++failures;
    }
}

void pump()
{
    for (int round = 0; round < 10; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

float sum(const std::vector<float>& v)
{
    float s = 0;
    for (float x : v) s += x;
    return s;
}

// Within a pixel at the window's scale (sizes round to device pixels).
bool close_to(float got, float want, float scale) { return std::fabs(got - want) <= 1.5f / scale; }

void print(const char* what, const std::vector<float>& v)
{
    std::fprintf(stderr, "  %s:", what);
    for (float x : v) std::fprintf(stderr, " %.1f", x);
    std::fprintf(stderr, "\n");
}

// Resizes the root for a client `w` device pixels wide, and gives the width
// it got: Windows caps a window at about the screen's size, and a CI
// runner's screen is small.
int client_width(HWND root, int w)
{
    RECT c, r;
    GetClientRect(root, &c);
    GetWindowRect(root, &r);
    const int extra = (r.right - r.left) - c.right;
    SetWindowPos(root, nullptr, 0, 0, w + extra, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump();
    GetClientRect(root, &c);
    return c.right;
}
}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using namespace rap::layout;
    rap::splitter *two = nullptr, *three = nullptr;
    rap::label* middle = nullptr;
    // Never shown: nothing reaches the screen.
    auto win = (*ui)->create(window{
        "Splitter sizes",
        {vbox{
            splitter{rap::orientation::horizontal, label{"a"}.stretch(), label{"b"}}.bind(two).stretch(),
            splitter{rap::orientation::horizontal, label{"a"}.stretch(), label{"b"}.bind(middle), label{"c"}}
                .bind(three)
                .stretch(),
        }}});
    HWND root = FindWindowW(nullptr, L"Splitter sizes");
    if (!root || !two || !three || !middle) return 2;
    expect(!IsWindowVisible(root), "the test window is never shown");
    const float scale = float(GetDpiForWindow(root)) / 96.f;
    char why[200];

    // Before any layout: what was asked for.
    two->set_sizes({300, 100});
    {
        const auto s = two->sizes();
        expect(s.size() == 2 && s[0] == 300 && s[1] == 100, "before layout, sizes() is what was asked for");
    }

    // Tortilla's forge panel: sizes restored, then the window grows. The
    // pane that stretches takes it, and sizes() says so.
    const int narrow = client_width(root, int(std::lround(400 * scale)));
    const auto narrowed = two->sizes();
    const int wide = client_width(root, int(std::lround(600 * scale)));
    const auto widened = two->sizes();
    const float grown = float(wide - narrow) / scale;
    std::snprintf(why, sizeof why, "the window grows at least 100 (by %.1f, from %d to %d px)", grown, narrow, wide);
    expect(grown >= 100, why);
    expect(narrowed.size() == 2 && widened.size() == 2, "two panes, two sizes");
    if (narrowed.size() == 2 && widened.size() == 2)
    {
        std::snprintf(why, sizeof why, "the window %.1f wider, the sizes add up to that more (%.1f, then %.1f)",
                      grown, sum(narrowed), sum(widened));
        expect(close_to(sum(widened) - sum(narrowed), grown, scale), why);
        expect(close_to(widened[0] - narrowed[0], grown, scale), "the stretching pane takes what the window gains");
        expect(close_to(widened[1], 100, scale), "the other keeps the size it was given");
        if (failures)
        {
            print("narrow", narrowed);
            print("wide", widened);
        }
    }

    // Between set_sizes and its layout: what was asked for. After it: laid out.
    two->set_sizes({0, 200});
    {
        const auto s = two->sizes();
        std::snprintf(why, sizeof why, "set_sizes 0 keeps the pane's size now (%.1f, not 300)",
                      s.empty() ? -1.f : s[0]);
        expect(s.size() == 2 && close_to(s[0], widened.empty() ? 0 : widened[0], scale), why);
        expect(s.size() == 2 && s[1] == 200, "until laid out, the size asked for");
    }
    pump();
    {
        const auto s = two->sizes();
        expect(s.size() == 2 && close_to(s[1], 200, scale), "laid out at the size asked for");
        std::snprintf(why, sizeof why, "laid out, the sizes still fill the splitter (%.1f, not %.1f)", sum(s),
                      sum(widened));
        expect(close_to(sum(s), sum(widened), scale), why);
    }

    // A hidden pane reports the size it comes back at, and comes back at it.
    three->set_sizes({0, 150, 120});
    pump();
    middle->set_visible(false);
    pump();
    {
        const auto s = three->sizes();
        std::snprintf(why, sizeof why, "a hidden pane reports the size it comes back at (%.1f)",
                      s.size() == 3 ? s[1] : -1.f);
        expect(s.size() == 3 && close_to(s[1], 150, scale), why);
        expect(s.size() == 3 && close_to(s[2], 120, scale), "the panes still shown keep theirs");
    }
    middle->set_visible(true);
    pump();
    {
        const auto s = three->sizes();
        expect(s.size() == 3 && close_to(s[1], 150, scale), "shown again, it's back at that size");
    }

    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
