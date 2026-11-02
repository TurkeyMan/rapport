// rapport-theme-roles: every theme_color as a swatch, beside a few native
// widgets and two lists with a selected row (soft and solid), so each
// backend's reading of the desktop's colours can be snapshotted:
//   RAPPORT_SNAPSHOT=roles.png ./rapport-theme-roles
//   RAPPORT_THEME=dark RAPPORT_SNAPSHOT=roles-dark.png ./rapport-theme-roles
#include <rapport/rapport.h>

#include <cstdio>
#include <string>

namespace
{

using rap::theme_color;

struct role_name
{
    theme_color role;
    const char* name;
};
// In theme_color's order, one per role.
const role_name roles[] = {
    {theme_color::window, "window"},
    {theme_color::window_text, "window_text"},
    {theme_color::base, "base"},
    {theme_color::alternate_base, "alternate_base"},
    {theme_color::text, "text"},
    {theme_color::placeholder_text, "placeholder_text"},
    {theme_color::highlight, "highlight"},
    {theme_color::highlight_text, "highlight_text"},
    {theme_color::mid, "mid"},
    {theme_color::accent, "accent"},
    {theme_color::link, "link"},
    {theme_color::positive, "positive"},
    {theme_color::negative, "negative"},
    {theme_color::neutral, "neutral"},
    {theme_color::warning, "warning"},
};
static_assert(std::size(roles) == size_t(rap::theme_color_count), "a name for every role");

std::string hex(rap::color c)
{
    char s[16];
    std::snprintf(s, sizeof s, c.a == 255 ? "#%02x%02x%02x" : "#%02x%02x%02x%02x", c.r, c.g, c.b,
                  c.a);
    return s;
}

void paint_swatches(rap::painter& p, rap::size_f size)
{
    p.rect({0, 0, size.w, size.h}, 0, rap::paint{theme_color::window}, std::nullopt);
    const float pad = 8, w = 200, h = 34;
    const int per_column = int(std::max(1.f, (size.h - pad) / (h + pad)));
    rap::text_style label, value;
    label.fg = theme_color::window_text;
    value.fg = theme_color::window_text;
    value.monospace = true;
    value.size = 9;
    for (size_t i = 0; i < std::size(roles); ++i)
    {
        const float x = pad + float(int(i) / per_column) * (w + pad);
        const float y = pad + float(int(i) % per_column) * (h + pad);
        p.rect({x, y, h, h}, 4, rap::paint{roles[i].role}, rap::paint{theme_color::mid});
        p.text({x + h + 6, y + 1}, roles[i].name, label);
        p.text({x + h + 6, y + h / 2 + 2}, hex(p.resolve(roles[i].role)), value);
    }
}

class rows_model : public rap::list_model
{
public:
    size_t row_count() const override { return 4; }
    std::string text(size_t row, size_t) const override
    {
        static const char* text[] = {"First row", "Selected row", "Third row", "Fourth row"};
        return text[row];
    }
    rap::text_style style(size_t row, size_t) const override
    {
        rap::text_style s;
        if (row == 2) s.fg = theme_color::positive;
        if (row == 3) s.fg = theme_color::negative;
        return s;
    }
};

}  // namespace

int main(int argc, char** argv)
{
    auto loaded = rap::ui::load(argc, argv);
    if (!loaded)
    {
        std::fprintf(stderr, "%s", loaded.error().c_str());
        return 1;
    }
    rap::ui& ui = **loaded;

    rows_model model;
    rap::list* soft = nullptr;
    rap::list* solid = nullptr;
    using namespace rap::layout;
    auto win = ui.create(window{
        "rapport theme roles — " + std::string(ui.backend_name()),
        {vbox{
            canvas{}.on_paint(paint_swatches).stretch(),
            hbox{
                button{"Button"},
                check{"Check box", rap::check_state::checked},
                entry{}.placeholder("Placeholder text"),
                drop_down{{"Drop-down", "Other"}, 0},
                label{"A link"}.link([] {}),
            },
            hbox{
                list{{{"Soft selection", {}, true}}, &model}.soft_selection().bind(soft).stretch(),
                list{{{"Solid selection", {}, true}}, &model}.bind(solid).stretch(),
            }
                .height(7_lines),
        }}}.size(660_px, 620_px));
    soft->select({1});
    solid->select({1});
    win->show();
    return ui.run();
}
