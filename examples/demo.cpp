// The rebase dialog from pengit's design notes, as an anyui smoke test.
//   ANYUI_BACKEND=null ./anyui-demo   prints the tree without a display
#include <anyui/anyui.h>

#include <cstdio>

namespace
{

class TodoModel : public anyui::ListModel
{
public:
    size_t rowCount() const override { return rows_.size(); }
    std::string text(size_t row, size_t col) const override
    {
        return col == 0 ? rows_[row].action : rows_[row].subject;
    }

private:
    struct Row
    {
        std::string action, subject;
    };
    std::vector<Row> rows_{
        {"pick", "Add anyui layout builders"},
        {"squash", "fixup: form labels"},
        {"pick", "Null backend"},
    };
};

}  // namespace

int main(int argc, char** argv)
{
    auto ui = anyui::Ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }
    std::printf("backend: %.*s\n", int((*ui)->backendName().size()), (*ui)->backendName().data());

    TodoModel model;
    anyui::Label* branchName = nullptr;
    anyui::Combo* upstream = nullptr;
    anyui::List* commitList = nullptr;
    anyui::TextView* commitMessage = nullptr;
    anyui::Check* preserveMerges = nullptr;

    using namespace anyui::layout;
    auto dlg = (*ui)->create(Dialog{
        "Rebase",
        {VBox{
            Form{
                {"Branch", Label{"main"}.bind(branchName)},
                {"Upstream", Combo{{"origin/main", "origin/next"}, "origin/main"}.bind(upstream)},
            },
            Splitter{
                Vertical,
                List{{{"Action", 8_ch}, {"Subject", {}, true}}, &model}.bind(commitList).stretch(),
                TextView{}.readOnly().monospace().height(6_lines).bind(commitMessage),
            }
                .stretch(),
            HBox{
                Check{"Preserve merges"}.bind(preserveMerges),
                Spacer{},
                Button{"Up"}.onClick([] {}),
                Button{"Down"}.onClick([] {}),
            },
            Buttons{Ok("Start rebase"), Cancel},
        }}}.size(80_ch, 30_lines));

    bool bound = branchName && upstream && commitList && commitMessage && preserveMerges;
    std::printf("binds: %s\n", bound ? "all filled" : "MISSING");

    dlg->onFinished([&](anyui::DialogRole) { (*ui)->quit(); });
    dlg->show();
    (*ui)->post([&] { std::printf("posted task ran\n"); });
    return (*ui)->run();
}
