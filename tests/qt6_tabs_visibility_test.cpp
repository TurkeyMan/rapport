// Tab page visibility on Qt 6 (rap::tabs::set_page_visible,
// layout::page::visible, tabs::hide_single_tab), read from the widgets
// themselves: QTabBar's tabs (isTabVisible, their order by tabRect) and a
// sidebar's QListWidget rows (isRowHidden, their order by visualItemRect);
// indices stable, the current page moving to the nearest visible one
// without on_changed, -1 with none visible, set_current on a hidden page
// ignored, pages hidden from the start (a lazy one unbuilt), the sidebar's
// natural height counting its visible rows, and hide_single_tab: no strip
// (or list) with one page visible, the page measuring alone. And #92: a
// canvas of fixed size as a tab page, a sidebar page, a stack page and a
// window's root, measured to include it. Never shown: measured only.
// Run off the desktop: QT_QPA_PLATFORM=offscreen.
#include <rapport/rapport.h>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>

#include <algorithm>
#include <cstdio>
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

std::string str(QSize s)
{
    return std::to_string(s.width()) + "x" + std::to_string(s.height());
}

QWidget* top_titled(const char* title)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->windowTitle() == title) return w;
    return nullptr;
}

// The tabs shown, by index, in the order the bar lays them out.
std::vector<int> bar_order(QTabBar* bar)
{
    std::vector<int> shown;
    for (int i = 0; i < bar->count(); ++i)
        if (bar->isTabVisible(i)) shown.push_back(i);
    std::stable_sort(shown.begin(), shown.end(),
                     [bar](int a, int b) { return bar->tabRect(a).x() < bar->tabRect(b).x(); });
    return shown;
}
// The rows shown, by index, in the order the list lays them out.
std::vector<int> list_order(QListWidget* list)
{
    list->doItemsLayout();
    std::vector<int> shown;
    for (int i = 0; i < list->count(); ++i)
        if (!list->isRowHidden(i)) shown.push_back(i);
    std::stable_sort(shown.begin(), shown.end(),
                     [list](int a, int b)
                     { return list->visualItemRect(list->item(a)).y() < list->visualItemRect(list->item(b)).y(); });
    return shown;
}
std::string str(const std::vector<int>& v)
{
    std::string s;
    for (int i : v) s += (s.empty() ? "" : ",") + std::to_string(i);
    return "[" + s + "]";
}

// The headers shown, in order, of a strip or a sidebar.
struct headers
{
    QTabBar* bar = nullptr;
    QListWidget* list = nullptr;
    std::vector<int> order() const { return bar ? bar_order(bar) : list_order(list); }
    int count() const { return bar ? bar->count() : list->count(); }
};

void run_visibility(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar: " : "strip: ";
    const char* title = sidebar ? "Visibility sidebar" : "Visibility strip";
    rap::tabs* t = nullptr;
    rap::label* lazy_label = nullptr;
    int changes = 0;
    auto win = ui.create(window{title,
                                {tabs{{"A", label{"a"}},
                                      {"B", label{"b"}},
                                      {"C", label{"c"}},
                                      {"D", lazy{[&] { return rap::node(label{"d"}.bind(lazy_label)); }}}}
                                     .sidebar(sidebar)
                                     .on_changed([&](int) { ++changes; })
                                     .bind(t)}});
    QWidget* top = top_titled(title);
    expect(t && top, kind + "bound, the window");
    if (!t || !top) return;
    headers h;
    QTabWidget* tw = nullptr;
    QStackedWidget* sidestack = nullptr;
    if (sidebar)
    {
        h.list = top->findChild<QListWidget*>();
        for (QStackedWidget* s : top->findChildren<QStackedWidget*>())
            if (s->count() == 4) sidestack = s;
    }
    else
    {
        tw = top->findChild<QTabWidget*>();
        h.bar = tw ? tw->tabBar() : nullptr;
    }
    expect(h.bar || h.list, kind + "the headers");
    if (!h.bar && !h.list) return;
    expect(h.order() == std::vector<int>{0, 1, 2, 3}, kind + "four headers " + str(h.order()));
    // The page actually shown, natively: the QTabWidget's or the stack's.
    auto native_current = [&] { return tw ? tw->currentIndex() : sidestack ? sidestack->currentIndex() : -2; };

    // A middle page hidden: still four headers, one hidden; indices stable.
    t->set_page_visible(1, false);
    expect(h.count() == 4 && h.order() == std::vector<int>{0, 2, 3},
           kind + "page 1 hidden: headers " + str(h.order()) + " of " + std::to_string(h.count()));
    expect(!t->page_visible(1) && t->page_visible(0) && t->page_visible(2), kind + "page_visible: 1 not, 0 and 2");
    t->set_current(2);
    expect(t->current() == 2 && native_current() == 2, kind + "page 2 is still 2");
    expect(!tw || tw->tabText(2) == "C", kind + "tab 2 is still C");
    // set_current on a hidden page: nothing.
    const int before = changes;
    t->set_current(1);
    expect(t->current() == 2 && native_current() == 2 && changes == before, kind + "set_current(hidden 1): still 2");
    changes = 0;

    // The current page hidden: the nearest after it (the lazy D, built).
    t->set_page_visible(2, false);
    expect(t->current() == 3 && native_current() == 3,
           kind + "page 2 (current) hidden: 3 current, " + std::to_string(t->current()) + "/" +
               std::to_string(native_current()));
    expect(lazy_label != nullptr, kind + "the lazy page, made current, built");
    // The last page hidden while current: the nearest before it.
    t->set_page_visible(3, false);
    expect(t->current() == 0 && native_current() == 0,
           kind + "page 3 (current, last) hidden: 0 current, " + std::to_string(t->current()) + "/" +
               std::to_string(native_current()));
    // All hidden: -1, no header and no page shown.
    t->set_page_visible(0, false);
    expect(t->current() == -1, kind + "every page hidden: current -1, " + std::to_string(t->current()));
    expect(h.order().empty(), kind + "every page hidden: no header, " + str(h.order()));
    if (tw)
    {
        auto* stack = tw->findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly);
        expect(stack && stack->isHidden(), kind + "every page hidden: the page stack hidden");
    }
    else
        expect(sidestack && sidestack->isHidden(), kind + "every page hidden: the page stack hidden");
    // Shown again: current again (it was -1); another shown keeps it, and
    // its header goes back at its place.
    t->set_page_visible(2, true);
    expect(t->current() == 2 && native_current() == 2, kind + "page 2 shown again with none current: 2 current");
    if (tw)
    {
        auto* stack = tw->findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly);
        expect(stack && !stack->isHidden(), kind + "a page shown: the page stack back");
    }
    else
        expect(sidestack && !sidestack->isHidden(), kind + "a page shown: the page stack back");
    t->set_page_visible(1, true);
    expect(t->current() == 2 && t->page_visible(1), kind + "page 1 shown again: current stays 2");
    t->set_page_visible(0, true);
    t->set_page_visible(3, true);
    expect(h.order() == std::vector<int>{0, 1, 2, 3}, kind + "all shown again, in order: " + str(h.order()));
    t->set_page_visible(1, false);
    t->set_page_visible(1, true);
    expect(h.order() == std::vector<int>{0, 1, 2, 3}, kind + "1 hidden and shown: back between 0 and 2, " + str(h.order()));
    // The same again: nothing.
    t->set_page_visible(1, true);
    expect(t->current() == 2, kind + "page 1 shown again, twice: current stays 2");
    expect(changes == 0, kind + "no on_changed from any of it, " + std::to_string(changes));
    t->set_page_visible(9, false);
    expect(!t->page_visible(9) && !t->page_visible(-1), kind + "pages out of range: not visible");

    // The sidebar's natural height: its visible rows (labels as pages, so
    // the list is the taller).
    if (sidebar)
    {
        QWidget* box = t ? h.list->parentWidget() : nullptr;
        const int list4 = h.list->sizeHint().height(), box4 = box->sizeHint().height();
        t->set_page_visible(3, false);
        const int list3 = h.list->sizeHint().height(), box3 = box->sizeHint().height();
        const int row = h.list->sizeHintForRow(0);
        expect(list3 == list4 - row && box3 < box4,
               kind + "a row hidden: the list " + std::to_string(list4) + " to " + std::to_string(list3) +
                   " (a row " + std::to_string(row) + "), the sidebar " + std::to_string(box4) + " to " +
                   std::to_string(box3));
        t->set_page_visible(3, true);
        expect(box->sizeHint().height() == box4, kind + "shown again: the sidebar " + std::to_string(box4) + " again");
        // Every row hidden: still a row's height (and never under Qt's own
        // minimum), not just the frame.
        for (int i = 0; i < 4; ++i) t->set_page_visible(i, false);
        const int none = h.list->sizeHint().height();
        expect(none >= row + 2 * h.list->frameWidth() && none >= h.list->minimumSizeHint().height(),
               kind + "every row hidden: the list a row high at least, " + std::to_string(none) + " (a row " +
                   std::to_string(row) + ", Qt's minimum " + std::to_string(h.list->minimumSizeHint().height()) +
                   ")");
        for (int i = 0; i < 4; ++i) t->set_page_visible(i, true);
    }
}

// Pages hidden from the start, and hide_single_tab.
void run_hidden(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, " : "strip, ";
    const char* title = sidebar ? "Hidden sidebar" : "Hidden strip";
    rap::tabs* t = nullptr;
    rap::label* lazy_label = nullptr;
    int changes = 0;
    auto win = ui.create(
        window{title,
               {tabs{page{"A", lazy{[&] { return rap::node(label{"a"}.bind(lazy_label)); }}}.visible(false),
                     // B larger than C: hidden, it takes no layout.
                     page{"B", label{"A page much wider and taller than C\nover\nfour\nlines"}}.visible(false),
                     {"C", label{"c"}}}
                    .sidebar(sidebar)
                    .hide_single_tab()
                    .on_changed([&](int) { ++changes; })
                    .bind(t)}});
    QWidget* top = top_titled(title);
    expect(t && top, kind + "hidden from the start: bound, the window");
    if (!t || !top) return;
    expect(!t->page_visible(0) && !t->page_visible(1) && t->page_visible(2),
           kind + "hidden from the start: A and B hidden, C not");
    expect(t->current() == 2, kind + "hidden from the start: C current, " + std::to_string(t->current()));
    expect(lazy_label == nullptr, kind + "hidden from the start: the lazy A unbuilt");
    expect(changes == 0, kind + "hidden from the start: no on_changed, " + std::to_string(changes));

    if (!sidebar)
    {
        auto* tw = top->findChild<QTabWidget*>();
        expect(tw != nullptr, kind + "the tab widget");
        if (!tw) return;
        QTabBar* bar = tw->tabBar();
        expect(!bar->isTabVisible(0) && !bar->isTabVisible(1) && bar->isTabVisible(2),
               kind + "hidden from the start: tabs A and B hidden");
        expect(tw->currentIndex() == 2, kind + "hidden from the start: the tab widget on C");
        // hide_single_tab: no strip, and the tab widget measures as C alone.
        QWidget* c = tw->widget(2);
        const QSize alone = tw->sizeHint(), alone_min = tw->minimumSizeHint();
        expect(bar->isHidden(), kind + "one page: no strip");
        expect(tw->documentMode(), kind + "one page: no frame round it");
        expect(alone == c->sizeHint(), kind + "one page: measured as the page alone, " + str(alone) + ", the page " +
                                           str(c->sizeHint()));
        t->set_page_visible(1, true);
        expect(!bar->isHidden(), kind + "a second page: the strip back");
        expect(!tw->documentMode(), kind + "a second page: the frame back");
        const QSize with = tw->sizeHint(), with_min = tw->minimumSizeHint();
        expect(with.height() >= alone.height() + bar->sizeHint().height(),
               kind + "a second page: measured with the strip, " + str(with) + " over " + str(alone));
        expect(with_min.height() > alone_min.height(),
               kind + "a second page: the least size with the strip, " + str(with_min) + " over " + str(alone_min));
        t->set_page_visible(1, false);
        expect(bar->isHidden() && tw->sizeHint() == alone && tw->minimumSizeHint() == alone_min,
               kind + "hidden again: no strip, measured alone again, " + str(tw->sizeHint()));
    }
    else
    {
        auto* list = top->findChild<QListWidget*>();
        expect(list != nullptr, kind + "the list");
        if (!list) return;
        QWidget* box = list->parentWidget();
        expect(list->isRowHidden(0) && list->isRowHidden(1) && !list->isRowHidden(2),
               kind + "hidden from the start: rows A and B hidden");
        QStackedWidget* stack = nullptr;
        for (QStackedWidget* s : box->findChildren<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly))
            stack = s;
        expect(stack && stack->currentIndex() == 2, kind + "hidden from the start: the stack on C");
        if (!stack) return;
        const QSize alone = box->sizeHint(), alone_min = box->minimumSizeHint();
        const QSize page = stack->widget(2)->sizeHint();
        expect(list->isHidden(), kind + "one page: no list");
        expect(alone == page, kind + "one page: measured as the page alone, " + str(alone) + ", the page " + str(page));
        t->set_page_visible(1, true);
        expect(!list->isHidden(), kind + "a second page: the list back");
        const QSize with = box->sizeHint(), with_min = box->minimumSizeHint();
        expect(with.width() >= alone.width() + list->width(),
               kind + "a second page: measured with the list, " + str(with) + " over " + str(alone));
        expect(with_min.width() > alone_min.width(),
               kind + "a second page: the least size with the list, " + str(with_min) + " over " + str(alone_min));
        t->set_page_visible(1, false);
        expect(list->isHidden() && box->sizeHint() == alone && box->minimumSizeHint() == alone_min,
               kind + "hidden again: no list, measured alone again, " + str(box->sizeHint()));
    }
    t->set_page_visible(0, true);
    expect(t->current() == 2 && lazy_label == nullptr, kind + "A shown: C stays current, A still unbuilt");
    t->set_current(0);
    expect(t->current() == 0 && lazy_label != nullptr, kind + "A made current: built");
}

// Whether `w` (itself, or under it) is a label reading `text`, not hidden.
bool holds_label(QWidget* w, const char* text)
{
    if (!w || w->isHidden()) return false;
    if (auto* l = qobject_cast<QLabel*>(w); l && l->text() == text) return true;
    for (QLabel* l : w->findChildren<QLabel*>())
        if (l->text() == text && l->isVisibleTo(w)) return true;
    return false;
}

// #98: set_current is the app's own: no on_changed, the page made current
// natively, a lazy one built and shown; to the current page, nothing. A lazy
// page made current by hiding the current one: built and shown, no
// on_changed. A user's selection, made on the native widget as a click's
// reaches rapport: on_changed once, with the page.
void run_set_current_quiet(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, set_current (#98): " : "strip, set_current (#98): ";
    const char* title = sidebar ? "Quiet sidebar" : "Quiet strip";
    rap::tabs* t = nullptr;
    rap::label* built[4] = {};
    std::vector<int> changes;
    auto lazy_page = [&built](int i, const char* text)
    { return lazy{[&built, i, text] { return rap::node(label{text}.bind(built[i])); }}; };
    auto win = ui.create(window{title,
                                {tabs{{"A", label{"page a"}},
                                      {"B", lazy_page(1, "page b")},
                                      {"C", lazy_page(2, "page c")},
                                      {"D", lazy_page(3, "page d")}}
                                     .sidebar(sidebar)
                                     .on_changed([&](int i) { changes.push_back(i); })
                                     .bind(t)}});
    QWidget* top = top_titled(title);
    QTabWidget* tw = nullptr;
    QListWidget* list = nullptr;
    QStackedWidget* sidestack = nullptr;
    if (top && sidebar)
    {
        list = top->findChild<QListWidget*>();
        for (QStackedWidget* s : top->findChildren<QStackedWidget*>())
            if (s->count() == 4) sidestack = s;
    }
    else if (top)
        tw = top->findChild<QTabWidget*>();
    expect(t && (tw || (list && sidestack)), kind + "bound, its native widgets found");
    if (!t || !(tw || (list && sidestack))) return;
    // The page natively current: the tab widget's, or the list's current
    // row when the stack shows the same page (-2 when they disagree).
    auto native = [&]() -> int
    {
        if (tw) return tw->currentIndex();
        return sidestack->currentIndex() == list->currentRow() ? list->currentRow() : -2;
    };
    // The page natively shown holds the label `text`.
    auto showing = [&](const char* text)
    { return holds_label(tw ? tw->currentWidget() : sidestack->currentWidget(), text); };
    auto seq = [](const std::vector<int>& v) { return str(v); };
    expect(t->current() == 0 && native() == 0 && showing("page a") && !built[1] && !built[2] && !built[3],
           kind + "A current and shown, the lazy B, C and D unbuilt");
    changes.clear();

    // To the lazy B: no on_changed; current, natively too; built, shown.
    t->set_current(1);
    QApplication::processEvents();
    expect(changes.empty(), kind + "set_current(1): no on_changed, " + seq(changes));
    expect(t->current() == 1 && native() == 1, kind + "set_current(1): B current, natively too, " +
                                                    std::to_string(t->current()) + "/" + std::to_string(native()));
    expect(built[1] && showing("page b"), kind + "set_current(1): the lazy B built and shown");
    // The current page again: nothing.
    t->set_current(1);
    QApplication::processEvents();
    expect(changes.empty(), kind + "set_current(1) again: no on_changed, " + seq(changes));
    expect(t->current() == 1 && native() == 1 && showing("page b") && !built[2] && !built[3],
           kind + "set_current(1) again: nothing changed, " + std::to_string(t->current()) + "/" +
               std::to_string(native()));
    // B (current) hidden: the lazy C, the nearest after it, built and shown,
    // no on_changed.
    t->set_page_visible(1, false);
    QApplication::processEvents();
    expect(t->current() == 2 && native() == 2, kind + "B (current) hidden: C current, natively too, " +
                                                    std::to_string(t->current()) + "/" + std::to_string(native()));
    expect(built[2] && showing("page c"), kind + "B (current) hidden: the lazy C built and shown");
    expect(changes.empty(), kind + "B (current) hidden: no on_changed, " + seq(changes));

    // A user's selection of the lazy D, on the native widget: on_changed
    // once, with 3; built and shown.
    if (list)
        list->setCurrentRow(3);
    else
        tw->setCurrentIndex(3);
    QApplication::processEvents();
    expect(changes == std::vector<int>{3}, kind + "a user's selection of D: on_changed once, with 3, " + seq(changes));
    expect(t->current() == 3 && native() == 3 && built[3] && showing("page d"),
           kind + "a user's selection of D: D current, built and shown, " + std::to_string(t->current()) + "/" +
               std::to_string(native()));
    // set_current to the current page: nothing.
    changes.clear();
    t->set_current(3);
    QApplication::processEvents();
    expect(changes.empty() && t->current() == 3 && native() == 3 && showing("page d"),
           kind + "set_current(3), current already: nothing, " + seq(changes));
    // Back to A, the app's: no on_changed.
    t->set_current(0);
    QApplication::processEvents();
    expect(changes.empty() && t->current() == 0 && native() == 0 && showing("page a"),
           kind + "set_current(0): A current and shown, no on_changed, " + seq(changes));
}

// Every page hidden from the start: none current, no page shown.
void run_none(rap::ui& ui, bool sidebar)
{
    using namespace rap::layout;
    const std::string kind = sidebar ? "sidebar, " : "strip, ";
    const char* title = sidebar ? "None sidebar" : "None strip";
    rap::tabs* t = nullptr;
    auto win = ui.create(window{
        title,
        {tabs{page{"A", label{"a"}}.visible(false), page{"B", label{"b"}}.visible(false)}.sidebar(sidebar).bind(t)}});
    expect(t && t->current() == -1, kind + "every page hidden from the start: current -1");
    if (!t) return;
    t->set_page_visible(1, true);
    expect(t->current() == 1, kind + "then B shown: B current, " + std::to_string(t->current()));
}

// #92: a canvas of fixed size as a page's (and a window's) root content.
void run_fixed_canvas(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    auto fixed = [] { return canvas{}.width(300_px).height(200_px); };
    const std::vector<std::pair<std::string, rap::node>> places{
        {"#92 tab page", tabs{{"A", fixed()}, {"B", label{"b"}}}},
        {"#92 sidebar page", tabs{{"A", fixed()}, {"B", label{"b"}}}.sidebar()},
        {"#92 stack page", stack{{"a", fixed()}, {"b", label{"b"}}}},
        {"#92 root", fixed()},
    };
    for (const auto& [title, content] : places)
    {
        auto win = ui.create(window{title, {content}});
        QWidget* top = top_titled(title.c_str());
        expect(top != nullptr, title + ": the window");
        if (!top) continue;
        const QSize s = top->sizeHint(), m = top->minimumSizeHint();
        expect(s.width() >= 300 && s.height() >= 200, title + ": measured to hold 300x200, " + str(s));
        expect(m.width() >= 300 && m.height() >= 200, title + ": its least size holds 300x200, " + str(m));
    }
}

// The default strip (shown, two pages or more visible): a large hidden page
// raises neither the tabs' minimum nor their natural size; shown again, it
// does.
void run_hidden_large(rap::ui& ui)
{
    using namespace rap::layout;
    using namespace rap::literals;
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Hidden large page",
                                {tabs{{"A", label{"a"}},
                                      page{"B", canvas{}.width(600_px).height(400_px)}.visible(false),
                                      {"C", label{"c"}}}
                                     .bind(t)}});
    QWidget* top = top_titled("Hidden large page");
    auto* tw = top ? top->findChild<QTabWidget*>() : nullptr;
    expect(t && tw, "hidden large page: the tab widget");
    if (!t || !tw) return;
    const QSize least = tw->minimumSizeHint(), natural = tw->sizeHint();
    expect(least.width() < 600 && least.height() < 400 && natural.width() < 600 && natural.height() < 400,
           "hidden large page, strip shown: not counted, least " + str(least) + ", natural " + str(natural));
    t->set_page_visible(1, true);
    const QSize least2 = tw->minimumSizeHint(), natural2 = tw->sizeHint();
    expect(least2.width() >= 600 && least2.height() >= 400 && natural2.width() >= 600 && natural2.height() >= 400,
           "hidden large page shown: counted, least " + str(least2) + ", natural " + str(natural2));
    t->set_page_visible(1, false);
    expect(tw->minimumSizeHint() == least && tw->sizeHint() == natural,
           "hidden large page hidden again: as before, " + str(tw->minimumSizeHint()));
}

// Many long titles, strip shown: the natural width is Qt's own (its bar
// bounded as it is with scroll buttons), not every title's width summed.
void run_long_titles(rap::ui& ui)
{
    using namespace rap::layout;
    rap::tabs* t = nullptr;
    std::vector<std::string> titles;
    for (int i = 0; i < 20; ++i) titles.push_back("A rather long tab title, number " + std::to_string(i));
    rap::spec::tabs s;
    for (const std::string& title : titles)
    {
        s.titles.push_back(title);
        s.pages.push_back(label{"x"});
    }
    s.bind = &t;
    auto win = ui.create(window{"Long titles", {rap::node{s, {}}}});
    QWidget* top = top_titled("Long titles");
    auto* tw = top ? top->findChild<QTabWidget*>() : nullptr;
    expect(t && tw, "long titles: the tab widget");
    if (!t || !tw) return;
    // Qt's own, the same titles and pages as plain labels.
    QTabWidget native;
    native.setFont(tw->font());
    native.setUsesScrollButtons(tw->usesScrollButtons());
    for (const std::string& title : titles) native.addTab(new QLabel("x"), QString::fromStdString(title));
    const int ours = tw->sizeHint().width(), qt = native.sizeHint().width();
    expect(ours <= qt + 16 && ours < 400,
           "long titles: natural width Qt's own, " + std::to_string(ours) + " (Qt's " + std::to_string(qt) + ")");
}

}  // namespace

// A sidebar given less height than its rows: it scrolls. Shown, on the
// offscreen platform (nothing on any desktop), its window made shorter than
// the list's natural height.
void run_scroll(rap::ui& ui)
{
    using namespace rap::layout;
    rap::tabs* t = nullptr;
    auto win = ui.create(window{"Scrolled sidebar",
                                {tabs{{"One", label{"1"}}, {"Two", label{"2"}}, {"Three", label{"3"}},
                                      {"Four", label{"4"}}, {"Five", label{"5"}}, {"Six", label{"6"}},
                                      {"Seven", label{"7"}}, {"Eight", label{"8"}}}
                                     .sidebar()
                                     .bind(t)}});
    win->show();
    QApplication::processEvents();
    QWidget* top = top_titled("Scrolled sidebar");
    auto* list = top ? top->findChild<QListWidget*>() : nullptr;
    expect(t && top && list, "scrolled sidebar: shown, its list");
    if (!t || !top || !list) return;
    const int natural = list->sizeHint().height();
    top->resize(top->width(), natural / 2);
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
    expect(list->height() < natural && list->verticalScrollBar()->maximum() > 0,
           "scrolled sidebar: given " + std::to_string(list->height()) + " of its " + std::to_string(natural) +
               ", it scrolls (range " + std::to_string(list->verticalScrollBar()->maximum()) + ")");
}

int main(int argc, char** argv)
{
    rap::load_options opts;
    opts.backend = "qt6";
    auto ui = rap::ui::load(argc, argv, opts);
    if (!ui)
    {
        std::printf("FAIL no qt6 backend: %s\n", ui.error().c_str());
        return 1;
    }
    for (const bool sidebar : {false, true}) run_visibility(**ui, sidebar);
    for (const bool sidebar : {false, true}) run_hidden(**ui, sidebar);
    for (const bool sidebar : {false, true}) run_set_current_quiet(**ui, sidebar);
    for (const bool sidebar : {false, true}) run_none(**ui, sidebar);
    run_fixed_canvas(**ui);
    run_scroll(**ui);
    run_hidden_large(**ui);
    run_long_titles(**ui);
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
