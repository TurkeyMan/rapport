// Interface implemented by toolkit backend plugins (anyui_qt.so, ...).
//
// Apps don't use this directly; see anyui.h. Backends and the core are
// built by the same compiler against the same headers - this is a C++
// interface, not a stable C ABI. Bump kAbiVersion on any change here or
// in widgets.h / layout.h.
#pragma once

#include "layout.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anyui
{

inline constexpr int kAbiVersion = 4;

struct MessageSpec
{
    enum class Icon
    {
        Info,
        Warning,
        Error,
        Question
    };
    Icon icon = Icon::Info;
    std::string title, text, detail;
    std::vector<DialogRole> buttons{DialogRole::Ok};
};

struct FileDialogSpec
{
    enum class Mode
    {
        Open,
        OpenMany,
        Save,
        Folder
    };
    Mode mode = Mode::Open;
    std::string title, initialPath;
    struct Filter  // {"Patches", "*.patch"}
    {
        std::string name, pattern;
    };
    std::vector<Filter> filters;
};

// Backend reports each widget it creates so the core can apply callbacks,
// binds and common state.
class BuildSink
{
public:
    virtual ~BuildSink() = default;
    virtual void created(const Node&, Widget&) = 0;
    virtual void created(const spec::DialogButton&, Button&) = 0;
};

class Backend
{
public:
    virtual ~Backend() = default;

    virtual std::string_view name() const = 0;

    // Builds the whole tree, calling sink.created() for every Node that
    // maps to a widget (not Spacer/Separator/Form/Box) and every dialog
    // button. The spec is only valid during the call.
    virtual std::unique_ptr<Window> createWindow(const WindowSpec&, Window* parent, BuildSink&) = 0;

    virtual int run() = 0;  // main loop; returns the quit() code
    virtual void quit(int code) = 0;
    virtual void post(std::function<void()>) = 0;  // thread-safe

    // Native dialogs (xdg-desktop-portal where available). Async.
    virtual void message(Window* parent, const MessageSpec&, std::function<void(DialogRole)>) = 0;
    virtual void chooseFiles(Window* parent, const FileDialogSpec&,
                             std::function<void(std::vector<std::string>)>) = 0;
    virtual void setClipboard(const std::string&) = 0;

    // Pixels for Painter::image; `pixels` is copied (or uploaded).
    virtual std::unique_ptr<Image> createImage(int width, int height, PixelFormat,
                                               const uint8_t* pixels, size_t stride) = 0;
    // Calls `fn` after `ms` milliseconds, then every `ms` if `repeat`, on
    // the UI thread, until the subscription is dropped.
    [[nodiscard]] virtual Subscription timer(int ms, bool repeat, std::function<void()> fn) = 0;
};

// Backend entry point. Returns nullptr if the ABI doesn't match or the
// toolkit can't start (e.g. no display), so the loader can fall back.
//
// Each backend exports it as anyui_create_backend_<name> (see
// ANYUI_BACKEND_ENTRY), so several can be linked into one program: as
// plugins they are dlopened by name, and statically linked ones are
// registered with ANYUI_BUILTIN_BACKEND (anyui.h).
using CreateBackendFn = Backend*(int abi, int* argc, char** argv);
inline std::string createBackendSymbol(std::string_view name)
{
    return "anyui_create_backend_" + std::string(name);
}

}  // namespace anyui

#if defined(_WIN32)
#define ANYUI_EXPORT __declspec(dllexport)
#else
#define ANYUI_EXPORT __attribute__((visibility("default")))
#endif

// In the backend: ANYUI_BACKEND_ENTRY(gtk3, anyui::gtk3::Gtk3Backend)
#define ANYUI_BACKEND_ENTRY(name, ...)                                                             \
    extern "C" ANYUI_EXPORT anyui::Backend* anyui_create_backend_##name(int abi, int* argc,        \
                                                                        char** argv)               \
    {                                                                                              \
        if (abi != anyui::kAbiVersion) return nullptr;                                             \
        return __VA_ARGS__::create(argc, argv);                                                    \
    }
