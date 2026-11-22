// What reads or acts on a control's laid-out geometry sees the layout as it
// will be, not as it was (#108). A label above a list (a tree, a splitter)
// grows to several lines: a layout's asked for (posted) that will shrink
// the list. Right then, outside any layout:
// - the list's scroll_to(99, end) puts row 99 at the rows' bottom as they'll
//   be (was: as they were, and the shrink left it out of view);
// - its row_offset reads as it'll be laid out;
// - its set_row_offset puts the row where it's asked, as it'll be;
// - a tree's select scrolls the item into view as it'll be;
// - a splitter's sizes() are as they'll be.
// Within a layout (from a canvas's on_resize, the layout shrinking the
// list): scroll_to is made once the layout's done (rows as laid out then),
// the latest asked for only (scroll_to(99, end) then scroll_to(50,
// nearest): from where it was, 50 goes to the bottom; made in turn, from
// 99, it'd go to the top), and one made there and then after it (outside
// the layout, before what was put off is run) is the one that stays.
// Shown without activating.
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

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
HWND find_class(HWND top, const wchar_t* cls)
{
    struct find
    {
        const wchar_t* cls;
        HWND found = nullptr;
    } f{cls};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(p);
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (std::wcscmp(c, f->cls)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
std::string num(std::optional<float> v)
{
    return v ? std::to_string(int(std::lround(*v))) : std::string("none (out of view)");
}
struct rows_model : rap::list_model
{
    size_t row_count() const override { return 100; }
    std::string text(size_t r, size_t) const override { return "Row " + std::to_string(r); }
};
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    using namespace rap::layout;
    using namespace rap::literals;
    rows_model model;
    std::string many;
    for (int i = 0; i < 8; ++i) many += "A longer line of text\n";

    // A window: a canvas (its on_resize the in-layout calls), the label, the
    // list. `grown` true: the label's grown and laid out already (the
    // reference: where things land as laid out).
    struct list_window
    {
        std::unique_ptr<rap::window> win;
        rap::label* lab = nullptr;
        rap::list* list = nullptr;
        std::function<void()> in_layout;
        HWND top = nullptr;
    };
    auto make = [&](list_window& w)
    {
        w.win = (*ui)->create(window{"Layout current", {vbox{canvas{}
                                                                  .on_resize(
                                                                      [&w](rap::size_f)
                                                                      {
                                                                          if (auto f = std::exchange(w.in_layout, nullptr))
                                                                              f();
                                                                      })
                                                                  .height(20_px),
                                                              label{"One line"}.bind(w.lab),
                                                              list{{{"Name", {}, true}}, &model}.bind(w.list).stretch()}}}
                                  .size(300_px, 420_px));
        w.top = w.win ? FindWindowW(nullptr, L"Layout current") : nullptr;
        if (!w.top || !w.lab || !w.list) return false;
        w.win->show();
        pump();
        w.list->scroll_to(0, rap::scroll_align::start);
        pump();
        return true;
    };

    // The reference: row 99 at the bottom, row 50 at the bottom (nearest
    // from the top), as laid out with the label grown.
    float bottom99 = -1, bottom50 = -1;
    {
        list_window w;
        expect(make(w), "the reference: the window and its list");
        if (!w.top) return 1;
        w.lab->set_text(many);
        pump();
        w.list->scroll_to(99, rap::scroll_align::end);
        pump();
        bottom99 = w.list->row_offset(99).value_or(-1);
        w.list->scroll_to(0, rap::scroll_align::start);
        pump();
        w.list->scroll_to(50, rap::scroll_align::nearest);
        pump();
        bottom50 = w.list->row_offset(50).value_or(-1);
        expect(bottom99 > 0 && bottom50 > 0, "the reference: rows 99 and 50 at the bottom as laid out, " +
                                                 num(bottom99) + ", " + num(bottom50));
        w.win.reset();
        pump();
    }

    // Outside a layout, one to come.
    {
        list_window w;
        expect(make(w), "pending: the window and its list");
        if (!w.top) return 1;
        w.lab->set_text(many);  // the list will shrink: posted
        w.list->scroll_to(99, rap::scroll_align::end);
        const auto read_now = w.list->row_offset(99);
        pump();
        const auto after = w.list->row_offset(99);
        expect(after && std::lround(*after) == std::lround(bottom99),
               "scroll_to with a layout to come: row 99 at the rows' bottom as laid out, " + num(after) + ", laid out " +
                   num(bottom99));
        expect(read_now && after && std::lround(*read_now) == std::lround(*after),
               "row_offset with a layout to come: as it'll be laid out, " + num(read_now) + ", after " + num(after));
        w.win.reset();
        pump();
    }
    {
        list_window w;
        expect(make(w), "pending, set_row_offset: the window and its list");
        if (!w.top) return 1;
        w.lab->set_text(many);
        w.list->set_row_offset(99, bottom99);
        pump();
        const auto after = w.list->row_offset(99);
        expect(after && std::lround(*after) == std::lround(bottom99),
               "set_row_offset with a layout to come: where it's asked as laid out, " + num(after) + ", asked " +
                   num(bottom99));
        w.win.reset();
        pump();
    }

    // Within a layout: the window resized smaller (its layout), the canvas
    // above the list asking as it's placed, the list not yet.
    {
        list_window w;
        expect(make(w), "within a layout: the window and its list");
        if (!w.top) return 1;
        w.lab->set_text(many);
        pump();
        RECT now;
        GetWindowRect(w.top, &now);
        // The window shorter by 40 px (and narrower, so the canvas is sized
        // again in its layout): row 99's bottom moves up by as much.
        w.in_layout = [&] { w.list->scroll_to(99, rap::scroll_align::end); };
        SetWindowPos(w.top, nullptr, 0, 0, now.right - now.left - 10, now.bottom - now.top - 40,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
        expect(!w.in_layout, "within a layout: the canvas's on_resize asked");
        const auto after = w.list->row_offset(99);
        // The rows' area as laid out now: the list's client area under its
        // header; row 99's bottom on its bottom.
        const HWND lv = find_class(w.top, WC_LISTVIEWW);
        RECT lc{}, hr{}, ir{};
        GetClientRect(lv, &lc);
        if (HWND hd = ListView_GetHeader(lv)) GetWindowRect(hd, &hr);
        ListView_GetItemRect(lv, 99, &ir, LVIR_BOUNDS);
        const float s = float(GetDpiForWindow(w.top)) / 96.f;
        const float rows_area = float(lc.bottom - (hr.bottom - hr.top)) / s, row = float(ir.bottom - ir.top) / s;
        // (ListView scrolls by whole rows: its bottom within a row of the area's.)
        expect(after && *after + row <= rows_area + 0.5f && *after + row > rows_area - row,
               "scroll_to within a layout: made once it's done, row 99 at the rows' bottom as laid out then, " +
                   num(after) + " + " + num(row) + ", the rows' area " + num(rows_area));
        w.win.reset();
        pump();
    }
    {
        list_window w;
        expect(make(w), "within a layout, two asked: the window and its list");
        if (!w.top) return 1;
        w.lab->set_text(many);
        pump();
        w.in_layout = [&]
        {
            w.list->scroll_to(99, rap::scroll_align::end);
            w.list->scroll_to(50, rap::scroll_align::nearest);  // the latest
        };
        RECT now;
        GetWindowRect(w.top, &now);
        SetWindowPos(w.top, nullptr, 0, 0, now.right - now.left + 20, now.bottom - now.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
        expect(!w.in_layout, "two within a layout: the canvas's on_resize asked");
        const auto at50 = w.list->row_offset(50);
        expect(at50 && std::lround(*at50) == std::lround(bottom50),
               "two within a layout: only the latest made, row 50 at the bottom from where it was, " + num(at50) +
                   " (made in turn, from 99, it'd be at the top, 0)");
        w.win.reset();
        pump();
    }
    {
        list_window w;
        expect(make(w), "within a layout, then one there and then: the window and its list");
        if (!w.top) return 1;
        w.lab->set_text(many);
        pump();
        w.in_layout = [&] { w.list->scroll_to(99, rap::scroll_align::end); };
        RECT now;
        GetWindowRect(w.top, &now);
        SetWindowPos(w.top, nullptr, 0, 0, now.right - now.left + 20, now.bottom - now.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        expect(!w.in_layout, "one put off: the canvas's on_resize asked");
        w.list->scroll_to(0, rap::scroll_align::start);  // after it, before what was put off runs
        pump();
        const auto at0 = w.list->row_offset(0);
        expect(at0 && std::lround(*at0) == 0,
               "one put off, then one there and then: the later stays, row 0 at the top, " + num(at0));
        w.win.reset();
        pump();
    }

    // A tree's select, a layout to come.
    {
        std::vector<rap::tree_item> items;
        for (int i = 0; i < 100; ++i) items.push_back({"Item " + std::to_string(i), "i" + std::to_string(i)});
        rap::label* lab = nullptr;
        rap::tree* tr = nullptr;
        auto win = (*ui)->create(
            window{"Layout current tree", {vbox{label{"One line"}.bind(lab), tree{items}.bind(tr).stretch()}}}.size(
                300_px, 400_px));
        HWND top = win ? FindWindowW(nullptr, L"Layout current tree") : nullptr;
        HWND tv = top ? find_class(top, WC_TREEVIEWW) : nullptr;
        expect(tv && lab && tr, "a tree: the window and its tree");
        if (!tv) return 1;
        win->show();
        pump();
        lab->set_text(many);
        tr->select("i99");
        pump();
        RECT ir{}, cr{};
        TreeView_GetItemRect(tv, TreeView_GetSelection(tv), &ir, FALSE);
        GetClientRect(tv, &cr);
        expect(tr->selected() == std::optional<std::string>("i99") && ir.top >= 0 && ir.bottom <= cr.bottom,
               "a tree's select with a layout to come: the item in view as laid out, " + std::to_string(ir.top) + ".." +
                   std::to_string(ir.bottom) + " in 0.." + std::to_string(cr.bottom));
        win.reset();
        pump();
    }

    // A splitter's sizes(), a layout to come.
    {
        rap::label* lab = nullptr;
        rap::splitter* sp = nullptr;
        auto win = (*ui)->create(window{"Layout current split",
                                        {vbox{label{"One line"}.bind(lab),
                                              splitter{rap::orientation::vertical, canvas{}.stretch(), canvas{}.stretch()}
                                                  .bind(sp)
                                                  .stretch()}}}
                                     .size(300_px, 400_px));
        expect(win && lab && sp, "a splitter: the window and its splitter");
        if (!win || !lab || !sp) return 1;
        win->show();
        pump();
        lab->set_text(many);
        const std::vector<float> now = sp->sizes();
        pump();
        const std::vector<float> after = sp->sizes();
        expect(now.size() == 2 && now == after, "a splitter's sizes() with a layout to come: as they'll be, " +
                                                    (now.size() == 2 ? num(now[0]) + "," + num(now[1]) : "?") +
                                                    ", after " +
                                                    (after.size() == 2 ? num(after[0]) + "," + num(after[1]) : "?"));
        win.reset();
        pump();
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
