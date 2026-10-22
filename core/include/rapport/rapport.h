// rapport: native widgets from whichever toolkit the desktop uses, picked at
// runtime.
//
//     auto ui = rap::Ui::load(argc, argv);
//     if (!ui) { fprintf(stderr, "%s\n", ui.error().c_str()); return 1; }
//     auto win = ui->create(rap::layout::Window{"Hello", {...}});
//     win->show();
//     return ui->run();
#pragma once

#include "backend.h"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace rap
{

struct LoadOptions
{
    // Force a backend ("qt6", "gtk4", "gtk3", "null"). Empty = detect. The
    // RAPPORT_BACKEND environment variable overrides detection too.
    std::string backend;
    // Extra plugin directories, searched first. RAPPORT_PLUGIN_PATH
    // (colon-separated) is searched next, then next to the executable,
    // then the install directory.
    std::vector<std::string> searchPaths;
};

struct Detection
{
    std::string desktop;                  // e.g. "KDE", "GNOME", "" if unknown
    std::vector<std::string> preference;  // backends to try, in order
};

// What the running desktop is and which backends suit it. Exposed for
// diagnostics (`--version`-style output, bug reports).
Detection detect(const LoadOptions& = {});

// A backend linked into the program (RAPPORT_STATIC builds, e.g. one
// self-contained .exe). Builtins are tried before plugins of the same name.
// Use the macro below, at namespace scope in one of the app's own source
// files: a registration inside a static library would be dropped by the
// linker as unreferenced.
void registerBuiltinBackend(std::string name, CreateBackendFn* create);

class Ui
{
public:
    static std::expected<std::unique_ptr<Ui>, std::string> load(int& argc, char** argv,
                                                                const LoadOptions& = {});

    ~Ui();

    std::string_view backendName() const { return backend_->name(); }
    Backend& backend() { return *backend_; }

    std::unique_ptr<Window> create(const WindowSpec&, Window* parent = nullptr);

    int run() { return backend_->run(); }
    void quit(int code = 0) { backend_->quit(code); }
    void post(std::function<void()> f) { backend_->post(std::move(f)); }

    void message(Window* parent, const MessageSpec& m, std::function<void(DialogRole)> done = {})
    {
        backend_->message(parent, m, std::move(done));
    }
    void chooseFiles(Window* parent, const FileDialogSpec& f,
                     std::function<void(std::vector<std::string>)> done)
    {
        backend_->chooseFiles(parent, f, std::move(done));
    }
    void setClipboard(const std::string& s) { backend_->setClipboard(s); }
    void setClipboardImage(int width, int height, PixelFormat format, const uint8_t* pixels,
                           size_t stride)
    {
        backend_->setClipboardImage(width, height, format, pixels, stride);
    }

    std::unique_ptr<Image> createImage(int width, int height, PixelFormat format,
                                       const uint8_t* pixels, size_t stride)
    {
        return backend_->createImage(width, height, format, pixels, stride);
    }
    [[nodiscard]] Subscription timer(int ms, bool repeat, std::function<void()> fn)
    {
        return backend_->timer(ms, repeat, std::move(fn));
    }

private:
    explicit Ui(std::unique_ptr<Backend> b)
        : backend_(std::move(b))
    {
    }
    std::unique_ptr<Backend> backend_;
};

}  // namespace rap

// RAPPORT_BUILTIN_BACKEND(gtk3) registers the statically linked gtk3 backend.
#define RAPPORT_BUILTIN_BACKEND(name)                                                              \
    extern "C" rap::Backend* rapport_create_backend_##name(int, int*, char**);                     \
    static const bool rapport_builtin_backend_##name =                                             \
        (rap::registerBuiltinBackend(#name, &rapport_create_backend_##name), true)
