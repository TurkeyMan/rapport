# Contributing to rapport

Thanks for your interest! rapport grows with the applications built on it,
and a change that makes one of them better is welcome:

- [Tortilla](https://github.com/TurkeyMan/tortilla), a TortoiseGit-style git client
- [IfritView](https://github.com/TurkeyMan/ifritview), an image viewer

Issues and pull requests are open. For anything larger than a fix, open an
issue first so we can agree on the shape before you build it.

## Building

```sh
cmake -S . -B build -G Ninja && cmake --build build
build/bin/rapport-gallery
```

Each backend builds when its toolkit's development files are installed, and
is skipped with a message otherwise. Before sending a change, build it both
ways and run the examples on every backend you can:

```sh
# backends as plugins (the default)
RAPPORT_BACKEND=gtk3 build/bin/rapport-gallery

# everything linked into one executable
cmake -S . -B build-static -G Ninja -DRAPPORT_STATIC=ON && cmake --build build-static
```

`RAPPORT_SNAPSHOT=out.png` saves the first window a program shows and quits,
which makes checking a layout on each backend quick.

## Design

- Every widget is the platform's own. A feature a toolkit can't do natively
  is better left out than imitated.
- Responsiveness is a feature: anything on the path of a scroll, a drag or a
  keystroke must keep pace with the display. Measure it.
- A new API is added to every backend at once; a backend that can't support
  it yet gets a stub that compiles and does nothing harmful.
- Text rapport draws itself (a canvas, an owner-drawn control, drawn chrome
  such as Windows 11's caption tabs) is ClearType unless the background is
  composed by DWM; grayscale everywhere when the system has ClearType off.
  ClearType blends each colour channel with what's beneath, so it needs an
  opaque background rapport has drawn: over Mica, through a translucent fill
  too (WinUI's layers, as the caption tabs' current tab on the toolbar band),
  or anything else the compositor fills in later, text is grayscale (a
  native control's too, over the extended frame: the toolbar band's give
  theirs a grayscale font). Use the
  monitor's rendering parameters (its ClearType level, gamma and pixel
  order, RGB or BGR) and the system's font smoothing setting, and make them
  again when the settings, the monitor or the DPI change.
- Changing a public header (`widgets.h`, `layout.h`, `backend.h`,
  `rapport.h`) bumps `abi_version` in `backend.h`, so mismatched plugins
  refuse to load.

## Code style

- The C++ standard library's naming: `snake_case` for types, functions,
  variables and enumerators; private members end in `_`; template
  parameters are `PascalCase`; macros are `RAPPORT_UPPER_CASE`. Where a
  member shares a type's name, qualify the type (`rap::color color() const`).
- Allman braces, four-space indents, 100 columns: run `clang-format` with the
  repository's `.clang-format` on the lines you change.
- Comments say why; the code says what.

## Licence

rapport is MIT licensed (see [LICENSE](LICENSE)); by contributing you agree
that your contributions are licensed under the same terms.
