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
#include <algorithm>
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

// Until nothing's due: a pane is drawn when nothing else is waiting
// (paint_pane), its moved pixels on screen until then, and on a busy
// machine a fixed number of rounds caught it before that (1 run in 25).
// At least 20 rounds, then until quiet for 5 in a row, or 3 seconds.
void pump()
{
    int quiet = 0;
    for (int round = 0; round < 300 && (round < 20 || quiet < 5); ++round)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
        quiet = HIWORD(GetQueueStatus(QS_PAINT | QS_POSTMESSAGE | QS_SENDMESSAGE)) ? 0 : quiet + 1;
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

// The same pixel, but for ClearType's rounding: text printed off screen
// (paint_pane) and painted on screen can differ by a level per channel.
// Text left behind differs by far more.
bool alike(DWORD a, DWORD b)
{
    for (int shift = 0; shift < 24; shift += 8)
        if (std::abs(int((a >> shift) & 0xff) - int((b >> shift) & 0xff)) > 2) return false;
    return true;
}

// On screen once it holds still: a themed control's state change fades in
// over a few frames (a button's, a scroll bar's arrows), and caught midway
// one capture differed from the next. Pixels left behind hold still, so
// this waits out only what's still changing: two captures 50ms apart the
// same, or 2 seconds.
std::vector<DWORD> settled(HWND root, int& w, int& h)
{
    auto last = on_screen(root, w, h);
    for (int tries = 0; tries < 40; ++tries)
    {
        // Its timers run on this thread.
        for (int round = 0; round < 5; ++round)
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        int w2 = 0, h2 = 0;
        auto next = on_screen(root, w2, h2);
        if (w2 == w && h2 == h && next == last) break;
        last = std::move(next);
        w = w2, h = h2;
    }
    return last;
}

// Whether the screen shows the window's client area: no other window over
// any of it, sampled on a grid. Topmost isn't enough: another topmost
// window (a desktop app someone's using) can come over it, and then both
// captures were of that window, differing only where it animated.
bool in_front(HWND root)
{
    RECT c;
    GetClientRect(root, &c);
    for (int i = 0; i <= 8; ++i)
        for (int j = 0; j <= 8; ++j)
        {
            POINT p{std::min<LONG>(c.right - 1, c.right * i / 8), std::min<LONG>(c.bottom - 1, c.bottom * j / 8)};
            ClientToScreen(root, &p);
            if (GetAncestor(WindowFromPoint(p), GA_ROOT) != root) return false;
        }
    return true;
}

// Brought in front if it isn't (without activating it: the DWM keeps a
// covered window's pixels, so nothing repaints). False if it can't be.
bool bring_in_front(HWND root)
{
    for (int tries = 0; tries < 3; ++tries)
    {
        if (in_front(root)) return true;
        SetWindowPos(root, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        DwmFlush();
    }
    return in_front(root);
}

int checked, covered;

// On screen as now, and as once everything's painted again.
void expect_painted(HWND root, const char* after)
{
    int w = 0, h = 0, w2 = 0, h2 = 0;
    if (!bring_in_front(root))
    {
        std::fprintf(stderr, "not checked after %s: another window is over this one\n", after);
        ++covered;
        return;
    }
    const auto now = settled(root, w, h);
    const bool was_in_front = in_front(root);
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    pump();
    if (!was_in_front || !bring_in_front(root))
    {
        std::fprintf(stderr, "not checked after %s: another window came over this one\n", after);
        ++covered;
        return;
    }
    const auto repainted = settled(root, w2, h2);
    if (!in_front(root))
    {
        std::fprintf(stderr, "not checked after %s: another window came over this one\n", after);
        ++covered;
        return;
    }
    ++checked;
    if (w != w2 || h != h2) return;
    size_t differ = 0;
    RECT box{w, h, 0, 0};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!alike(now[size_t(y) * w + x], repainted[size_t(y) * w + x]))
            {
                ++differ;
                box = {std::min<LONG>(box.left, x), std::min<LONG>(box.top, y), std::max<LONG>(box.right, x + 1),
                       std::max<LONG>(box.bottom, y + 1)};
            }
    if (!differ) return;
    std::fprintf(stderr, "failed: after %s, %zu pixels on screen aren't as painted (within %ld,%ld-%ld,%ld)\n", after,
                 differ, box.left, box.top, box.right, box.bottom);
    ++failures;
    // RAPPORT_TEST_DUMP=dir: both, as dir/<n>-before.bmp and -after.bmp.
    if (const char* dir = std::getenv("RAPPORT_TEST_DUMP"))
    {
        auto save = [&](const std::vector<DWORD>& px, const char* which)
        {
            char path[512];
            std::snprintf(path, sizeof path, "%s/%d-%s.bmp", dir, failures, which);
            FILE* f = std::fopen(path, "wb");
            if (!f) return;
            BITMAPINFOHEADER bi{sizeof bi, w, -h, 1, 32, BI_RGB};
            BITMAPFILEHEADER fh{0x4d42, DWORD(sizeof fh + sizeof bi + px.size() * 4), 0, 0, DWORD(sizeof fh + sizeof bi)};
            std::fwrite(&fh, sizeof fh, 1, f);
            std::fwrite(&bi, sizeof bi, 1, f);
            std::fwrite(px.data(), 4, px.size(), f);
            std::fclose(f);
        };
        save(now, "before");
        save(repainted, "after");
    }
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
    // Shown and moved without an animation: captured while it faded in, the
    // whole window differed.
    const BOOL no_transitions = TRUE;
    DwmSetWindowAttribute(root, DWMWA_TRANSITIONS_FORCEDISABLED, &no_transitions, sizeof no_transitions);
    win->show();
    pump();
    // On the desktop (a CI machine's is small, and pixels off it can't be
    // read), or RAPPORT_TEST_AT=x,y (away from someone's work); in front of
    // everything, so its pixels on screen are its own.
    // Not under the pointer, either: a list row it's over is hot-tracked,
    // differently from one capture to the next. The first corner of the
    // primary monitor's work area that the window (as big as it'll grow)
    // keeps clear of it.
    int x = 0, y = 0;
    {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        POINT pointer{};
        GetCursorPos(&pointer);
        const SIZE most{560, 720};
        const POINT corners[] = {{work.left, work.top},
                                 {work.right - most.cx, work.top},
                                 {work.left, work.bottom - most.cy},
                                 {work.right - most.cx, work.bottom - most.cy}};
        for (POINT c : corners)
        {
            const RECT r{c.x, c.y, c.x + most.cx, c.y + most.cy};
            if (!PtInRect(&r, pointer))
            {
                x = c.x, y = c.y;
                break;
            }
        }
    }
    if (const char* at = std::getenv("RAPPORT_TEST_AT")) std::sscanf(at, "%d,%d", &x, &y);
    // Square corners (Windows 11 rounds them): a rounded one shows what's
    // behind the window, which changes under it (1 run in 5 failed on the
    // client's bottom-right pixels). DWMWA_WINDOW_CORNER_PREFERENCE (33),
    // DWMWCP_DONOTROUND (1), by number for older SDKs; Windows 10 has neither.
    const DWORD square = 1;
    DwmSetWindowAttribute(root, 33, &square, sizeof square);
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
    if (failures) return 1;
    // Nothing could be seen: skipped (ctest's SKIP_RETURN_CODE), not passed.
    if (!checked) return 77;
    if (covered) std::fprintf(stderr, "%d of %d checks not made (covered)\n", covered, covered + checked);
    return 0;
}
