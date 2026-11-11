// Where a mouse side button's press goes, for a backend that can't ask the
// toolkit (gtk4: a window's controllers aren't told an event's target). A
// canvas has the side buttons first, so the window has to know whether the
// press is going to one. The toolkit's rule for a pointer press:
//  - No button held: to what's under the pointer, which then has the
//    pointer (an implicit grab) until every button is let go.
//  - A button held (a drag): to whatever has that grab, wherever the
//    pointer is now, over another widget or out of the window.
// The ordinary buttons held come from the event's modifier state; the side
// buttons have no bits there, so this keeps them itself. A side button
// pressed again without its release (lost to another window) starts afresh.
// No toolkit here, so tests/side_buttons_test.cpp can check it.
#pragma once

namespace rap::side_buttons
{

// The side buttons as the toolkits number them: 8 back, 9 forward.
inline unsigned bit(int button)
{
    return button == 8 ? 1u : button == 9 ? 2u : 0u;
}

class router
{
public:
    // A press of `button`, with `held` saying whether an ordinary button
    // was already down: whether it goes to a canvas. `under()` says whether
    // what's under the pointer is in one; asked only when nothing is held.
    template <class F>
    bool press(int button, bool held, F&& under)
    {
        const unsigned b = bit(button);
        if (!held && !(side_held_ & ~b)) in_canvas_ = under();
        side_held_ |= b;
        return in_canvas_;
    }
    void release(int button) { side_held_ &= ~bit(button); }

private:
    unsigned side_held_ = 0;
    bool in_canvas_ = false;  // where the pointer's grab is, or was last
};

}  // namespace rap::side_buttons
