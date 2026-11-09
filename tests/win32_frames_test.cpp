// A Win32 list, tree and text view with and without their frames.
//   rapport-win32-frames-test
#include <rapport/rapport.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
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

class rows : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t, size_t) const override { return "row"; }
};

// The controls of a kind on this thread, in creation order.
std::vector<HWND> controls(const wchar_t* cls)
{
    struct search
    {
        const wchar_t* cls;
        std::vector<HWND> found;
    } s{cls, {}};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND top, LPARAM p) -> BOOL
        {
            EnumChildWindows(
                top,
                [](HWND c, LPARAM q) -> BOOL
                {
                    auto* s = reinterpret_cast<search*>(q);
                    wchar_t name[32] = {};
                    GetClassNameW(c, name, 32);
                    if (!_wcsicmp(name, s->cls)) s->found.push_back(c);
                    return TRUE;
                },
                p);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

bool framed(HWND h)
{
    return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0;
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
    auto win = (*ui)->create(window{
        "t",
        {vbox{hbox{list{{{"Name"}}, &model}.stretch(), tree{}.stretch(), text_view{}.stretch()},
              hbox{list{{{"Name"}}, &model}.frame(false).stretch(), tree{}.frame(false).stretch(),
                   text_view{}.frame(false).stretch()}}}});
    win->show();

    for (const wchar_t* cls : {WC_LISTVIEWW, WC_TREEVIEWW, L"RICHEDIT50W"})
    {
        const std::vector<HWND> found = controls(cls);
        std::string name;
        for (const wchar_t* c = cls; *c; ++c) name += char(*c);
        expect(found.size() == 2, name + ": both are there");
        if (found.size() != 2) continue;
        expect(framed(found[0]) != framed(found[1]), name + ": one framed, one not");
    }
    win->close();
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
