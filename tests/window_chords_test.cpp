// Which keys a window's on_key has while a text field has the focus
// (backends/common/window_chords.h), the contract each backend keeps; then
// the headless backend's window taking a handler that says whether it took
// the key.
#include "window_chords.h"

#include <rapport/rapport.h>

#include <cstdio>
#include <string>

#ifdef RAPPORT_TEST_BUILTIN_NULL
RAPPORT_BUILTIN_BACKEND(null);
#endif

namespace
{

int fails = 0;

void expect(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}

rap::key_event chord(char32_t c, unsigned mods)
{
    rap::key_event e;
    e.key = rap::key::character;
    e.codepoint = c;
    e.modifiers = mods;
    return e;
}
rap::key_event chord(rap::key k, unsigned mods)
{
    rap::key_event e;
    e.key = k;
    e.modifiers = mods;
    return e;
}

constexpr unsigned ctrl = rap::mod_ctrl, shift = rap::mod_shift, alt = rap::mod_alt;

void contract()
{
    using rap::window_chords::to_window;
    expect(to_window(chord(U'n', ctrl)), "Ctrl+N: the window's");
    expect(to_window(chord(U',', ctrl)), "Ctrl+,: the window's");
    expect(to_window(chord(U'n', ctrl | shift)), "Ctrl+Shift+N: the window's");
    expect(to_window(chord(U'd', alt)), "Alt+D: the window's");
    expect(to_window(chord(rap::key::left, alt)), "Alt+Left: the window's");
    expect(to_window(chord(rap::key::enter, ctrl)), "Ctrl+Enter: the window's");
    expect(to_window(chord(rap::key::up, ctrl)), "Ctrl+Up: the window's");
    for (char32_t c : {U'a', U'c', U'v', U'x', U'z', U'y'})
    {
        const std::string name(1, char(c - U'a' + U'A'));
        expect(!to_window(chord(c, ctrl)), "Ctrl+" + name + ": the field's");
        expect(!to_window(chord(c, ctrl | shift)), "Ctrl+Shift+" + name + ": the field's");
    }
    for (rap::key k : {rap::key::left, rap::key::right, rap::key::home, rap::key::end, rap::key::backspace,
                       rap::key::delete_})
    {
        expect(!to_window(chord(k, ctrl)), "Ctrl+" + std::to_string(int(k)) + " (word, line): the field's");
        expect(!to_window(chord(k, ctrl | shift)), "Ctrl+Shift+" + std::to_string(int(k)) + ": the field's");
    }
    expect(!to_window(chord(rap::key::insert, ctrl)), "Ctrl+Insert: the field's");
    expect(to_window(chord(rap::key::insert, ctrl | shift)), "Ctrl+Shift+Insert: not an editing key");
    expect(!to_window(chord(rap::key::insert, shift)), "Shift+Insert: the field's");
    expect(!to_window(chord(rap::key::delete_, shift)), "Shift+Delete: the field's");
    expect(rap::window_chords::editing(chord(rap::key::insert, shift)), "Shift+Insert is an editing key");
    expect(to_window(chord(U'a', alt)), "Alt+A: the window's (only Ctrl+A is the field's)");
    expect(to_window(chord(U'v', ctrl | alt | shift)) == false, "Ctrl+Alt+Shift+V: typing (AltGr)");
    expect(!to_window(chord(U'q', ctrl | alt)), "Ctrl+Alt+Q: typing (AltGr+Q is @ on German keyboards)");
    expect(to_window(chord(rap::key::left, ctrl | alt)), "Ctrl+Alt+Left (no character): the window's");
    expect(!to_window(chord(U'n', 0)), "N alone: typing");
    expect(!to_window(chord(U'n', shift)), "Shift+N: typing");
    expect(!to_window(chord(U'n', rap::mod_meta)), "Super+N: not a chord here (the desktop's)");
    expect(!to_window(chord(rap::key::other, alt)), "Alt alone (no key of its own): the menu bar's");
    expect(!to_window(chord(rap::key::other, ctrl)), "Ctrl alone: nothing");
    rap::key_event up = chord(U'n', ctrl);
    up.press = false;
    expect(!to_window(up), "Ctrl+N's release: the field's");
    rap::key_event repeat = chord(U'n', ctrl);
    repeat.repeat = true;
    expect(to_window(repeat), "Ctrl+N held (a repeat): the window's, as each press");
}

}  // namespace

int main(int argc, char** argv)
{
    contract();

    // The headless backend: a window's on_key takes a handler that says
    // whether it took the key (it has no keys to give it).
    rap::load_options opts;
    opts.backend = "null";
    auto ui = rap::ui::load(argc, argv, opts);
    expect(bool(ui), "the null backend loads");
    if (ui)
    {
        using namespace rap::layout;
        rap::entry* field = nullptr;
        auto win = (*ui)->create(window{"Chords", {vbox{entry{"hello"}.bind(field)}}});
        win->on_key([](const rap::key_event& e) { return rap::window_chords::to_window(e) && e.codepoint == U'n'; });
        expect(field != nullptr, "and makes the window, its handler set");
    }
    std::printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
