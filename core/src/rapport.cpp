#include "rapport/rapport.h"
#include "rapport/sys.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>

#ifndef RAPPORT_INSTALL_PLUGIN_DIR
#define RAPPORT_INSTALL_PLUGIN_DIR ""
#endif

namespace fs = std::filesystem;

namespace rap
{
namespace
{

std::string env(const char* name)
{
    return sys::env(name).value_or("");
}

#ifdef _WIN32
constexpr char path_separator = ';';
constexpr const char* plugin_suffix = ".dll";
#else
constexpr char path_separator = ':';
constexpr const char* plugin_suffix = ".so";
#endif

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string part; std::getline(ss, part, sep);)
        if (!part.empty()) out.push_back(part);
    return out;
}

// Desktops whose native toolkit is Qt; everything else known is GTK-based.
bool is_qt_desktop(const std::string& d)
{
    static const char* qt[] = {"kde", "plasma", "lxqt", "deepin", "dde", "ukui", "lomiri"};
    return std::ranges::any_of(qt, [&](const char* q) { return d == q; });
}

bool is_gtk_desktop(const std::string& d)
{
    static const char* gtk[] = {
        "gnome",  "unity", "xfce",   "x-cinnamon",      "cinnamon", "mate",   "pantheon",
        "budgie", "lxde",  "cosmic", "gnome-flashback", "phosh",    "ubuntu", "pop"};
    return std::ranges::any_of(gtk, [&](const char* g) { return d == g; });
}

fs::path exe_dir()
{
    return sys::executable_path().parent_path();
}

std::vector<fs::path> plugin_dirs(const load_options& opts)
{
    std::vector<fs::path> dirs;
    for (auto& p : opts.search_paths) dirs.push_back(sys::path_from_utf8(p));
    for (auto& p : split(env("RAPPORT_PLUGIN_PATH"), path_separator))
        dirs.push_back(sys::path_from_utf8(p));
    if (auto d = exe_dir(); !d.empty())
    {
        dirs.push_back(d);
        dirs.push_back(d / "rapport");
        dirs.push_back(d / ".." / "lib" / "rapport");
    }
    if (*RAPPORT_INSTALL_PLUGIN_DIR) dirs.emplace_back(RAPPORT_INSTALL_PLUGIN_DIR);
    return dirs;
}

std::map<std::string, create_backend_fn*>& builtins()
{
    static std::map<std::string, create_backend_fn*> registry;
    return registry;
}

// Applies everything the backend leaves to the core: callbacks, binds,
// list models and common state.
class applier : public build_sink
{
public:
    void created(const node& n, widget& w) override
    {
        if (!n.common.enabled) w.set_enabled(false);
        if (!n.common.visible) w.set_visible(false);
        if (!n.common.tooltip.empty()) w.set_tooltip(n.common.tooltip);
        if (n.common.handle) *n.common.handle = &w;
        std::visit([&](const auto& s) { apply(s, w); }, n.spec);
    }

    void created(const spec::dialog_button& b, button& w) override
    {
        if (b.on_click) w.on_click(b.on_click);
        if (b.bind) *b.bind = &w;
    }

private:
    template <class T>
    static void bind(T** handle, widget& w)
    {
        if (handle) *handle = &static_cast<T&>(w);
    }

    static void apply(const spec::splitter& s, widget& w) { bind(s.bind, w); }
    static void apply(const spec::tabs& s, widget& w)
    {
        auto& t = static_cast<tabs&>(w);
        if (s.on_changed) t.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::label& s, widget& w) { bind(s.bind, w); }
    static void apply(const spec::button& s, widget& w)
    {
        auto& b = static_cast<button&>(w);
        if (s.on_click) b.on_click(s.on_click);
        bind(s.bind, w);
    }
    static void apply(const spec::tool_button& s, widget& w)
    {
        auto& b = static_cast<tool_button&>(w);
        if (s.on_click) b.on_click(s.on_click);
        if (s.on_toggled) b.on_toggled(s.on_toggled);
        bind(s.bind, w);
    }
    static void apply(const spec::slider& s, widget& w)
    {
        auto& c = static_cast<slider&>(w);
        if (s.on_changed) c.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::spin& s, widget& w)
    {
        auto& c = static_cast<spin&>(w);
        if (s.on_changed) c.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::color_button& s, widget& w)
    {
        auto& c = static_cast<color_button&>(w);
        if (s.on_changed) c.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::check& s, widget& w)
    {
        auto& c = static_cast<check&>(w);
        if (s.on_toggled) c.on_toggled(s.on_toggled);
        bind(s.bind, w);
    }
    static void apply(const spec::entry& s, widget& w)
    {
        auto& e = static_cast<entry&>(w);
        if (s.read_only) e.set_read_only(true);
        if (s.on_changed) e.on_changed(s.on_changed);
        if (s.on_activate) e.on_activate(s.on_activate);
        bind(s.bind, w);
    }
    static void apply(const spec::drop_down& s, widget& w)
    {
        auto& c = static_cast<drop_down&>(w);
        if (s.on_changed) c.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::combo& s, widget& w)
    {
        auto& c = static_cast<combo&>(w);
        if (s.on_changed) c.on_changed(s.on_changed);
        if (s.on_activate) c.on_activate(s.on_activate);
        bind(s.bind, w);
    }
    static void apply(const spec::text_view& s, widget& w)
    {
        auto& t = static_cast<text_view&>(w);
        if (s.on_changed) t.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::list& s, widget& w)
    {
        auto& l = static_cast<list&>(w);
        if (s.model) l.set_model(s.model);
        if (s.on_selection_changed) l.on_selection_changed(s.on_selection_changed);
        if (s.on_activate) l.on_activate(s.on_activate);
        if (s.on_sort_requested) l.on_sort_requested(s.on_sort_requested);
        if (s.on_context_menu) l.on_context_menu(s.on_context_menu);
        if (s.on_header_menu) l.on_header_menu(s.on_header_menu);
        if (s.on_reorder) l.on_reorder(s.on_reorder);
        if (s.on_cell_drag.begin) l.on_cell_drag(s.on_cell_drag);
        if (s.on_cell_context_menu) l.on_cell_context_menu(s.on_cell_context_menu);
        bind(s.bind, w);
    }
    static void apply(const spec::menu_button& s, widget& w)
    {
        auto& b = static_cast<menu_button&>(w);
        if (s.on_menu) b.on_menu(s.on_menu);
        bind(s.bind, w);
    }
    static void apply(const spec::progress_bar& s, widget& w) { bind(s.bind, w); }
    static void apply(const spec::date_picker& s, widget& w)
    {
        auto& d = static_cast<date_picker&>(w);
        if (s.on_changed) d.on_changed(s.on_changed);
        bind(s.bind, w);
    }
    static void apply(const spec::canvas& s, widget& w)
    {
        auto& c = static_cast<canvas&>(w);
        if (s.on_paint) c.on_paint(s.on_paint);
        if (s.on_resize) c.on_resize(s.on_resize);
        if (s.on_pointer) c.on_pointer(s.on_pointer);
        if (s.on_scroll) c.on_scroll(s.on_scroll);
        if (s.on_key) c.on_key(s.on_key);
        bind(s.bind, w);
    }
    // Containers with no widget interface of their own.
    static void apply(const auto&, widget&) {}
};

}  // namespace

void register_builtin_backend(std::string name, create_backend_fn* create)
{
    builtins()[std::move(name)] = create;
}

detection detect(const load_options& opts)
{
    detection d;

    std::string forced = !opts.backend.empty() ? opts.backend : env("RAPPORT_BACKEND");
    if (!forced.empty())
    {
        d.preference = {lower(forced)};
        return d;
    }

#if defined(_WIN32)
    d.preference = {"win32", "qt6"};
#elif defined(__APPLE__)
    d.preference = {"cocoa", "qt6"};
#else
    // XDG_CURRENT_DESKTOP is a colon-separated list, most specific first
    // (e.g. "ubuntu:GNOME"). Older sessions only set the others.
    std::vector<std::string> names = split(env("XDG_CURRENT_DESKTOP"), ':');
    for (const char* var : {"XDG_SESSION_DESKTOP", "DESKTOP_SESSION"})
        if (auto v = env(var); !v.empty()) names.push_back(v);
    if (!env("KDE_FULL_SESSION").empty()) names.push_back("KDE");
    if (!env("GNOME_DESKTOP_SESSION_ID").empty()) names.push_back("GNOME");

    // The desktop's own toolkit first. After it, GTK 3 before GTK 4: it's the
    // toolkit most likely to be installed anywhere (Firefox, LibreOffice and
    // GIMP bring it), and KDE's Breeze themes it, where GTK 4 often isn't
    // installed on a Qt desktop and ignores its theme.
    for (auto& n : names)
    {
        auto l = lower(n);
        if (is_qt_desktop(l))
        {
            d.desktop = n;
            d.preference = {"qt6", "gtk3", "gtk4"};
            break;
        }
        if (is_gtk_desktop(l))
        {
            d.desktop = n;
            d.preference = {"gtk4", "gtk3", "qt6"};
            break;
        }
    }
    if (d.preference.empty())
    {
        // Unknown desktop or bare window manager: no native look to match,
        // so the toolkit most likely to be there.
        d.desktop = names.empty() ? "" : names.front();
        d.preference = {"gtk3", "qt6", "gtk4"};
    }
#endif
    return d;
}

std::expected<std::unique_ptr<ui>, std::string> ui::load(int& argc, char** argv,
                                                         const load_options& opts)
{
    auto det = detect(opts);
    auto dirs = plugin_dirs(opts);
    std::string errors;

    for (auto& name : det.preference)
    {
        if (auto it = builtins().find(name); it != builtins().end())
        {
            if (rap::backend* b = it->second(abi_version, &argc, argv))
                return std::unique_ptr<ui>(new ui(std::unique_ptr<rap::backend>(b)));
            errors += "  " + name + ": built in, but failed to start (no display?)\n";
            continue;
        }
        std::string file = "rapport_" + name + plugin_suffix;
        bool found = false;
        for (auto& dir : dirs)
        {
            auto path = dir / file;
            std::error_code ec;
            if (!fs::exists(path, ec)) continue;
            found = true;

            auto lib = sys::library::open(path);
            if (!lib)
            {
                errors += "  " + name + ": " + lib.error() + "\n";
                break;
            }
            auto* create = lib->function<create_backend_fn>(create_backend_symbol(name).c_str());
            if (!create)
            {
                errors += "  " + name + ": " + sys::path_to_utf8(path) + " is not a rapport backend\n";
                break;
            }
            // Never unload: toolkits register atexit handlers and TLS.
            lib->release();
            if (rap::backend* b = create(abi_version, &argc, argv))
                return std::unique_ptr<ui>(new ui(std::unique_ptr<rap::backend>(b)));
            errors += "  " + name + ": failed to start (ABI mismatch or no display)\n";
            break;
        }
        if (!found) errors += "  " + name + ": " + file + " not found\n";
    }

    std::string desktop = det.desktop.empty() ? "unknown" : det.desktop;
    return std::unexpected("rapport: no usable UI backend (desktop: " + desktop + ")\n" + errors);
}

ui::~ui() = default;

std::unique_ptr<window> ui::create(const window_spec& spec, window* parent)
{
    static applier sink;  // stateless; backends may keep it (build_sink)
    return backend_->create_window(spec, parent, sink);
}

}  // namespace rap
