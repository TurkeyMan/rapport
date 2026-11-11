#include "win32_shell_icons.h"

#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace rap::win32
{

namespace
{

struct stock_name
{
    const char* name;
    SHSTOCKICONID id;
};
constexpr stock_name stock_names[] = {
    {"drive-harddisk", SIID_DRIVEFIXED},
    {"drive-removable-media", SIID_DRIVEREMOVE},
    {"drive-optical", SIID_DRIVECD},
    {"folder-remote", SIID_DRIVENET},
};

struct known_name
{
    const char* name;
    const KNOWNFOLDERID* id;
};
const known_name known_names[] = {
    {"user-home", &FOLDERID_Profile},
    {"user-desktop", &FOLDERID_Desktop},
    {"folder-download", &FOLDERID_Downloads},
    {"folder-documents", &FOLDERID_Documents},
    {"folder-pictures", &FOLDERID_Pictures},
    {"folder-music", &FOLDERID_Music},
    {"folder-videos", &FOLDERID_Videos},
};

// The icon at `index` in `file` (a negative index is a resource id), made at
// `px` from the image nearest that size, as the shell itself does.
HICON extract(const wchar_t* file, int index, int px)
{
    HICON h = nullptr;
    if (!file || !file[0]) return nullptr;
    if (SHDefExtractIconW(file, index, 0, &h, nullptr, MAKELONG(px, 0)) != S_OK) return nullptr;
    return h;
}

HICON lookup(const std::string& name, int px)
{
    for (const stock_name& s : stock_names)
        if (name == s.name)
        {
            SHSTOCKICONINFO info{};
            info.cbSize = sizeof info;
            if (FAILED(SHGetStockIconInfo(s.id, SHGSI_ICONLOCATION, &info))) return nullptr;
            return extract(info.szPath, info.iIcon, px);
        }
    for (const known_name& k : known_names)
        if (name == k.name)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (FAILED(SHGetKnownFolderIDList(*k.id, KF_FLAG_DEFAULT, nullptr, &pidl))) return nullptr;
            SHFILEINFOW info{};
            HICON h = nullptr;
            if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof info, SHGFI_PIDL | SHGFI_ICONLOCATION))
                h = extract(info.szDisplayName, info.iIcon, px);
            CoTaskMemFree(pidl);
            return h;
        }
    return nullptr;
}

// An icon's pixels, premultiplied; its mask stands in for alpha when its
// colour has none. Empty unless it's `px` square in colour.
std::vector<uint32_t> pixels_of(HICON h, int px)
{
    ICONINFO ii{};
    if (!GetIconInfo(h, &ii)) return {};
    std::vector<uint32_t> out;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth == px && bm.bmHeight == px)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = px;
        bi.bmiHeader.biHeight = -px;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> colour(size_t(px) * size_t(px)), mask(colour.size());
        HDC dc = GetDC(nullptr);
        const bool got = GetDIBits(dc, ii.hbmColor, 0, UINT(px), colour.data(), &bi, DIB_RGB_COLORS) == px;
        bool has_alpha = false;
        for (const uint32_t c : colour) has_alpha |= (c >> 24) != 0;
        bool masked = has_alpha;
        if (got && !has_alpha && ii.hbmMask)
            masked = GetDIBits(dc, ii.hbmMask, 0, UINT(px), mask.data(), &bi, DIB_RGB_COLORS) == px;
        ReleaseDC(nullptr, dc);
        if (got && masked)
        {
            for (size_t i = 0; i < colour.size(); ++i)
            {
                uint32_t c = colour[i];
                if (!has_alpha) c = (mask[i] & 0xffffff) ? 0 : (c | 0xff000000u);
                const uint32_t a = c >> 24;
                if (a < 255)
                {
                    auto pm = [a](uint32_t v) { return (v * a + 127) / 255; };
                    c = a << 24 | pm((c >> 16) & 0xff) << 16 | pm((c >> 8) & 0xff) << 8 | pm(c & 0xff);
                }
                colour[i] = c;
            }
            out = std::move(colour);
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

}  // namespace

const std::vector<uint32_t>* shell_icon_pixels(const std::string& name, int px)
{
    if (px <= 0) return nullptr;
    static std::mutex m;
    static std::map<std::pair<std::string, int>, std::optional<std::vector<uint32_t>>> cache;
    std::lock_guard lock(m);
    auto key = std::pair{name, px};
    auto it = cache.find(key);
    if (it == cache.end())
    {
        std::optional<std::vector<uint32_t>> pixels;
        if (HICON h = lookup(name, px))
        {
            if (std::vector<uint32_t> p = pixels_of(h, px); !p.empty()) pixels = std::move(p);
            DestroyIcon(h);  // ours is the copy
        }
        it = cache.emplace(std::move(key), std::move(pixels)).first;
    }
    return it->second ? &*it->second : nullptr;
}

}  // namespace rap::win32
