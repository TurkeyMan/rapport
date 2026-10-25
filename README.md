# rapport

Rapport is a portable platform toolkit abstraction; making desktop apps feel native everywhere.

Write your application once, in modern C++, and rapport presents it through
each platform's own toolkit: Win32 on Windows, and GTK or Qt on Linux, chosen
to match the desktop it runs on. Your users get the real thing: native
controls, menus, dialogs, keyboard handling and accessibility, looking and
behaving exactly as they expect. You get one clean API.

```cpp
#include <rapport/rapport.h>

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv);  // the toolkit for this desktop
    if (!ui) return 1;

    rap::label* greeting = nullptr;
    using namespace rap::layout;
    auto win = (*ui)->create(dialog{"Hello", {
        vbox{
            label{"What's your name?"},
            entry{}.placeholder("Name").on_changed(
                [&](const std::string& name) { greeting->set_text("Hello, " + name + "!"); }),
            label{}.bind(greeting),
            buttons{ok, cancel},
        }}});
    win->on_finished([&](rap::dialog_role) { (*ui)->quit(); });
    win->show();
    return (*ui)->run();
}
```

## Principles

- **Lightning fast.** No web runtime, no garbage collector, no layers of
  emulation: just your code and the platform's own toolkit. Apps start in an
  instant, stay small, and every scroll, drag and keystroke keeps pace with
  the display, at 60 Hz or 240 Hz and beyond. Responsiveness isn't an
  afterthought; it's the point.
- **Native, never imitation.** rapport drives each platform's real widgets.
  No web views, no custom-drawn lookalikes.
- **Declarative.** You describe what the interface is; rapport takes care of
  each platform's conventions for spacing, alignment and dialog layout.
- **More than UI.** The platform services a desktop app needs, behind the
  same portable API.
- **Light.** Plain C++ and CMake, with nothing to ship beyond the toolkit the
  platform already has.

## Known limitations

- GTK 3 on Wayland at fractional scales draws at the next whole scale and
  the compositor scales it down: the right size, slightly soft. GTK 4 and Qt
  are exact. See [docs/gtk3-fractional-scale.md](docs/gtk3-fractional-scale.md).
- GTK 4 lays widgets out in logical pixels, so at a fractional scale one can
  start part way into a device pixel. rapport moves canvas, list cell and
  icon painting onto whole pixels, but GTK clips each cell at its own
  edges: where two painted rows meet inside a pixel, that pixel row is
  blended (a faint seam in a line running down a column). Rows partly
  scrolled out of view can stay off the grid until they next draw.

## Licence

MIT: see [LICENSE](LICENSE).
