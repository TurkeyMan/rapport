// rap::i18n: translations, by gettext's .po files read as they ship (no
// gettext library, no build step). Each app has its own domain beside
// rapport's ("rapport": dialog buttons, key names), installed as
//   <folder>/<domain>.<tag>.po      tortilla.de.po, rapport.pt-BR.po
//
// Start-up, before the ui is created:
//   rap::i18n::set_languages(saved.empty() ? rap::sys::ui_languages()
//                                          : std::vector<std::string>{saved});
//   rap::i18n::load("rapport", dir);
//   rap::i18n::load("myapp", dir);
// then look strings up anywhere, from any thread (they're read-only once
// loaded). An app wraps tr() in a short macro of its own for xgettext:
//   #define _(s) rap::i18n::tr("myapp", s)
//   label{_("Commit")}
//   rap::i18n::format(rap::i18n::tr_n("myapp", "{0} file", "{0} files", n), n)
// English is the source language: it needs no file, and choosing it (or
// preferring it to any language there's a file for) gives the strings as
// written.
#pragma once

#include "rapport/widgets.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rap::i18n
{

// The languages to use, most preferred first, as BCP 47 tags ("de-AT",
// "pt-BR", "en"): the user's override, or sys::ui_languages(). Call once,
// before load() and before the ui is created. With an override it also
// tells the platform, so text it draws follows (the toolkit's own dialogs;
// on Windows, message boxes and common dialogs where the language is
// installed) and so do child processes (LANGUAGE, which git and other
// gettext programs read).
void set_languages(std::vector<std::string> preferred, bool override = false);
const std::vector<std::string>& languages();

// Loads `domain`'s translation from `folder`: the file for the first of
// languages() there is one for, trying a tag then its language alone
// (de-AT, then de). None when English comes first. Returns the tag loaded,
// or "" for none (the strings stay as written).
std::string load(std::string_view domain, const std::filesystem::path& folder);
// The tag `domain` is in, "" for English.
std::string loaded(std::string_view domain);

// Lookups: the translation, else `text` itself. The result lives as long
// as the process (or as `text`).
const char* tr(std::string_view domain, const char* text);
// For a text that means different things in different places ("Open" a
// file, "Open" as a state).
const char* tr_ctx(std::string_view domain, std::string_view context, const char* text);
// `one` or `many` by `n` and the language's plural rule.
const char* tr_n(std::string_view domain, const char* one, const char* many, uint64_t n);

// The translations present in `folder` for `domain`, for a Settings
// choice: each tag and the language's name in itself ("Deutsch").
struct language
{
    std::string tag, name;
};
std::vector<language> available(std::string_view domain, const std::filesystem::path& folder);
// A language's name in itself, or the tag when it isn't known.
std::string native_name(std::string_view tag);

// Numbered placeholders, which a translation can reorder: "{0} of {1}".
// "{{" and "}}" are literal braces; a placeholder with no argument is
// left as written.
namespace detail
{
std::string format(std::string_view text, const std::vector<std::string>& args);
template <class T>
std::string arg(const T& v)
{
    if constexpr (std::is_convertible_v<const T&, std::string_view>)
        return std::string(std::string_view(v));
    else
        return std::format("{}", v);  // shortest form: 1.5, not 1.500000
}
}  // namespace detail
template <class... Args>
std::string format(std::string_view text, const Args&... args)
{
    return detail::format(text, {detail::arg(args)...});
}

// rapport's own strings, translated: a dialog button's label, with its
// access key marked '_' ("_OK"), and a shortcut as shown ("Ctrl+Shift+S" with
// the key names in the user's language; letters and symbols as they are).
std::string stock_label(dialog_role);
std::string shortcut_label(std::string_view shortcut);

}  // namespace rap::i18n
