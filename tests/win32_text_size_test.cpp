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
// - an editable text view (RichEdit, which WM_GETFONT says nothing of, and
//   which WM_SETFONT would reset to one font, telling on_changed) its
//   text's sizes 1.5 times, run by run: plain text, a bold red span,
//   Consolas, a run made Arial bold at 14 pt of its own (as pasted) and
//   appended italic text keep their faces, effects and colours; the
//   selection and the line at the top stay; on_changed isn't called; and
//   one EM_UNDO undoes the user's last edit, made before the change, not
//   the change (the sizes stay).
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
        std::printf("     run at %ld: %ld twips, %ls%s%s, colour %06lx\n", at, cf.yHeight, cf.szFaceName,
                    cf.dwEffects & CFE_BOLD ? ", bold" : "", cf.dwEffects & CFE_ITALIC ? ", italic" : "",
                    cf.crTextColor & 0xffffff);
    }
    std::printf("     selection %ld..%ld, line %ld at the top\n", t.selection.cpMin, t.selection.cpMax,
                t.first_line);
    put_back(rich, t.selection, t.first_line);
    return t;
}

void check_text(const text_seen& was, const text_seen& now, float ratio, const std::string& at)
{
    bool sizes = was.runs.size() == now.runs.size(), kept = sizes;
    for (size_t i = 0; sizes && i < was.runs.size(); ++i)
    {
        sizes = sizes && std::abs(now.runs[i].size - std::lround(was.runs[i].size * ratio)) <= 1;
        kept = kept && now.runs[i].face == was.runs[i].face && now.runs[i].effects == was.runs[i].effects;
    }
    expect(sizes, "the text view's runs " + str(std::lround(ratio * 100)) + "% of their size" + at);
    expect(kept, "their faces, bold, italic and colours kept" + at);
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
    expect(text_before.first_line == 20, "the text view scrolled to line 20: " + str(text_before.first_line));
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
    check_text(text_before, look_text(rich), 1.5f, " at 150%");
    expect(changes == 0, "on_changed not called: " + str(changes));
    // One undo: the user's edit, not the new sizes.
    expect(SendMessageW(rich, EM_UNDO, 0, 0) != 0, "an undo");
    expect(text_of(rich).find(L" typed") == std::wstring::npos, "one undo undoes the user's edit");
    {
        const text_seen undone = look_text(rich);
        bool sizes = undone.runs.size() == text_before.runs.size();
        for (size_t i = 0; sizes && i < undone.runs.size(); ++i)
            sizes = std::abs(undone.runs[i].size - std::lround(text_before.runs[i].size * 1.5f)) <= 1;
        expect(sizes, "and not the new sizes");
    }
    put_back(rich, text_before.selection, text_before.first_line);
    const int undo_changes = changes;  // the undo's own (an edit)
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
    check_text(text_before, look_text(rich), 1.f, " at 100% again");
    expect(changes == undo_changes, "on_changed not called by the change back: " + str(changes - undo_changes));
    expect(after.label_text == before.label_text && after.other_text == before.other_text &&
               after.field_text == before.field_text && after.caption_text == before.caption_text,
           "the text as it was");
    expect(after.label_height == before.label_height && after.field_height == before.field_height &&
               after.least.cx == before.least.cx && after.least.cy == before.least.cy,
           "the sizes as they were");
    expect(rap::win32::retired_ui_font_count() == 0, "the old fonts freed again");

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
#endif
}
