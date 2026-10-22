# Working on rapport

rapport is shared by two applications, developed in parallel:

- **pengit** (`../pengit`): a TortoiseGit-style git GUI. Owns the core
  layout design, `List`, `TextView`, and the gtk4 and qt6 backends.
- **IfritView** (`../IfritView`): an image viewer. Owns `Canvas`, images
  in `Painter`, `Slider`/`Spin`/`ColorButton`/`Toolbar`, timers, window
  sizing and fullscreen, image clipboard, the static-backend build option,
  and the win32 backend.

## Where to work

`/mnt/wsl/home/manu/anyui` (the directory keeps the old name for now) holds
`main` and nobody edits in it. Each app's session works in its own worktree on
its own branch:

    git -C ../anyui worktree add ../anyui-ifritview ifritview/<topic>
    git -C ../anyui worktree add ../anyui-pengit pengit/<topic>

## Merging

1. Rebase your branch onto `main`; it must build (plugin and
   `-DRAPPORT_STATIC=ON`) and the examples must run.
2. Propose it to the other side: what changed, and the diff of any shared
   header (`backend.h`, `widgets.h`, `layout.h`, `rapport.h`), plus any
   `kAbiVersion` bump.
3. Merge only once they agree: fast-forward `main` to your branch
   (`git -C ../anyui merge --ff-only <branch>`).
4. Tell them it's in; they rebase their branch onto the new `main`.

## Rules

- A backend that lacks a new feature gets a stub, which its owner fills.
- Style: the Allman `.clang-format` at the root.

## Building

    cmake -S . -B build -G Ninja && cmake --build build
    ./build/bin/rapport-gallery

    # one self-contained executable per example, backends linked in
    cmake -S . -B build-static -G Ninja -DRAPPORT_STATIC=ON && cmake --build build-static

Backends whose toolkit isn't installed are skipped with a message.

## Licence

rapport is MIT licensed (see LICENSE): contributions are made under the same terms.
