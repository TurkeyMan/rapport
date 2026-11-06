# Rapport design review

Reviewed 7 November 2026 against `3269ddc890fe1b3d790413e7233f72488fd8e56e`, backend ABI 67. Open findings reconciled against master at `19ec54b`. Finding numbers are stable for cross-reference. This is a source review of the repository at that revision. The acceptance checks below are proposed checks. Build results and timing measurements from earlier PRs belong to those revisions.

## Decisions and work queue

Rapport's native-widget model is worth keeping. The biggest design debt lies where two paths implement the same promise to an application but keep independent state. The next rendering review needs measurements of resize and pane painting in IfritView. D3D and Vulkan resource management, native widget construction, and file-manager entry points have legitimate reasons to differ.

| ID | Order | Disposition | Outcome that closes the item |
|---|---|---|---|
| F5 | With IfritView | Measured redesign | Window resizing has a bounded scope and unrelated GPU work cannot stall it |
| F6 | Alongside F5 | Measure, then simplify | Buffered panes retain coherent painting with demonstrated cost proportional to the work required |
| F7 | At next packaging change | Decide supported compatibility | Installed applications and backend plugins each have an explicit compatibility story |
| F8 | When revisiting Explorer integration | Validate before simplifying | Real host traces justify the snapshot state retained across calls |
| F9 | Opportunistic | Remove mechanical duplication | Equivalent line-control classifications have one owner |
| F12 | Before supporting catalog reload | Translation lifetime | The documented lookup-string lifetime agrees with domain replacement |
| F13 | At next watcher API change | Callback ownership | Clients can determine where it is safe to destroy a watch |
| F14 | When extending shader tooling | Generated output ownership | Independent targets can use the same shader name in a CMake directory |
| F17 | Open | Win32 container properties | Disabled and tooltipped containers work with and without a bound handle |

## F5. Window resizing coordinates three systems with a global switch

The visible Win32 resize path lays out ahead for the requested size, moves children through the larger extent of old and new sizes, then lays out for the actual window. During the intermediate pass `g_canvases_quiet` suppresses canvas work. The resize path can fence the shared GPU context with a one-second wait. The quiet-pass CPU target already needed a special case because actual child size and application notification serve different purposes. A fence can tell when GPU work completed. It cannot establish when the compositor showed that frame and the resized HWND together.

Start by recording which canvas actually submitted a frame during each resize. A window with only native controls should have no reason to wait for another window's rendering. Separate desired layout size, actual drawable size, and last completed frame. Keep any suppression scoped to the affected window and layout operation, including nested calls. Decide whether speculative rendering earns its cost using the same workload before and after. Preserve user-visible continuity during a shrink as well as a grow.

**Acceptance.** Measure ordinary resize, splitter drag, and IfritView fitting its window to the next image. Run with a static texture, then with image development active. Include two windows sharing the GPU, a CPU-only canvas, mixed grow/shrink by axis and device loss. Record UI-thread wait, paint time, present time and visible stale/blank frames separately.

Evidence: [window_impl::resize, moves_in_one_pass, g_canvases_quiet, wait_for_gpu and gpu_target](../backends/win32/win32_backend.cpp).

## F6. A scroll pane changes the cost and ownership of painting

When next changing pane presentation, decide what a rejected texture submission should do to an earlier pending frame. Today an unsupported texture (or unavailable device) returns before superseding that readback. Verify that choice alongside the formats advertised by each canvas path. [Frame-order fix](https://github.com/TurkeyMan/rapport/pull/13).

The Win32 pane allocates a compatible DC and bitmap sized to the content panel, prints the children into it and copies the result to the screen. Pane canvases can read a GPU texture back to CPU memory, copy its pixels, swap channels on the UI thread and create a D2D bitmap to participate in that print. The approach solved a real presentation problem in IfritView. Its cost depends on the content area and controls printed even when a small visible region changes. The fallback repairs child invalidations after an allocation or blit failure; preserve that behavior.

The design question is which surface needs coherent presentation. Measure the viewport and damaged region against the full content panel before introducing a cache. If the full print dominates, reuse a viewport-sized buffer or redraw only damaged content, with explicit handling for moved children, transparent group boxes, scroll offsets and DPI changes. For the ordinary GPU canvas, account separately for the peak-sized retained bitmap and the full-buffer blit per present. A 3840 × 2160 BGRA bitmap alone costs about 32 MiB per canvas.

**Acceptance.** Sweep short and tall panels, partial invalidations, nested scroll panes and repeated dragging. Report bytes copied, painted area, readback latency and time per control alongside frame times. Check a deliberately failed offscreen paint and verify that children repaint.

Evidence: [paint_pane, render_into and gpu_target](../backends/win32/win32_backend.cpp), [IfritView measurements and original comparison](https://github.com/TurkeyMan/rapport/pull/2).

## F7. Plugin compatibility and installed application compatibility

The plugin factory takes an ABI number and rejects mismatches. A private bundled core and plugins can move together. The installed library also has a C++ application-facing ABI, while the core build declares project version 0.1 and SOVERSION 0. A factory check protects the boundary at which it runs. An installed application compiled against an earlier virtual interface can already disagree with a replacement core before that check helps. This is a packaging-policy question for independently updated installations. The two pinned downstream clients have a simpler deployment model.

Decide which upgrade combinations Rapport supports. If applications must rebuild with the core, make that a package dependency or private-install rule. If independent upgrades are supported, tie the appropriate library version to breaking interface changes. Avoid designing a new C ABI unless independently versioned binary plugins become a requirement.

**Acceptance.** Install an old application, core and plugin, then vary one component at a time in an isolated install tree. Every supported combination should run. Each unsupported combination should be rejected at installation, load or startup with an identifiable cause.

Evidence: [factory and ABI number](../core/include/rapport/backend.h), [loader](../core/src/rapport.cpp), [version and install rules](../core/CMakeLists.txt).

## F8. Explorer state reconstructs an invocation across host calls

The provider API takes an ordinary selection. The Windows 11 adapter must distinguish a folder-background click from selecting the same folder, and can receive separate fast and slow state calls. Its bookkeeping now includes site generations, item-path keys, ambiguity counts, poisoned snapshots and permanent fallback after overflow. The tests explain why each guard exists. Removing one locally risks directing an action at another folder.

The simplification opportunity is upstream of the bookkeeping: establish what information the packaged host supplies for the actual downstream menus. Compare classic and modern Explorer calls with identical selections. If the product accepts treating an ambiguous background as its folder, it may be possible to rely on less inferred state. Retain conservative fallback and keep the COM-specific logic within the adapter until that decision is made.

**Acceptance.** Trace item, background, rapid repeated and dismissed menus in packaged Explorer. Record whether a provider ever receives the wrong folder, and separately count degraded or hidden menus. Treat wrong-target actions as correctness failures.

Evidence: [provider contract](../shell/include/rapport/shell.h), [Explorer adapter](../shell/src/win32/explorer_command.cpp), [snapshot policy](../shell/src/win32/explorer_view.h), [policy tests](../tests/explorer_view_test.cpp).

## F9. Widget builders repeat the line-control classification

The GTK 3, GTK 4 and Qt builders each classify line controls. Compare the three predicates and share the cases with identical layout semantics. Keep toolkit-specific measurements local.

**Acceptance.** Exercise single-line controls and multiline content in the gallery on all three backends. Adding a widget with shared line-control semantics should require one classification change.

Evidence: [GTK 3 builder](../backends/gtk3/gtk3_backend.cpp), [GTK 4 builder](../backends/gtk4/gtk4_backend.cpp), [Qt builder](../backends/qt6/qt6_backend.cpp).

## Coverage and boundaries to preserve

### Additional implementation traces

**F12. Catalog storage can invalidate a promised process-lifetime pointer.** The localization header describes a startup loading phase followed by concurrent read-only lookups. That is a simple and viable design. However, it also promises that a translated string returned by lookup lives as long as the process. Calling `load` a second time for the same domain erases the catalog before searching for the replacement, even if the replacement fails. A previously returned translation then points into destroyed storage. Decide whether repeat loading is supported and specify the lifetime accordingly. **Acceptance:** exercise repeated domain loads including a missing replacement and verify the chosen pointer and lookup guarantees. [Localization contract](../core/include/rapport/i18n.h) · [load and tr](../core/src/i18n.cpp)

**F13. Watch callbacks and destruction have a thread-affinity contract.** Windows and Linux deliver changes on the watch's own worker, and both destructors join it. A client that drops its last owning pointer from the callback attempts to join the current thread. The public comment describes delivery on the worker and advises posting UI work, but does not explain this destruction restriction. Decide first whether self-destruction must be supported. Document owner-thread disposal if sufficient; supporting self-disposal requires worker state that can finish after the public handle goes away. Simply detaching the thread in the destructor would leave its state lifetime unresolved. **Acceptance:** cover ordinary stop during idle I/O and specify or exercise disposal initiated by a callback. [directory_watch interface](../core/include/rapport/sys.h) · [dispatcher and platform implementations](../core/src/watch.cpp)

**F14. Shader outputs are scoped to the build directory while the helper takes a target.** Two targets declared in the same CMake directory can each reasonably request a shader called `render`, with different source or defines. The helper emits both under the same `shaders/render.h` and intermediate binary path. The target argument does not distinguish those producers. Put generated files under a target-specific directory while keeping includes usable, or explicitly require directory-wide unique names and diagnose conflicting declarations. **Acceptance:** build two targets with identically named shaders and different defines in one directory, then edit one source and confirm only its consumer changes. [rapport_add_shader](../core/cmake/RapportShaders.cmake)

**F17. Win32 plain containers ignore enabled state and tooltips even with a handle.** The Win32 `plain_widget` represents a layout item. Visibility is implemented by changing the item and requesting layout, while `set_enabled` and `set_tooltip` have empty implementations. The owner identified IfritView's overlay form as a downstream example. Decide where a container with no native window applies inherited disabled state and where tooltip hit testing belongs. Preserve a child's own disabled state when re-enabling its parent, including children constructed on first show. **Acceptance:** test initial properties on bound and unbound containers, toggle a bound parent around an individually disabled child, and check tooltip behavior over the form and its children. [Win32 implementation at the merged revision](https://github.com/TurkeyMan/rapport/blob/5933b874a0b89bbee33614db5efdd3bfecee3438/backends/win32/win32_backend.cpp#L1031)

### Cross-checks on the remaining subsystem boundaries

- **Vulkan synchronization and resource lifetime.** Traced texture readback through layout transitions, transfer to host-coherent staging, queue submission, retained texture ownership, worker copy and ticket signaling. A staging readback map failure permanently marks that device lost. The caller can retire the completed operation and must recreate the device before using it again. [Readback contract](https://github.com/TurkeyMan/rapport/pull/12). Image barriers and staging ownership belong in the native driver. Driver-loss and barrier behavior across hardware remain runtime checks.
- **Native input and accessibility.** Compared the same entry-action surface through GTK 3 entry icons, GTK 4 buttons with explicit accessible state, Qt actions and Win32 owner-drawn button children. A populated callback alone cannot establish keyboard reachability. In particular, the Win32 edit button's creation style omits `WS_TABSTOP`. Verify keyboard and assistive-technology activation in real composite controls before promising cross-backend parity. Keep adapter-specific implementation and test the shared interaction contract.
- **File-manager integration.** Traced selection construction and menu invocation through Nautilus, Caja, Nemo and Thunar to the common GObject menu helper and shared Linux request object. Dolphin adapts its native selection separately. Classic Explorer constructs selection from shell data and the modern adapter needs F8's extra correlation. The small repeated host entry points preserve SDK signatures; consolidating them further offers less than maintaining a common provider-level selection/activation fixture.
- **Image, secrets and optional platform services.** The Windows image path checks dimensions before output allocation and scopes COM resources. The pixbuf path checks dimensions during streaming and again before copying rows. Its size-prepared callback marks oversized data refused; chunk boundaries alone do not prove a bound on allocations inside the decoder. Verify process peak memory separately if decoding hostile input is a product requirement. Secret lookup copies returned text before releasing the library-owned password. Network status caches a system-bus lookup, including a null first result, for process lifetime. A failed first connection therefore prevents subsequent status recovery through that path; test late bus availability if consumers require live recovery. These lifetimes differ enough to warrant service-specific rules.
- **Packaging and clients.** Distribution builds consume the installed core. In-tree client builds take a different route. Tortilla's shell integration and IfritView's GPU presentation remain the relevant acceptance workloads for F5, F6 and F8. The client codebases have their own reviewers and revisions.

These are representative implementation traces across the repository. Per-control screen-reader behavior, exhaustive driver synchronization, cross-version binary compatibility, and native performance measurements remain separate verification work.

| Area swept | Assessment at the reviewed revision |
|---|---|
| Public layout, widget and backend APIs | Preserve declarative specs, live widget handles and native construction. Focus changes on Win32 container properties (F17). |
| Core detection, loading and static registration | Desktop preference, plugin search and built-in registration address different deployment needs. Keep mismatch handling and toolkit lifetime rules explicit. See F7. |
| GTK 3, GTK 4, Qt and Null | Native input, focus and accessibility justify toolkit-specific code. Shared list-press policy and GTK common text/splitter code are precedents for extracting decisions with independent tests. Null is a contract harness with limited ability to validate native behavior. |
| Win32 layout, controls, painter and theme | Resize, child batching, printing and presentation are the highest-coupling areas (F5, F6). Preserve per-DPI measurements, high-contrast handling and runtime OS gates when rearranging them. |
| GPU API, D3D11, Vulkan and shader tools | Review boundary was device creation, texture ownership, backend consumption and build integration. Keep platform synchronization and shader compilation specific to the device. The clearest duplication lies above the drivers, in frame replacement. Driver-level command and barrier correctness needs a separate workload audit. |
| Themes, icons and printing | Shared palette resolution makes fixed, semantic and scheme colors testable. Toolkit translation and printer overrides belong near the drawing context. Arithmetic tests cannot establish theme contrast on a real desktop. |
| System services | Directory watching already separates shared coalescing and dispatch from OS readers. Image decoding, credential storage, GIO and network detection use platform facilities and optional libraries. No common service framework is justified by that fact alone. |
| Localization | The catalog reader and plural evaluator give the core a self-contained translation service. Preserve fixture coverage and domain behavior before considering any library substitution. |
| Shell and file-manager adapters | Keep providers independent of toolkit and host ABI. Linux shares GObject menu/badge code where hosts are related and keeps Dolphin separate. The Windows 11 adapter is the exceptional stateful boundary (F8). |
| Build, packaging, CI and examples | Preserve standalone backend builds, optional toolkit discovery, static/plugin modes and the Windows import audit. Examples exercise real workflows. Reusable pass/fail contracts belong in the CTest registry. |
| Downstream clients | Use IfritView for image presentation, GPU setup and pane resize. Use Tortilla for native list interaction, forge HTTP credentials and packaged Explorer invocation. Review the actual client pin for each change. |
