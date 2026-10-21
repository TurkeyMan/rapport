// Headless backend: builds stub widgets, prints the layout tree, and runs
// posted tasks until quit(). For tests and for checking plugin loading on
// machines without a toolkit.
#include "anyui/backend.h"

#include <cstdio>
#include <deque>
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
    void onClick(std::function<void()> f) override { click = std::move(f); }
    std::function<void()> click;
};

class NCheck : public Stub<anyui::Check>
{
public:
    bool checked() const override { return value; }
    void setChecked(bool v) override { value = v; }
    void onToggled(std::function<void(bool)>) override {}
    bool value = false;
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
    std::string value;
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
    void setSortIndicator(std::optional<size_t>, SortOrder) override {}
    std::vector<float> columnWidths() const override { return {}; }
    void setColumnWidths(const std::vector<float>&) override {}
    void onSelectionChanged(std::function<void()>) override {}
    void onActivate(std::function<void(size_t)>) override {}
    void onSortRequested(std::function<void(size_t, SortOrder)>) override {}
    void onContextMenu(std::function<Menu(std::optional<size_t>)>) override {}
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
    int current() const override { return 0; }
    void setCurrent(int) override {}
    void onChanged(std::function<void(int)>) override {}
};

class NWindow : public anyui::Window
{
public:
    void setTitle(const std::string&) override {}
    void show() override {}
    void close() override {}
    void onFinished(std::function<void(DialogRole)>) override {}
    void onCloseRequest(std::function<bool()>) override {}

    std::vector<std::unique_ptr<Widget>> widgets;
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
    }

private:
    template <class W>
    void make(const Node& n)
    {
        auto w = std::make_unique<W>();
        sink_.created(n, *w);
        win_.widgets.push_back(std::move(w));
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
    void visit(const Node& n, const spec::Group& s, int d)
    {
        std::printf("Group \"%s\"", s.title.c_str());
        suffix(n);
        children(s.content, d);
    }
    void visit(const Node& n, const spec::Tabs& s, int d)
    {
        std::printf("Tabs");
        suffix(n);
        make<NTabs>(n);
        for (size_t i = 0; i < s.pages.size(); ++i) build(s.pages[i], d + 1, s.titles[i]);
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
        std::printf("Check \"%s\" %s", s.text.c_str(), s.checked ? "on" : "off");
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

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace anyui::null

ANYUI_BACKEND_ENTRY(anyui::null::NullBackend)
