// A Win32 file_view list's first column, against a plain list's: on Windows
// 11 its content and its title start further in by the same inset, which is
// the list_view's own image slot, and the header stays as tall. Printed with
// WM_PRINTCLIENT, not captured. Before Windows 11 the two are the same.
//   rapport-win32-file-view-inset-test
#include <rapport/rapport.h>

#define NOMINMAX
#include <windows.h>
// after windows.h
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// Both columns' text starts with the same letter, as do their titles, so
// where their ink starts compares the cells' offsets alone.
class rows : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t, size_t col) const override { return col == 0 ? "Nfile" : "Nnote"; }
};

std::vector<HWND> lists()
{
    std::vector<HWND> found;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND c, LPARAM q) -> BOOL
                {
                    wchar_t name[32] = {};
                    GetClassNameW(c, name, 32);
                    if (!_wcsicmp(name, WC_LISTVIEWW))
                        reinterpret_cast<std::vector<HWND>*>(q)->push_back(c);
                    return TRUE;
                },
                p);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND top_window()
{
    HWND top = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM p) -> BOOL
        {
            if (!GetParent(h) && IsWindow(h)) *reinterpret_cast<HWND*>(p) = h;
            return *reinterpret_cast<HWND*>(p) == nullptr;
        },
        reinterpret_cast<LPARAM>(&top));
    return top;
}

DWORD windows_build()
{
    using fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof v};
    if (auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        f(&v);
    return v.dwBuildNumber;
}

// A window's client area printed into a top-down 32-bit bitmap.
struct printed
{
    int w = 0, h = 0;
    std::vector<DWORD> px;
    DWORD at(int x, int y) const { return px[size_t(y) * w + x]; }
};

printed print(HWND h)
{
    printed out;
    RECT c{};
    GetClientRect(h, &c);
    out.w = c.right;
    out.h = c.bottom;
    if (out.w <= 0 || out.h <= 0) return out;
    BITMAPINFO bi{};
    bi.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                    .biWidth = out.w,
                    .biHeight = -out.h,
                    .biPlanes = 1,
                    .biBitCount = 32,
                    .biCompression = BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ old = SelectObject(dc, bmp);
    SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    const DWORD* p = static_cast<const DWORD*>(bits);
    out.px.assign(p, p + size_t(out.w) * out.h);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

int distance(DWORD a, DWORD b)
{
    return std::abs(int(a & 255) - int(b & 255)) + std::abs(int(a >> 8 & 255) - int(b >> 8 & 255)) +
           std::abs(int(a >> 16 & 255) - int(b >> 16 & 255));
}

// The first column in [left, right) of rows [top, bottom) with ink on it:
// a pixel unlike the background just inside `left` on its own line. -1: none.
int ink(const printed& p, int left, int right, int top, int bottom)
{
    left = std::max(left, 0);
    right = std::min(right, p.w);
    top = std::max(top, 0);
    bottom = std::min(bottom, p.h);
    for (int x = left + 2; x < right; ++x)
        for (int y = top; y < bottom; ++y)
            if (distance(p.at(x, y), p.at(left + 1, y)) > 120) return x;
    return -1;
}

struct measures
{
    int slot = 0;     // the list_view's image slot in column 0
    int label = 0;    // its label's left, from the column's
    UINT margin = 0;  // the header's bitmap margin
    bool title_image = false, other_image = false;
    int header_image = 0;  // the header's image width, 0: none
    int header_h = 0;
    int title_ink[2] = {-1, -1};  // from each header item's left
    int row_ink[2] = {-1, -1};    // row 0's, from each column's left
};

measures measure(HWND list)
{
    measures m;
    HWND header = ListView_GetHeader(list);
    RECT icon{}, label{}, item[2]{};
    ListView_GetItemRect(list, 0, &icon, LVIR_ICON);
    ListView_GetItemRect(list, 0, &label, LVIR_LABEL);
    Header_GetItemRect(header, 0, &item[0]);
    Header_GetItemRect(header, 1, &item[1]);
    m.slot = icon.right - icon.left;
    m.label = label.left - item[0].left;
    m.margin = UINT(SendMessageW(header, HDM_GETBITMAPMARGIN, 0, 0));
    for (int i = 0; i < 2; ++i)
    {
        HDITEMW hd{};
        hd.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hd);
        (i ? m.other_image : m.title_image) = (hd.fmt & HDF_IMAGE) != 0;
    }
    if (auto il = reinterpret_cast<HIMAGELIST>(SendMessageW(header, HDM_GETIMAGELIST, 0, 0)))
    {
        int cx = 0, cy = 0;
        ImageList_GetIconSize(il, &cx, &cy);
        m.header_image = cx;
    }
    RECT hr{};
    GetWindowRect(header, &hr);
    m.header_h = hr.bottom - hr.top;

    const printed titles = print(header);
    const printed body = print(list);
    RECT row{};
    ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS);
    for (int i = 0; i < 2; ++i)
    {
        const int t = ink(titles, item[i].left, item[i].right - 4, 2, titles.h - 2);
        const int r = ink(body, item[i].left, item[i].right - 4, row.top + 1, row.bottom - 1);
        m.title_ink[i] = t < 0 ? -1 : t - item[i].left;
        m.row_ink[i] = r < 0 ? -1 : r - item[i].left;
    }
    return m;
}

std::string str(int v)
{
    return std::to_string(v);
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::printf("FAIL %s\n", ui.error().c_str());
        return 1;
    }
    rows model;
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 160_px}, {"Notes", 160_px}};
    auto win = (*ui)->create(window{"t",
                                    {vbox{list{cols, &model}.frame(false).stretch(),
                                          list{cols, &model}.frame(false).file_view().stretch()}}});
    win->show();
    const std::vector<HWND> found = lists();
    expect(found.size() == 2, "both lists are there");
    HWND top = top_window();
    if (found.size() != 2 || !top) return 1;

    const DWORD build = windows_build();
    const bool eleven = build >= 22000;
    std::printf("build %lu: %s\n", build, eleven ? "the inset is expected" : "no inset expected");

    auto check = [&](const char* when)
    {
        const UINT dpi = GetDpiForWindow(top);
        const int inset = eleven ? int(std::lround(16.0 * dpi / 96)) : 0;
        const measures plain = measure(found[0]), fv = measure(found[1]);
        const std::string at = std::string(" (") + when + ", " + str(int(dpi)) + " dpi)";
        std::printf(
            "     plain: slot %d label %d margin %u title ink %d/%d row ink %d/%d header %d px\n",
            plain.slot, plain.label, plain.margin, plain.title_ink[0], plain.title_ink[1],
            plain.row_ink[0], plain.row_ink[1], plain.header_h);
        std::printf("     file_view: slot %d label %d margin %u image %d title ink %d/%d row ink "
                    "%d/%d header %d px\n",
                    fv.slot, fv.label, fv.margin, fv.header_image, fv.title_ink[0], fv.title_ink[1],
                    fv.row_ink[0], fv.row_ink[1], fv.header_h);
        expect(plain.title_ink[0] >= 0 && plain.title_ink[1] >= 0 && plain.row_ink[0] >= 0 &&
                   plain.row_ink[1] >= 0 && fv.title_ink[0] >= 0 && fv.title_ink[1] >= 0 &&
                   fv.row_ink[0] >= 0 && fv.row_ink[1] >= 0,
               "every title and cell printed some text" + at);
        expect(fv.title_ink[0] - plain.title_ink[0] == inset,
               "the first title starts " + str(inset) + " px further in" + at);
        expect(fv.row_ink[0] - plain.row_ink[0] == inset,
               "the first column's text starts " + str(inset) + " px further in" + at);
        expect(fv.title_ink[0] - fv.row_ink[0] == plain.title_ink[0] - plain.row_ink[0],
               "the first title lines up with its column's text as a plain list's does" + at);
        expect(fv.title_ink[1] == plain.title_ink[1] && fv.row_ink[1] == plain.row_ink[1],
               "the second column and its title are where a plain list's are" + at);
        expect(fv.header_h == plain.header_h, "the header is as tall as a plain list's" + at);
        if (eleven)
        {
            expect(fv.slot == inset && fv.label - plain.label == inset - plain.slot,
                   "the inset is the list_view's own image slot" + at);
            expect(
                fv.title_image && !fv.other_image && fv.margin == 0 && fv.header_image == inset,
                "the first title alone has an image, a blank as wide as the inset, with no margin" +
                    at);
        }
        else
            expect(!fv.title_image && fv.slot == plain.slot,
                   "the file_view list's first column is a plain one's" + at);
    };
    check("shown");

    // A new DPI: the inset follows it.
    const UINT dpi = GetDpiForWindow(top);
    const UINT to = dpi == 144 ? 120 : 144;
    RECT r{};
    GetWindowRect(top, &r);
    SendMessageW(top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&r));
    // GetDpiForWindow still answers the monitor's: the lists' font says what they're at.
    const int expect_inset = eleven ? int(std::lround(16.0 * to / 96)) : 0;
    {
        RECT icon{};
        ListView_GetItemRect(found[1], 0, &icon, LVIR_ICON);
        HWND header = ListView_GetHeader(found[1]);
        int cx = 0, cy = 0;
        if (auto il = reinterpret_cast<HIMAGELIST>(SendMessageW(header, HDM_GETIMAGELIST, 0, 0)))
            ImageList_GetIconSize(il, &cx, &cy);
        std::printf("     after WM_DPICHANGED to %u: slot %ld header image %d margin %u\n", to,
                    icon.right - icon.left, cx,
                    UINT(SendMessageW(header, HDM_GETBITMAPMARGIN, 0, 0)));
        if (eleven)
            expect(icon.right - icon.left == expect_inset && cx == expect_inset &&
                       SendMessageW(header, HDM_GETBITMAPMARGIN, 0, 0) == 0,
                   "after WM_DPICHANGED to " + str(int(to)) +
                       " dpi the slot and the title's blank are " + str(expect_inset) + " px");
    }

    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
