# Working on anyui

anyui is shared by two applications, developed in parallel:

- **pengit** (`../pengit`): a TortoiseGit-style git GUI. Owns the core
  layout design, `List`, `TextView`, and the gtk4 and qt6 backends.
- **IfritView** (`../IfritView`): an image viewer. Owns `Canvas`, images
  in `Painter`, `Slider`/`Spin`/`ColorButton`/`Toolbar`, timers, window
  sizing and fullscreen, image clipboard, the static-backend build option,
  and the win32 backend.

Rules:

- Work on a branch named for the app (`ifritview/canvas`, `pengit/qt6`);
  `main` only takes finished, building work.
- Before changing `backend.h`, `widgets.h` or `layout.h`, or bumping
  `kAbiVersion`, tell the other side what changes; both apps break on it.
- A backend that lacks a new feature gets a stub, which its owner fills.
- Style: the Allman `.clang-format` at the root.

## Building

    cmake -S . -B build -G Ninja && cmake --build build
    ./build/bin/anyui-gallery

Backends whose toolkit isn't installed are skipped with a message.
