// libadwaita for the GTK 4 backend, loaded at run time (never linked):
// GNOME's stylesheet, metrics, colour scheme and accent, adopted where
// they're the norm (look_options::adwaita).
#pragma once

#include "gtk_common.h"

#include <dlfcn.h>

#include <cstdlib>
#include <string_view>

namespace rap::gtk4::adw
{

// The few entry points used, as adw's headers declare them.
struct api
{
    void (*init)() = nullptr;
    GObject* (*style_manager_get_default)() = nullptr;
    void (*style_manager_set_color_scheme)(GObject*, int scheme) = nullptr;
    guint (*get_major_version)() = nullptr;
    guint (*get_minor_version)() = nullptr;
    // From 1.5 (AdwAlertDialog) and before (AdwMessageDialog), for
    // message boxes.
    void* alert_dialog_new = nullptr;
};

// AdwColorScheme.
inline constexpr int color_scheme_default = 0, color_scheme_force_light = 1,
                     color_scheme_force_dark = 4;

// The API once adopted; nullptr when the app has plain GTK.
inline const api*& adopted()
{
    static const api* a = nullptr;
    return a;
}

// Whether libadwaita is the norm here: GTK's own theme, chosen nowhere
// else (GtkSettings, GTK_THEME), so a user's Breeze or Mint-Y is kept.
inline bool desktop_wants_adwaita()
{
    if (const char* env = std::getenv("GTK_THEME"); env && *env)
    {
        const std::string_view t = env;
        if (!t.starts_with("Adwaita") && !t.starts_with("Default")) return false;
    }
    return gtkcommon::default_gtk_theme();
}

// Calls adw_init when `look` and the desktop want libadwaita and it's
// installed, after gtk_init. RAPPORT_THEME=dark|light goes through the
// style manager (libadwaita owns the colour scheme, and warns about GTK's
// own setting). True when adopted.
inline bool adopt(const look_options& look)
{
    look_options::adwaita_mode mode = look.adwaita;
    if (const char* env = std::getenv("RAPPORT_ADWAITA"))
    {
        const std::string_view v = env;
        if (v == "off" || v == "0") mode = look_options::adwaita_mode::off;
        if (v == "auto" || v == "1") mode = look_options::adwaita_mode::auto_;
    }
    if (mode == look_options::adwaita_mode::off || !desktop_wants_adwaita()) return false;
    void* lib = dlopen("libadwaita-1.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) return false;
    static api a;
    auto sym = [lib](const char* name) { return dlsym(lib, name); };
    a.init = reinterpret_cast<void (*)()>(sym("adw_init"));
    a.style_manager_get_default =
        reinterpret_cast<GObject* (*)()>(sym("adw_style_manager_get_default"));
    a.style_manager_set_color_scheme =
        reinterpret_cast<void (*)(GObject*, int)>(sym("adw_style_manager_set_color_scheme"));
    a.get_major_version = reinterpret_cast<guint (*)()>(sym("adw_get_major_version"));
    a.get_minor_version = reinterpret_cast<guint (*)()>(sym("adw_get_minor_version"));
    a.alert_dialog_new = sym("adw_alert_dialog_new");
    if (!a.init || !a.style_manager_get_default || !a.style_manager_set_color_scheme) return false;
    a.init();
    GObject* manager = a.style_manager_get_default();
    if (const int forced = palette::forced_scheme())
        a.style_manager_set_color_scheme(manager, forced == 1 ? color_scheme_force_dark
                                                              : color_scheme_force_light);
    // Dark, high contrast and the accent (1.6) are the style manager's,
    // which writes them into the stylesheet's named colours (accent_bg_color
    // and accent_fg_color, the desktop's accent and its text): each change
    // resolves the roles again.
    g_signal_connect(
        manager, "notify",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer) { gtkcommon::theme_changed(); }), nullptr);
    adopted() = &a;
    return true;
}

}  // namespace rap::gtk4::adw
