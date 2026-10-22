// Every rapport widget in one window, plus a log-style list with a painted
// graph column over 10,000 rows.
//   ./rapport-gallery                 backend picked from the desktop
//   RAPPORT_BACKEND=gtk3 ./rapport-gallery
#include <rapport/rapport.h>

#include <cstdio>

namespace
{

// Fake history: a main line with feature branches forking off and merging
// back, enough to exercise lines, curves, dots and ref pills.
class log_model : public rap::list_model
{
public:
    size_t row_count() const override { return 10000; }

    std::string text(size_t row, size_t col) const override
    {
        static const char* authors[] = {"Ada", "Linus", "Grace", "Ken", "Barbara"};
        switch (col)
        {
        case 0: return lane(row) ? "branch" : "main";
        case 1: return "Commit message number " + std::to_string(row_count() - row);
        case 2: return authors[row % 5];
        case 3: return "2026-10-" + std::to_string(22 - int(row / 500 % 20));
        }
        return {};
    }

    rap::text_style style(size_t row, size_t col) const override
    {
        rap::text_style s;
        if (row == 0 && col == 1) s.bold = true;
        return s;
    }

    void paint(size_t row, size_t, rap::painter& p, const rap::cell_info& cell) const override
    {
        using rap::theme_color;
        const rap::color lanes[] = {{0x35, 0x84, 0xe4}, {0xe6, 0x61, 0x00}, {0x26, 0xa2, 0x69}};
        float w = 14, mid = cell.size.h / 2;
        auto x = [&](int l) { return 8 + l * w; };

        int here = lane(row);
        // Lane 1 runs straight through the branch's rows; the merge (row 2)
        // and fork (row 9) rows only draw the curve, which meets it.
        bool branch_row = row % 12 >= 3 && row % 12 <= 8;

        p.line({x(0), 0}, {x(0), cell.size.h}, lanes[0], 2);
        if (branch_row) p.line({x(1), 0}, {x(1), cell.size.h}, lanes[1], 2);
        if (row % 12 == 2)  // merge commit on main pulls lane 1 in
            p.curve({x(0), mid}, {x(1), mid}, {x(1), mid}, {x(1), cell.size.h}, lanes[1], 2);
        if (row % 12 == 9)  // fork point
            p.curve({x(1), 0}, {x(1), mid}, {x(1), mid}, {x(0), mid}, lanes[1], 2);

        p.circle({x(here), mid}, 4, lanes[here], rap::paint{theme_color::base});

        // Ref pill after the lanes.
        if (row % 40 == 0)
        {
            std::string ref = row == 0 ? "HEAD -> main" : "v" + std::to_string(250 - row / 40);
            rap::text_style ts;
            ts.fg = rap::color{0xff, 0xff, 0xff};
            ts.bold = true;
            rap::size_f sz = p.measure(ref, ts);
            float px = x(2) + 6, py = (cell.size.h - sz.h) / 2;
            p.rect({px - 4, py, sz.w + 8, sz.h}, 4,
                   rap::paint{row == 0 ? rap::color{0x26, 0xa2, 0x69}
                                         : rap::color{0x91, 0x41, 0xac}},
                   std::nullopt);
            p.text({px, py}, ref, ts);
        }
    }

private:
    static int lane(size_t row) { return row % 12 >= 3 && row % 12 <= 8 ? 1 : 0; }
};

const char* sample_diff = "diff --git a/src/main.cpp b/src/main.cpp\n"
                    "@@ -1,5 +1,6 @@\n"
                    " #include <cstdio>\n"
                    "-int main() { puts(\"hi\"); }\n"
                    "+int main()\n"
                    "+{\n"
                    "+    puts(\"hello\");\n"
                    "+}\n";

std::vector<rap::text_view::span> diff_spans(const std::string& text)
{
    std::vector<rap::text_view::span> spans;
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        rap::text_style s;
        if (text.compare(at, 2, "@@") == 0)
        {
            s.fg = rap::theme_color::accent;
            s.bold = true;
        }
        else if (text[at] == '+')
            s.fg = rap::theme_color::positive;
        else if (text[at] == '-')
            s.fg = rap::theme_color::negative;
        else if (text.compare(at, 4, "diff") == 0)
            s.bold = true;
        if (s.fg || s.bold) spans.push_back({at, end, s});
        at = end + 1;
    }
    return spans;
}

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

    log_model model;
    rap::label* status = nullptr;
    rap::list* log = nullptr;
    rap::text_view* details = nullptr;

    auto say = [&](const std::string& s) { status->set_text(s); };

    using namespace rap::layout;
    auto win = ui.create(window{
        "rapport gallery — " + std::string(ui.backend_name()),
        {vbox{
            tabs{
                {"_Log",
                 vbox{
                     entry{}
                         .placeholder("Filter: message, author, path")
                         .on_changed([&](const std::string& t) { say("filter: " + t); }),
                     splitter{
                         vertical,
                         list{{{"Graph", 26_ch, false, true},
                               {"Message", {}, true, false, true},
                               {"Author", 12_ch, false, false, true},
                               {"Date", 12_ch, false, false, true}},
                              &model}
                             .bind(log)
                             .on_selection_changed(
                                 [&]
                                 {
                                     auto sel = log->selection();
                                     say(std::to_string(sel.size()) + " selected");
                                 })
                             .on_activate(
                                 [&](size_t row)
                                 {
                                     ui.message(nullptr,
                                                {.icon = rap::message_spec::icon::info,
                                                 .title = "Commit",
                                                 .text = model.text(row, 1),
                                                 .detail = "Activated row " + std::to_string(row)});
                                 })
                             .on_sort_requested(
                                 [&](size_t col, rap::sort_order o)
                                 {
                                     log->set_sort_indicator(col, o);
                                     say("sort by column " + std::to_string(col) +
                                         (o == rap::sort_order::ascending ? " ascending"
                                                                           : " descending"));
                                 })
                             .on_context_menu(
                                 [&](std::optional<size_t> row) -> rap::menu
                                 {
                                     if (!row) return {};
                                     size_t r = *row;
                                     return {
                                         {.label = "Copy commit hash",
                                          .action = [&ui, r]
                                          { ui.set_clipboard("deadbeef" + std::to_string(r)); },
                                          .shortcut = "Ctrl+C"},
                                         {.label = "Show log of this branch",
                                          .action = [&] { say("show log"); }},
                                         rap::menu_item::separator(),
                                         {.label = "Reset",
                                          .children =
                                              {
                                                  {.label = "Soft",
                                                   .action = [&] { say("reset --soft"); }},
                                                  {.label = "Mixed",
                                                   .action = [&] { say("reset --mixed"); }},
                                                  {.label = "Hard",
                                                   .action = [&] { say("reset --hard"); }},
                                              }},
                                         {.label = "Cherry-pick (disabled)", .enabled = false},
                                         {.label = "Show whitespace",
                                          .action = [&] { say("toggled"); },
                                          .checked = true},
                                     };
                                 })
                             .stretch(3),
                         text_view{}.read_only().monospace().wrap(false).bind(details).stretch(1),
                     }
                         .stretch(),
                 }},
                {"_Widgets",
                 vbox{
                     form{
                         {"Entry", entry{"some text"}.placeholder("type here")},
                         {"Select",
                          drop_down{{"pick", "reword", "edit", "squash", "fixup", "drop"}, 0}
                              .on_changed([&](int i) { say("select " + std::to_string(i)); })},
                         {"Combo",
                          combo{{"origin/main", "origin/next", "upstream/main"}, "origin/main"}
                              .on_changed([&](const std::string& t) { say("combo: " + t); })},
                         {"", check{"A check box", true}.on_toggled(
                                  [&](bool on) { say(on ? "checked" : "unchecked"); })},
                     },
                     separator{},
                     group{"Group",
                           hbox{
                               button{"_Message box"}.on_click(
                                   [&]
                                   {
                                       ui.message(
                                           nullptr,
                                           {.icon = rap::message_spec::icon::question,
                                            .title = "Question",
                                            .text = "Really do the thing?",
                                            .buttons = {rap::dialog_role::yes,
                                                        rap::dialog_role::no}},
                                           [&](rap::dialog_role r)
                                           { say(r == rap::dialog_role::yes ? "yes" : "no"); });
                                   }),
                               button{"_Open files…"}.on_click(
                                   [&]
                                   {
                                       ui.choose_files(
                                           nullptr,
                                           {.mode = rap::file_dialog_spec::mode::open_many,
                                            .title = "Pick files"},
                                           [&](std::vector<std::string> files)
                                           { say(std::to_string(files.size()) + " files"); });
                                   }),
                               spacer{},
                               button{"Disabled"}.enabled(false).tooltip("Can't touch this"),
                           }},
                     text_view{"Editable text area.\nType here."}.stretch(),
                 }},
            }
                .stretch(),
            hbox{
                label{"Ready"}.bind(status).stretch(),
                button{"_Quit"}.on_click([&] { ui.quit(); }),
            },
        }}}.size(110_ch, 36_lines));

    details->set_text(sample_diff, diff_spans(sample_diff));
    win->on_close_request(
        [&]
        {
            ui.quit();
            return true;
        });
    win->show();
    return ui.run();
}
