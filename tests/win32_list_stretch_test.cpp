// A list's stretch columns follow its width while the user hasn't sized a
// column: laid out narrow, then widened, a stretch column that isn't the
// last widens too (it took its share once, at the first size, and kept it
// at any later one). Once the user starts sizing a column (a header divider
// dragged), the widths are theirs: the window resized, the stretch column
// keeps its width. Widths restored with set_column_widths are kept as well.
#include <rapport/rapport.h>
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <cmath>
#include <cstdio>
#include <string>
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

struct rows : rap::list_model
{
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t column) const override
    {
        return column == 0 ? "artifact-" + std::to_string(row) : "5.4 MB";
    }
};

std::vector<rap::column> columns()
{
    // The stretch column first, as an artifact list has it: the last one
    // fills by other means.
    return {{.title = "Name", .stretch = true}, {.title = "Size"}, {.title = "Where"}};
}

// The list_view in the window titled `title`.
HWND list_view(const wchar_t* title)
{
    HWND top = FindWindowW(L"rapportWindow", title);
    HWND found = nullptr;
    if (!top) return nullptr;
    EnumChildWindows(
        top,
        [](HWND h, LPARAM l)
        {
            wchar_t cls[32];
            GetClassNameW(h, cls, 32);
            if (std::wstring(cls) != WC_LISTVIEWW) return TRUE;
            *reinterpret_cast<HWND*>(l) = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// What a header divider dragged by the user sends first, as the list_view
// passes it on to its parent.
void begin_track(HWND lv)
{
    HWND header = ListView_GetHeader(lv);
    NMHEADERW n{};
    n.hdr.hwndFrom = header;
    n.hdr.idFrom = UINT_PTR(GetDlgCtrlID(header));
    n.hdr.code = HDN_BEGINTRACKW;
    n.iItem = 1;
    n.iButton = 0;
    SendMessageW(GetParent(lv), WM_NOTIFY, n.hdr.idFrom, reinterpret_cast<LPARAM>(&n));
}

float width(rap::list* l, size_t c)
{
    const auto w = l->column_widths();
    return c < w.size() ? w[c] : 0;
}
}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using namespace rap::layout;
    char why[200];

    {
        rows model;
        rap::list* l = nullptr;
        auto win = (*ui)->create(window{"List stretch", {vbox{list{columns(), &model}.bind(l).stretch()}}}.size(
            30_ch, 12_lines));
        win->show();
        pump();
        HWND lv = list_view(L"List stretch");
        expect(lv && l, "the list is there");
        if (!lv || !l) return 1;

        const float narrow = width(l, 0);
        win->resize({win->size().w * 3, win->size().h});
        pump();
        const float wide = width(l, 0);
        std::snprintf(why, sizeof why, "widened, the stretch column widens (%.0f, then %.0f)", narrow, wide);
        expect(wide > narrow + 50, why);

        win->resize({win->size().w / 2, win->size().h});
        pump();
        const float back = width(l, 0);
        std::snprintf(why, sizeof why, "narrowed, it narrows again (%.0f, then %.0f)", wide, back);
        expect(back < wide - 20, why);

        // The user takes the widths over.
        begin_track(lv);
        pump();
        const float held = width(l, 0);
        win->resize({win->size().w * 2, win->size().h});
        pump();
        std::snprintf(why, sizeof why, "after the user sizes a column, the stretch column keeps its width (%.0f, then %.0f)",
                      held, width(l, 0));
        expect(std::abs(width(l, 0) - held) < 1, why);
    }

    {
        // Restored widths are kept, the stretch column's included.
        rows model;
        rap::list* l = nullptr;
        auto win = (*ui)->create(window{"List stretch, restored", {vbox{list{columns(), &model}.bind(l).stretch()}}}
                                     .size(30_ch, 12_lines));
        l->set_column_widths({90, 60, 60});
        win->show();
        pump();
        win->resize({win->size().w * 3, win->size().h});
        pump();
        std::snprintf(why, sizeof why, "a restored stretch column keeps its width (90, then %.0f)", width(l, 0));
        expect(std::abs(width(l, 0) - 90) < 1, why);
    }

    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
