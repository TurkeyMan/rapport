// What Explorer's view said, carried from the entry's first GetState to its
// second (shell/src/win32/explorer_view.h): a snapshot taken once by the slow
// call about the same items, none across a change of site, and what a
// right-click is with a view and without one.
//   rapport-explorer-view-test
#include "explorer_view.h"

#include <cstdio>
#include <string>

namespace
{

int fails = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

namespace d = rap::shell::detail;

const std::filesystem::path dir = "C:/work/repo";
const std::filesystem::path file = "C:/work/repo/a.txt";
const std::filesystem::path other = "C:/work/other";

d::view_info in(const std::filesystem::path& folder, bool has_selection)
{
    return {folder, has_selection};
}

bool same(const std::optional<d::view_info>& v, const std::filesystem::path& folder,
          bool has_selection)
{
    return v && v->folder == folder && v->has_selection == has_selection;
}

}  // namespace

int main()
{
    // The fast call, then the slow call about the same items.
    {
        d::view_snapshots s;
        check(s.publish(s.begin(), {file}, in(dir, true)), "a snapshot lands");
        check(same(s.take({file}), dir, true), "the slow call about the same items gets it");
        check(!s.take({file}), "and only once: a reused entry gets no view, not the last");
    }

    // A change of site between the calls.
    {
        d::view_snapshots s;
        s.publish(s.begin(), {dir}, in(dir, false));
        s.site_changed();
        check(!s.take({dir}), "a new site drops what the last one said");

        const auto asked = s.begin();
        s.site_changed();
        check(!s.publish(asked, {dir}, in(other, false)) && !s.take({dir}),
              "an ask under way when the site changed doesn't land");
        check(s.publish(s.begin(), {dir}, in(dir, false)) && same(s.take({dir}), dir, false),
              "an ask of the new site does");
    }

    // Overlapping right-clicks: two fast calls before either slow one.
    {
        d::view_snapshots s;
        s.publish(s.begin(), {file}, in(dir, true));
        s.publish(s.begin(), {}, in(other, false));
        check(same(s.take({file}), dir, true), "the first slow call gets its own snapshot");
        check(same(s.take({}), other, false), "and the second its own");

        s.publish(s.begin(), {file}, in(dir, true));
        s.publish(s.begin(), {file}, in(other, true));
        check(same(s.take({file}), other, true) && !s.take({file}),
              "a second right-click about the same items replaces the first");

        s.publish(s.begin(), {file}, in(dir, true));
        check(!s.take({other}) && same(s.take({file}), dir, true),
              "a slow call about other items doesn't take one that isn't its own");

        for (size_t i = 0; i <= d::view_snapshots::most; ++i)
            s.publish(s.begin(), {std::filesystem::path("C:/f") / std::to_string(i)},
                      in(dir, true));
        check(!s.take({"C:/f/0"}) && s.take({"C:/f/1"}), "past the most kept, the oldest goes");
    }

    // What was right-clicked, with the view.
    check(d::selection_of({dir}, in(dir, false)).background &&
              d::selection_of({dir}, in(dir, false)).folder == dir &&
              d::selection_of({dir}, in(dir, false)).paths.empty(),
          "the view's own folder, nothing selected: a background click");
    check(d::selection_of({}, in(dir, false)).background,
          "no items, nothing selected: a background click on the view's folder");
    {
        const auto sel = d::selection_of({dir}, in(dir, true));
        check(!sel.background && sel.paths.size() == 1 && sel.folder == dir.parent_path(),
              "the view's folder with something selected (the nav pane): that folder");
    }
    {
        const auto sel = d::selection_of({file}, in(dir, true));
        check(!sel.background && sel.paths.size() == 1 && sel.folder == dir,
              "a file: that file, in its folder");
    }

    // Without one: the slow call found no snapshot.
    {
        const auto sel = d::selection_of({dir}, std::nullopt);
        check(!sel.background && sel.paths.size() == 1 && sel.paths.front() == dir,
              "no view: a background click is taken for its folder");
    }
    {
        d::view_snapshots s;
        s.publish(s.begin(), {}, in(other, false));
        s.site_changed();
        const auto sel = d::selection_of({}, s.take({}));
        check(!sel.background && sel.paths.empty() && sel.folder.empty(),
              "no items and no view: nothing, never a stale folder");
    }

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
