// A new text size (Settings > Accessibility > Text size) while windows are
// open, on Win32: native apps take it from WM_SETTINGCHANGE, and so do
// rapport's windows, even with the palette unchanged.
//
// Two never-shown windows, the first with the tabs in the caption and a base
// surface (Windows 11: the toolbar band, its address field a band field).
// The text size taken to be 150% through the test hooks (the text size,
// rap::win32::set_text_scale_for_tests, and the message font Windows makes
// larger with it, rap::win32::set_message_font_size_for_tests: 18 DIPs for
// 12), the setting itself untouched, and WM_SETTINGCHANGE sent to the first
// window as Windows sends it (SPI_SETNONCLIENTMETRICS, "WindowMetrics"):
// - every control of both windows has the larger message font, the band
//   field WinUI's 14 enlarged by the text size (21), the caption's tabs'
//   text the message font's size;
// - the label and the band field taller for it, the windows' least size
//   larger;
// - the old fonts freed, none left retired;
// - the text views (RichEdit, which WM_GETFONT says nothing of) shown
//   larger by RichEdit's zoom, 3/2, their content untouched: every run's
//   format as it was (plain text, typed text, a bold red span, Consolas, a
//   run made Arial bold at 14 pt, appended italic), its colours compared;
//   their lines 1.5 times as tall as drawn and the text view's window's
//   least height larger; the selection and the line at the top kept;
//   on_changed not called; one EM_UNDO undoes the user's last edit (typed
//   before the change), and undoing a deletion made before it brings the
//   deleted styled run back as it was, shown at the zoom; a zoom of the
//   user's own (5/4, as Ctrl+wheel sets) times the text size's (15/8), and
//   the user's again at 100%.
// A WM_SETTINGCHANGE that changes neither (most of them) changes nothing:
// the same fonts. Then 100% again, and everything as it was.
//
// The hooks are the static backend's: a plugin build skips (77).
//   rapport-win32-text-size-test
#include <rapport/rapport.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <richedit.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
namespace rap::win32
{
// win32_common.h (which wants the backend's include paths)
void set_text_scale_for_tests(float scale);
void set_dpi_for_tests(UINT dpi);
void set_message_font_size_for_tests(float dip);
size_t retired_ui_font_count();
}  // namespace rap::win32
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

std::string str(long v)
{
    return std::to_string(v);
}

void pump(int rounds = 20)
{
    for (int round = 0; round < rounds; ++round)
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

int px(float logical, UINT dpi)
{
    return int(std::lround(logical * float(dpi) / 96.f));
}

HWND top_of(const char* title)
{
    return FindWindowW(nullptr, std::wstring(title, title + std::strlen(title)).c_str());
}

HWND child_of(HWND top, const wchar_t* cls, const wchar_t* text = nullptr)
{
    return top ? FindWindowExW(top, nullptr, cls, text) : nullptr;
}

HFONT font_of(HWND h)
{
    return reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
}

int font_px(HWND h)
{
    LOGFONTW lf{};
    if (!h || !GetObjectW(font_of(h), sizeof lf, &lf)) return 0;
    return std::abs(int(lf.lfHeight));
}

int height_of(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return r.bottom - r.top;
}

SIZE least_of(HWND top)
{
    MINMAXINFO mm{};
    SendMessageW(top, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mm));
    return {mm.ptMinTrackSize.x, mm.ptMinTrackSize.y};
}

// caption_tabs_fonts (win32_caption_tabs.h), query_message's 7.
struct caption_fonts
{
    wchar_t text[64];
    wchar_t glyphs[64];
    float text_size, glyph_size;
    int text_weight;
    int cleartype;
    float add_size;
};
float caption_text_px(HWND top)
{
    HWND strip = child_of(top, L"rapportCaptionTabs");
    static const UINT query = RegisterWindowMessageW(L"rapport-caption-tabs");
    caption_fonts f{};
    if (!strip || !SendMessageW(strip, query, MAKEWPARAM(7, 0), reinterpret_cast<LPARAM>(&f))) return 0;
    return f.text_size;
}

// A text view's character formats, one character of each run, and its
// selection.
struct run_format
{
    long size;  // twips
    std::wstring face;
    DWORD effects;  // CFE_BOLD, CFE_ITALIC, CFE_AUTOCOLOR
    COLORREF colour;
};
struct text_seen
{
    std::vector<run_format> runs;
    CHARRANGE selection;
    long first_line;
};
// Plain, bold red, mono, Arial bold 14 pt, appended italic.
const std::vector<long> g_run_at = {1, 8, 14, 20, 33};

long first_line(HWND rich)
{
    return long(SendMessageW(rich, EM_GETFIRSTVISIBLELINE, 0, 0));
}
// The selection and the line at the top put back as they were.
void put_back(HWND rich, CHARRANGE sel, long line)
{
    SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    pump(2);  // RichEdit scrolls to the selection once it's idle: first
    SendMessageW(rich, EM_LINESCROLL, 0, line - first_line(rich));
}

text_seen look_text(HWND rich)
{
    text_seen t{};
    SendMessageW(rich, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&t.selection));
    t.first_line = first_line(rich);
    for (long at : g_run_at)
    {
        CHARRANGE cr{at, at + 1};
        SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        SendMessageW(rich, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
        const DWORD effects = cf.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_AUTOCOLOR);
        t.runs.push_back({cf.yHeight, cf.szFaceName, effects, cf.crTextColor});
        std::printf("     run at %ld: %ld twips, %ls%s%s, colour %02x%02x%02x%s\n", at, cf.yHeight, cf.szFaceName,
                    cf.dwEffects & CFE_BOLD ? ", bold" : "", cf.dwEffects & CFE_ITALIC ? ", italic" : "",
                    GetRValue(cf.crTextColor), GetGValue(cf.crTextColor), GetBValue(cf.crTextColor),
                    cf.dwEffects & CFE_AUTOCOLOR ? " (automatic)" : "");
    }
    std::printf("     selection %ld..%ld, line %ld at the top\n", t.selection.cpMin, t.selection.cpMax,
                t.first_line);
    put_back(rich, t.selection, t.first_line);
    return t;
}

// A COLORREF as RRGGBB.
std::string rgb_hex(COLORREF c)
{
    char b[16];
    std::snprintf(b, sizeof b, "%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

void check_text(const text_seen& was, const text_seen& now, const std::string& at)
{
    const bool same_count = was.runs.size() == now.runs.size();
    bool sizes = same_count, kept = same_count, colours = same_count;
    for (size_t i = 0; same_count && i < was.runs.size(); ++i)
    {
        sizes = sizes && now.runs[i].size == was.runs[i].size;
        kept = kept && now.runs[i].face == was.runs[i].face && now.runs[i].effects == was.runs[i].effects;
        // Every run's colour (the red span's, and the others' automatic
        // or theme text colour) the same value, CFE_AUTOCOLOR with it.
        colours = colours && now.runs[i].colour == was.runs[i].colour;
    }
    expect(sizes, "the text view's runs their own sizes, untouched" + at);
    expect(kept, "their faces, bold and italic kept" + at);
    expect(colours, "their colours kept" + at);
    // The bold span's red (a diff's colour, applied only as it's loaded).
    const COLORREF red = same_count && now.runs.size() > 1 ? now.runs[1].colour : CLR_INVALID;
    expect(red == RGB(200, 0, 0), "the red span's colour c80000" + at + ": " + rgb_hex(red));
    expect(now.selection.cpMin == was.selection.cpMin && now.selection.cpMax == was.selection.cpMax,
           "its selection kept" + at + ": " + str(now.selection.cpMin) + ".." + str(now.selection.cpMax));
    expect(now.first_line == was.first_line,
           "the line at its top kept" + at + ": " + str(was.first_line) + " -> " + str(now.first_line));
}

std::wstring text_of(HWND rich)
{
    std::wstring w(size_t(GetWindowTextLengthW(rich)) + 1, L'\0');
    w.resize(size_t(GetWindowTextW(rich, w.data(), int(w.size()))));
    return w;
}

// What's measured of the two windows at one text size.
struct seen
{
    HFONT label_font, other_font;
    int label_text, other_text, field_text, caption_text;
    int label_height, field_height;
    SIZE least;
};

seen look(HWND top, HWND other)
{
    HWND label = child_of(top, L"Static", L"The current tab's content");
    HWND field = child_of(top, L"Edit");
    HWND other_label = child_of(other, L"Static", L"Another window");
    seen s{};
    s.label_font = font_of(label);
    s.other_font = font_of(other_label);
    s.label_text = font_px(label);
    s.other_text = font_px(other_label);
    s.field_text = font_px(field);
    s.caption_text = int(std::lround(caption_text_px(top)));
    s.label_height = label ? height_of(label) : 0;
    s.field_height = field ? height_of(field) : 0;
    s.least = least_of(top);
    std::printf("     label %d px (%d tall), other window's %d px, field %d px (%d tall), caption %d px, "
                "least %ldx%ld\n",
                s.label_text, s.label_height, s.other_text, s.field_text, s.field_height, s.caption_text,
                s.least.cx, s.least.cy);
    return s;
}

// Windows' broadcast for a new text size, to the first window only: the
// second is given its fonts all the same (by whichever hears of it first).
// Every character's size in twips (one per character RichEdit has: CR LF
// as one).
std::vector<long> sizes_of(HWND rich)
{
    GETTEXTLENGTHEX len{GTL_NUMCHARS | GTL_PRECISE, 1200};
    const long n = long(SendMessageW(rich, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&len), 0));
    std::vector<long> out;
    for (long at = 0; at < n; ++at)
    {
        CHARRANGE cr{at, at + 1};
        SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        SendMessageW(rich, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
        out.push_back(cf.yHeight);
    }
    return out;
}

// An editable text view whose text takes the view's own size (no explicit
// sizes in it, as SetWindowText and typing leave it), with the user's edit
// typed at its end.
struct plain_view
{
    std::string name;
    rap::text_view* tv = nullptr;
    HWND rich = nullptr;
    std::vector<long> before;  // every character's size at 100%, the edit's included
    int changes = 0;
};

// Each of `sizes` (the first `count` of them) `ratio` times `was`'s, exactly.
bool sizes_are(const std::vector<long>& sizes, const std::vector<long>& was, float ratio, size_t count,
               std::string& first_wrong)
{
    if (sizes.size() < count || was.size() < count) return false;
    for (size_t i = 0; i < count; ++i)
        if (sizes[i] != std::lround(was[i] * ratio))
        {
            first_wrong = "character " + std::to_string(i) + " " + std::to_string(sizes[i]) + " twips, not " +
                          std::to_string(std::lround(was[i] * ratio));
            return false;
        }
    return true;
}

void check_plain(plain_view& v, float ratio, const std::string& at, size_t count)
{
    const std::vector<long> now = sizes_of(v.rich);
    std::string wrong;
    const bool ok = sizes_are(now, v.before, ratio, count, wrong);
    expect(ok, v.name + ": every character " + str(std::lround(ratio * 100)) + "% of its size, exactly" + at +
                   (ok ? "" : ": " + wrong));
}

// RichEdit's zoom, as EM_GETZOOM has it (0/0: none).
std::pair<int, int> zoom_of(HWND rich)
{
    int num = -1, den = -1;
    SendMessageW(rich, EM_GETZOOM, reinterpret_cast<WPARAM>(&num), reinterpret_cast<LPARAM>(&den));
    return {num, den};
}
void expect_zoom(HWND rich, int num, int den, const std::string& what)
{
    const auto [n, d] = zoom_of(rich);
    expect(n == num && d == den, what + " zoom " + str(num) + "/" + str(den) + ": " + str(n) + "/" + str(d));
}

// How far down line `line + 1` starts from line `line`, as drawn.
long line_height(HWND rich, long line)
{
    POINTL a{}, b{};
    SendMessageW(rich, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&a), SendMessageW(rich, EM_LINEINDEX, line, 0));
    SendMessageW(rich, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&b), SendMessageW(rich, EM_LINEINDEX, line + 1, 0));
    return b.y - a.y;
}
void expect_taller(long was, long now, float ratio, const std::string& what)
{
    const float r = was > 0 ? float(now) / float(was) : 0.f;
    expect(std::abs(r - ratio) <= 0.1f * ratio, what + " about " + std::to_string(ratio).substr(0, 4) +
                                                    " times as tall: " + str(was) + " -> " + str(now) + " px");
}

void setting_changed(HWND top)
{
    SendMessageW(top, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS, reinterpret_cast<LPARAM>(L"WindowMetrics"));
    pump();
}

}  // namespace

int main(int argc, char** argv)
{
#ifndef RAPPORT_TEST_BUILTIN_WIN32
    (void)argc;
    (void)argv;
    std::printf("skipped: the text size test hooks are the static backend's\n");
    return 77;
#else
    // 100% to start, whatever the desktop's (the setting itself untouched).
    rap::win32::set_text_scale_for_tests(1.f);
    rap::win32::set_message_font_size_for_tests(12.f);
    auto ui = rap::ui::load(argc, argv, rap::load_options{.backend = "win32"});
    if (!ui) return 2;

    using namespace rap::layout;
    const char* title = "Text size";
    auto win = (*ui)->create(
        window{title, {toolbar{label{"Address"}, entry{"C:\\Text size"}.stretch()}, label{"The current tab's content"},
                       button{"A button"}}}
            .size(800_px, 300_px)
            .margins(false)
            .base_surface()
            .tabs(document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}));
    const char* other_title = "Text size, another window";
    rap::text_view* tv = nullptr;
    auto other = (*ui)->create(
        window{other_title, {label{"Another window"}, text_view{}.wrap(false).bind(tv)}}.size(400_px, 300_px));
    pump();
    expect(tv != nullptr, "the text view");
    if (!tv) return 1;
    std::string lines = "Plain, Bold, mono, and plain.\n";
    tv->set_text(lines, {{7, 11, {.fg = rap::color{200, 0, 0}, .bold = true}}, {13, 17, {.monospace = true}}});
    tv->append("Appended", {.italic = true});
    for (int i = 0; i < 60; ++i) tv->append("\nline " + std::to_string(i));
    pump();
    HWND rich = child_of(top_of(other_title), L"RICHEDIT50W");
    expect(rich != nullptr, "the text view's RichEdit");
    if (!rich) return 1;

    // Plain views: one of plain text (set_text, no spans: its text takes
    // the view's size, as a commit message box's does) with the user's edit
    // typed at its end and a zoom of the user's own; one of plain text with
    // a styled span in it, Arial bold 14 pt blue, which the user deletes.
    const char* plain_title = "Text size, plain views";
    plain_view plain{"the plain text view"}, mixed{"the mixed text view"};
    auto plains = (*ui)->create(
        window{plain_title, {text_view{}.bind(plain.tv), text_view{}.bind(mixed.tv)}}.size(400_px, 300_px));
    pump();
    expect(plain.tv && mixed.tv, "the plain views");
    if (!plain.tv || !mixed.tv) return 1;
    plain.tv->set_text("Plain words, as written.\nA second line.");
    mixed.tv->set_text("Plain with a blue bold word in it.", {{13, 17, {.fg = rap::color{0, 0, 200}, .bold = true}}});
    pump();
    for (HWND c = FindWindowExW(top_of(plain_title), nullptr, L"RICHEDIT50W", nullptr); c;
         c = FindWindowExW(top_of(plain_title), c, L"RICHEDIT50W", nullptr))
        (text_of(c).rfind(L"Plain words", 0) == 0 ? plain.rich : mixed.rich) = c;
    expect(plain.rich && mixed.rich, "their RichEdits");
    if (!plain.rich || !mixed.rich) return 1;
    {
        CHARRANGE end{-1, -1};
        SendMessageW(plain.rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
        SendMessageW(plain.rich, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" typed"));
        SendMessageW(plain.rich, EM_SETZOOM, 5, 4);  // as Ctrl+wheel would
    }
    {
        CHARRANGE blue{13, 17};
        SendMessageW(mixed.rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&blue));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_FACE | CFM_SIZE;
        cf.yHeight = 280;
        wcscpy_s(cf.szFaceName, L"Arial");
        SendMessageW(mixed.rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }
    for (plain_view* v : {&plain, &mixed})
    {
        v->before = sizes_of(v->rich);
        std::printf("     %s: %zu characters, the first %ld twips\n", v->name.c_str(), v->before.size(),
                    v->before.empty() ? 0L : v->before[0]);
    }
    {
        CHARRANGE blue{13, 17};
        SendMessageW(mixed.rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&blue));
        SendMessageW(mixed.rich, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    }
    expect(text_of(mixed.rich).find(L"blue") == std::wstring::npos, "the blue word deleted");
    for (plain_view* v : {&plain, &mixed}) v->tv->on_changed([v] { ++v->changes; });
    const long plain_line = line_height(plain.rich, 0);
    {
        // "and" Arial bold at 14 pt, a size of its own, as pasted text has.
        CHARRANGE cr{19, 22};
        SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        cf.dwMask = CFM_FACE | CFM_BOLD | CFM_SIZE;
        cf.dwEffects = CFE_BOLD;
        cf.yHeight = 280;
        wcscpy_s(cf.szFaceName, L"Arial");
        SendMessageW(rich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
    }
    // The user's last edit, before the text size changes: typed at the end.
    CHARRANGE end{-1, -1};
    SendMessageW(rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(rich, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L" typed"));
    expect(text_of(rich).find(L" typed") != std::wstring::npos, "the user's edit made");
    int changes = 0;
    tv->on_changed([&changes] { ++changes; });
    // A selection, and scrolled down 20 lines. Unwrapped, its lines are its
    // paragraphs, whatever width a never-shown RichEdit has laid itself out
    // for: line 20 is the same text at either size.
    put_back(rich, CHARRANGE{2, 9}, 20);
    pump();
    HWND top = top_of(title), other_top = top_of(other_title);
    expect(top && other_top, "the two windows");
    if (!top || !other_top) return 1;
    const UINT dpi = GetDpiForWindow(top);
    const bool band = child_of(top, L"rapportCaptionTabs") != nullptr;
    std::printf("%u dpi, %s\n", dpi, band ? "the tabs in the caption" : "no caption tabs (before Windows 11)");

    std::printf("at 100%%:\n");
    const seen before = look(top, other_top);
    const text_seen text_before = look_text(rich);
    expect(text_before.runs.size() > 1 && text_before.runs[1].colour == RGB(200, 0, 0),
           "the red span's colour c80000 before the change: " +
               rgb_hex(text_before.runs.size() > 1 ? text_before.runs[1].colour : CLR_INVALID));
    expect(text_before.first_line == 20, "the text view scrolled to line 20: " + str(text_before.first_line));
    const long styled_line = line_height(rich, 30);
    const SIZE other_least = least_of(other_top);
    std::printf("     line heights %ld px (styled), %ld px (plain, zoomed 5/4)\n", styled_line, plain_line);
    expect(before.label_text == px(12, dpi), "the label's text " + str(px(12, dpi)) + " px: " + str(before.label_text));
    if (band)
        expect(before.field_text == px(14, dpi),
               "the band field's text " + str(px(14, dpi)) + " px: " + str(before.field_text));

    // Most WM_SETTINGCHANGEs change neither: nothing made again.
    setting_changed(top);
    const seen same = look(top, other_top);
    expect(same.label_font == before.label_font && same.other_font == before.other_font,
           "a WM_SETTINGCHANGE that changes no font: the same fonts");

    // 150%.
    rap::win32::set_text_scale_for_tests(1.5f);
    rap::win32::set_message_font_size_for_tests(18.f);
    setting_changed(top);
    std::printf("at 150%%:\n");
    const seen larger = look(top, other_top);
    check_text(text_before, look_text(rich), " at 150%");
    expect_zoom(rich, 3, 2, "the text view's");
    expect_taller(styled_line, line_height(rich, 30), 1.5f, "its lines");
    expect(least_of(other_top).cy > other_least.cy, "its window's least height larger: " + str(other_least.cy) +
                                                        " -> " + str(least_of(other_top).cy));
    expect(changes == 0, "on_changed not called: " + str(changes));
    // One undo: the user's edit, the formats as they were.
    expect(SendMessageW(rich, EM_UNDO, 0, 0) != 0, "an undo");
    expect(text_of(rich).find(L" typed") == std::wstring::npos, "one undo undoes the user's edit");
    {
        const text_seen undone = look_text(rich);
        bool sizes = undone.runs.size() == text_before.runs.size();
        for (size_t i = 0; sizes && i < undone.runs.size(); ++i) sizes = undone.runs[i].size == text_before.runs[i].size;
        expect(sizes, "the runs' formats as they were after it");
    }
    expect_zoom(rich, 3, 2, "the text view's, after the undo,");
    put_back(rich, text_before.selection, text_before.first_line);
    const int undo_changes = changes;  // the undo's own (an edit)

    // The plain view: its text's formats untouched, the user's 5/4 times
    // 3/2; one undo undoes the user's edit.
    check_plain(plain, 1.f, " at 150%", plain.before.size());
    expect_zoom(plain.rich, 15, 8, "the plain text view's (the user's 5/4 times 3/2)");
    expect_taller(plain_line, line_height(plain.rich, 0), 1.5f, "the plain text view's lines");
    expect(plain.changes == 0, "the plain text view: on_changed not called: " + str(plain.changes));
    expect(SendMessageW(plain.rich, EM_UNDO, 0, 0) != 0, "the plain text view: an undo");
    expect(text_of(plain.rich).find(L" typed") == std::wstring::npos,
           "the plain text view: one undo undoes the user's edit");
    check_plain(plain, 1.f, " after the undo", plain.before.size() - 6);
    const int plain_undo_changes = plain.changes;
    // The mixed view: undoing the deletion brings the blue word back as it
    // was (Arial 14 pt bold blue), shown at the zoom.
    expect_zoom(mixed.rich, 3, 2, "the mixed text view's");
    expect(mixed.changes == 0, "the mixed text view: on_changed not called: " + str(mixed.changes));
    expect(SendMessageW(mixed.rich, EM_UNDO, 0, 0) != 0 && text_of(mixed.rich).find(L"blue") != std::wstring::npos,
           "the mixed text view: one undo brings the deleted word back");
    check_plain(mixed, 1.f, " after the undo", mixed.before.size());
    {
        CHARRANGE blue{13, 17};
        SendMessageW(mixed.rich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&blue));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof cf;
        SendMessageW(mixed.rich, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
        expect(cf.yHeight == 280 && std::wstring(cf.szFaceName) == L"Arial" && (cf.dwEffects & CFE_BOLD) &&
                   cf.crTextColor == RGB(0, 0, 200),
               "the word back at 14 pt, Arial, bold, 0000c8: " + str(cf.yHeight) + " twips, " +
                   (std::wstring(cf.szFaceName) == L"Arial" ? "Arial" : "not Arial") + ", " +
                   rgb_hex(cf.crTextColor));
    }
    expect_zoom(mixed.rich, 3, 2, "the mixed text view's, after the undo,");
    const int mixed_undo_changes = mixed.changes;
    expect(larger.label_font != before.label_font, "the label given a new font");
    expect(larger.label_text == px(18, dpi), "the label's text " + str(px(18, dpi)) + " px: " + str(larger.label_text));
    expect(larger.other_text == px(18, dpi),
           "the other window's label's " + str(px(18, dpi)) + " px: " + str(larger.other_text));
    expect(larger.label_height > before.label_height,
           "the label taller: " + str(before.label_height) + " -> " + str(larger.label_height));
    expect(larger.least.cy > before.least.cy,
           "the window's least height larger: " + str(before.least.cy) + " -> " + str(larger.least.cy));
    if (band)
    {
        expect(larger.field_text == px(21, dpi),
               "the band field's text " + str(px(21, dpi)) + " px: " + str(larger.field_text));
        expect(larger.field_height > before.field_height,
               "the band field taller: " + str(before.field_height) + " -> " + str(larger.field_height));
        expect(larger.caption_text == px(18, dpi),
               "the caption's tabs' text " + str(px(18, dpi)) + " px: " + str(larger.caption_text));
    }
    expect(rap::win32::retired_ui_font_count() == 0,
           "the old fonts freed: " + str(long(rap::win32::retired_ui_font_count())) + " left");

    // 100% again: as it was.
    rap::win32::set_text_scale_for_tests(1.f);
    rap::win32::set_message_font_size_for_tests(12.f);
    setting_changed(top);
    std::printf("at 100%% again:\n");
    const seen after = look(top, other_top);
    check_text(text_before, look_text(rich), " at 100% again");
    expect_zoom(rich, 0, 0, "the text view's");
    expect(line_height(rich, 30) == styled_line, "its lines as tall as they were: " + str(line_height(rich, 30)));
    expect(changes == undo_changes, "on_changed not called by the change back: " + str(changes - undo_changes));
    check_plain(plain, 1.f, " at 100% again", plain.before.size() - 6);
    expect_zoom(plain.rich, 5, 4, "the plain text view's (the user's)");
    expect(line_height(plain.rich, 0) == plain_line, "its lines as tall as they were");
    expect(plain.changes == plain_undo_changes, "the plain text view: on_changed not called by the change back");
    check_plain(mixed, 1.f, " at 100% again", mixed.before.size());
    expect_zoom(mixed.rich, 0, 0, "the mixed text view's");
    expect(mixed.changes == mixed_undo_changes, "the mixed text view: on_changed not called by the change back");
    expect(after.label_text == before.label_text && after.other_text == before.other_text &&
               after.field_text == before.field_text && after.caption_text == before.caption_text,
           "the text as it was");
    expect(after.label_height == before.label_height && after.field_height == before.field_height &&
               after.least.cx == before.least.cx && after.least.cy == before.least.cy,
           "the sizes as they were");
    expect(rap::win32::retired_ui_font_count() == 0, "the old fonts freed again");

    // RichEdit's zoom takes a ratio strictly between 1/64 and 64: a refused
    // one returns 0 and leaves the zoom as it was.
    expect(SendMessageW(plain.rich, EM_SETZOOM, 64, 1) == 0 && zoom_of(plain.rich) == std::pair{5, 4},
           "EM_SETZOOM refuses 64/1, the zoom left 5/4");
    expect(SendMessageW(plain.rich, EM_SETZOOM, 1, 64) == 0 && zoom_of(plain.rich) == std::pair{5, 4},
           "and 1/64");
    // The text size at a message font of `px` (12 its base).
    auto text_at = [&](float px)
    {
        rap::win32::set_text_scale_for_tests(px / 12.f);
        rap::win32::set_message_font_size_for_tests(px);
        setting_changed(top);
    };
    // The user at 50 times: at 150%, 75 wanted, as far as RichEdit goes
    // (6399/100); at 100%, the user's 50 exactly.
    expect(SendMessageW(plain.rich, EM_SETZOOM, 50, 1) != 0, "the user's zoom 50/1 taken");
    text_at(18);
    expect_zoom(plain.rich, 6399, 100, "at 150%, the user's 50 times 3/2 clamped:");
    text_at(12);
    expect_zoom(plain.rich, 50, 1, "at 100%, the user's again:");
    // At the bottom: the user at 1/50, the text 2/3 (8 px).
    expect(SendMessageW(plain.rich, EM_SETZOOM, 1, 50) != 0, "the user's zoom 1/50 taken");
    text_at(8);
    expect_zoom(plain.rich, 100, 6399, "at 2/3, the user's 1/50 times 2/3 clamped:");
    text_at(12);
    expect_zoom(plain.rich, 1, 50, "at 100%, the user's again:");
    // The user zooming between two text size changes, while clamped: theirs
    // is what's shown over the text size's (40 over 3/2), and stays.
    expect(SendMessageW(plain.rich, EM_SETZOOM, 50, 1) != 0, "the user's zoom 50/1 again");
    text_at(18);
    expect_zoom(plain.rich, 6399, 100, "at 150%, clamped:");
    expect(SendMessageW(plain.rich, EM_SETZOOM, 40, 1) != 0, "the user zooms to 40/1 meanwhile");
    text_at(12);
    expect_zoom(plain.rich, 80, 3, "at 100%, the user's 40 over 3/2:");
    text_at(15);
    expect_zoom(plain.rich, 100, 3, "at 125%, 80/3 times 5/4:");
    text_at(12);
    expect_zoom(plain.rich, 80, 3, "at 100% again:");
    // Back to the user's 5/4.
    expect(SendMessageW(plain.rich, EM_SETZOOM, 5, 4) != 0, "the user's zoom 5/4 again");

    // A new DPI while the text size is 150%: refont gives the RichEdit the
    // new DPI's font, at the message font's size then, which is what its
    // text is made at from then on; the text size's part of the zoom goes,
    // the user's 5/4 stays. And back at 100%, the text shown 12/18 of that.
    rap::win32::set_text_scale_for_tests(1.5f);
    rap::win32::set_message_font_size_for_tests(18.f);
    setting_changed(top);
    expect_zoom(plain.rich, 15, 8, "the plain text view's at 150% again");
    HWND plain_top = top_of(plain_title);
    for (const UINT to : {dpi * 5 / 4, dpi})
    {
        rap::win32::set_dpi_for_tests(to == dpi ? 0 : to);
        RECT wr{};
        GetWindowRect(plain_top, &wr);
        SendMessageW(plain_top, WM_DPICHANGED, MAKEWPARAM(to, to), reinterpret_cast<LPARAM>(&wr));
        pump();
        expect_zoom(plain.rich, 5, 4, "after WM_DPICHANGED to " + str(long(to)) + " dpi, the plain text view's");
    }
    rap::win32::set_text_scale_for_tests(1.f);
    rap::win32::set_message_font_size_for_tests(12.f);
    setting_changed(top);
    expect_zoom(plain.rich, 5, 6, "at 100%, the plain text view's (5/4 times 12/18)");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
#endif
}
