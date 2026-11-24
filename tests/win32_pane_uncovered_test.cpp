// What a scroll pane uncovers when a layout pass moves it is painted. In a
// layout pass a pane's host moves without redrawing (SWP_NOREDRAW, its panel
// drawn off screen later, paint_pane), and Windows then leaves the strip of
// its parent it moved off as it was: the pane's old text stayed on screen
// behind whatever took that place (Offcut's right panel, a list and a row of
// buttons over a Properties pane, after its divider moved down or the window
// was resized). After each move, the window on screen must be what painting
// it all again gives.
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
#endif

namespace
{
int failures;

void pump()
{
    for (int round = 0; round < 20; ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

// The window's client area as on screen (it's in front, topmost).
std::vector<DWORD> on_screen(HWND root, int& w, int& h)
{
    RECT c;
    GetClientRect(root, &c);
    MapWindowPoints(root, nullptr, reinterpret_cast<POINT*>(&c), 2);
    w = c.right - c.left, h = c.bottom - c.top;
    DwmFlush();
    std::vector<DWORD> out(size_t(w) * h);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, c.left, c.top, SRCCOPY);
    GdiFlush();
    std::memcpy(out.data(), bits, out.size() * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    for (DWORD& p : out) p &= 0xffffff;
    return out;
}

// On screen as now, and as once everything's painted again.
void expect_painted(HWND root, const char* after)
{
    int w = 0, h = 0, w2 = 0, h2 = 0;
    const auto now = on_screen(root, w, h);
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    pump();
    const auto repainted = on_screen(root, w2, h2);
    if (w != w2 || h != h2) return;
    size_t differ = 0;
    RECT box{w, h, 0, 0};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (now[size_t(y) * w + x] != repainted[size_t(y) * w + x])
            {
                ++differ;
                box = {std::min<LONG>(box.left, x), std::min<LONG>(box.top, y), std::max<LONG>(box.right, x + 1),
                       std::max<LONG>(box.bottom, y + 1)};
            }
    if (!differ) return;
    std::fprintf(stderr, "failed: after %s, %zu pixels on screen aren't as painted (within %ld,%ld-%ld,%ld)\n", after,
                 differ, box.left, box.top, box.right, box.bottom);
    ++failures;
}

struct rows : rap::list_model
{
    size_t row_count() const override { return 4; }
    std::string text(size_t row, size_t) const override { return "layer " + std::to_string(row); }
};
}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using namespace rap::layout;
    rows model;
    rap::splitter* split = nullptr;
    // The pane full of text, so whatever of it stays behind shows.
    vbox properties{label{"PROPERTIES — tortilla-master.png"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"},
                    label{"28 pixels — a property of the layer, set here"}};
    auto win = (*ui)->create(window{
        "Rapport pane uncovered",
        {splitter{rap::orientation::vertical,
                  vbox{
                      label{"ARTWORK"},
                      list{{{"Name", {}, true}}, &model}.headers(false).stretch(),
                      hbox{button{"+"}, button{"-"}, spacer{}, button{"Flatten"}},
                  },
                  scroll{std::move(properties)}.stretch()}
             .thin()
             .bind(split)}});
    HWND root = FindWindowW(nullptr, L"Rapport pane uncovered");
    if (!root || !split) return 2;
    win->show();
    pump();
    // On the desktop (a CI machine's is small, and pixels off it can't be
    // read), or RAPPORT_TEST_AT=x,y (away from someone's work); in front of
    // everything, so its pixels on screen are its own.
    int x = 0, y = 0;
    if (const char* at = std::getenv("RAPPORT_TEST_AT")) std::sscanf(at, "%d,%d", &x, &y);
    SetWindowPos(root, HWND_TOPMOST, x, y, 420, 560, SWP_NOACTIVATE);
    pump();
    expect_painted(root, "showing");

    // The divider down, a step at a time (a drag): the top pane's button row
    // moves onto where the pane's text was.
    auto sizes = split->sizes();
    if (sizes.size() != 2) return 2;
    for (int step = 1; step <= 4; ++step)
    {
        split->set_sizes({sizes[0] + 30.f * step, sizes[1] - 30.f * step});
        pump();
        const auto now = split->sizes();
        if (now.size() != 2 || now[0] < sizes[0] + 30.f * step - 2)
        {
            std::fprintf(stderr, "the divider didn't move (step %d: %.1f, not %.1f)\n", step,
                         now.empty() ? -1.f : now[0], sizes[0] + 30.f * step);
            return 2;
        }
        char why[64];
        std::snprintf(why, sizeof why, "the divider moved down (step %d)", step);
        expect_painted(root, why);
    }
    // And back up.
    split->set_sizes({sizes[0], sizes[1]});
    pump();
    expect_painted(root, "the divider moved up");

    // The window resized, taller then shorter: the pane stretches, and moves
    // if the top one changes (its rows by lines).
    RECT r;
    GetWindowRect(root, &r);
    for (int i = 0; i < 4; ++i)
    {
        SetWindowPos(root, nullptr, 0, 0, r.right - r.left + (i % 2 ? 0 : 60), r.bottom - r.top + (i % 2 ? -40 : 80),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
        expect_painted(root, "a resize");
    }

    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
