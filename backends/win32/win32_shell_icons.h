// Windows' own icons for the freedesktop names it has an equivalent of: the
// drives' (drive-harddisk, drive-removable-media, drive-optical,
// folder-remote) and the user's folders' (user-home, user-desktop,
// folder-download, folder-documents, folder-pictures, folder-music,
// folder-videos). Other names, "-symbolic" ones included, have none here and
// keep being drawn as before.
//
// Asking the shell is slow, so it's never done while painting: an icon is
// prepared when a control takes it (and again for a new DPI), and painting
// only reads what was prepared.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rap::win32
{

// The prepared icon for `name` at exactly `px` square, as premultiplied
// BGRA pixels, rows top-down; null when Windows has none by that name or it
// hasn't been prepared at that size. Never asks the shell: safe from paint.
const std::vector<uint32_t>* shell_icon_pixels(const std::string& name, int px);

// Asks the shell for `name` at `px` once, keeping it for the process. Taken
// from where the shell keeps the icon, at that size (not a scaled small or
// large one). Not from paint.
void prepare_shell_icon(const std::string& name, int px);

// A control that paints `name` at `logical` px: prepared now at `dpi`, and
// at each new DPI (prepare_wanted_shell_icons). Not from paint.
void want_shell_icon(const std::string& name, int logical, UINT dpi);
void prepare_wanted_shell_icons(UINT dpi);

// How many times the shell has been asked, for tests.
size_t shell_icon_extractions();

// -- a path's own icon (icon::for_path) ---------------------------------------
//
// The shell's icon for one path: a drive's (the system drive's with the
// Windows logo, a removable or optical drive's, its autorun's), a folder's
// own (desktop.ini), a program's. Finding it reads the path, which can take
// seconds (a disconnected network drive, an optical drive spinning up), so
// it's found on a background thread, never the UI thread, once per path and
// size for the process. Meanwhile, and where there's none, the icon's name is
// drawn. A path found to have none isn't asked again (not in v1): a drive
// that comes back later keeps its name's icon until the process restarts.

enum class path_icon_state
{
    pending,  // being found: draw the fallback meanwhile
    ready,    // path_icon_pixels has it
    none,     // the shell has none (no such path, or none at that size)
};

// Where `path`'s icon at exactly `px` square stands; the first ask starts
// finding it, in the background. Takes a lock and returns: never waits for
// the path or asks the shell itself, so it may be called from paint. (The
// lock is never held across a shell call: a finder takes its job out,
// unlocks, asks the shell, and locks again to keep what it found.)
path_icon_state request_path_icon(const std::string& path, int px);

// The one spelling the cache keys a path by, and listeners are told:
// "C:\" for "C:" and "C:/".
std::string path_icon_key(const std::string& path);

// Its pixels once ready (premultiplied BGRA, px square, rows top-down), else
// null. Safe from paint.
const std::vector<uint32_t>* path_icon_pixels(const std::string& path, int px);

// How a found icon is told on the UI thread: `post` is given a function to
// run there (the backend's post). Until it's set, nobody is told (the pixels
// are kept all the same). Set it on the UI thread, before the first
// request: setting it warms the shell there (see the .cpp) before any
// finder can start.
void set_path_icon_post(std::function<void(std::function<void()>)> post);

// Run on the UI thread each time an icon is found, or found to be none, with
// its path (path_icon_key's spelling) and size. Returns an id for
// remove_path_icon_listener.
size_t add_path_icon_listener(std::function<void(const std::string& path, int px)> f);
void remove_path_icon_listener(size_t id);

// A control that paints `path`'s icon at `logical` px: asked for now at
// `dpi`, and at each new DPI (prepare_wanted_shell_icons).
void want_path_icon(const std::string& path, int logical, UINT dpi);

// How many times the shell has been asked for a path's icon, for tests.
size_t path_icon_lookups();

// How many shell calls this module has made on the calling thread, for
// tests: none may be made while painting.
size_t shell_calls_on_this_thread();

}  // namespace rap::win32
