#include "anyui/anyui.h"
#include "anyui/sys.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#ifndef ANYUI_INSTALL_PLUGIN_DIR
#define ANYUI_INSTALL_PLUGIN_DIR ""
#endif

namespace fs = std::filesystem;

namespace anyui
{
namespace
{

std::string env(const char* name)
{
    return sys::env(name).value_or("");
}

#ifdef _WIN32
constexpr char kPathSeparator = ';';
constexpr const char* kPluginSuffix = ".dll";
#else
constexpr char kPathSeparator = ':';
constexpr const char* kPluginSuffix = ".so";
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
bool isQtDesktop(const std::string& d)
{
    static const char* qt[] = {"kde", "plasma", "lxqt", "deepin", "dde", "ukui", "lomiri"};
    return std::ranges::any_of(qt, [&](const char* q) { return d == q; });
}

bool isGtkDesktop(const std::string& d)
{
    static const char* gtk[] = {
        "gnome",  "unity", "xfce",   "x-cinnamon",      "cinnamon", "mate",   "pantheon",
        "budgie", "lxde",  "cosmic", "gnome-flashback", "phosh",    "ubuntu", "pop"};
    return std::ranges::any_of(gtk, [&](const char* g) { return d == g; });
}

fs::path exeDir()
{
    return sys::executablePath().parent_path();
}

std::vector<fs::path> pluginDirs(const LoadOptions& opts)
{
    std::vector<fs::path> dirs;
    for (auto& p : opts.searchPaths) dirs.push_back(sys::pathFromUtf8(p));
    for (auto& p : split(env("ANYUI_PLUGIN_PATH"), kPathSeparator))
        dirs.push_back(sys::pathFromUtf8(p));
    if (auto d = exeDir(); !d.empty())
    {
        dirs.push_back(d);
        dirs.push_back(d / "anyui");
        dirs.push_back(d / ".." / "lib" / "anyui");
    }
    if (*ANYUI_INSTALL_PLUGIN_DIR) dirs.emplace_back(ANYUI_INSTALL_PLUGIN_DIR);
    return dirs;
}

// Applies everything the backend leaves to the core: callbacks, binds,
// list models and common state.
class Applier : public BuildSink
{
public:
    void created(const Node& n, Widget& w) override
    {
        if (!n.common.enabled) w.setEnabled(false);
        if (!n.common.tooltip.empty()) w.setTooltip(n.common.tooltip);
        std::visit([&](const auto& s) { apply(s, w); }, n.spec);
    }

    void created(const spec::DialogButton& b, Button& w) override
    {
        if (b.onClick) w.onClick(b.onClick);
        if (b.bind) *b.bind = &w;
    }

private:
    template <class T>
    static void bind(T** handle, Widget& w)
    {
        if (handle) *handle = &static_cast<T&>(w);
    }

    static void apply(const spec::Splitter& s, Widget& w) { bind(s.bind, w); }
    static void apply(const spec::Tabs& s, Widget& w)
    {
        auto& t = static_cast<Tabs&>(w);
        if (s.onChanged) t.onChanged(s.onChanged);
        bind(s.bind, w);
    }
    static void apply(const spec::Label& s, Widget& w) { bind(s.bind, w); }
    static void apply(const spec::Button& s, Widget& w)
    {
        auto& b = static_cast<Button&>(w);
        if (s.onClick) b.onClick(s.onClick);
        bind(s.bind, w);
    }
    static void apply(const spec::Check& s, Widget& w)
    {
        auto& c = static_cast<Check&>(w);
        if (s.onToggled) c.onToggled(s.onToggled);
        bind(s.bind, w);
    }
    static void apply(const spec::Entry& s, Widget& w)
    {
        auto& e = static_cast<Entry&>(w);
        if (s.onChanged) e.onChanged(s.onChanged);
        if (s.onActivate) e.onActivate(s.onActivate);
        bind(s.bind, w);
    }
    static void apply(const spec::Select& s, Widget& w)
    {
        auto& c = static_cast<Select&>(w);
        if (s.onChanged) c.onChanged(s.onChanged);
        bind(s.bind, w);
    }
    static void apply(const spec::Combo& s, Widget& w)
    {
        auto& c = static_cast<Combo&>(w);
        if (s.onChanged) c.onChanged(s.onChanged);
        if (s.onActivate) c.onActivate(s.onActivate);
        bind(s.bind, w);
    }
    static void apply(const spec::TextView& s, Widget& w)
    {
        auto& t = static_cast<TextView&>(w);
        if (s.onChanged) t.onChanged(s.onChanged);
        bind(s.bind, w);
    }
    static void apply(const spec::List& s, Widget& w)
    {
        auto& l = static_cast<List&>(w);
        if (s.model) l.setModel(s.model);
        if (s.onSelectionChanged) l.onSelectionChanged(s.onSelectionChanged);
        if (s.onActivate) l.onActivate(s.onActivate);
        if (s.onSortRequested) l.onSortRequested(s.onSortRequested);
        if (s.onContextMenu) l.onContextMenu(s.onContextMenu);
        bind(s.bind, w);
    }
    // Containers with no widget interface of their own.
    static void apply(const auto&, Widget&) {}
};

}  // namespace

Detection detect(const LoadOptions& opts)
{
    Detection d;

    std::string forced = !opts.backend.empty() ? opts.backend : env("ANYUI_BACKEND");
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

    for (auto& n : names)
    {
        auto l = lower(n);
        if (isQtDesktop(l))
        {
            d.desktop = n;
            d.preference = {"qt6", "gtk4", "gtk3"};
            break;
        }
        if (isGtkDesktop(l))
        {
            d.desktop = n;
            d.preference = {"gtk4", "gtk3", "qt6"};
            break;
        }
    }
    if (d.preference.empty())
    {
        // Unknown desktop or bare window manager: no native look to match.
        d.desktop = names.empty() ? "" : names.front();
        d.preference = {"qt6", "gtk4", "gtk3"};
    }
#endif
    return d;
}

std::expected<std::unique_ptr<Ui>, std::string> Ui::load(int& argc, char** argv,
                                                         const LoadOptions& opts)
{
    auto det = detect(opts);
    auto dirs = pluginDirs(opts);
    std::string errors;

    for (auto& name : det.preference)
    {
        std::string file = "anyui_" + name + kPluginSuffix;
        bool found = false;
        for (auto& dir : dirs)
        {
            auto path = dir / file;
            std::error_code ec;
            if (!fs::exists(path, ec)) continue;
            found = true;

            auto lib = sys::Library::open(path);
            if (!lib)
            {
                errors += "  " + name + ": " + lib.error() + "\n";
                break;
            }
            auto* create = lib->function<CreateBackendFn>(kCreateBackendSymbol);
            if (!create)
            {
                errors += "  " + name + ": " + sys::pathToUtf8(path) + " is not an anyui backend\n";
                break;
            }
            // Never unload: toolkits register atexit handlers and TLS.
            lib->release();
            if (Backend* b = create(kAbiVersion, &argc, argv))
                return std::unique_ptr<Ui>(new Ui(std::unique_ptr<Backend>(b)));
            errors += "  " + name + ": failed to start (ABI mismatch or no display)\n";
            break;
        }
        if (!found) errors += "  " + name + ": " + file + " not found\n";
    }

    std::string desktop = det.desktop.empty() ? "unknown" : det.desktop;
    return std::unexpected("anyui: no usable UI backend (desktop: " + desktop + ")\n" + errors);
}

Ui::~Ui() = default;

std::unique_ptr<Window> Ui::create(const WindowSpec& spec, Window* parent)
{
    Applier sink;
    return backend_->createWindow(spec, parent, sink);
}

}  // namespace anyui
