// rapport-frames: a list, a tree and a text view with the toolkit's frame round
// them (the top row) and without (the bottom), for checking how they look.
#include <rapport/rapport.h>

#include <cstdio>
#include <string>

namespace
{

class rows : public rap::list_model
{
public:
    size_t row_count() const override { return 40; }
    std::string text(size_t row, size_t) const override { return "Row " + std::to_string(row + 1); }
};

std::vector<rap::tree_item> items()
{
    rap::tree_item parent;
    parent.label = parent.id = "Parent";
    parent.expanded = true;
    rap::tree_item child;
    child.label = child.id = "Child";
    parent.children = {child};
    return {parent};
}

rap::node row(rows* model, bool frame)
{
    using namespace rap::layout;
    return hbox{list{{{"Name", {}, true}}, model}.frame(frame).stretch(),
                tree{items()}.frame(frame).stretch(),
                text_view{"Some text"}.read_only().frame(frame).stretch()}
        .stretch();
}

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    rows model;
    using namespace rap::layout;
    auto win = (*ui)->create(window{"Frames", {vbox{row(&model, true), row(&model, false)}}}.size(70_ch, 24_lines));
    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
