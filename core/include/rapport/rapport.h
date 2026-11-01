// rapport: native widgets from whichever toolkit the desktop uses, picked at
// runtime.
//
//     auto ui = rap::ui::load(argc, argv);
//     if (!ui) { fprintf(stderr, "%s\n", ui.error().c_str()); return 1; }
//     auto win = ui->create(rap::layout::window{"Hello", {...}});
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

struct load_options
{
    // Force a backend ("qt6", "gtk4", "gtk3", "null"). Empty = detect. The
    // RAPPORT_BACKEND environment variable overrides detection too.
    std::string backend;
    // Extra plugin directories, searched first. RAPPORT_PLUGIN_PATH
    // (colon-separated) is searched next, then next to the executable,
    // then the install directory.
    std::vector<std::string> search_paths;
};

struct detection
{
    std::string desktop;                  // e.g. "KDE", "GNOME", "" if unknown
    std::vector<std::string> preference;  // backends to try, in order
};

// What the running desktop is and which backends suit it. Exposed for
// diagnostics (`--version`-style output, bug reports).
detection detect(const load_options& = {});

// A backend linked into the program (RAPPORT_STATIC builds, e.g. one
// self-contained .exe). Builtins are tried before plugins of the same name.
// Use the macro below, at namespace scope in one of the app's own source
// files: a registration inside a static library would be dropped by the
// linker as unreferenced.
void register_builtin_backend(std::string name, create_backend_fn* create);

class ui
{
public:
    static std::expected<std::unique_ptr<ui>, std::string> load(int& argc, char** argv,
                                                                const load_options& = {});

    ~ui();

    std::string_view backend_name() const { return backend_->name(); }
    rap::backend& backend() { return *backend_; }

    std::unique_ptr<window> create(const window_spec&, window* parent = nullptr);

    int run() { return backend_->run(); }
    void quit(int code = 0) { backend_->quit(code); }
    void post(std::function<void()> f) { backend_->post(std::move(f)); }

    void message(window* parent, const message_spec& m, std::function<void(dialog_role)> done = {})
    {
        backend_->message(parent, m, std::move(done));
    }
    void choose_files(window* parent, const file_dialog_spec& f,
                     std::function<void(std::vector<std::string>)> done)
    {
        backend_->choose_files(parent, f, std::move(done));
    }
    // The platform's print dialog, then the job's pages (see print_job).
    void print(window* parent, print_job job) { backend_->print(parent, std::move(job)); }
    void set_clipboard(const std::string& s) { backend_->set_clipboard(s); }
    // The clipboard's text, or nullopt when it holds none. `done` runs on the
    // UI thread, possibly before this returns, or later: GTK reads
    // asynchronously, and under Wayland, which gives the clipboard only to
    // the focused client, the read waits for one of the app's windows to
    // have focus (up to 1.5 s).
    void clipboard_text(std::function<void(std::optional<std::string>)> done)
    {
        backend_->read_clipboard_text(std::move(done));
    }
    // The clipboard's image, or nullopt when it holds none, read as
    // clipboard_text is. Its pixels as the platform has them: straight RGBA
    // or premultiplied BGRA (ui::create_image takes either).
    void clipboard_image(std::function<void(std::optional<sys::decoded_image>)> done)
    {
        backend_->read_clipboard_image(std::move(done));
    }
    void set_clipboard_image(int width, int height, pixel_format format, const uint8_t* pixels,
                           size_t stride)
    {
        backend_->set_clipboard_image(width, height, format, pixels, stride);
    }
    bool save_snapshot(const std::string& png_path) { return backend_->save_snapshot(png_path); }

    std::unique_ptr<image> create_image(int width, int height, pixel_format format,
                                       const uint8_t* pixels, size_t stride)
    {
        return backend_->create_image(width, height, format, pixels, stride);
    }
    [[nodiscard]] subscription timer(int ms, bool repeat, std::function<void()> fn)
    {
        return backend_->timer(ms, repeat, std::move(fn));
    }

private:
    explicit ui(std::unique_ptr<rap::backend> b)
        : backend_(std::move(b))
    {
    }
    std::unique_ptr<rap::backend> backend_;
};

}  // namespace rap

// RAPPORT_BUILTIN_BACKEND(gtk3) registers the statically linked gtk3 backend.
#define RAPPORT_BUILTIN_BACKEND(name)                                                              \
    extern "C" rap::backend* rapport_create_backend_##name(int, int*, char**);                     \
    static const bool rapport_builtin_backend_##name =                                             \
        (rap::register_builtin_backend(#name, &rapport_create_backend_##name), true)
