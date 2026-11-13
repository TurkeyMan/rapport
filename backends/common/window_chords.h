// Which keys a window's on_key has while an editable text field has the
// focus. Typing is the field's; so are Escape's and the function keys'
// routes (each backend's own). A shortcut chord - Ctrl or Alt held - is the
// window's first, as an accelerator is, unless it's one of the field's
// editing keys: Ctrl+N opens a window from an address bar, as Explorer's
// does, while Ctrl+V still pastes there. A chord the window doesn't take
// goes on to the field unchanged (and from there, Alt+letter, to the menu
// bar's access keys).
//   The editing keys: Ctrl+A, C, V, X, Z, Y; Ctrl+Left, Right, Home, End,
//   Backspace, Delete; each of those with Shift too; Shift+Insert,
//   Shift+Delete and Ctrl+Insert.
// Ctrl+Alt with a character is typing: it's AltGr on Windows (as the
// document tabs have it), where it makes @, { and the like.
// No toolkit here, so tests/window_chords_test.cpp can check it.
#pragma once

#include <rapport/widgets.h>

namespace rap::window_chords
{

inline bool editing(const key_event& e)
{
    const unsigned m = e.modifiers & (mod_shift | mod_ctrl | mod_alt | mod_meta);
    if (m == mod_shift) return e.key == key::insert || e.key == key::delete_;
    if (m == mod_ctrl && e.key == key::insert) return true;
    if ((m & ~unsigned(mod_shift)) != mod_ctrl) return false;
    switch (e.key)
    {
    case key::left:
    case key::right:
    case key::home:
    case key::end:
    case key::backspace:
    case key::delete_: return true;
    case key::character:
        switch (e.codepoint)
        {
        case U'a':
        case U'c':
        case U'v':
        case U'x':
        case U'z':
        case U'y': return true;
        default: return false;
        }
    default: return false;
    }
}

// A press the window's on_key has before the text field: a chord, not one
// of the editing keys. Modifiers alone (key::other) never are: plain Alt
// stays the menu bar's. Releases stay the field's.
inline bool to_window(const key_event& e)
{
    if (!e.press || e.key == key::other) return false;
    if (!(e.modifiers & (mod_ctrl | mod_alt))) return false;
    if ((e.modifiers & mod_ctrl) && (e.modifiers & mod_alt) && e.key == key::character) return false;
    return !editing(e);
}

}  // namespace rap::window_chords
