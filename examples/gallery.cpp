// Every anyui widget in one window, plus a log-style list with a painted
// graph column over 10,000 rows.
//   ./anyui-gallery                 backend picked from the desktop
//   ANYUI_BACKEND=gtk3 ./anyui-gallery
#include <anyui/anyui.h>

#include <cstdio>

namespace
{

// Fake history: a main line with feature branches forking off and merging
// back, enough to exercise lines, curves, dots and ref pills.
class LogModel : public anyui::ListModel
{
public:
    size_t rowCount() const override { return 10000; }

    std::string text(size_t row, size_t col) const override
    {
        static const char* authors[] = {"Ada", "Linus", "Grace", "Ken", "Barbara"};
        switch (col)
        {
        case 0: return lane(row) ? "branch" : "main";
        case 1: return "Commit message number " + std::to_string(rowCount() - row);
        case 2: return authors[row % 5];
        case 3: return "2026-10-" + std::to_string(22 - int(row / 500 % 20));
        }
        return {};
    }

    anyui::TextStyle style(size_t row, size_t col) const override
    {
        anyui::TextStyle s;
        if (row == 0 && col == 1) s.bold = true;
        return s;
    }

    void paint(size_t row, size_t, anyui::Painter& p, const anyui::CellInfo& cell) const override
    {
        using anyui::ThemeColor;
        const anyui::Color lanes[] = {{0x35, 0x84, 0xe4}, {0xe6, 0x61, 0x00}, {0x26, 0xa2, 0x69}};
        float w = 14, mid = cell.size.h / 2;
        auto x = [&](int l) { return 8 + l * w; };

        int here = lane(row);
        bool branchOpen = row % 12 >= 2 && row % 12 <= 9;  // lane 1 alive through these rows

        p.line({x(0), 0}, {x(0), cell.size.h}, lanes[0], 2);
        if (branchOpen)
        {
            if (row % 12 != 2) p.line({x(1), 0}, {x(1), mid}, lanes[1], 2);
            if (row % 12 != 9) p.line({x(1), mid}, {x(1), cell.size.h}, lanes[1], 2);
        }
        if (row % 12 == 2)  // merge commit on main pulls lane 1 in
            p.curve({x(0), mid}, {x(1), mid}, {x(1), mid}, {x(1), cell.size.h}, lanes[1], 2);
        if (row % 12 == 9)  // fork point
            p.curve({x(1), 0}, {x(1), mid}, {x(1), mid}, {x(0), mid}, lanes[1], 2);

        p.circle({x(here), mid}, 4, lanes[here], anyui::Paint{ThemeColor::Base});

        // Ref pill after the lanes.
        if (row % 40 == 0)
        {
            std::string ref = row == 0 ? "HEAD -> main" : "v" + std::to_string(250 - row / 40);
            anyui::TextStyle ts;
            ts.fg = anyui::Color{0xff, 0xff, 0xff};
            ts.bold = true;
            anyui::SizeF sz = p.measure(ref, ts);
            float px = x(2) + 6, py = (cell.size.h - sz.h) / 2;
            p.rect({px - 4, py, sz.w + 8, sz.h}, 4,
                   anyui::Paint{row == 0 ? anyui::Color{0x26, 0xa2, 0x69}
                                         : anyui::Color{0x91, 0x41, 0xac}},
                   std::nullopt);
            p.text({px, py}, ref, ts);
        }
    }

private:
    static int lane(size_t row) { return row % 12 >= 3 && row % 12 <= 8 ? 1 : 0; }
};

const char* kDiff = "diff --git a/src/main.cpp b/src/main.cpp\n"
                    "@@ -1,5 +1,6 @@\n"
                    " #include <cstdio>\n"
                    "-int main() { puts(\"hi\"); }\n"
                    "+int main()\n"
                    "+{\n"
                    "+    puts(\"hello\");\n"
                    "+}\n";

std::vector<anyui::TextView::Span> diffSpans(const std::string& text)
{
    std::vector<anyui::TextView::Span> spans;
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        anyui::TextStyle s;
        if (text.compare(at, 2, "@@") == 0)
        {
            s.fg = anyui::ThemeColor::Accent;
            s.bold = true;
        }
        else if (text[at] == '+')
            s.fg = anyui::ThemeColor::Positive;
        else if (text[at] == '-')
            s.fg = anyui::ThemeColor::Negative;
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
    auto loaded = anyui::Ui::load(argc, argv);
    if (!loaded)
    {
        std::fprintf(stderr, "%s", loaded.error().c_str());
        return 1;
    }
    anyui::Ui& ui = **loaded;

    LogModel model;
    anyui::Label* status = nullptr;
    anyui::List* log = nullptr;
    anyui::TextView* details = nullptr;

    auto say = [&](const std::string& s) { status->setText(s); };

    using namespace anyui::layout;
    auto win = ui.create(Window{
        "anyui gallery — " + std::string(ui.backendName()),
        {VBox{
            Tabs{
                {"_Log",
                 VBox{
                     Entry{}
                         .placeholder("Filter: message, author, path")
                         .onChanged([&](const std::string& t) { say("filter: " + t); }),
                     Splitter{
                         Vertical,
                         List{{{"Graph", 26_ch, false, true},
                               {"Message", {}, true, false, true},
                               {"Author", 12_ch, false, false, true},
                               {"Date", 12_ch, false, false, true}},
                              &model}
                             .bind(log)
                             .onSelectionChanged(
                                 [&]
                                 {
                                     auto sel = log->selection();
                                     say(std::to_string(sel.size()) + " selected");
                                 })
                             .onActivate(
                                 [&](size_t row)
                                 {
                                     ui.message(nullptr,
                                                {.icon = anyui::MessageSpec::Icon::Info,
                                                 .title = "Commit",
                                                 .text = model.text(row, 1),
                                                 .detail = "Activated row " + std::to_string(row)});
                                 })
                             .onSortRequested(
                                 [&](size_t col, anyui::SortOrder o)
                                 {
                                     log->setSortIndicator(col, o);
                                     say("sort by column " + std::to_string(col) +
                                         (o == anyui::SortOrder::Ascending ? " ascending"
                                                                           : " descending"));
                                 })
                             .onContextMenu(
                                 [&](std::optional<size_t> row) -> anyui::Menu
                                 {
                                     if (!row) return {};
                                     size_t r = *row;
                                     return {
                                         {.label = "Copy commit hash",
                                          .action = [&ui, r]
                                          { ui.setClipboard("deadbeef" + std::to_string(r)); },
                                          .shortcut = "Ctrl+C"},
                                         {.label = "Show log of this branch",
                                          .action = [&] { say("show log"); }},
                                         anyui::MenuItem::separator(),
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
                         TextView{}.readOnly().monospace().wrap(false).bind(details).stretch(1),
                     }
                         .stretch(),
                 }},
                {"_Widgets",
                 VBox{
                     Form{
                         {"Entry", Entry{"some text"}.placeholder("type here")},
                         {"Select",
                          Select{{"pick", "reword", "edit", "squash", "fixup", "drop"}, 0}
                              .onChanged([&](int i) { say("select " + std::to_string(i)); })},
                         {"Combo",
                          Combo{{"origin/main", "origin/next", "upstream/main"}, "origin/main"}
                              .onChanged([&](const std::string& t) { say("combo: " + t); })},
                         {"", Check{"A check box", true}.onToggled(
                                  [&](bool on) { say(on ? "checked" : "unchecked"); })},
                     },
                     Separator{},
                     Group{"Group",
                           HBox{
                               Button{"_Message box"}.onClick(
                                   [&]
                                   {
                                       ui.message(
                                           nullptr,
                                           {.icon = anyui::MessageSpec::Icon::Question,
                                            .title = "Question",
                                            .text = "Really do the thing?",
                                            .buttons = {anyui::DialogRole::Yes,
                                                        anyui::DialogRole::No}},
                                           [&](anyui::DialogRole r)
                                           { say(r == anyui::DialogRole::Yes ? "yes" : "no"); });
                                   }),
                               Button{"_Open files…"}.onClick(
                                   [&]
                                   {
                                       ui.chooseFiles(
                                           nullptr,
                                           {.mode = anyui::FileDialogSpec::Mode::OpenMany,
                                            .title = "Pick files"},
                                           [&](std::vector<std::string> files)
                                           { say(std::to_string(files.size()) + " files"); });
                                   }),
                               Spacer{},
                               Button{"Disabled"}.enabled(false).tooltip("Can't touch this"),
                           }},
                     TextView{"Editable text area.\nType here."}.stretch(),
                 }},
            }
                .stretch(),
            HBox{
                Label{"Ready"}.bind(status).stretch(),
                Button{"_Quit"}.onClick([&] { ui.quit(); }),
            },
        }}}.size(110_ch, 36_lines));

    details->setText(kDiff, diffSpans(kDiff));
    win->onCloseRequest(
        [&]
        {
            ui.quit();
            return true;
        });
    win->show();
    return ui.run();
}
