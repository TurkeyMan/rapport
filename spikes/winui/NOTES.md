# WinUI 3 backend: feasibility spike

The question: should Tortilla's Windows 11 build get a WinUI 3 backend for
rapport, or stay on Win32? This directory holds a small WinUI 3 app built the
way such a backend would be, with timings and sizes from it. It isn't part of
rapport's build.

**Verdict: no-go for now.** Stay on the Win32 backend. The build side works
fine. The cost is in the backend itself: WinUI is missing the controls
rapport's lists rely on, its app model doesn't fit how rapport starts up, it
adds start-up time, and it adds 57 MB to the install. Details below.

## What was built

- `main.cpp` is all in C++ with no XAML compiler. It has a `MenuBar` with
  keyboard accelerators, two panes with a home-made splitter between them, a
  `ListView` of 100,000 rows, a Direct3D canvas in a `SwapChainPanel`, a
  `ContentDialog`, and `ui.post` done through `DispatcherQueue`.
  - The list's rows come from an `IObservableVector<IInspectable>` we
    implement, the way rapport's `list_model` works: a count, and each row
    made only when it's shown. Each row has 3 cells, from a `DataTemplate`
    loaded at run time with `XamlReader::Load`. The cells are filled in
    `ContainerContentChanging`.
- `CMakeLists.txt` uses CMake 3.28 and Ninja from VS 2022 17.x, with MSVC from
  vcvars64 and Windows SDK 10.0.22621. The NuGet packages are
  Microsoft.WindowsAppSDK 2.6.4 and Microsoft.Windows.CppWinRT 3.0.260818.1.
  `fetch_packages.ps1` fetches them into a folder outside the tree. Building
  needs no MSBuild and no vcxproj. The build does what the Windows App SDK's
  MSBuild targets would:
  - runs `cppwinrt.exe` over the SDK's and WinAppSDK's metadata, with `-base`
    so it doesn't pick up the SDK's older base.h. This writes 82 MB of headers
    once.
  - **self-contained** (`WINUI_SELF_CONTAINED=ON`, the default):
    - copies each component's `runtimes-framework/win-x64/native` beside the
      exe.
    - turns each component's `package.appxfragment` into a registration-free
      WinRT manifest that the linker merges with `app.manifest`, as
      `GenerateAppManifestFromAppxFragments` does.
    - runs `makepri` to merge the runtime's .pri files into `resources.pri`.
      **Without resources.pri, `XamlControlsResources` throws "Cannot locate
      resource from ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml".**
    - calls `WindowsAppRuntime_EnsureIsLoaded()` at start-up.
  - **framework-dependent** (`OFF`): links the bootstrapper and calls
    `MddBootstrapInitialize2` with no install prompt.
- No XAML compiler is needed. The `Application` subclass implements
  `IXamlMetadataProvider` by passing every call on to WinUI's
  `XamlControlsXamlMetaDataProvider`. Deriving from WinUI classes in C++ works
  too: `splitter_bar : GridT<>` is one, which it needs in order to set the
  protected `ProtectedCursor`.
- `hidden_run.cpp` runs a program on a desktop of its own, created with
  `CreateDesktop`, so nothing ever reaches the screen. It's what made the
  timings possible to take unattended.

How to build (from a VS 2022 x64 developer prompt, with the CMake and Ninja
that ship with VS):

    powershell -File spikes/winui/fetch_packages.ps1 -Out <packages>
    cmake -G Ninja -S spikes/winui -B <build> -DCMAKE_BUILD_TYPE=Release -DWINAPPSDK_PACKAGES=<packages>
    cmake --build <build>

Both configurations compile and link. One compile of main.cpp takes about
70 s on the loaded machine and gives a 4.4 MB .obj. A real backend would need
precompiled headers.

## Start-up time

Measured with `hidden_run winui_spike --measure out.txt --activate`, self-contained, Release, on Windows 11 22621. Each figure is milliseconds since the process was created:

| step | 1st run of a new exe | warm (4 runs) |
|---|---|---|
| main() | 511 | 27-31 |
| Application started (OnLaunched) | 640 | 52-69 |
| XamlControlsResources merged | 664 | 58-77 |
| Window created | 798 | 85-120 |
| menu and list built | 802 | 87-123 |
| Direct3D device and swap chain | 1038 | 254-289 |
| Activate | 1056 | 264-301 |
| root Loaded / first CompositionTarget.Rendering | 1168 | 320-364 |

- The ~170 ms canvas step is `D3D11CreateDevice`, which the Win32 backend
  pays too. Without it, WinUI reaches its first frame about 150-190 ms after
  the process starts, warm.
- On the 1st run of a new exe, 0.5 s goes before main(), most likely Defender
  scanning the new exe and loading ~50 MB of new DLLs.
- A true cold start (after a reboot, nothing cached) wasn't measured.
  Self-contained DLLs aren't shared with other apps, so they won't already be
  in the cache.
- On a desktop no one is looking at, DWM may not composite, so "first frame"
  here means XAML's first Rendering tick, not pixels on the screen.
- Process exit takes another ~150 ms.

**For Manu:** run the same command on the real desktop (`winui_spike
--measure t.txt --activate` exits at its first frame). Run it again after a
reboot for cold figures. For a baseline, compare with the Win32 backend
opening a similar window: time to its first WM_PAINT.

## Deployment

- **Self-contained:** 57 MB in 121 files beside the exe; 24 MB zipped.
  - WinUI: 33 MB.
  - InteractiveExperiences (composition, input, windowing): 16 MB.
  - DWriteCore: 3 MB.
  - Foundation: 3.2 MB.
  - resources.pri: 2.4 MB.
  - winmd metadata: about 2 MB.
  - Some of it could be trimmed: Phone.dll, DeploymentAgent and RestartAgent,
    the PushNotifications proxy, and 172 .mui files.
  - Nothing is installed system-wide. The installer just copies files.
- **Framework-dependent:** the exe plus `Microsoft.WindowsAppRuntime.Bootstrap.dll`
  (0.4 MB). The machine needs the Windows App Runtime 2.x:
  - the framework MSIX (50 MB), plus the Main, Singleton and DDLM packages.
    `WindowsAppRuntimeInstall-x64.exe` installs them, or the Store does.
  - On this machine `Microsoft.WindowsAppRuntime.2` 2.6.4 and `WinAppRuntime.Main.2` 2.6.4 are
    installed, but no 2.x DDLM. Bootstrapping fails with **0x80670016**,
    probably because that DDLM is missing (unconfirmed). Finishing this test
    means running the runtime installer, which wasn't done.
  - A packaged or sparse-package Tortilla could instead declare a
    `PackageDependency` on the framework in its manifest and skip the
    bootstrapper. The framework would still have to be installed (or bundled)
    before the package is registered.
  - Windows App SDK releases are supported for a short time. Either way,
    Tortilla would take on keeping up with them.

## How rapport's backend interface maps onto WinUI

- **Message loop, `run`, `quit`, `post`, `timer`.** This is the structural
  problem.
  - WinUI's normal model is `Application::Start(callback)`, which owns the
    loop. No XAML object can exist before it runs.
  - rapport apps make and show their windows before calling `ui->run()` (see
    examples/demo.cpp).
  - The fit is **XAML Islands** instead: each rapport window is a Win32 HWND
    hosting a `DesktopWindowXamlSource`. The thread is set up with
    `WindowsXamlManager::InitializeForCurrentThread`, and the loop is our own
    (pre-translating keys through the island).
  - With that, `post` goes through `DispatcherQueue::TryEnqueue`, and `timer`
    through `DispatcherQueueTimer`.
  - The spike doesn't test Islands. Still unknown there: whether WinUI's theme
    resources load without an `Application` object.
- **Windows.**
  - `Window`, or an island in an HWND, handles title, size and owner. Mica
    comes from `SystemBackdrop`.
  - Modal dialogs have to be emulated: disable the owner, and use
    `OverlappedPresenter` with `IsModal` and an owner.
  - `ContentDialog`, WinUI's message box, is drawn inside its window, and only
    one can be open per window. That's a different thing from rapport's
    `message()`. `TaskDialogIndirect`, which Win32 already uses, would stay.
- **Layout.** `box`, `form` and `group` map onto Grid and StackPanel.
  - WinUI's metrics are touch-sized: rows 32-40 px and wider padding. Fitting
    Tortilla's dense forms into rapport's `_ch`/`_lines` sizes means a
    compact resource dictionary and per-control tuning.
  - `splitter`: WinUI has none (the Community Toolkit's is C#), so it's our
    own. The spike's took about 30 lines.
- **Lists.** The biggest gap: **WinUI 3 has no C++ DataGrid or ListView with
  column headers.**
  - Row virtualisation itself works, as the spike shows.
  - Everything else would have to be built: a column header row with resize,
    reorder and sort indicators; `painted` cells; check boxes; tooltips for
    each part of a cell; cell drag; type-ahead.
  - Painted cells mean a Direct2D surface for each cell (`SurfaceImageSource`),
    or drawing the whole list ourselves, at which point it's no longer native.
- **Trees.** `TreeView` loads children lazily, but it doesn't scale to large
  trees the way the Win32 tree view does.
- **Canvas.** `SwapChainPanel`, as in the spike.
  - The Direct2D painter from win32_painter.cpp can draw into its swap chain.
  - `on_frame` comes from `CompositionTarget::Rendering`.
  - Pointer and key input need routed events, plus a focusable wrapper
    (SwapChainPanel can't take focus).
  - `present(gpu_frame)` and `stretch_while_resizing` have to be redone in
    terms of the panel.
  - `save_snapshot`: `RenderTargetBitmap` doesn't capture SwapChainPanel
    content, so tests would need a capture of their own.
- **File dialogs, print, clipboard.** Keep the Win32 code: `IFileDialog`
  with the HWND, `PrintDlgEx`, and the Win32 clipboard.
- **Theme.** `set_color_scheme` maps onto `RequestedTheme` on each window's
  root. That part fits well.
- **`set_busy`.** XAML sets the pointer per element (`ProtectedCursor`),
  overriding Win32's `SetCursor`. Showing busy across the whole app means
  walking every root.

**Effort**, roughly, for one person to reach parity with today's Win32
backend (which is about 9,000 lines of win32_backend.cpp alone):

| part | weeks |
|---|---|
| Islands host and loop | 1-2 |
| Layout and metrics | 2-3 |
| Simple widgets | 2 |
| List with columns and painted cells | 4-6 |
| Tree | 1-2 |
| Canvas re-hosting | 2-3 |
| Dialogs, menus, tabs and the rest | 2 |
| Tests and snapshots | 2 |
| **Total** | **16-22** |

Keeping pace with rapport's ABI after that is its own ongoing cost.

## Recommendation

Stay on Win32. It already has Windows 11's frame: Mica, rounded corners,
dark mode. WinUI's real gains are its Fluent controls and animations. Against
that:

- it adds start-up time;
- it adds 57 MB to the install, or a runtime install with its own servicing
  cycle;
- its app model doesn't fit how rapport starts up (Islands might fix that);
- its central control, a list with columns, doesn't exist in C++ and would be
  built from scratch.

If a Fluent look is the goal, adopting WinUI's colours and metrics in the
Win32 backend, or using Islands for one or two controls (a `NavigationView`,
say), costs far less than a whole backend.
