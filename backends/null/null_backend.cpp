// Headless backend: builds stub widgets, prints the layout tree, and runs
// posted tasks until quit(). For tests and for checking plugin loading on
// machines without a toolkit.
#include "rapport/backend.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>

namespace rap::null
{
namespace
{

template <class Base>
class stub : public Base
{
public:
    void set_enabled(bool) override {}
    void set_visible(bool) override {}
    void set_tooltip(const std::string&) override {}
    void focus() override {}
    bool has_focus() const override { return false; }
};

class null_label : public stub<rap::label>
{
public:
    void set_text(const std::string&) override {}
};

class null_button : public stub<rap::button>
{
public:
    void set_text(const std::string&) override {}
    void set_icon(const icon&) override {}
    void on_click(std::function<void()> f) override { click = std::move(f); }
    std::function<void()> click;
};

class null_check : public stub<rap::check>
{
public:
    bool checked() const override { return value == check_state::checked; }
    void set_checked(bool v) override { value = v ? check_state::checked : check_state::unchecked; }
    check_state state() const override { return value; }
    void set_state(check_state s) override { value = s; }
    void on_toggled(std::function<void(bool)>) override {}
    check_state value = check_state::unchecked;
};

class null_menu_button : public stub<rap::menu_button>
{
public:
    void set_text(const std::string&) override {}
    void set_icon(const icon&) override {}
    void set_pushed(bool) override {}
    void on_menu(std::function<menu()>) override {}
};

class null_progress_bar : public stub<rap::progress_bar>
{
public:
    void set_fraction(float) override {}
    void pulse() override {}
};

class null_date_picker : public stub<rap::date_picker>
{
public:
    rap::date date() const override { return value; }
    void set_date(rap::date d) override { value = d; }
    void on_changed(std::function<void(rap::date)>) override {}
    rap::date value;
};

// Knows its ids, so select() and selected() behave.
class null_tree : public stub<rap::tree>
{
public:
    void set_items(std::vector<tree_item> items) override
    {
        ids.clear();
        add(items);
        if (current && std::ranges::find(ids, *current) == ids.end()) current.reset();
    }
    std::optional<std::string> selected() const override { return current; }
    void select(const std::string& id) override
    {
        if (std::ranges::find(ids, id) != ids.end())
            current = id;
        else
            current.reset();
    }
    void set_expanded(const std::string&, bool) override {}
    void on_selection_changed(std::function<void()>) override {}
    void on_activate(std::function<void(const std::string&)>) override {}
    void on_context_menu(std::function<menu(std::optional<std::string>)>) override {}

    std::vector<std::string> ids;
    std::optional<std::string> current;

private:
    void add(const std::vector<tree_item>& items)
    {
        for (const tree_item& i : items)
        {
            ids.push_back(i.id);
            add(i.children);
        }
    }
};

class null_entry : public stub<rap::entry>
{
public:
    std::string text() const override { return value; }
    void set_text(const std::string& t) override { value = t; }
    void set_placeholder(const std::string&) override {}
    void on_changed(std::function<void(const std::string&)>) override {}
    void on_activate(std::function<void()>) override {}
    std::string value;
};

class null_drop_down : public stub<rap::drop_down>
{
public:
    void set_items(std::vector<std::string>) override {}
    int current() const override { return value; }
    void set_current(int i) override { value = i; }
    void on_changed(std::function<void(int)>) override {}
    int value = -1;
};

class null_combo : public stub<rap::combo>
{
public:
    void set_items(std::vector<std::string>) override {}
    std::string text() const override { return value; }
    void set_text(const std::string& t) override { value = t; }
    void on_changed(std::function<void(const std::string&)>) override {}
    void on_activate(std::function<void()>) override {}
    std::string value;
};

class null_text_view : public stub<rap::text_view>
{
public:
    std::string text() const override { return value; }
    void set_text(const std::string& t, std::vector<span>) override { value = t; }
    void append(const std::string& t, const text_style&) override { value += t; }
    void set_read_only(bool) override {}
    void on_changed(std::function<void()>) override {}
    std::pair<size_t, size_t> selection() const override { return selected; }
    void select(size_t begin, size_t end) override
    {
        selected = {std::min(begin, value.size()), std::min(end, value.size())};
    }
    std::string value;
    std::pair<size_t, size_t> selected{0, 0};
};

class null_list : public stub<rap::list>
{
public:
    void set_model(list_model* m) override { model = m; }
    std::vector<size_t> selection() const override { return {}; }
    std::optional<size_t> current() const override { return {}; }
    void select(const std::vector<size_t>&) override {}
    void set_current(size_t) override {}
    void scroll_to(size_t) override {}
    void on_cell_drag(cell_drag) override {}
    void on_cell_context_menu(std::function<menu(size_t, size_t, point_f)>) override {}
    void set_sort_indicator(std::optional<size_t>, sort_order) override {}
    std::vector<float> column_widths() const override { return {}; }
    void set_column_widths(const std::vector<float>&) override {}
    bool column_visible(size_t) const override { return true; }
    void set_column_visible(size_t, bool) override {}
    std::vector<size_t> column_order() const override { return order_; }
    void set_column_order(const std::vector<size_t>& order) override { order_ = order; }
    std::vector<size_t> order_;
    void on_selection_changed(std::function<void()>) override {}
    void on_activate(std::function<void(size_t)>) override {}
    void on_sort_requested(std::function<void(size_t, sort_order)>) override {}
    void on_context_menu(std::function<menu(std::optional<size_t>)>) override {}
    void on_header_menu(std::function<menu(size_t)>) override {}
    void on_reorder(reorder_fn) override {}
    void set_reorderable(bool) override {}
    list_model* model = nullptr;
};

class null_splitter : public stub<rap::splitter>
{
public:
    std::vector<float> sizes() const override { return {}; }
    float divider() const override { return 0; }
    void on_moved(std::function<void()>) override {}
    void set_sizes(const std::vector<float>&) override {}
};

class null_tabs : public stub<rap::tabs>
{
public:
    int current() const override { return current_; }
    void set_current(int i) override
    {
        current_ = i;
        if (i >= 0 && size_t(i) < lazy.size() && lazy[size_t(i)])
        {
            const auto build = std::move(lazy[size_t(i)]);
            lazy[size_t(i)] = nullptr;
            build_page(build(), size_t(i));
        }
    }
    void on_changed(std::function<void(int)>) override {}

    // Lazy pages: built (and printed) when first made current.
    std::vector<std::function<node()>> lazy;
    std::function<void(const node&, size_t)> build_page;

private:
    int current_ = 0;
};

class null_tool_button : public stub<rap::tool_button>
{
public:
    void set_icon(const icon&) override {}
    bool checked() const override { return checked_; }
    void set_checked(bool c) override { checked_ = c; }
    void on_click(std::function<void()>) override {}
    void on_toggled(std::function<void(bool)>) override {}

private:
    bool checked_ = false;
};

// Slider and Spin: hold the value.
template <class Base>
class null_value : public stub<Base>
{
public:
    double value() const override { return value_; }
    void set_value(double v) override { value_ = v; }
    void set_range(double, double) override {}
    void on_changed(std::function<void(double)>) override {}

private:
    double value_ = 0;
};

class null_color_button : public stub<rap::color_button>
{
public:
    rap::color color() const override { return color_; }
    void set_color(rap::color c) override { color_ = c; }
    void on_changed(std::function<void(rap::color)>) override {}

private:
    rap::color color_;
};

class null_canvas : public stub<rap::canvas>
{
public:
    void invalidate() override {}
    size_f size() const override { return {}; }
    float device_scale() const override { return 1; }
    void set_cursor(cursor) override {}
    void on_paint(std::function<void(rap::painter&, rap::size_f)>) override {}
    void on_resize(std::function<void(size_f)>) override {}
    void on_pointer(std::function<bool(const pointer_event&)>) override {}
    void on_scroll(std::function<bool(const scroll_event&)>) override {}
    void on_key(std::function<bool(const key_event&)>) override {}
    subscription on_frame(std::function<void(double)>) override { return {}; }
    rap::frame_target target() const override { return {}; }
    void on_frame_target(std::function<void(const rap::frame_target&)>) override {}
    void present(std::shared_ptr<const rap::image>, frame_color) override {}
    void clear_frame() override {}
};

// Keeps its size; there is nothing to draw on.
class null_image : public rap::image
{
public:
    null_image(int w, int h)
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

class null_window : public rap::window
{
public:
    void set_title(const std::string&) override {}
    void show() override {}
    void close() override {}
    void on_finished(std::function<void(dialog_role)>) override {}
    void on_close_request(std::function<bool()>) override {}
    void on_files_dropped(std::function<void(const std::vector<std::string>&)>) override {}

    size_f size() const override { return size_; }
    void resize(size_f s) override { size_ = s; }
    void set_min_size(size_f) override {}
    std::optional<point_f> position() const override { return std::nullopt; }
    void move(point_f) override {}
    rect_f work_area() const override { return {0, 0, 1920, 1080}; }
    bool fullscreen() const override { return fullscreen_; }
    bool maximized() const override { return false; }
    void set_fullscreen(bool on) override { fullscreen_ = on; }
    void on_fullscreen_changed(std::function<void(bool)>) override {}
    void set_icon(const icon&) override {}
    void set_menu_bar_visible(bool) override {}
    void on_key(std::function<bool(const key_event&)>) override {}

    std::vector<std::unique_ptr<widget>> widgets;

private:
    size_f size_{640, 480};
    bool fullscreen_ = false;
};

const char* role_name(dialog_role r)
{
    switch (r)
    {
    case dialog_role::ok: return "Ok";
    case dialog_role::cancel: return "Cancel";
    case dialog_role::apply: return "Apply";
    case dialog_role::close: return "Close";
    case dialog_role::yes: return "Yes";
    case dialog_role::no: return "No";
    case dialog_role::help: return "Help";
    case dialog_role::reset: return "Reset";
    case dialog_role::none: break;
    }
    return "None";
}

// Builds stubs depth-first and prints one line per node.
class builder
{
public:
    builder(null_window& w, build_sink& sink)
        : win_(w)
        , sink_(sink)
    {
    }

    void build(const node& n, int depth, const std::string& form_label = {})
    {
        std::printf("%*s", depth * 2, "");
        if (!form_label.empty()) std::printf("[%s] ", form_label.c_str());
        std::visit([&](const auto& s) { visit(n, s, depth); }, n.spec);
        const bool container = std::holds_alternative<spec::box>(n.spec) ||
                               std::holds_alternative<spec::form>(n.spec) ||
                               std::holds_alternative<spec::group>(n.spec) ||
                               std::holds_alternative<spec::scroll>(n.spec) ||
                               std::holds_alternative<spec::toolbar>(n.spec);
        if (container && (n.common.handle || !n.common.visible)) make<stub<rap::widget>>(n);
    }

private:
    template <class W>
    W* make(const node& n)
    {
        auto w = std::make_unique<W>();
        W* raw = w.get();
        sink_.created(n, *w);
        win_.widgets.push_back(std::move(w));
        return raw;
    }

    void suffix(const node& n)
    {
        if (n.common.stretch) std::printf(" stretch=%d", n.common.stretch);
        std::printf("\n");
    }

    void children(const nodes& ns, int depth)
    {
        for (auto& c : ns) build(c, depth + 1);
    }

    void visit(const node& n, const spec::box& s, int d)
    {
        if (s.deferred) return visit(n, spec::box{s.dir, {s.deferred()}, s.margins, {}}, d);
        std::printf("%s", s.dir == orientation::vertical ? "vbox" : "hbox");
        suffix(n);
        children(s.items, d);
    }
    void visit(const node& n, const spec::form& s, int d)
    {
        std::printf("form");
        suffix(n);
        for (size_t i = 0; i < s.fields.size(); ++i) build(s.fields[i], d + 1, s.labels[i]);
    }
    void visit(const node& n, const spec::splitter& s, int d)
    {
        std::printf("splitter %s", s.dir == orientation::vertical ? "vertical" : "horizontal");
        suffix(n);
        make<null_splitter>(n);
        children(s.panes, d);
    }
    void visit(const node& n, const spec::scroll& s, int d)
    {
        std::printf("scroll%s", s.horizontal ? " both" : "");
        suffix(n);
        children(s.content, d);
    }
    void visit(const node& n, const spec::group& s, int d)
    {
        std::printf("group \"%s\"", s.title.c_str());
        suffix(n);
        children(s.content, d);
    }
    void visit(const node& n, const spec::tabs& s, int d)
    {
        std::printf(s.sidebar ? "tabs (sidebar)" : "tabs");
        suffix(n);
        auto* tabs = make<null_tabs>(n);
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
            tabs->build_page =
                [&win = win_, &sink = sink_, d, titles = s.titles](const node& page, size_t i)
            { builder(win, sink).build(page, d + 1, titles[i]); };
            tabs->set_current(0);
        }
    }
    void visit(const node& n, const spec::spacer&, int)
    {
        std::printf("spacer");
        suffix(n);
    }
    void visit(const node& n, const spec::separator&, int)
    {
        std::printf("separator");
        suffix(n);
    }
    void visit(const node& n, const spec::label& s, int)
    {
        std::printf("label \"%s\"%s", s.text.c_str(), s.icon ? " with icon" : "");
        suffix(n);
        make<null_label>(n);
    }
    void visit(const node& n, const spec::button& s, int)
    {
        std::printf("button \"%s\"", s.text.c_str());
        suffix(n);
        make<null_button>(n);
    }
    void visit(const node& n, const spec::check& s, int)
    {
        std::printf("check \"%s\" %s", s.text.c_str(),
                    s.state == check_state::checked         ? "on"
                    : s.state == check_state::indeterminate ? "mixed"
                                                           : "off");
        suffix(n);
        make<null_check>(n);
    }
    void visit(const node& n, const spec::entry& s, int)
    {
        std::printf("entry \"%s\"%s", s.text.c_str(), s.frame ? "" : " frameless");
        suffix(n);
        make<null_entry>(n);
    }
    void visit(const node& n, const spec::drop_down& s, int)
    {
        std::printf("drop_down (%zu items)", s.items.size());
        suffix(n);
        make<null_drop_down>(n);
    }
    void visit(const node& n, const spec::combo& s, int)
    {
        std::printf("combo \"%s\" (%zu items)", s.text.c_str(), s.items.size());
        suffix(n);
        make<null_combo>(n);
    }
    void visit(const node& n, const spec::text_view& s, int)
    {
        std::printf("text_view%s%s", s.read_only ? " readonly" : "", s.monospace ? " mono" : "");
        suffix(n);
        make<null_text_view>(n);
    }
    void visit(const node& n, const spec::list& s, int)
    {
        std::printf("list [");
        for (size_t i = 0; i < s.columns.size(); ++i)
            std::printf("%s%s%s", i ? ", " : "", s.columns[i].title.c_str(),
                        s.columns[i].painted ? "*" : "");
        std::printf("] rows=%zu", s.model ? s.model->row_count() : 0);
        suffix(n);
        make<null_list>(n);
    }
    void visit(const node& n, const spec::toolbar& s, int d)
    {
        std::printf("toolbar");
        suffix(n);
        children(s.items, d);
    }
    void visit(const node& n, const spec::tool_button& s, int)
    {
        std::printf("tool_button \"%s\"%s", s.label.c_str(), s.toggle ? " toggle" : "");
        suffix(n);
        make<null_tool_button>(n);
    }
    void visit(const node& n, const spec::slider& s, int)
    {
        std::printf("slider %g..%g = %g", s.min, s.max, s.value);
        suffix(n);
        make<null_value<rap::slider>>(n);
    }
    void visit(const node& n, const spec::spin& s, int)
    {
        std::printf("spin %g..%g = %g", s.min, s.max, s.value);
        suffix(n);
        make<null_value<rap::spin>>(n);
    }
    void visit(const node& n, const spec::color_button&, int)
    {
        std::printf("color_button");
        suffix(n);
        make<null_color_button>(n);
    }
    void visit(const node& n, const spec::canvas& s, int)
    {
        std::printf("canvas%s", s.focusable ? " focusable" : "");
        suffix(n);
        make<null_canvas>(n);
    }
    void visit(const node& n, const spec::menu_button& s, int)
    {
        std::printf("menu_button \"%s\"", s.text.c_str());
        suffix(n);
        make<null_menu_button>(n);
    }
    void visit(const node& n, const spec::progress_bar&, int)
    {
        std::printf("progress_bar");
        suffix(n);
        make<null_progress_bar>(n);
    }
    void visit(const node& n, const spec::date_picker& s, int)
    {
        std::printf("date_picker %04d-%02d-%02d", s.date.year, s.date.month, s.date.day);
        suffix(n);
        make<null_date_picker>(n);
    }
    void visit(const node& n, const spec::tree& s, int)
    {
        std::printf("tree (%zu)", s.items.size());
        suffix(n);
        make<null_tree>(n)->set_items(s.items);
    }
    void visit(const node& n, const spec::buttons& s, int)
    {
        std::printf("buttons");
        for (auto& b : s.buttons)
            std::printf(" %s%s%s%s", role_name(b.role), b.text.empty() ? "" : "(\"", b.text.c_str(),
                        b.text.empty() ? "" : "\")");
        suffix(n);
        for (auto& b : s.buttons)
        {
            auto w = std::make_unique<null_button>();
            sink_.created(b, *w);
            win_.widgets.push_back(std::move(w));
        }
    }

    null_window& win_;
    build_sink& sink_;
};

class null_backend : public backend
{
public:
    static backend* create(int*, char**) { return new null_backend; }

    std::string_view name() const override { return "null"; }

    std::unique_ptr<rap::window> create_window(const window_spec& spec, rap::window*,
                                                build_sink& sink) override
    {
        auto w = std::make_unique<null_window>();
        std::printf("%s \"%s\"\n", spec.dialog ? "dialog" : "window", spec.title.c_str());
        builder(*w, sink).build(spec.content, 1);
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

    void message(rap::window*, const message_spec& m,
                 std::function<void(dialog_role)> done) override
    {
        std::printf("message: %s: %s\n", m.title.c_str(), m.text.c_str());
        if (done) done(m.buttons.empty() ? dialog_role::ok : m.buttons.front());
    }

    void choose_files(rap::window*, const file_dialog_spec&,
                     std::function<void(std::vector<std::string>)> done) override
    {
        if (done) done({});
    }

    // Not yet: the dialog and pages come with this backend's print support.
    void print(rap::window*, print_job job) override
    {
        post(
            [job = std::move(job)]
            {
                if (job.done) job.done(false, "printing isn't available with this toolkit yet");
            });
    }
    void set_clipboard(const std::string&) override {}
    void read_clipboard_text(std::function<void(std::optional<std::string>)> done) override { done(std::nullopt); }
    void set_clipboard_image(int, int, pixel_format, const uint8_t*, size_t) override {}
    bool save_snapshot(const std::string&) override { return false; }

    std::unique_ptr<rap::image> create_image(int w, int h, pixel_format, const uint8_t*,
                                              size_t) override
    {
        return std::make_unique<null_image>(w, h);
    }

    // No clock here: a timer fires once, as soon as the queue reaches it
    // (repeating ones too, so headless runs terminate).
    subscription timer(int, bool, std::function<void()> fn) override
    {
        auto alive = std::make_shared<bool>(true);
        post(
            [alive, fn = std::move(fn)]
            {
                if (*alive) fn();
            });
        return subscription([alive] { *alive = false; });
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace rap::null

RAPPORT_BACKEND_ENTRY(null, rap::null::null_backend)
