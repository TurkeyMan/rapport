# GTK 3 at fractional scales

GTK 3 knows only whole scales. On a Wayland display at 150% it draws every
window at 2x and the compositor scales the result down by 0.75, so text and
controls come out the right size, slightly soft. GTK 4 and Qt draw at the
true scale and are exact; on X11, GTK 3's scale (`GDK_SCALE`) is whole and
nothing is resampled, so it's exact there too.

## What rapport does

`painter::device_scale()` and `canvas::device_scale()` report the true
scale, read from the compositor's fractional-scale protocol
(`wp_fractional_scale_v1`, `preferred_scale` / 120) for each toplevel. Apps
size windows and place images 1:1 by it, so an image shown "at 100%" has
one image pixel per screen pixel in size. `painter::pixel_scale()` reports
the whole scale GTK 3 draws at, and `pixel_grid` snaps to it, so lines and
boxes stay on the buffer's pixel grid. At whole scales the two are equal and
nothing changes.

The implementation is in `backends/gtk3/gtk3_backend.cpp` (namespace
`fractional`) and `backends/gtk-common/gtk_common.h` (`fractional_scale_of`,
`device_scale_of`, the cairo painter's `pixel_scale`):

- The manager is bound once per `wl_display`. The registry roundtrip runs on
  a private `wl_event_queue`, so it dispatches none of GDK's events; the
  manager then moves to GDK's default queue, so its objects' events arrive on
  the main thread during GDK's own dispatch.
- One `wp_fractional_scale_v1` per toplevel `GdkWindow`, shared by its
  canvases. GTK 3 creates a toplevel's `wl_surface` when it maps and destroys
  it when it unmaps, so the object lives from the toplevel's `map` to its
  `unmap`, destroyed before its surface.
- A new scale is stored on the `GdkWindow` and queues a draw; a canvas
  compares its scale as it draws and calls `on_resize` before the frame that
  uses it.
- Only toplevels that contain a canvas are watched (`fractional::watch` runs
  at a canvas's realize). In other windows `device_scale_of()` stays GTK's
  whole scale, which their painted cells (lists) snap to anyway.
- It needs `wayland-protocols`, `wayland-scanner`, `wayland-client` and
  `gtk+-wayland-3.0` at build time (CMake prints "GTK 3 fractional scale:
  on/off"); without them, or on X11 and XWayland, GTK 3 reports its whole
  scale as before.

## Deferred: exact pixels for canvases

Exact 1:1 would mean a canvas drawing at the true pixel count, bypassing
GTK 3's whole-scale buffer. Considered and deferred ("maybe we consider a
resolution in the future"); this records the design so far, so a later
attempt starts from it.

### Design, as scoped

- Only canvases, only on Wayland at a fractional scale, and only when both
  `wp_fractional_scale_v1` and `wp_viewporter` are present; otherwise the
  path above.
- A sync `wl_subsurface` of the toplevel's `wl_surface`, with an empty input
  region, so input stays with GTK.
- A ring of 2 or 3 `wl_shm` buffers of its own at the true pixel size, which
  the cairo painter draws into. The viewport's destination is the canvas's
  logical size, its source the visible part.
- Position and size from the canvas's allocation, translated to toplevel
  coordinates, applied in the frame clock's layout or after-paint phase, so
  they take effect with GTK's next commit.
- Popovers and menus are `xdg_popup`s, above the subsurface. Whether any
  rapport layout puts a GTK widget over a canvas in the same window is still
  to be checked.
- Created on realize, destroyed on unrealize, re-created if the toplevel's
  surface changes (GTK 3 replaces it on every map). Any failure falls back to
  the path above.

### Why it was judged no-go at first

- GDK 3 has no public subsurface API: rapport would own `wl_shm` buffers and
  commit outside GTK's frame clock.
- A sync subsurface's state applies only with a commit of its parent, which
  GTK makes when it has something to draw; ordering our commit before GTK's
  every frame means hooking the frame clock, which is fragile.
- Stacking against GTK 3's own popover subsurfaces and menus isn't exposed.
- The subsurface's position has to follow every relayout and scroll.
- The canvas's cairo drawing moves to buffers of our own, outside GTK's
  widget drawing.
- It's a lot of fragile code for the legacy toolkit, where GTK 4 and Qt are
  already exact.

### Open questions

1. **Parent commits.** Sync mode applies only on a parent commit. When the
   canvas redraws and GTK has nothing else to draw, GTK 3 won't commit the
   parent, and the new buffer sits pending. Force a parent commit (a minimal
   `queue_draw` on the toplevel), and what does that cost per frame at
   165 Hz? Or desync for content-only updates, sync only for geometry
   changes?
2. **Frame pacing.** `wl_surface.frame` callbacks on the subsurface against
   GTK's frame clock: no double pacing, no starvation.
3. **Coordinates.** The subsurface position is relative to the `wl_surface`
   origin, which includes GTK 3's client-side shadow and margins, not the
   `GdkWindow`'s content origin.
4. **Under the canvas.** What GTK paints beneath it in the parent buffer
   (the window background, to avoid flashes on resize), and an opaque region
   on the subsurface.
5. **Resizing.** A new buffer size on an allocation change without a
   stretched frame, and buffer release handling for the ring.
6. **Hidden canvases.** An unmapped tab page, or a canvas scrolled out of
   view, must unmap the subsurface (attach a null buffer).
7. **Test plan.** A one-pixel checker exact at 1.25, 1.5 and 1.75 (1.667 on a
   3840x2160 output, which has no 1.75); a live scale change; a resize drag;
   Tortilla's welcome mascot; a canvas in a scrolled view, if any.

No answers are settled yet. Partly answered from the phase above: (3) the
toplevel's `wl_surface` is replaced on every map, so the subsurface must be
re-created there as well as at realize; and `preferred_scale` already
arrives on GDK's default queue, which is where a subsurface's frame
callbacks would be dispatched too.
