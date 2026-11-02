// The GObject file managers that take emblems from extensions (Nautilus,
// Nemo, Caja) share one info provider shape: update_file_info, answered at
// once or later through a closure with a handle, cancel_update, and
// add_emblem and invalidate_extension_info on their file infos. Each
// adapter describes its own types to the code here with an Fm:
//
//   struct Fm
//   {
//       using file_info = NautilusFileInfo;
//       using info_provider = NautilusInfoProvider;
//       using handle = NautilusOperationHandle;
//       static GFile* location(file_info*);     // a new reference
//       static void add_emblem(file_info*, const char* name);
//       static void invalidate(file_info*);    // asks update_file_info again
//       static void complete(GClosure*, info_provider*, handle*);
//   };
//
// The badge is asked on a worker thread, never the file manager's: the
// provider may wait on its app (Tortilla, up to 50 ms). The answer comes
// back on the main loop, where the emblem is added and the file manager
// told. Files given an emblem are remembered (weakly), so when the provider
// says a badge changed, the file manager is told to ask again.
#pragma once

#include "gobject_menu.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rap::shell::adapter
{

namespace badges_detail
{

// One update_file_info's badge, from the main loop to a worker and back.
struct job
{
    GObject* provider;
    GObject* file;
    GClosure* done;
    std::filesystem::path path;
    std::string icon;
    bool cancelled = false;  // main loop only
    void (*add_emblem)(GObject*, const char*);
    void (*complete)(GClosure*, GObject*, void*);
};

// Everything below is the main loop's alone.
inline std::unordered_set<job*>& live()
{
    static auto* s = new std::unordered_set<job*>;
    return *s;
}

// The files asked about, by path, held weakly: the file manager keeps a
// file info while it shows the file.
struct shown_files
{
    std::unordered_map<std::string, GWeakRef> files;
    size_t sweep_at = 1024;
    void (*invalidate)(GObject*) = nullptr;
};

inline shown_files& shown()
{
    static auto* s = new shown_files;
    return *s;
}

inline void remember(GObject* file, const std::filesystem::path& path)
{
    shown_files& s = shown();
    auto [it, added] = s.files.try_emplace(path.string());
    if (added)
        g_weak_ref_init(&it->second, file);
    else
        g_weak_ref_set(&it->second, file);
    if (s.files.size() < s.sweep_at) return;
    // Let go of the files gone, now and then.
    for (auto i = s.files.begin(); i != s.files.end();)
    {
        if (GObject* f = static_cast<GObject*>(g_weak_ref_get(&i->second)))
        {
            g_object_unref(f);
            ++i;
            continue;
        }
        g_weak_ref_clear(&i->second);
        i = s.files.erase(i);
    }
    s.sweep_at = std::max<size_t>(1024, s.files.size() * 2);
}

// Tells the file manager to ask about `path` again, if it's still shown.
inline void invalidate(std::unordered_map<std::string, GWeakRef>::iterator it)
{
    shown_files& s = shown();
    if (GObject* f = static_cast<GObject*>(g_weak_ref_get(&it->second)))
    {
        s.invalidate(f);
        g_object_unref(f);
    }
}

inline gboolean on_changed(gpointer data)
{
    std::unique_ptr<std::vector<std::filesystem::path>> paths(
        static_cast<std::vector<std::filesystem::path>*>(data));
    shown_files& s = shown();
    if (!s.invalidate) return G_SOURCE_REMOVE;
    if (paths->empty())
        for (auto it = s.files.begin(); it != s.files.end(); ++it) invalidate(it);
    else
        for (const auto& p : *paths)
            if (auto it = s.files.find(p.string()); it != s.files.end()) invalidate(it);
    return G_SOURCE_REMOVE;
}

inline gboolean finish(gpointer data)
{
    auto* j = static_cast<job*>(data);
    live().erase(j);
    if (!j->cancelled)
    {
        if (!j->icon.empty()) j->add_emblem(j->file, j->icon.c_str());
        j->complete(j->done, j->provider, j);
    }
    g_closure_unref(j->done);
    g_object_unref(j->file);
    g_object_unref(j->provider);
    delete j;
    return G_SOURCE_REMOVE;
}

inline void work(gpointer data, gpointer)
{
    auto* j = static_cast<job*>(data);
    j->icon = badge_icon(j->path);
    g_idle_add(finish, j);
}

inline GThreadPool* pool()
{
    // A few at once: Nautilus asks one file at a time per folder, the
    // others more.
    static GThreadPool* p = g_thread_pool_new(work, nullptr, 4, FALSE, nullptr);
    return p;
}

}  // namespace badges_detail

// update_file_info: true when the badge will come later through `done`
// (IN_PROGRESS, `*handle` set), false when there's none to give (COMPLETE).
template <class Fm>
bool start_badge(typename Fm::info_provider* provider, typename Fm::file_info* file, GClosure* done,
                 typename Fm::handle** handle) noexcept
{
    using namespace badges_detail;
    try
    {
        if (!has_badges()) return false;
        auto path = local_path(Fm::location(file));
        if (!path) return false;
        shown_files& s = shown();
        if (!s.invalidate)
        {
            s.invalidate =
                +[](GObject* f) { Fm::invalidate(reinterpret_cast<typename Fm::file_info*>(f)); };
            // The provider's word that badges changed comes from its own
            // thread: the paths are handed to the main loop.
            watch_badges(
                [](std::vector<std::filesystem::path> paths)
                {
                    g_idle_add(on_changed,
                               new std::vector<std::filesystem::path>(std::move(paths)));
                });
        }
        remember(G_OBJECT(file), *path);
        auto* j = new job{
            .provider = G_OBJECT(g_object_ref(provider)),
            .file = G_OBJECT(g_object_ref(file)),
            .done = g_closure_ref(done),
            .path = std::move(*path),
            .add_emblem = +[](GObject* f, const char* name)
                          { Fm::add_emblem(reinterpret_cast<typename Fm::file_info*>(f), name); },
            .complete =
                +[](GClosure* c, GObject* p, void* h)
                {
                    Fm::complete(c, reinterpret_cast<typename Fm::info_provider*>(p),
                                 static_cast<typename Fm::handle*>(h));
                },
        };
        live().insert(j);
        *handle = reinterpret_cast<typename Fm::handle*>(j);
        g_thread_pool_push(pool(), j, nullptr);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// cancel_update: the answer, when it comes, is dropped.
inline void cancel_badge(void* handle) noexcept
{
    auto* j = static_cast<badges_detail::job*>(handle);
    if (badges_detail::live().contains(j)) j->cancelled = true;
}

}  // namespace rap::shell::adapter
