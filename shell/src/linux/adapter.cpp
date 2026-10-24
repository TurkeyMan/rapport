#include "adapter.h"

#include <dlfcn.h>

#include <system_error>

namespace rap::shell::adapter
{

namespace
{

// No separator first, last or twice in a row, at any depth.
std::vector<menu_item> tidy(std::vector<menu_item> items, bool separators)
{
    std::vector<menu_item> out;
    for (menu_item& item : items)
    {
        if (item.is_separator())
        {
            if (separators && !out.empty() && !out.back().is_separator()) out.push_back(item);
            continue;
        }
        item.submenu = tidy(std::move(item.submenu), separators);
        out.push_back(std::move(item));
    }
    if (!out.empty() && out.back().is_separator()) out.pop_back();
    return out;
}

// '&' marks the mnemonic and "&&" is a literal '&'; `mark` replaces the
// mnemonic's '&' (0 drops it), and `escape` is written for a literal `mark`.
std::string relabel(std::string_view label, char mark, std::string_view escape)
{
    std::string out;
    for (size_t i = 0; i < label.size(); ++i)
    {
        const char c = label[i];
        if (c == '&')
        {
            if (i + 1 < label.size() && label[i + 1] == '&')
            {
                out += '&';
                ++i;
            }
            else if (mark)
                out += mark;
        }
        else if (mark && c == mark)
            out += escape;
        else
            out += c;
    }
    return out;
}

}  // namespace

std::shared_ptr<request> ask(selection sel, std::vector<menu_item>& items, bool separators) noexcept
{
    items.clear();
    try
    {
        auto r = std::make_shared<request>(request{make_provider(), std::move(sel)});
        if (!r->provider) return nullptr;
        items = tidy(r->provider->menu(r->sel), separators);
        if (!items.empty()) return r;
    }
    catch (...)
    {
    }
    items.clear();
    return nullptr;
}

void choose(const request& r, std::string_view id) noexcept
{
    try
    {
        r.provider->invoke(id, r.sel);
    }
    catch (...)
    {
    }
}

std::string gtk_label(std::string_view label)
{
    return relabel(label, '_', "__");
}

std::string plain_label(std::string_view label)
{
    return relabel(label, 0, {});
}

std::string icon_spec(const std::string& icon)
{
    if (icon.empty()) return {};
    // A name with no directory or extension is the icon theme's, even where
    // a file of that name happens to sit in the resource directory.
    if (icon.find_first_of("/.") == std::string::npos) return icon;
    const std::filesystem::path p = resource_dir() / icon;
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) ? p.string() : std::string();
}

// Looked up in the file manager's process rather than linked: the adapters
// build against SDKs that don't all bring GTK's headers, and must not pull a
// second GTK in.
bool shift_held() noexcept
{
    constexpr unsigned shift_mask = 1;  // GDK_SHIFT_MASK, in GTK 3 and 4
    auto sym = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
    using display_fn = void* (*)();
    auto display_default = reinterpret_cast<display_fn>(sym("gdk_display_get_default"));
    void* display = display_default ? display_default() : nullptr;
    if (!display) return false;
    // GTK 4: the default seat's keyboard.
    using seat_fn = void* (*)(void*);
    using state_fn = unsigned (*)(void*);
    auto seat = reinterpret_cast<seat_fn>(sym("gdk_display_get_default_seat"));
    auto keyboard = reinterpret_cast<seat_fn>(sym("gdk_seat_get_keyboard"));
    auto device_state = reinterpret_cast<state_fn>(sym("gdk_device_get_modifier_state"));
    if (seat && keyboard && device_state)
        if (void* s = seat(display))
            if (void* k = keyboard(s)) return device_state(k) & shift_mask;
    // GTK 3: the keymap.
    auto keymap = reinterpret_cast<seat_fn>(sym("gdk_keymap_get_for_display"));
    auto keymap_state = reinterpret_cast<state_fn>(sym("gdk_keymap_get_modifier_state"));
    if (keymap && keymap_state)
        if (void* k = keymap(display)) return keymap_state(k) & shift_mask;
    return false;
}

}  // namespace rap::shell::adapter
