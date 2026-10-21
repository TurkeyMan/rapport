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

inline constexpr int kAbiVersion = 1;

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
};

// Plugin entry point. Returns nullptr if the ABI doesn't match or the
// toolkit can't start (e.g. no display), so the loader can fall back.
using CreateBackendFn = Backend*(int abi, int* argc, char** argv);
inline constexpr const char* kCreateBackendSymbol = "anyui_create_backend";

}  // namespace anyui

#define ANYUI_BACKEND_ENTRY(...)                                                                   \
    extern "C" __attribute__((visibility("default"))) anyui::Backend* anyui_create_backend(        \
        int abi, int* argc, char** argv)                                                           \
    {                                                                                              \
        if (abi != anyui::kAbiVersion) return nullptr;                                             \
        return __VA_ARGS__::create(argc, argv);                                                    \
    }
