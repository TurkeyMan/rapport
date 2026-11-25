// A page switch in a shown window (relayout_at_once: a tab strip's, a
// sidebar's, a stack's, a lazy page built) never turns its window's drawing
// off (WM_SETREDRAW FALSE), nor any other ancestor's of a canvas. Drawing
// off clears WS_VISIBLE, and while it was off the DWM composed the window
// without its canvases' GPU layers, showing the GDI surface under them (the
// canvas's first frame, drawn there on the CPU) for a frame at every switch.
// The document tabs' strip switched too.
//
// Every message sent to this thread's windows watched (WH_CALLWNDPROC). The
// window is cloaked before it's shown (DWMWA_CLOAK): shown as far as the
// window manager has it (IsWindowVisible), never on the desktop; and shown
// without activating (RAPPORT_NO_ACTIVATE). Skipped (77) if it can't be
// cloaked.
//   rapport-win32-at-once-redraw-test
#include <rapport/rapport.h>
#include "win32_test_no_dialogs.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "win32_test_windows.h"
// After windows.h, which they need.
#include <commctrl.h>
#include <dwmapi.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
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
    std::fflush(stdout);
    if (!ok) ++fails;
}

void pump(int rounds = 10)
{
    for (int round = 0; round < rounds; ++round)
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

// Each WM_SETREDRAW FALSE sent to one of this thread's windows: to which.
std::vector<HWND> g_redraw_off;
unsigned g_seen = 0;  // messages seen at all (the hook's working)
LRESULT CALLBACK watch(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
    {
        const auto* m = reinterpret_cast<const CWPSTRUCT*>(lp);
        ++g_seen;
        if (m->message == WM_SETREDRAW && !m->wParam) g_redraw_off.push_back(m->hwnd);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::vector<HWND> canvases(HWND top)
{
    std::vector<HWND> out;
    EnumChildWindows(
        top,
        [](HWND h, LPARAM lp) -> BOOL
        {
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (!_wcsicmp(c, L"rapportCanvas")) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// The tab control with `tabs` tabs (the strip's: the document tabs' row is
// a tab control too, on some builds).
HWND tab_control(HWND top, int tabs)
{
    struct q
    {
        int tabs;
        HWND found = nullptr;
    } f{tabs};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM lp) -> BOOL
        {
            auto* f = reinterpret_cast<q*>(lp);
            wchar_t c[64]{};
            GetClassNameW(h, c, 64);
            if (_wcsicmp(c, WC_TABCONTROLW) || TabCtrl_GetItemCount(h) != f->tabs) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

}  // namespace

int main(int argc, char** argv)
{
    test::no_dialogs();  // a failure to the console, never a dialog on the desktop
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    using namespace rap::layout;
    using namespace rap::literals;
    rap::tabs* strip = nullptr;
    rap::tabs* side = nullptr;
    rap::stack* stk = nullptr;
    rap::document_tabs* docs = nullptr;
    auto win = (*ui)->create(
        window{"At-once redraw",
               {vbox{tabs{{"One", label{"First"}},
                          {"Two", vbox{canvas{}.stretch()}},
                          {"Three", lazy{[] { return rap::node(vbox{label{"Built"}, canvas{}.stretch()}); }}}}
                         .bind(strip)
                         .stretch(),
                     hbox{tabs{{"A", label{"Side first"}}, {"B", vbox{canvas{}.stretch()}}}.sidebar().bind(side).stretch(),
                          stack{{"p", label{"Stack first"}}, {"q", vbox{canvas{}.stretch()}}}.bind(stk).stretch()}
                         .stretch()}}}
            .tabs(document_tabs{{{"a", "Alpha", {}, {}}, {"b", "Beta", {}, {}}}}.current("a").bind(docs))
            .size(640_px, 480_px));
    HWND top = test_window(L"At-once redraw");
    expect(top && strip && side && stk && docs, "the window and its pages");
    if (!top || !strip || !side || !stk || !docs) return 1;

    // Never on the desktop: cloaked before it's shown, or not shown at all.
    const BOOL cloak = TRUE;
    BOOL cloaked = FALSE;
    if (FAILED(DwmSetWindowAttribute(top, 13 /* DWMWA_CLOAK */, &cloak, sizeof cloak)) ||
        FAILED(DwmGetWindowAttribute(top, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked)) || !cloaked)
    {
        std::printf("skip: the window can't be cloaked, so it isn't shown\n");
        return 77;
    }
    win->show();
    pump();
    expect(IsWindowVisible(top) != FALSE, "shown (cloaked): a switch is made at once, as on screen");

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, watch, nullptr, GetCurrentThreadId());
    expect(hook != nullptr, "the hook set");
    if (!hook) return 1;

    const auto check = [&](const std::string& what)
    {
        pump();
        // Each canvas there now (a lazy page's, built by the switch), and
        // every window it lies in.
        std::vector<HWND> off_over_canvas;
        const std::vector<HWND> cs = canvases(top);
        for (HWND h : g_redraw_off)
        {
            bool over = h == top;
            for (HWND c : cs) over = over || h == c || IsChild(h, c);
            if (over) off_over_canvas.push_back(h);
        }
        char buf[64];
        std::string list;
        for (HWND h : off_over_canvas)
        {
            std::snprintf(buf, sizeof buf, " %p%s", static_cast<void*>(h), h == top ? " (the window)" : "");
            list += buf;
        }
        expect(off_over_canvas.empty(), what + ": no WM_SETREDRAW FALSE to the window or a canvas's ancestor" +
                                            (list.empty() ? "" : ":" + list));
        expect(IsWindowVisible(top) != FALSE, what + ": still shown");
        g_redraw_off.clear();
    };

    strip->set_current(1);
    check("strip: set_current to the canvas's page");
    strip->set_current(2);
    check("strip: set_current to the lazy page (built)");
    expect(canvases(top).size() == 4, "the lazy page's canvas built");
    // A user's switch: the selection, then TCN_SELCHANGE to its parent.
    if (HWND tab = tab_control(top, 3))
    {
        TabCtrl_SetCurSel(tab, 0);
        NMHDR n{tab, UINT_PTR(GetDlgCtrlID(tab)), UINT(TCN_SELCHANGE)};
        SendMessageW(GetParent(tab), WM_NOTIFY, n.idFrom, reinterpret_cast<LPARAM>(&n));
        check("strip: a user's switch (TCN_SELCHANGE)");
        expect(strip->current() == 0, "strip: the user's switch made");
    }
    else
        expect(false, "strip: its tab control found");
    strip->set_current(1);
    check("strip: back to the canvas's page");
    side->set_current(1);
    check("sidebar: set_current to the canvas's page");
    side->set_current(0);
    check("sidebar: set_current back");
    stk->show("q");
    check("stack: show the canvas's page");
    stk->show("p");
    check("stack: show back");
    docs->select("b");
    check("document tabs: select");
    docs->select("a");
    check("document tabs: select back");
    expect(g_seen > 0, "the hook saw the switches' messages");

    UnhookWindowsHookEx(hook);
    win.reset();
    pump();
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
