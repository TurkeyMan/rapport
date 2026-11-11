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

}  // namespace rap::win32
