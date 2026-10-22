// The rebase dialog from pengit's design notes, as a rapport smoke test.
//   RAPPORT_BACKEND=null ./rapport-demo   prints the tree without a display
#include <rapport/rapport.h>

#include <cstdio>

namespace
{

class todo_model : public rap::list_model
{
public:
    size_t row_count() const override { return rows_.size(); }
    std::string text(size_t row, size_t col) const override
    {
        return col == 0 ? rows_[row].action : rows_[row].subject;
    }

private:
    struct row
    {
        std::string action, subject;
    };
    std::vector<row> rows_{
        {"pick", "Add rapport layout builders"},
        {"squash", "fixup: form labels"},
        {"pick", "Null backend"},
    };
};

}  // namespace

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s", ui.error().c_str());
        return 1;
    }
    std::printf("backend: %.*s\n", int((*ui)->backend_name().size()), (*ui)->backend_name().data());

    todo_model model;
    rap::label* branch_name = nullptr;
    rap::combo* upstream = nullptr;
    rap::list* commit_list = nullptr;
    rap::text_view* commit_message = nullptr;
    rap::check* preserve_merges = nullptr;

    using namespace rap::layout;
    auto dlg = (*ui)->create(dialog{
        "Rebase",
        {vbox{
            form{
                {"Branch", label{"main"}.bind(branch_name)},
                {"Upstream", combo{{"origin/main", "origin/next"}, "origin/main"}.bind(upstream)},
            },
            splitter{
                vertical,
                list{{{"Action", 8_ch}, {"Subject", {}, true}}, &model}.bind(commit_list).stretch(),
                text_view{}.read_only().monospace().height(6_lines).bind(commit_message),
            }
                .stretch(),
            hbox{
                check{"Preserve merges"}.bind(preserve_merges),
                spacer{},
                button{"Up"}.on_click([] {}),
                button{"Down"}.on_click([] {}),
            },
            buttons{ok("Start rebase"), cancel},
        }}}.size(80_ch, 30_lines));

    bool bound = branch_name && upstream && commit_list && commit_message && preserve_merges;
    std::printf("binds: %s\n", bound ? "all filled" : "MISSING");

    dlg->on_finished([&](rap::dialog_role) { (*ui)->quit(); });
    dlg->show();
    (*ui)->post([&] { std::printf("posted task ran\n"); });
    return (*ui)->run();
}
