// Windows' own icons for the freedesktop names it has an equivalent of: the
// drives' (drive-harddisk, drive-removable-media, drive-optical,
// folder-remote) and the user's folders' (user-home, user-desktop,
// folder-download, folder-documents, folder-pictures, folder-music,
// folder-videos). Other names, "-symbolic" ones included, have none here and
// keep being drawn as before.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rap::win32
{

// The icon for `name` at exactly `px` square, as premultiplied BGRA pixels,
// rows top-down; null when Windows has none by that name. Taken from where
// the shell keeps the icon, at that size (not a scaled small or large one),
// on first use, then kept for the process: callers don't free it.
const std::vector<uint32_t>* shell_icon_pixels(const std::string& name, int px);

}  // namespace rap::win32
