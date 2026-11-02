// rapport-split-show: splitter panes hidden at first and shown later, as an
// app shows a side panel only when there's something for it (pengit's log
// shows its pull request column this way). The window starts with the left
// column alone; then, on timers:
//   1 s   the right column shows, 60 characters wide, and a third pane
//         shows under the left column's two;
//   2 s   both hide again (their sizes kept, as an app saves them);
//   3 s   both show again, given back the sizes they had.
// Shown, the right column has its own room beside the left one (never over
// it), and its long texts wrap inside it rather than running off its edge.
//   rapport-split-show --stay   stops after the first show
#include <rapport/rapport.h>

#include <cstdio>
#include <string_view>
#include <vector>

namespace
{

class files_model : public rap::list_model
{
public:
    size_t row_count() const override { return 12; }
    std::string text(size_t row, size_t col) const override
    {
        return col == 0 ? "src/part_" + std::to_string(row) + ".cpp"
                        : std::string(row % 3 ? "Modified" : "Added");
    }
};

constexpr const char* long_text =
    "This pull request's description is one long paragraph, as people write them: it explains what "
    "changed and why, mentions the issue it fixes, and goes on well past the width of any side "
    "panel, "
    "so it has to wrap inside the column it's shown in rather than run off its right edge.";

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }
    bool stay = false;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--stay") stay = true;

    files_model files;
    rap::splitter* columns = nullptr;
    rap::splitter* rows = nullptr;
    rap::widget* side = nullptr;
    rap::widget* notes = nullptr;
    rap::text_view* message = nullptr;

    using namespace rap::layout;
    auto win = (*ui)->create(window{
        "Panes shown later",
        {splitter{
            horizontal,
            splitter{
                vertical,
                text_view{}.read_only().bind(message).stretch(),
                list{{{"Path", {}, true}, {"Status", 12_ch}}, &files}.stretch(2),
                vbox{
                    label{"Notes (the third pane, shown later)"},
                    label{long_text}.wrap(),
                }
                    .visible(false)
                    .handle(notes),
            }
                .bind(rows)
                .stretch(3),
            vbox{
                scroll{vbox{
                           label{"Pull request #42"},
                           label{"Wrap long lines in the side panel"}.wrap().selectable(),
                           hbox{
                               label{"Status:"},
                               label{"Checks are running; this line also wraps when the column is "
                                     "narrow."}
                                   .wrap()
                                   .stretch(),
                           },
                           label{long_text}.wrap().selectable(),
                           separator{},
                           label{"Build"},
                           label{"Linux, Windows and macOS builds, each with its tests; the last "
                                 "one failed in a "
                                 "test that's known to be flaky."}
                               .wrap(),
                           spacer{},
                       }}
                    .stretch(),
            }
                .width(60_ch)
                .visible(false)
                .handle(side),
        }
             .bind(columns)}}.size(140_ch, 36_lines));
    message->set_text(
        "The commit's message: the left column's top pane.\n\nThe right column is hidden at "
        "first; it shows a second in, beside this one.",
        {});

    std::vector<float> column_sizes, row_sizes;
    auto show = [&](bool on)
    {
        if (!on) column_sizes = columns->sizes(), row_sizes = rows->sizes();
        side->set_visible(on);
        notes->set_visible(on);
        if (on && !column_sizes.empty()) columns->set_sizes(column_sizes);
        if (on && !row_sizes.empty()) rows->set_sizes(row_sizes);
        auto print = [](const char* what, const std::vector<float>& v)
        {
            std::printf("%s:", what);
            for (float s : v) std::printf(" %.0f", s);
            std::printf("\n");
        };
        std::printf("%s\n", on ? "shown" : "hidden");
        print("  columns", columns->sizes());
        print("  rows", rows->sizes());
        std::fflush(stdout);
    };
    std::vector<rap::subscription> timers;
    timers.push_back((*ui)->timer(1000, false, [&] { show(true); }));
    if (!stay)
    {
        timers.push_back((*ui)->timer(2000, false, [&] { show(false); }));
        timers.push_back((*ui)->timer(3000, false, [&] { show(true); }));
    }

    win->on_close_request(
        [&]
        {
            (*ui)->quit();
            return true;
        });
    win->show();
    return (*ui)->run();
}
