// An app's row height on Qt 6 (list::row_height, tree::row_height,
// look_options::row_height): each row max(row_height, the font's line and
// 2), in Qt's logical px (Qt has the DPI); measured as the view lays it out
// (visualRect of row 0). A list's own row_height(20), a tree's, an unset
// list its default (the font's line and 6, 22 at least); with the app's 20
// (--app, which this runs itself as a second process: the look is the
// app's, set as it loads), a plain list, a tree and a sidebar's rows 20, a
// list's own 28 over it, and a sidebar at its natural size every row whole.
// Run off the desktop: QT_QPA_PLATFORM=offscreen.
//   rapport-row-height-test-qt6 [--app]
#include <rapport/rapport.h>

#include <QApplication>
#include <QFontMetrics>
#include <QListWidget>
#include <QTreeView>
#include <QTreeWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

int fails = 0;
void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++fails;
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 3; }
    std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row); }
};
rows_model g_model;

void spin()
{
    for (int i = 0; i < 20; ++i) QApplication::processEvents();
}

// The row `logical` px asks for in `w`: its font's line and 2 at least.
int want(QWidget* w, float logical)
{
    return std::max(int(std::lround(logical)), QFontMetrics(w->font()).height() + 2);
}

// A window's item views of type T, left to right.
template <class T>
std::vector<T*> views(QWidget* top)
{
    std::vector<T*> out;
    for (T* v : top->findChildren<T*>()) out.push_back(v);
    std::sort(out.begin(), out.end(),
              [top](QWidget* a, QWidget* b) { return a->mapTo(top, QPoint()).x() < b->mapTo(top, QPoint()).x(); });
    return out;
}
// The lists (rapport's QTreeViews that aren't QTreeWidgets), left to right.
std::vector<QTreeView*> lists_of(QWidget* top)
{
    std::vector<QTreeView*> out;
    for (QTreeView* v : views<QTreeView>(top))
        if (!qobject_cast<QTreeWidget*>(v)) out.push_back(v);
    return out;
}
int row_of(QTreeView* v)
{
    return v->visualRect(v->model()->index(0, 0)).height();
}

QWidget* top_titled(const char* title)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == title) return w;
    return nullptr;
}

int run_own(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    auto win = ui.create(window{"Row height",
                                {hbox{list{cols, &g_model}.row_height(20).stretch(), list{cols, &g_model}.stretch(),
                                      tree{items}.row_height(20).stretch()}}}
                             .size(600_px, 300_px));
    win->show();
    spin();
    QWidget* top = top_titled("Row height");
    expect(top != nullptr, "the window");
    if (!top) return 1;
    const std::vector<QTreeView*> lists = lists_of(top);
    const std::vector<QTreeWidget*> trees = views<QTreeWidget>(top);
    expect(lists.size() == 2 && trees.size() == 1, "two lists and a tree");
    if (lists.size() != 2 || trees.size() != 1) return 1;
    expect(row_of(lists[0]) == want(lists[0], 20),
           "a list's own 20: " + std::to_string(want(lists[0], 20)) + ", " + std::to_string(row_of(lists[0])));
    const int plain = std::max(QFontMetrics(lists[1]->font()).height() + 6, 22);
    expect(row_of(lists[1]) == plain,
           "an unset list its default, " + std::to_string(plain) + ": " + std::to_string(row_of(lists[1])));
    expect(row_of(trees[0]) == want(trees[0], 20),
           "a tree's own 20: " + std::to_string(want(trees[0], 20)) + ", " + std::to_string(row_of(trees[0])));
    return 0;
}

int run_app(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    const std::vector<rap::column> cols{{"Name", 120_px}};
    const std::vector<rap::tree_item> items{{"Alpha", "a"}, {"Beta", "b"}};
    auto win = ui.create(window{"Row height app",
                                {hbox{list{cols, &g_model}.row_height(28).stretch(), list{cols, &g_model}.stretch(),
                                      tree{items}.stretch(),
                                      tabs{{"One", label{"one"}}, {"Two", label{"two"}}}.sidebar().stretch()}}}
                             .size(900_px, 300_px));
    win->show();
    spin();
    QWidget* top = top_titled("Row height app");
    expect(top != nullptr, "the window");
    if (!top) return 1;
    const std::vector<QTreeView*> lists = lists_of(top);
    const std::vector<QTreeWidget*> trees = views<QTreeWidget>(top);
    const std::vector<QListWidget*> sides = views<QListWidget>(top);
    expect(lists.size() == 2 && trees.size() == 1 && sides.size() == 1, "two lists, a tree and a sidebar");
    if (lists.size() != 2 || trees.size() != 1 || sides.size() != 1) return 1;
    expect(row_of(lists[0]) == want(lists[0], 28), "a list's own 28 over the app's 20: " +
                                                       std::to_string(row_of(lists[0])));
    expect(row_of(lists[1]) == want(lists[1], 20), "a plain list the app's 20: " + std::to_string(row_of(lists[1])));
    expect(row_of(trees[0]) == want(trees[0], 20), "a tree the app's 20: " + std::to_string(row_of(trees[0])));
    const int side = sides[0]->visualItemRect(sides[0]->item(0)).height();
    expect(side == want(sides[0], 20), "a sidebar's rows the app's 20: " + std::to_string(side));

    // A sidebar of four pages in a window of its natural size: every row whole.
    auto fit = ui.create(window{"Row height sidebar",
                                {tabs{{"General", label{"one"}},
                                      {"Appearance", label{"two"}},
                                      {"Fonts", label{"three"}},
                                      {"Advanced", label{"four"}}}
                                     .sidebar()}});
    fit->show();
    spin();
    QWidget* fit_top = top_titled("Row height sidebar");
    const std::vector<QListWidget*> fit_sides = fit_top ? views<QListWidget>(fit_top) : std::vector<QListWidget*>{};
    expect(fit_sides.size() == 1, "a sidebar in a window of its natural size");
    if (fit_sides.size() == 1)
    {
        QListWidget* l = fit_sides[0];
        const QRect view = l->viewport()->rect();
        for (int i = 0; i < l->count(); ++i)
        {
            const QRect r = l->visualItemRect(l->item(i));
            expect(r.top() >= view.top() && r.bottom() <= view.bottom(),
                   "its row " + std::to_string(i) + " whole, " + std::to_string(r.top()) + ".." +
                       std::to_string(r.bottom()) + " in " + std::to_string(view.top()) + ".." +
                       std::to_string(view.bottom()));
        }
        // A much larger font (a larger text size): its rows its line and 2
        // at least, and its least height every row, again.
        QFont big = l->font();
        big.setPointSizeF(big.pointSizeF() * 3);
        l->setFont(big);
        spin();
        const int line = QFontMetrics(l->font()).height();
        const int row = l->visualItemRect(l->item(0)).height();
        expect(row >= line + 2, "a larger font: its rows its line and 2, " + std::to_string(line + 2) + ": " +
                                    std::to_string(row));
        const int least = row * l->count() + 2 * l->frameWidth();
        expect(l->minimumHeight() >= least, "a larger font: its least height every row, " + std::to_string(least) +
                                                ": " + std::to_string(l->minimumHeight()));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const bool app = argc > 1 && std::string(argv[1]) == "--app";
    rap::load_options opts;
    opts.backend = "qt6";
    if (app) opts.look.row_height = 20;
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 2;
    }
    int r = app ? run_app(**ui) : run_own(**ui);
    // The app's row height: the look's, set as the backend loads, so in a
    // process of its own.
    if (!app && r == 0)
    {
        const std::string self = std::string("\"") + argv[0] + "\" --app";
        if (std::system(self.c_str()) != 0) ++fails;
    }
    std::printf(fails || r ? "%s failed\n" : "%s passed\n", app ? "--app" : "row height");
    return fails || r ? 1 : 0;
}
