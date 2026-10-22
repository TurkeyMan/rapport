# rapport

Rapport is a portable platform toolkit abstraction; making desktop apps feel native everywhere.

Write your application once, in modern C++, and rapport presents it through
each platform's own toolkit: Win32 on Windows, and GTK or Qt on Linux, chosen
to match the desktop it runs on. Your users get the real thing: native
controls, menus, dialogs, keyboard handling and accessibility, looking and
behaving exactly as they expect. You get one clean API.

## Principles

- **Native, never imitation.** rapport drives each platform's real widgets.
  No web views, no custom-drawn lookalikes.
- **Fast.** Built to keep pace with the display, so interaction stays
  smooth at any refresh rate.
- **Declarative.** You describe what the interface is; rapport takes care of
  each platform's conventions for spacing, alignment and dialog layout.
- **More than UI.** The platform services a desktop app needs, behind the
  same portable API.
- **Light.** Plain C++ and CMake, with nothing to ship beyond the toolkit the
  platform already has.

## Licence

MIT: see [LICENSE](LICENSE).
