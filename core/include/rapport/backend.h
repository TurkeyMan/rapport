// Interface implemented by toolkit backend plugins (rapport_qt.so, ...).
//
// Apps don't use this directly; see rapport.h. Backends and the core are
// built by the same compiler against the same headers - this is a C++
// interface, not a stable C ABI. Bump abi_version when what this header or
// widgets.h / layout.h compiles to changes: a type, member, virtual, signature,
// default argument, constant or inline function body. Comment-only
// changes don't bump it.
#pragma once

#include "layout.h"
#include "sys.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rap
{

// 88: spec::list and spec::tree's row_height, look_options::row_height.
// 89: tabs::set_page_visible and page_visible; spec::tabs visible and
//     hide_single_tab.
// 90: tabs::set_page_icon and set_page_title; spec::tabs icons.
// 91: look_options::content_surface.
// 92: tabs::set_hide_single_tab and hide_single_tab.
// 93: spec::label focusable; a link that's ellipsized, or only an icon.
inline constexpr int abi_version = 93;

// For backends' tabs (rap::tabs::set_page_visible): the page made current
// when page `from` is hidden, the nearest visible after it, else before it;
// -1 with none visible. `visible` is per page; past its end, visible.
inline int nearest_visible_page(const std::vector<bool>& visible, int pages, int from)
{
    auto shown = [&](int i) { return size_t(i) >= visible.size() || visible[size_t(i)]; };
    for (int i = from + 1; i < pages; ++i)
        if (shown(i)) return i;
    for (int i = std::min(from, pages) - 1; i >= 0; --i)
        if (shown(i)) return i;
    return -1;
}
// The page current at the start: the first visible, or -1.
inline int first_visible_page(const std::vector<bool>& visible, int pages)
{
    return nearest_visible_page(visible, pages, -1);
}
// Whether the strip shows: with hide_single_tab, only for two pages or more.
inline bool tabs_strip_shown(const std::vector<bool>& visible, int pages, bool hide_single_tab)
{
    if (!hide_single_tab) return true;
    int n = 0;
    for (int i = 0; i < pages && n < 2; ++i) n += size_t(i) >= visible.size() || visible[size_t(i)];
    return n >= 2;
}

// Light or dark: as the desktop has it (system), or the app's own choice
// (an Appearance setting). RAPPORT_THEME=dark|light overrides both, for
// snapshots and tests.
enum class color_scheme
{
    system,
    light,
    dark,
};

// Windows: rapport's own controls (entries, buttons, menu buttons,
// drop-downs, combo boxes, tool buttons) in WinUI's look, as Windows 11's
// apps have them (fluent), or Win32's own (native). automatic is fluent on
// Windows 11 (build 22000 and later, checked as it runs) and native before
// it; fluent before Windows 11 is native too (its controls never had
// WinUI's look). High contrast is always native. GTK and Qt ignore it.
enum class control_style
{
    automatic,
    native,
    fluent,
};

// Windows: what a base surface window's content (window_spec::base_surface,
// below its toolbar band if it has one) sits on. layer is WinUI's layer,
// as WinUI's apps put their controls on one, never on bare white:
// LayerFillColorDefault over WinUI's base background, #F9F9F9 in light and
// #282828 in dark. Controls' grounds and tab pages follow it; lists, trees,
// text views and canvases keep their own backgrounds. plain is the base
// colour, Win32's (COLOR_WINDOW; Explorer's dark #191919). automatic
// follows the control style: layer where controls are fluent, plain where
// they're native. High contrast is always the system colours. GTK and Qt
// ignore it.
enum class content_surface
{
    automatic,
    layer,
    plain,
};

// WinUI's densities, where controls are drawn in its look: standard (a text
// box 32 px tall, Windows 11 Explorer's address row 46) or compact (24, a
// row 36, the text the message font's size, for a dense window).
// RAPPORT_DENSITY=compact overrides it, a development aid as RAPPORT_THEME
// is. GTK and Qt ignore it.
enum class density
{
    standard,
    compact,
};

// For list::set_column_order: whether `order` names each of `n` columns once.
inline bool is_column_order(const std::vector<size_t>& order, size_t n)
{
    if (order.size() != n) return false;
    std::vector<bool> seen(n);
    for (size_t i : order)
    {
        if (i >= n || seen[i]) return false;
        seen[i] = true;
    }
    return true;
}

struct message_spec
{
    enum class icon
    {
        info,
        warning,
        error,
        question
    };
    icon icon = icon::info;
    std::string title, text, detail;
    std::vector<dialog_role> buttons{dialog_role::ok};
};

struct file_dialog_spec
{
    enum class mode
    {
        open,
        open_many,
        save,
        folder
    };
    mode mode = mode::open;
    std::string title, initial_path;
    struct filter  // {"Patches", "*.patch;*.diff"}
    {
        std::string name;
        std::string pattern;  // one or more globs, separated by ';'
    };
    std::vector<filter> filters;
};

// A document to print, through the platform's own print dialog (printer,
// copies, pages, paper; its own preview where it has one).
//
// The painter in draw_page is the screen's kind: its unit is the point
// (1/72 inch), device_scale() is the printer's dots per inch over 72, and
// theme colours resolve to print_color(). Images from ui::create_image
// draw in it. draw_page is called only for the pages being printed, as
// many times as the copies need where the printer doesn't make them.
enum class page_orientation
{
    automatic,  // the platform's default
    portrait,
    landscape,
};
struct print_job
{
    std::string title;  // the document's name in the print queue
    int pages = 1;
    page_orientation orientation = page_orientation::automatic;  // the dialog's starting choice
    // Once the printer and paper are chosen, before any page: the area the
    // pages may draw in, after the paper's margins (points), and the page
    // count for that size, or -1 to keep `pages`. Optional.
    std::function<int(size_f printable)> on_begin;
    // Page `page` (from 0), its printable area's top-left at (0, 0).
    std::function<void(painter&, int page, size_f printable)> draw_page;
    // When it's spooled, cancelled (printed false, no error) or failed.
    std::function<void(bool printed, std::string error)> done;
};

// Backend reports each widget it creates so the core can apply callbacks,
// binds and common state. The sink outlives every window, so a backend may
// keep it to build content later (lazy tab pages).
class build_sink
{
public:
    virtual ~build_sink() = default;
    virtual void created(const node&, widget&) = 0;
    virtual void created(const spec::dialog_button&, button&) = 0;
    // A window's document tabs (window_spec::tabs), once made with its items.
    virtual void created(const spec::document_tabs&, document_tabs&) = 0;
};

class backend
{
public:
    virtual ~backend() = default;

    virtual std::string_view name() const = 0;

    // Builds the whole tree, calling sink.created() for every Node that
    // maps to a widget (not Spacer/Separator/Form/Box) and every dialog
    // button. Containers (Box, Form, Group, Toolbar) with a common.handle
    // also get created(), with a plain Widget for the container.
    // The spec is only valid during the call, except that a Tabs' lazy page
    // builders and a deferred Box's builder may be kept and called (on the
    // UI thread) later; the Nodes they return go through the sink the same way,
    // as do the pages given to a Stack's add().
    virtual std::unique_ptr<window> create_window(const window_spec&, window* parent, build_sink&) = 0;

    virtual int run() = 0;  // main loop; returns the quit() code
    virtual void quit(int code) = 0;
    // Light, dark, or the desktop's again, live in every window; before the
    // first window is shown, so it never shows in the other one.
    virtual void set_color_scheme(color_scheme) = 0;
    // Runs the function on the UI thread, later. Callable from any thread
    // while the backend is alive: the caller keeps it alive for the whole
    // call, and destroying the backend while another thread is inside post()
    // is the caller's bug. (Rapport's own producers, its background workers,
    // hold a shared handle that outlives it safely.) One that can't be
    // delivered (the thread's queue full, or the backend being torn down) is
    // destroyed without being run, on the calling thread, usually a worker:
    // so captures that own UI objects (a widget's shared_ptr) need the
    // backend alive, and posted from the UI thread. Torn down, the backend
    // destroys every post still queued, on the UI thread, without running
    // it. Implemented on Win32 and the null backend; GTK's idle sources and
    // Qt's queued calls not yet checked (#108).
    virtual void post(std::function<void()>) = 0;

    // Native dialogs (xdg-desktop-portal where available). Async.
    virtual void message(window* parent, const message_spec&, std::function<void(dialog_role)>) = 0;
    virtual void choose_files(window* parent, const file_dialog_spec&,
                             std::function<void(std::vector<std::string>)>) = 0;
    // Shows the print dialog over `parent` and prints the job; async, ends
    // with job.done. With RAPPORT_PRINT_TO set (a .png, or a .pdf where the
    // toolkit writes PDF), prints there without a dialog, for tests.
    virtual void print(window* parent, print_job job) = 0;
    virtual void set_clipboard(const std::string&) = 0;
    // The clipboard's text, or nullopt when it holds none; `done` runs on
    // the UI thread, possibly before this returns.
    virtual void read_clipboard_text(std::function<void(std::optional<std::string>)> done) = 0;
    // An image on the clipboard (copied now; the caller keeps its pixels).
    virtual void set_clipboard_image(int width, int height, pixel_format, const uint8_t* pixels,
                                   size_t stride) = 0;
    // The clipboard's image, or nullopt when it holds none; `done` runs on
    // the UI thread, possibly before this returns.
    virtual void
    read_clipboard_image(std::function<void(std::optional<sys::decoded_image>)> done) = 0;
    // A window as a PNG, as drawn now: `w`, or with none the one shown most
    // recently that's still there (a dialog over the main window, say),
    // failing any the oldest there; false if there's none or it couldn't be
    // written. For tests.
    virtual bool save_snapshot(window* w, const std::string& png_path) = 0;

    // Pixels for painter::image; `pixels` is copied (or uploaded).
    virtual std::unique_ptr<image> create_image(int width, int height, pixel_format,
                                               const uint8_t* pixels, size_t stride) = 0;
    // Calls `fn` after `ms` milliseconds, then every `ms` if `repeat`, on
    // the UI thread, until the subscription is dropped.
    [[nodiscard]] virtual subscription timer(int ms, bool repeat, std::function<void()> fn) = 0;
    // The pointer over all the app's windows: the platform's working-in-the-
    // background shape (an arrow with a spinner), or as it was; with each
    // window's own (window::begin_busy), either shows it.
    virtual void set_busy(bool) = 0;
};

// How far an app takes on its desktop's own look beyond the toolkit's
// (load_options::look). Each backend takes what applies to it.
struct look_options
{
    // GTK 4: libadwaita, GNOME's own stylesheet, metrics, colour scheme and
    // accent, as GNOME's apps have them. auto adopts it where it's the
    // norm: GTK's own theme in use (no other chosen in GTK's settings or
    // GTK_THEME) and libadwaita installed (loaded at run time, never
    // linked). off keeps plain GTK, whatever is installed, so an app's
    // metrics never change under it. RAPPORT_ADWAITA=off|auto overrides.
    enum class adwaita_mode
    {
        auto_,
        off,
    };
    adwaita_mode adwaita = adwaita_mode::auto_;
    // The scheme from the start, as ui::set_color_scheme sets it later.
    rap::color_scheme color_scheme = color_scheme::system;
    // GNOME's idioms (its Human Interface Guidelines), where libadwaita is
    // adopted; each opt-in, nothing changes without it. RAPPORT_HIG=1 turns
    // all three on, for a look at an app that hasn't.
    //  header_bars: dialogs' title and buttons in a header bar, Cancel at its
    //    start and the affirmative (suggested) at its end, in place of the
    //    row of buttons; a window says for itself (window_spec::header_bar).
    //  boxed_lists: a group whose form rows all map cleanly (text, entries,
    //    drop-downs, check boxes) as a boxed list under its title, a row
    //    per field; any other group, whole, stays a grid.
    //  alert_dialogs: message boxes as libadwaita's alert dialog (1.5).
    bool header_bars = false;
    bool boxed_lists = false;
    bool alert_dialogs = false;
    // Windows: its controls in WinUI's look or Win32's (control_style), and
    // at which of WinUI's densities.
    control_style controls = control_style::automatic;
    rap::density density = rap::density::standard;
    // Every list's, tree's and sidebar's (tabs::sidebar) rows this tall, in
    // logical px (at 96 DPI and a 100% text size), instead of the
    // platform's or the density's: scaled with the DPI and the text size,
    // never shorter than the font's line and 2 px. A list's or a tree's own
    // row_height wins over it; tab strips keep the density's. 0: unset, the
    // platform's rows, as ever. Every backend, with the platforms' own
    // limits: on Windows a sidebar's rows (a list box's) are 255 device px
    // at most, that cap winning over the scaling and the line-and-2 least
    // (at extreme DPI and text sizes a row can be shorter than its line and
    // 2), and a list's never under its font's line and 4 (the list view's
    // own least); on Qt the text size enters only through the line-and-2
    // least (Qt has no text size apart from the app's font).
    float row_height = 0;
    // Windows: a base surface window's content on WinUI's layer or the
    // plain base colour (content_surface). Last, so an initialiser that
    // lists the members before it still compiles.
    rap::content_surface content_surface = rap::content_surface::automatic;
};

// Backend entry point. Returns nullptr if the ABI doesn't match or the
// toolkit can't start (e.g. no display), so the loader can fall back.
//
// Each backend exports it as rapport_create_backend_<name> (see
// RAPPORT_BACKEND_ENTRY), so several can be linked into one program: as
// plugins they are dlopened by name, and statically linked ones are
// registered with RAPPORT_BUILTIN_BACKEND (rapport.h).
using create_backend_fn = backend*(int abi, int* argc, char** argv, const look_options& look);
inline std::string create_backend_symbol(std::string_view name)
{
    return "rapport_create_backend_" + std::string(name);
}

}  // namespace rap

#if defined(_WIN32)
#define RAPPORT_EXPORT __declspec(dllexport)
#else
#define RAPPORT_EXPORT __attribute__((visibility("default")))
#endif

// In the backend: RAPPORT_BACKEND_ENTRY(gtk3, rap::gtk3::gtk3_backend)
#define RAPPORT_BACKEND_ENTRY(name, ...)                                                           \
    extern "C" RAPPORT_EXPORT rap::backend* rapport_create_backend_##name(                         \
        int abi, int* argc, char** argv, const rap::look_options& look)                            \
    {                                                                                              \
        if (abi != rap::abi_version) return nullptr;                                               \
        return __VA_ARGS__::create(argc, argv, look);                                              \
    }
