// Interface implemented by toolkit backend plugins (rapport_qt.so, ...).
//
// Apps don't use this directly; see rapport.h. Backends and the core are
// built by the same compiler against the same headers - this is a C++
// interface, not a stable C ABI. Bump abi_version on any change here or
// in widgets.h / layout.h.
#pragma once

#include "layout.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rap
{

inline constexpr int abi_version = 25;

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
    // builders may be kept and called (on the UI thread) when a page is
    // first shown; the Nodes they return go through the sink the same way.
    virtual std::unique_ptr<window> create_window(const window_spec&, window* parent, build_sink&) = 0;

    virtual int run() = 0;  // main loop; returns the quit() code
    virtual void quit(int code) = 0;
    virtual void post(std::function<void()>) = 0;  // thread-safe

    // Native dialogs (xdg-desktop-portal where available). Async.
    virtual void message(window* parent, const message_spec&, std::function<void(dialog_role)>) = 0;
    virtual void choose_files(window* parent, const file_dialog_spec&,
                             std::function<void(std::vector<std::string>)>) = 0;
    virtual void set_clipboard(const std::string&) = 0;
    // An image on the clipboard (copied now; the caller keeps its pixels).
    virtual void set_clipboard_image(int width, int height, pixel_format, const uint8_t* pixels,
                                   size_t stride) = 0;

    // Pixels for painter::image; `pixels` is copied (or uploaded).
    virtual std::unique_ptr<image> create_image(int width, int height, pixel_format,
                                               const uint8_t* pixels, size_t stride) = 0;
    // Calls `fn` after `ms` milliseconds, then every `ms` if `repeat`, on
    // the UI thread, until the subscription is dropped.
    [[nodiscard]] virtual subscription timer(int ms, bool repeat, std::function<void()> fn) = 0;
};

// Backend entry point. Returns nullptr if the ABI doesn't match or the
// toolkit can't start (e.g. no display), so the loader can fall back.
//
// Each backend exports it as rapport_create_backend_<name> (see
// RAPPORT_BACKEND_ENTRY), so several can be linked into one program: as
// plugins they are dlopened by name, and statically linked ones are
// registered with RAPPORT_BUILTIN_BACKEND (rapport.h).
using create_backend_fn = backend*(int abi, int* argc, char** argv);
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
#define RAPPORT_BACKEND_ENTRY(name, ...)                                                             \
    extern "C" RAPPORT_EXPORT rap::backend* rapport_create_backend_##name(int abi, int* argc,        \
                                                                        char** argv)               \
    {                                                                                              \
        if (abi != rap::abi_version) return nullptr;                                             \
        return __VA_ARGS__::create(argc, argv);                                                    \
    }
