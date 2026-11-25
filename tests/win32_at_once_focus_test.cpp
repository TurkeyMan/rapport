// focus() within a relayout_at_once (#108): the window is shown and active.
// When the switch turned its drawing off (WM_SETREDRAW FALSE clears
// WS_VISIBLE), a focus() that asked IsWindowVisible kept the request for
// when the window next activates, which it never does: the focus stayed
// where it was. A
// control's focus(), a stack's (into its page), a toolbar button's (its
// strip, that button current), each from app code run as a tab's page
// comes in (its canvas first sized), lands. And a focus() within a layout
// is made once it's done (its shows wait for its moves): into a stack
// coming in with the page, shown by then; the latest one asked for only,
// each from its own place among what waits for the layout, a stack's own
// move of the focus into its new page among them.
//
// It activates its window and moves the focus: only on a CI runner
// (GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1); else skipped (77).
#include <rapport/rapport.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
bool env_is(const char* name, const char* value)
{
    const char* v = std::getenv(name);
    return v && std::strcmp(v, value) == 0;
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
std::vector<HWND> g_focused;  // what took the focus (WM_SETFOCUS), in order
LRESULT CALLBACK see_focus(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION)
        if (const auto* m = reinterpret_cast<const CWPSTRUCT*>(lp); m->message == WM_SETFOCUS)
            g_focused.push_back(m->hwnd);
    return CallNextHookEx(nullptr, code, wp, lp);
}
HWND edit_with_text(HWND top, const wchar_t* text)
{
    struct find
    {
        const wchar_t* text;
        HWND found = nullptr;
    } f{text};
    EnumChildWindows(
        top,
        [](HWND h, LPARAM p) -> BOOL
        {
            auto* f = reinterpret_cast<find*>(p);
            wchar_t t[64]{};
            GetWindowTextW(h, t, 64);
            if (wcscmp(t, f->text)) return TRUE;
            f->found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}
}  // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!env_is("GITHUB_ACTIONS", "true") || !env_is("RAPPORT_TEST_SEND_INPUT", "1"))
    {
        std::printf("skipped: needs activation (it activates its window and moves the focus), only on a CI "
                    "runner (GITHUB_ACTIONS=true, RAPPORT_TEST_SEND_INPUT=1)\n");
        return 77;
    }
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui)
    {
        std::printf("no win32 backend: %s\n", ui.error().c_str());
        return 77;
    }
    using namespace rap::layout;
    using namespace rap::literals;

    enum class target
    {
        control,
        stack,
        tool_button,
    };
    for (const target t : {target::control, target::stack, target::tool_button})
    {
        const std::string what = t == target::control ? "a control's focus()"
                                 : t == target::stack ? "a stack's focus()"
                                                      : "a toolbar button's focus()";
        std::unique_ptr<rap::window> win;
        rap::tabs* pages = nullptr;
        rap::entry *start = nullptr, *field = nullptr, *in_stack = nullptr;
        rap::stack* stk = nullptr;
        rap::tool_button* tool = nullptr;
        std::function<void()> in_layout;  // once, from the second page's canvas's first size
        win = (*ui)->create(window{
            "At-once focus",
            // The stack above the tabs, its page shown all along: one
            // coming in with the tab's page isn't shown yet as the
            // page's canvas is sized (a layout's shows wait for its
            // moves), and has no Tab stop to take the focus then.
            {vbox{stack{{"a", vbox{entry{}.bind(in_stack)}}}.bind(stk),
                  tabs{{"One", vbox{entry{}.bind(start)}},
                       {"Two",
                        vbox{canvas{}
                                 .on_resize(
                                     [&](rap::size_f)
                                     {
                                         if (auto f = std::exchange(in_layout, nullptr)) f();
                                     })
                                 .height(60_px),
                             entry{}.bind(field),
                             toolbar{tool_button{rap::icon::named("go-up"), "Up"}.bind(tool)}}}}
                      .bind(pages)
                      .stretch()}
                 .stretch()}}.size(360_px, 340_px));
        HWND top = win ? FindWindowW(nullptr, L"At-once focus") : nullptr;
        expect(top && pages && start && field && stk && in_stack && tool, what + ": the window and its controls");
        if (!top || !pages || !start || !field || !stk || !in_stack || !tool) return 1;
        win->present();
        pump();
        expect(GetForegroundWindow() == top, what + ": the window active");
        start->focus();
        pump();
        expect(start->has_focus(), what + ": the focus on the first page's field");
        in_layout = [&]
        {
            switch (t)
            {
            case target::control: field->focus(); break;
            case target::stack: stk->focus(); break;
            case target::tool_button: tool->focus(); break;
            }
        };
        pages->set_current(1);  // relayout_at_once: the page laid out, its canvas sized
        pump();
        expect(!in_layout, what + ": called within the switch (the second page's canvas sized)");
        const bool landed = t == target::control ? field->has_focus()
                            : t == target::stack ? in_stack->has_focus() && stk->has_focus()
                                                 : tool->has_focus();
        expect(landed, what + " within a relayout_at_once: the focus there, not left on the first page's field (" +
                           (start->has_focus() ? "still there" : "elsewhere") + ")");
        win.reset();
        pump();
    }
    // A stack coming in with the tab's page: its page isn't shown yet as
    // the page's canvas is sized (a layout's shows wait for its moves), so a
    // focus() then is made once the layout's done, into the page shown by
    // then. Only the latest focus() asked for within the layout is made: an
    // earlier one's control never takes the focus (WM_SETFOCUS, seen by a
    // hook on this thread).
    enum class order
    {
        stack_alone,
        field_then_stack,
        stack_then_field,
        stack_then_field_after,  // the field's after the switch returns, before what it put off is made
    };
    for (const order o : {order::stack_alone, order::field_then_stack, order::stack_then_field,
                          order::stack_then_field_after})
    {
        const std::string what = o == order::stack_alone ? "a stack coming in, its focus()"
                                 : o == order::field_then_stack
                                     ? "a field's focus() then the stack's"
                                 : o == order::stack_then_field
                                     ? "the stack's focus() then a field's"
                                     : "the stack's focus() within the switch, a field's after it";
        std::unique_ptr<rap::window> win;
        rap::tabs* pages = nullptr;
        rap::entry *start = nullptr, *field = nullptr, *in_stack = nullptr;
        rap::stack* stk = nullptr;
        std::function<void()> in_layout;
        win = (*ui)->create(window{
            "Deferred focus",
            {tabs{{"One", vbox{entry{}.bind(start)}},
                  {"Two",
                   vbox{canvas{}
                            .on_resize(
                                [&](rap::size_f)
                                {
                                    if (auto f = std::exchange(in_layout, nullptr)) f();
                                })
                            .height(60_px),
                        entry{}.bind(field), stack{{"a", vbox{entry{}.bind(in_stack)}}}.bind(stk)}}}
                 .bind(pages)
                 .stretch()}}.size(360_px, 300_px));
        HWND top = win ? FindWindowW(nullptr, L"Deferred focus") : nullptr;
        expect(top && pages && start && field && stk && in_stack,
               what + ": the window and its controls");
        if (!top || !pages || !start || !field || !stk || !in_stack) return 1;
        in_stack->set_text("in the stack");
        const HWND stack_edit = edit_with_text(top, L"in the stack");
        expect(stack_edit != nullptr, what + ": the stack's field");
        win->present();
        pump();
        expect(GetForegroundWindow() == top, what + ": the window active");
        start->focus();
        pump();
        expect(start->has_focus(), what + ": the focus on the first page's field");
        in_layout = [&]
        {
            if (o == order::field_then_stack) field->focus();
            stk->focus();
            if (o == order::stack_then_field) field->focus();
        };
        g_focused.clear();
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, see_focus, nullptr, GetCurrentThreadId());
        expect(hook != nullptr, what + ": the WM_SETFOCUS hook on this thread");
        if (!hook) return 1;
        pages->set_current(1);
        // Made outside any layout, before what the switch put off is: the
        // later one, so the one that stays.
        if (o == order::stack_then_field_after) field->focus();
        pump();
        if (hook) UnhookWindowsHookEx(hook);
        expect(!in_layout, what + ": called within the switch (the page's canvas sized)");
        if (o == order::stack_then_field || o == order::stack_then_field_after)
        {
            expect(field->has_focus(), what + ": the field has the focus, the latest asked for");
            size_t to_stack = 0;
            for (HWND h : g_focused) to_stack += h == stack_edit;
            expect(to_stack == 0, what + ": the stack's field never took it (" +
                                      std::to_string(to_stack) +
                                      " WM_SETFOCUS), the earlier focus() not made");
        }
        else
            expect(in_stack->has_focus() && stk->has_focus(),
                   what + ": the focus in the stack's page, shown by then (" +
                       (start->has_focus() ? "still on the first page's field" : "elsewhere") +
                       ")");
        win.reset();
        pump();
    }
    // Each focus() put off is made by its own continuation, in its place
    // among what waits for the layouts, and only while it's the latest: A
    // put off by one layout, then B made there and then (A's not made),
    // then, in a later layout before what waits is run, the stack's own
    // move of the focus into its new page (show(), the focus in its old
    // one: queued after the layout), then C put off. C lands last, after
    // the stack's move; made by A's continuation, ahead of that, it was
    // undone by it.
    {
        const std::string what =
            "A put off, B made, the stack's move then C put off by a later layout";
        std::unique_ptr<rap::window> win;
        rap::tabs* pages = nullptr;
        rap::entry *start = nullptr, *in_a = nullptr, *in_b = nullptr, *field = nullptr;
        rap::stack* stk = nullptr;
        std::function<void()> on_two,
            on_three;  // once each, from those pages' canvases' first sizes
        const auto sized_once = [](std::function<void()>& f)
        {
            return [&f](rap::size_f)
            {
                if (auto g = std::exchange(f, nullptr)) g();
            };
        };
        win = (*ui)->create(window{
            "Focus order",
            {vbox{stack{{"a", vbox{entry{}.bind(in_a)}}, {"b", vbox{entry{}.bind(in_b)}}}.bind(stk),
                  tabs{{"One", vbox{entry{}.bind(start)}},
                       {"Two", vbox{canvas{}.on_resize(sized_once(on_two)).height(60_px)}},
                       {"Three", vbox{canvas{}.on_resize(sized_once(on_three)).height(60_px),
                                      entry{}.bind(field)}}}
                      .bind(pages)
                      .stretch()}
                 .stretch()}}.size(360_px, 340_px));
        HWND top = win ? FindWindowW(nullptr, L"Focus order") : nullptr;
        expect(top && pages && start && in_a && in_b && field && stk,
               what + ": the window and its controls");
        if (!top || !pages || !start || !in_a || !in_b || !field || !stk) return 1;
        win->present();
        pump();
        expect(GetForegroundWindow() == top, what + ": the window active");
        start->focus();
        pump();
        on_two = [&] { start->focus(); };  // A: put off
        on_three = [&]
        {
            stk->show("b");  // the stack's move of the focus, queued after this layout
            field->focus();  // C: put off, after it
        };
        // No pumping between: what the layouts put off waits (its run
        // posted) until the end.
        pages->set_current(1);  // A
        in_a->focus();          // B, there and then: the focus in the stack's page "a"
        expect(in_a->has_focus(), what + ": B made there and then");
        pages->set_current(2);  // the stack's move, then C
        expect(!on_two && !on_three, what + ": both pages' canvases sized within their switches");
        pump();
        expect(field->has_focus(), what + ": C has the focus, after the stack's move (" +
                                       (in_b->has_focus()    ? "the stack's page \"b\" has it"
                                        : start->has_focus() ? "A's field has it"
                                                             : "elsewhere") +
                                       ")");
        win.reset();
        pump();
    }
    // The stack's own move of the focus into its new page (show(), with the
    // focus in its old one) is a focus request as an explicit one is: the
    // latest of them is made, of either kind. show() within a layout, then
    // a field's focus() made there and then before what waits is run: the
    // field keeps it. And a field's focus() put off, then show() in the
    // same layout: the stack's move, the later one. (That one held before
    // too, both run in their order; it's here as the other half.)
    for (const bool show_first : {true, false})
    {
        const std::string what =
            show_first ? "the stack's show() within a switch, a field's focus() after it"
                       : "a field's focus() within a switch, then the stack's show()";
        std::unique_ptr<rap::window> win;
        rap::tabs* pages = nullptr;
        rap::entry *in_a = nullptr, *in_b = nullptr, *field = nullptr;
        rap::stack* stk = nullptr;
        std::function<void()> on_two;
        win = (*ui)->create(window{
            "Stack move",
            {vbox{stack{{"a", vbox{entry{}.bind(in_a)}}, {"b", vbox{entry{}.bind(in_b)}}}.bind(stk),
                  tabs{{"One", vbox{label{"First"}}},
                       {"Two", vbox{canvas{}
                                        .on_resize(
                                            [&](rap::size_f)
                                            {
                                                if (auto f = std::exchange(on_two, nullptr)) f();
                                            })
                                        .height(60_px),
                                    entry{}.bind(field)}}}
                      .bind(pages)
                      .stretch()}
                 .stretch()}}.size(360_px, 340_px));
        HWND top = win ? FindWindowW(nullptr, L"Stack move") : nullptr;
        expect(top && pages && in_a && in_b && field && stk,
               what + ": the window and its controls");
        if (!top || !pages || !in_a || !in_b || !field || !stk) return 1;
        win->present();
        pump();
        expect(GetForegroundWindow() == top, what + ": the window active");
        in_a->focus();  // in the stack's page "a": its show() moves the focus into "b"
        pump();
        expect(in_a->has_focus(), what + ": the focus in the stack's page \"a\"");
        if (show_first)
            on_two = [&] { stk->show("b"); };
        else
            on_two = [&]
            {
                field->focus();
                stk->show("b");
            };
        pages->set_current(1);
        if (show_first) field->focus();  // there and then, before what the switch put off is run
        expect(!on_two, what + ": called within the switch (the page's canvas sized)");
        pump();
        const auto where = [&]
        {
            return std::string(field->has_focus()  ? "the field has it"
                               : in_b->has_focus() ? "the stack's page \"b\" has it"
                               : in_a->has_focus() ? "the stack's page \"a\" has it"
                                                   : "elsewhere");
        };
        if (show_first)
            expect(field->has_focus(), what + ": the field keeps it, the later (" + where() + ")");
        else
            expect(in_b->has_focus(), what + ": the stack's move, the later (" + where() + ")");
        win.reset();
        pump();
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
