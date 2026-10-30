// rapport-list-dnd: rows dragged from one list to another through the
// system's drag and drop (list::on_drag_out, list::on_drop_in). The left
// list offers its selected rows as "application/x-rapport-demo" (one row a
// line) and as "text/plain", so they also drop into a text editor; dragged
// within itself it reorders, and the system drag starts once the pointer
// leaves it. The right list takes either type and inserts the rows where
// they're dropped; it takes text from other programs too.
//   rapport-list-dnd           both windows in one process
//   rapport-list-dnd --left    only the left (run --right in another
//   rapport-list-dnd --right   process: the drag crosses processes)
#include <rapport/rapport.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view demo_type = "application/x-rapport-demo";

class rows_model : public rap::list_model
{
public:
    explicit rows_model(std::vector<std::string> rows)
        : rows_(std::move(rows))
    {
    }
    size_t row_count() const override { return rows_.size(); }
    std::string text(size_t row, size_t) const override { return rows_[row]; }

    const std::string& at(size_t row) const { return rows_[row]; }
    void insert(size_t at, const std::vector<std::string>& rows)
    {
        rows_.insert(rows_.begin() + std::ptrdiff_t(at), rows.begin(), rows.end());
        notify_inserted(at, rows.size());
    }
    // Moves `rows` (sorted) in front of `dest`; returns where they start.
    size_t move(const std::vector<size_t>& rows, size_t dest)
    {
        std::vector<std::string> moved;
        for (size_t i = rows.size(); i-- > 0;)
        {
            moved.insert(moved.begin(), rows_[rows[i]]);
            rows_.erase(rows_.begin() + std::ptrdiff_t(rows[i]));
            if (rows[i] < dest) --dest;
        }
        rows_.insert(rows_.begin() + std::ptrdiff_t(dest), moved.begin(), moved.end());
        notify_reset();
        return dest;
    }

private:
    std::vector<std::string> rows_;
};

std::vector<std::string> lines(const std::string& s)
{
    std::vector<std::string> out;
    size_t from = 0;
    while (from < s.size())
    {
        size_t end = s.find('\n', from);
        if (end == std::string::npos) end = s.size();
        if (end > from) out.push_back(s.substr(from, end - from));
        from = end + 1;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }
    bool left_side = true, right_side = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--left") right_side = false;
        if (a == "--right") left_side = false;
    }

    rows_model left_rows({"Apple", "Banana", "Cherry", "Damson", "Elderberry", "Fig", "Grape",
                          "Hawthorn", "Kiwi", "Lemon", "Mango", "Nectarine", "Orange", "Pear",
                          "Quince", "Raspberry", "Strawberry", "Tangerine"});
    rows_model right_rows({"Asparagus", "Beetroot", "Carrot"});
    rap::list* left_list = nullptr;
    rap::list* right_list = nullptr;
    std::unique_ptr<rap::window> left_win, right_win;
    int open = 0;
    auto closed = [&]
    {
        if (--open == 0) (*ui)->quit();
        return true;
    };

    using namespace rap::layout;
    if (left_side)
    {
        left_win = (*ui)->create(window{
            "Drag from here",
            {vbox{
                label{"Drag rows to the other window, or within this one to reorder."},
                list{{{"Fruit", {}, true}}, &left_rows}
                    .on_reorder(
                        [&](const std::vector<size_t>& rows, size_t dest)
                        {
                            const size_t first = left_rows.move(rows, dest);
                            std::vector<size_t> sel;
                            for (size_t i = 0; i < rows.size(); ++i) sel.push_back(first + i);
                            left_list->select(sel);
                            return true;
                        })
                    .on_drag_out(
                        {[&](const std::vector<size_t>& rows) -> std::optional<rap::data_offer>
                         {
                             std::string text;
                             for (size_t r : rows) text += left_rows.at(r) + "\n";
                             std::printf("drag out: %zu rows\n", rows.size());
                             std::fflush(stdout);
                             return rap::data_offer{
                                 {{std::string(demo_type), text}, {"text/plain", text}}};
                         }})
                    .bind(left_list)
                    .stretch(),
            }}}.size(30_ch, 20_lines));
        left_win->on_close_request(closed);
        left_win->show();
        ++open;
    }
    if (right_side)
    {
        right_win = (*ui)->create(window{
            "Drop here",
            {vbox{
                label{"Rows (or text) dropped here go where the line shows."},
                list{{{"Dropped", {}, true}}, &right_rows}
                    .on_drop_in({{std::string(demo_type), "text/plain"},
                                 {},
                                 [&](size_t at, const std::string& type, const std::string& data)
                                 {
                                     const std::vector<std::string> rows = lines(data);
                                     std::printf("drop in: %zu rows of %s at %zu\n", rows.size(),
                                                 type.c_str(), at);
                                     std::fflush(stdout);
                                     if (rows.empty()) return false;
                                     right_rows.insert(at, rows);
                                     return true;
                                 }})
                    .bind(right_list)
                    .stretch(),
            }}}.size(30_ch, 20_lines));
        right_win->on_close_request(closed);
        right_win->show();
        ++open;
    }
    std::printf("binds: %s\n", (!left_side || left_list) && (!right_side || right_list)
                                   ? "all filled"
                                   : "MISSING");
    std::fflush(stdout);
    return (*ui)->run();
}
