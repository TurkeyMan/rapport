// Interface implemented by toolkit backend plugins (rapport_qt.so, ...).
//
// Apps don't use this directly; see rapport.h. Backends and the core are
// built by the same compiler against the same headers - this is a C++
// interface, not a stable C ABI. Bump abi_version on any change here or
// in widgets.h / layout.h.
#pragma once

#include "layout.h"
#include "sys.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rap
{

inline constexpr int abi_version = 70;

// Light or dark: as the desktop has it (system), or the app's own choice
// (an Appearance setting). RAPPORT_THEME=dark|light overrides both, for
// snapshots and tests.
enum class color_scheme
{
    system,
    light,
    dark,
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
    // UI thread) later; the Nodes they return go through the sink the same way.
    virtual std::unique_ptr<window> create_window(const window_spec&, window* parent, build_sink&) = 0;

    virtual int run() = 0;  // main loop; returns the quit() code
    virtual void quit(int code) = 0;
    // Light, dark, or the desktop's again, live in every window; before the
    // first window is shown, so it never shows in the other one.
    virtual void set_color_scheme(color_scheme) = 0;
    virtual void post(std::function<void()>) = 0;  // thread-safe

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
    // The window shown last (a dialog over the main window, say) as a PNG, as
    // drawn now; false if there's none or it couldn't be written. For tests.
    virtual bool save_snapshot(const std::string& png_path) = 0;

    // Pixels for painter::image; `pixels` is copied (or uploaded).
    virtual std::unique_ptr<image> create_image(int width, int height, pixel_format,
                                               const uint8_t* pixels, size_t stride) = 0;
    // Calls `fn` after `ms` milliseconds, then every `ms` if `repeat`, on
    // the UI thread, until the subscription is dropped.
    [[nodiscard]] virtual subscription timer(int ms, bool repeat, std::function<void()> fn) = 0;
    // The pointer over the app's windows: the platform's working-in-the-
    // background shape (an arrow with a spinner), or as it was.
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
