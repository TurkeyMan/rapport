// Headless backend: builds stub widgets, prints the layout tree, and runs
// posted tasks until quit(). For tests and for checking plugin loading on
// machines without a toolkit.
#include "anyui/backend.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>

namespace anyui::null
{
namespace
{

template <class Base>
class Stub : public Base
{
public:
    void setEnabled(bool) override {}
    void setVisible(bool) override {}
    void setTooltip(const std::string&) override {}
    void focus() override {}
};

class NLabel : public Stub<anyui::Label>
{
public:
    void setText(const std::string&) override {}
};

class NButton : public Stub<anyui::Button>
{
public:
    void setText(const std::string&) override {}
    void setIcon(const Icon&) override {}
    void onClick(std::function<void()> f) override { click = std::move(f); }
    std::function<void()> click;
};

class NCheck : public Stub<anyui::Check>
{
public:
    bool checked() const override { return value == CheckState::Checked; }
    void setChecked(bool v) override { value = v ? CheckState::Checked : CheckState::Unchecked; }
    CheckState state() const override { return value; }
    void setState(CheckState s) override { value = s; }
    void onToggled(std::function<void(bool)>) override {}
    CheckState value = CheckState::Unchecked;
};

class NMenuButton : public Stub<anyui::MenuButton>
{
public:
    void setText(const std::string&) override {}
    void setIcon(const Icon&) override {}
    void setPushed(bool) override {}
    void onMenu(std::function<Menu()>) override {}
};

class NProgressBar : public Stub<anyui::ProgressBar>
{
public:
    void setFraction(float) override {}
    void pulse() override {}
};

class NDatePicker : public Stub<anyui::DatePicker>
{
public:
    Date date() const override { return value; }
    void setDate(Date d) override { value = d; }
    void onChanged(std::function<void(Date)>) override {}
    Date value;
};

class NEntry : public Stub<anyui::Entry>
{
public:
    std::string text() const override { return value; }
    void setText(const std::string& t) override { value = t; }
    void setPlaceholder(const std::string&) override {}
    void onChanged(std::function<void(const std::string&)>) override {}
    void onActivate(std::function<void()>) override {}
    std::string value;
};

class NSelect : public Stub<anyui::Select>
{
public:
    void setItems(std::vector<std::string>) override {}
    int current() const override { return value; }
    void setCurrent(int i) override { value = i; }
    void onChanged(std::function<void(int)>) override {}
    int value = -1;
};

class NCombo : public Stub<anyui::Combo>
{
public:
    void setItems(std::vector<std::string>) override {}
    std::string text() const override { return value; }
    void setText(const std::string& t) override { value = t; }
    void onChanged(std::function<void(const std::string&)>) override {}
    void onActivate(std::function<void()>) override {}
    std::string value;
};

class NTextView : public Stub<anyui::TextView>
{
public:
    std::string text() const override { return value; }
    void setText(const std::string& t, std::vector<Span>) override { value = t; }
    void append(const std::string& t, const TextStyle&) override { value += t; }
    void setReadOnly(bool) override {}
    void onChanged(std::function<void()>) override {}
    std::pair<size_t, size_t> selection() const override { return selected; }
    void select(size_t begin, size_t end) override
    {
        selected = {std::min(begin, value.size()), std::min(end, value.size())};
    }
    std::string value;
    std::pair<size_t, size_t> selected{0, 0};
};

class NList : public Stub<anyui::List>
{
public:
    void setModel(ListModel* m) override { model = m; }
    std::vector<size_t> selection() const override { return {}; }
    std::optional<size_t> current() const override { return {}; }
    void select(const std::vector<size_t>&) override {}
    void setCurrent(size_t) override {}
    void scrollTo(size_t) override {}
    void onCellDrag(CellDrag) override {}
    void setSortIndicator(std::optional<size_t>, SortOrder) override {}
    std::vector<float> columnWidths() const override { return {}; }
    void setColumnWidths(const std::vector<float>&) override {}
    bool columnVisible(size_t) const override { return true; }
    void setColumnVisible(size_t, bool) override {}
    void onSelectionChanged(std::function<void()>) override {}
    void onActivate(std::function<void(size_t)>) override {}
    void onSortRequested(std::function<void(size_t, SortOrder)>) override {}
    void onContextMenu(std::function<Menu(std::optional<size_t>)>) override {}
    void onHeaderMenu(std::function<Menu(size_t)>) override {}
    void onReorder(ReorderFn) override {}
    void setReorderable(bool) override {}
    ListModel* model = nullptr;
};

class NSplitter : public Stub<anyui::Splitter>
{
public:
    std::vector<float> sizes() const override { return {}; }
    void setSizes(const std::vector<float>&) override {}
};

class NTabs : public Stub<anyui::Tabs>
{
public:
    int current() const override { return current_; }
    void setCurrent(int i) override
    {
        current_ = i;
        if (i >= 0 && size_t(i) < lazy.size() && lazy[size_t(i)])
        {
            const auto build = std::move(lazy[size_t(i)]);
            lazy[size_t(i)] = nullptr;
            buildPage(build(), size_t(i));
        }
    }
    void onChanged(std::function<void(int)>) override {}

    // Lazy pages: built (and printed) when first made current.
    std::vector<std::function<Node()>> lazy;
    std::function<void(const Node&, size_t)> buildPage;

private:
    int current_ = 0;
};

class NToolButton : public Stub<anyui::ToolButton>
{
public:
    void setIcon(const Icon&) override {}
    bool checked() const override { return checked_; }
    void setChecked(bool c) override { checked_ = c; }
    void onClick(std::function<void()>) override {}
    void onToggled(std::function<void(bool)>) override {}

private:
    bool checked_ = false;
};

// Slider and Spin: hold the value.
template <class Base>
class NValue : public Stub<Base>
{
public:
    double value() const override { return value_; }
    void setValue(double v) override { value_ = v; }
    void setRange(double, double) override {}
    void onChanged(std::function<void(double)>) override {}

private:
    double value_ = 0;
};

class NColorButton : public Stub<anyui::ColorButton>
{
public:
    Color color() const override { return color_; }
    void setColor(Color c) override { color_ = c; }
    void onChanged(std::function<void(Color)>) override {}

private:
    Color color_;
};

class NCanvas : public Stub<anyui::Canvas>
{
public:
    void invalidate() override {}
    SizeF size() const override { return {}; }
    float deviceScale() const override { return 1; }
    void setCursor(Cursor) override {}
    void onPaint(std::function<void(Painter&, SizeF)>) override {}
    void onResize(std::function<void(SizeF)>) override {}
    void onPointer(std::function<bool(const PointerEvent&)>) override {}
    void onScroll(std::function<bool(const ScrollEvent&)>) override {}
    void onKey(std::function<bool(const KeyEvent&)>) override {}
    Subscription onFrame(std::function<void(double)>) override { return {}; }
};

// Keeps its size; there is nothing to draw on.
class NImage : public anyui::Image
{
public:
    NImage(int w, int h)
        : w_(w)
        , h_(h)
    {
    }
    int width() const override { return w_; }
    int height() const override { return h_; }
    void update(const uint8_t*, size_t) override {}

private:
    int w_, h_;
};

class NWindow : public anyui::Window
{
public:
    void setTitle(const std::string&) override {}
    void show() override {}
    void close() override {}
    void onFinished(std::function<void(DialogRole)>) override {}
    void onCloseRequest(std::function<bool()>) override {}

    SizeF size() const override { return size_; }
    void resize(SizeF s) override { size_ = s; }
    void setMinSize(SizeF) override {}
    std::optional<PointF> position() const override { return std::nullopt; }
    void move(PointF) override {}
    RectF workArea() const override { return {0, 0, 1920, 1080}; }
    bool fullscreen() const override { return fullscreen_; }
    void setFullscreen(bool on) override { fullscreen_ = on; }
    void onFullscreenChanged(std::function<void(bool)>) override {}
    void setIcon(const Icon&) override {}
    void setMenuBarVisible(bool) override {}
    void onKey(std::function<bool(const KeyEvent&)>) override {}

    std::vector<std::unique_ptr<Widget>> widgets;

private:
    SizeF size_{640, 480};
    bool fullscreen_ = false;
};

const char* roleName(DialogRole r)
{
    switch (r)
    {
    case DialogRole::Ok: return "Ok";
    case DialogRole::Cancel: return "Cancel";
    case DialogRole::Apply: return "Apply";
    case DialogRole::Close: return "Close";
    case DialogRole::Yes: return "Yes";
    case DialogRole::No: return "No";
    case DialogRole::Help: return "Help";
    case DialogRole::Reset: return "Reset";
    case DialogRole::None: break;
    }
    return "None";
}

// Builds stubs depth-first and prints one line per node.
class Builder
{
public:
    Builder(NWindow& w, BuildSink& sink)
        : win_(w)
        , sink_(sink)
    {
    }

    void build(const Node& n, int depth, const std::string& formLabel = {})
    {
        std::printf("%*s", depth * 2, "");
        if (!formLabel.empty()) std::printf("[%s] ", formLabel.c_str());
        std::visit([&](const auto& s) { visit(n, s, depth); }, n.spec);
        const bool container = std::holds_alternative<spec::Box>(n.spec) ||
                               std::holds_alternative<spec::Form>(n.spec) ||
                               std::holds_alternative<spec::Group>(n.spec) ||
                               std::holds_alternative<spec::Scroll>(n.spec) ||
                               std::holds_alternative<spec::Toolbar>(n.spec);
        if (container && n.common.handle) make<Stub<anyui::Widget>>(n);
    }

private:
    template <class W>
    W* make(const Node& n)
    {
        auto w = std::make_unique<W>();
        W* raw = w.get();
        sink_.created(n, *w);
        win_.widgets.push_back(std::move(w));
        return raw;
    }

    void suffix(const Node& n)
    {
        if (n.common.stretch) std::printf(" stretch=%d", n.common.stretch);
        std::printf("\n");
    }

    void children(const Nodes& ns, int depth)
    {
        for (auto& c : ns) build(c, depth + 1);
    }

    void visit(const Node& n, const spec::Box& s, int d)
    {
        std::printf("%s", s.dir == Orientation::Vertical ? "VBox" : "HBox");
        suffix(n);
        children(s.items, d);
    }
    void visit(const Node& n, const spec::Form& s, int d)
    {
        std::printf("Form");
        suffix(n);
        for (size_t i = 0; i < s.fields.size(); ++i) build(s.fields[i], d + 1, s.labels[i]);
    }
    void visit(const Node& n, const spec::Splitter& s, int d)
    {
        std::printf("Splitter %s", s.dir == Orientation::Vertical ? "vertical" : "horizontal");
        suffix(n);
        make<NSplitter>(n);
        children(s.panes, d);
    }
    void visit(const Node& n, const spec::Scroll& s, int d)
    {
        std::printf("Scroll%s", s.horizontal ? " both" : "");
        suffix(n);
        children(s.content, d);
    }
    void visit(const Node& n, const spec::Group& s, int d)
    {
        std::printf("Group \"%s\"", s.title.c_str());
        suffix(n);
        children(s.content, d);
    }
    void visit(const Node& n, const spec::Tabs& s, int d)
    {
        std::printf(s.sidebar ? "Tabs (sidebar)" : "Tabs");
        suffix(n);
        auto* tabs = make<NTabs>(n);
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            if (i < s.lazy.size() && s.lazy[i])
                std::printf("%*s[%s] (lazy)\n", (d + 1) * 2, "", s.titles[i].c_str());
            else
                build(s.pages[i], d + 1, s.titles[i]);
        }
        if (!s.lazy.empty())
        {
            tabs->lazy = s.lazy;
            tabs->buildPage =
                [&win = win_, &sink = sink_, d, titles = s.titles](const Node& page, size_t i)
            { Builder(win, sink).build(page, d + 1, titles[i]); };
            tabs->setCurrent(0);
        }
    }
    void visit(const Node& n, const spec::Spacer&, int)
    {
        std::printf("Spacer");
        suffix(n);
    }
    void visit(const Node& n, const spec::Separator&, int)
    {
        std::printf("Separator");
        suffix(n);
    }
    void visit(const Node& n, const spec::Label& s, int)
    {
        std::printf("Label \"%s\"", s.text.c_str());
        suffix(n);
        make<NLabel>(n);
    }
    void visit(const Node& n, const spec::Button& s, int)
    {
        std::printf("Button \"%s\"", s.text.c_str());
        suffix(n);
        make<NButton>(n);
    }
    void visit(const Node& n, const spec::Check& s, int)
    {
        std::printf("Check \"%s\" %s", s.text.c_str(),
                    s.state == CheckState::Checked         ? "on"
                    : s.state == CheckState::Indeterminate ? "mixed"
                                                           : "off");
        suffix(n);
        make<NCheck>(n);
    }
    void visit(const Node& n, const spec::Entry& s, int)
    {
        std::printf("Entry \"%s\"", s.text.c_str());
        suffix(n);
        make<NEntry>(n);
    }
    void visit(const Node& n, const spec::Select& s, int)
    {
        std::printf("Select (%zu items)", s.items.size());
        suffix(n);
        make<NSelect>(n);
    }
    void visit(const Node& n, const spec::Combo& s, int)
    {
        std::printf("Combo \"%s\" (%zu items)", s.text.c_str(), s.items.size());
        suffix(n);
        make<NCombo>(n);
    }
    void visit(const Node& n, const spec::TextView& s, int)
    {
        std::printf("TextView%s%s", s.readOnly ? " readonly" : "", s.monospace ? " mono" : "");
        suffix(n);
        make<NTextView>(n);
    }
    void visit(const Node& n, const spec::List& s, int)
    {
        std::printf("List [");
        for (size_t i = 0; i < s.columns.size(); ++i)
            std::printf("%s%s%s", i ? ", " : "", s.columns[i].title.c_str(),
                        s.columns[i].painted ? "*" : "");
        std::printf("] rows=%zu", s.model ? s.model->rowCount() : 0);
        suffix(n);
        make<NList>(n);
    }
    void visit(const Node& n, const spec::Toolbar& s, int d)
    {
        std::printf("Toolbar");
        suffix(n);
        children(s.items, d);
    }
    void visit(const Node& n, const spec::ToolButton& s, int)
    {
        std::printf("ToolButton \"%s\"%s", s.label.c_str(), s.toggle ? " toggle" : "");
        suffix(n);
        make<NToolButton>(n);
    }
    void visit(const Node& n, const spec::Slider& s, int)
    {
        std::printf("Slider %g..%g = %g", s.min, s.max, s.value);
        suffix(n);
        make<NValue<anyui::Slider>>(n);
    }
    void visit(const Node& n, const spec::Spin& s, int)
    {
        std::printf("Spin %g..%g = %g", s.min, s.max, s.value);
        suffix(n);
        make<NValue<anyui::Spin>>(n);
    }
    void visit(const Node& n, const spec::ColorButton&, int)
    {
        std::printf("ColorButton");
        suffix(n);
        make<NColorButton>(n);
    }
    void visit(const Node& n, const spec::Canvas& s, int)
    {
        std::printf("Canvas%s", s.focusable ? " focusable" : "");
        suffix(n);
        make<NCanvas>(n);
    }
    void visit(const Node& n, const spec::MenuButton& s, int)
    {
        std::printf("MenuButton \"%s\"", s.text.c_str());
        suffix(n);
        make<NMenuButton>(n);
    }
    void visit(const Node& n, const spec::ProgressBar&, int)
    {
        std::printf("ProgressBar");
        suffix(n);
        make<NProgressBar>(n);
    }
    void visit(const Node& n, const spec::DatePicker& s, int)
    {
        std::printf("DatePicker %04d-%02d-%02d", s.date.year, s.date.month, s.date.day);
        suffix(n);
        make<NDatePicker>(n);
    }
    void visit(const Node& n, const spec::Buttons& s, int)
    {
        std::printf("Buttons");
        for (auto& b : s.buttons)
            std::printf(" %s%s%s%s", roleName(b.role), b.text.empty() ? "" : "(\"", b.text.c_str(),
                        b.text.empty() ? "" : "\")");
        suffix(n);
        for (auto& b : s.buttons)
        {
            auto w = std::make_unique<NButton>();
            sink_.created(b, *w);
            win_.widgets.push_back(std::move(w));
        }
    }

    NWindow& win_;
    BuildSink& sink_;
};

class NullBackend : public Backend
{
public:
    static Backend* create(int*, char**) { return new NullBackend; }

    std::string_view name() const override { return "null"; }

    std::unique_ptr<anyui::Window> createWindow(const WindowSpec& spec, anyui::Window*,
                                                BuildSink& sink) override
    {
        auto w = std::make_unique<NWindow>();
        std::printf("%s \"%s\"\n", spec.dialog ? "Dialog" : "Window", spec.title.c_str());
        Builder(*w, sink).build(spec.content, 1);
        return w;
    }

    int run() override
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex_);
                if (quit_ || queue_.empty()) break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
        return code_;
    }

    void quit(int code) override
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        code_ = code;
    }

    void post(std::function<void()> f) override
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(f));
    }

    void message(anyui::Window*, const MessageSpec& m,
                 std::function<void(DialogRole)> done) override
    {
        std::printf("message: %s: %s\n", m.title.c_str(), m.text.c_str());
        if (done) done(m.buttons.empty() ? DialogRole::Ok : m.buttons.front());
    }

    void chooseFiles(anyui::Window*, const FileDialogSpec&,
                     std::function<void(std::vector<std::string>)> done) override
    {
        if (done) done({});
    }

    void setClipboard(const std::string&) override {}
    void setClipboardImage(int, int, PixelFormat, const uint8_t*, size_t) override {}

    std::unique_ptr<anyui::Image> createImage(int w, int h, PixelFormat, const uint8_t*,
                                              size_t) override
    {
        return std::make_unique<NImage>(w, h);
    }

    // No clock here: a timer fires once, as soon as the queue reaches it
    // (repeating ones too, so headless runs terminate).
    Subscription timer(int, bool, std::function<void()> fn) override
    {
        auto alive = std::make_shared<bool>(true);
        post(
            [alive, fn = std::move(fn)]
            {
                if (*alive) fn();
            });
        return Subscription([alive] { *alive = false; });
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace anyui::null

ANYUI_BACKEND_ENTRY(null, anyui::null::NullBackend)
