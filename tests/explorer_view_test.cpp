// What Explorer's view said, carried from the entry's first GetState to its
// second (shell/src/win32/explorer_view.h): a snapshot taken once by the slow
// call about the same items, none across a change of site, and what a
// right-click is with a view and without one.
//   rapport-explorer-view-test
#include "explorer_view.h"

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

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

    // A change of site between the calls: the site, the generation and the
    // snapshots change together.
    {
        int site = 1;  // stands in for the entry's site_
        d::view_snapshots s;
        s.publish(s.begin(), {dir}, in(dir, false));
        s.change_site([&] { site = 2; });
        check(!s.take({dir}), "a new site drops what the last one said");

        int read = 0;
        const auto asked = s.begin([&] { read = site; });
        s.change_site([&] { site = 3; });
        check(read == 2 && !s.publish(asked, {dir}, in(other, false)) && !s.take({dir}),
              "an ask of the last site under way when it changed doesn't land");

        // The swap and the drop are one step: a slow call that comes while
        // the site is being swapped waits, then finds nothing of the last.
        s.publish(s.begin([&] { read = site; }), {file}, in(dir, true));
        std::optional<d::view_info> mid;
        std::thread slow;
        s.change_site(
            [&]
            {
                slow = std::thread([&] { mid = s.take({file}); });
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                site = 4;
            });
        slow.join();
        check(read == 3 && !mid, "a slow call during the swap gets nothing of the last site");

        s.begin([&] { read = site; });
        check(read == 4 && s.publish(s.begin(), {dir}, in(dir, false)) &&
                  same(s.take({dir}), dir, false),
              "an ask of the new site lands");
    }

    // Overlapping right-clicks: two fast calls before either slow one.
    {
        d::view_snapshots s;
        s.publish(s.begin(), {file}, in(dir, true));
        s.publish(s.begin(), {other}, in(other, false));
        check(same(s.take({file}), dir, true), "the first slow call gets its own snapshot");
        check(same(s.take({other}), other, false), "and the second its own");

        // Two background clicks with no items, A's then B's, before A's slow
        // call: neither can be told which it is.
        s.publish(s.begin(), {}, in(dir, false));
        s.publish(s.begin(), {}, in(other, false));
        check(!s.take({}) && !s.take({}),
              "two right-clicks about the same items: no view for either");
        check(!s.take({}), "and nothing left after both slow calls");

        // A folder item and a background, both {dir}.
        s.publish(s.begin(), {dir}, in(dir, true));
        s.publish(s.begin(), {dir}, in(dir, false));
        check(!s.take({dir}) && !s.take({dir}),
              "a folder and its background, both {dir}: no view for either");
        check(s.publish(s.begin(), {dir}, in(dir, false)) && same(s.take({dir}), dir, false),
              "a right-click about them after both slow calls gets its own again");

        s.publish(s.begin(), {file}, in(dir, true));
        check(!s.take({other}) && same(s.take({file}), dir, true),
              "a slow call about other items doesn't take one that isn't its own");
    }

    // A slow call from before a new site, about the same items as one of the
    // new site's: it can't be told apart, so neither gets a view.
    {
        d::view_snapshots s;
        s.publish(s.begin(), {}, in(dir, false));
        s.change_site([] {});
        check(!s.publish(s.begin(), {}, in(other, false)),
              "the new site's right-click about the same items keeps no view");
        check(!s.take({}), "the last site's slow call, first: no view, never the new site's");
        check(!s.take({}), "the new site's own: no view either, it can't be told apart");
        check(s.publish(s.begin(), {}, in(other, false)) && same(s.take({}), other, false),
              "once both have come, a right-click gets its own again");

        s.publish(s.begin(), {file}, in(dir, true));
        s.change_site([] {});
        s.publish(s.begin(), {dir}, in(dir, false));
        check(same(s.take({dir}), dir, false) && !s.take({file}),
              "the new site's right-click about other items keeps its view");
    }

    // A new site before the last one's ask lands: Explorer still makes that
    // right-click's slow call, which mustn't take the new site's snapshot.
    {
        d::view_snapshots s;
        const auto asked = s.begin();
        s.change_site([] {});
        check(!s.publish(asked, {}, in(dir, false)), "the last site's ask keeps no view");
        check(!s.publish(s.begin(), {}, in(other, false)),
              "the new site's right-click about the same items keeps none either");
        check(!s.take({}), "the last site's slow call, first: no view, never the new site's");
        check(!s.take({}), "the new site's own: no view, it can't be told apart");
        check(s.publish(s.begin(), {}, in(other, false)) && same(s.take({}), other, false),
              "once both have come, a right-click gets its own again");

        const auto alone = s.begin();
        s.change_site([] {});
        s.publish(alone, {dir}, in(dir, false));
        check(!s.take({dir}), "a last-site ask alone: its slow call gets no view");
        check(s.publish(s.begin(), {dir}, in(dir, false)) && same(s.take({dir}), dir, false),
              "and is owed no more after it");
    }

    // Past the most tracked: none is dropped (its slow call could take a
    // later snapshot about the same items); tracking is given up instead.
    {
        d::view_snapshots s;
        s.publish(s.begin(), {}, in(dir, false));
        for (size_t i = 1; i < d::view_snapshots::most; ++i)
            s.publish(s.begin(), {std::filesystem::path("C:/f") / std::to_string(i)},
                      in(dir, true));
        check(!s.publish(s.begin(), {"C:/f/new"}, in(dir, true)),
              "one more than the most tracked doesn't land");
        check(!s.publish(s.begin(), {}, in(other, false)),
              "nor, after, a background click about the items of one that was waiting");
        check(!s.take({}), "whose older slow call, first, gets no view, not the later folder's");
        check(!s.take({}) && !s.take({"C:/f/1"}), "and nothing gets one from then on");
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
        s.change_site([] {});
        const auto sel = d::selection_of({}, s.take({}));
        check(!sel.background && sel.paths.empty() && sel.folder.empty(),
              "no items and no view: nothing, never a stale folder");
    }

    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
