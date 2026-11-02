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
    // AdwAlertDialog (1.5), for message boxes.
    GtkWidget* (*alert_dialog_new)(const char* heading, const char* body) = nullptr;
    void (*alert_dialog_add_response)(GtkWidget*, const char* id, const char* label) = nullptr;
    void (*alert_dialog_set_response_appearance)(GtkWidget*, const char* id,
                                                 int appearance) = nullptr;
    void (*alert_dialog_set_default_response)(GtkWidget*, const char* id) = nullptr;
    void (*alert_dialog_set_close_response)(GtkWidget*, const char* id) = nullptr;
    void (*alert_dialog_choose)(GtkWidget*, GtkWidget* parent, GCancellable*, GAsyncReadyCallback,
                                gpointer) = nullptr;
    const char* (*alert_dialog_choose_finish)(GtkWidget*, GAsyncResult*) = nullptr;
};

// AdwResponseAppearance.
inline constexpr int response_suggested = 1;

// AdwColorScheme.
inline constexpr int color_scheme_default = 0, color_scheme_force_light = 1,
                     color_scheme_force_dark = 4;

// The API once adopted; nullptr when the app has plain GTK.
inline const api*& adopted()
{
    static const api* a = nullptr;
    return a;
}

// The app's look options (from create), and whether one of GNOME's idioms
// is to be used: asked for (or RAPPORT_HIG=1) and libadwaita adopted.
inline look_options& look()
{
    static look_options l;
    return l;
}
inline bool idiom(bool look_options::* flag)
{
    if (!adopted()) return false;
    const char* env = std::getenv("RAPPORT_HIG");
    return (env && std::string_view(env) == "1") || look().*flag;
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

// The app's or RAPPORT_THEME's scheme through the style manager, else the
// desktop's (libadwaita follows it itself).
inline void apply_scheme()
{
    const api* a = adopted();
    if (!a) return;
    const int forced = palette::forced_scheme();
    a->style_manager_set_color_scheme(a->style_manager_get_default(),
                                      forced == 1   ? color_scheme_force_dark
                                      : forced == 2 ? color_scheme_force_light
                                                    : color_scheme_default);
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
    a.alert_dialog_new =
        reinterpret_cast<GtkWidget* (*)(const char*, const char*)>(sym("adw_alert_dialog_new"));
    a.alert_dialog_add_response = reinterpret_cast<void (*)(GtkWidget*, const char*, const char*)>(
        sym("adw_alert_dialog_add_response"));
    a.alert_dialog_set_response_appearance =
        reinterpret_cast<void (*)(GtkWidget*, const char*, int)>(
            sym("adw_alert_dialog_set_response_appearance"));
    a.alert_dialog_set_default_response = reinterpret_cast<void (*)(GtkWidget*, const char*)>(
        sym("adw_alert_dialog_set_default_response"));
    a.alert_dialog_set_close_response = reinterpret_cast<void (*)(GtkWidget*, const char*)>(
        sym("adw_alert_dialog_set_close_response"));
    a.alert_dialog_choose =
        reinterpret_cast<void (*)(GtkWidget*, GtkWidget*, GCancellable*, GAsyncReadyCallback,
                                  gpointer)>(sym("adw_alert_dialog_choose"));
    a.alert_dialog_choose_finish = reinterpret_cast<const char* (*)(GtkWidget*, GAsyncResult*)>(
        sym("adw_alert_dialog_choose_finish"));
    if (!a.init || !a.style_manager_get_default || !a.style_manager_set_color_scheme) return false;
    a.init();
    GObject* manager = a.style_manager_get_default();
    adopted() = &a;
    apply_scheme();
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
