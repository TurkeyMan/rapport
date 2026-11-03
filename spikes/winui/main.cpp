// WinUI 3 spike: rapport's shapes built in C++, with no XAML compiler. A menu
// bar, two panes with a splitter between them, a list of 100,000 rows made as
// they scroll into view (as rapport's list_model gives them), and a canvas
// drawn with Direct3D in a SwapChainPanel.
//
//   winui_spike                         shows the window
//   winui_spike --measure out.txt       builds it, writes how long each step
//                                       took and quits, never showing it
//   winui_spike --measure out.txt --activate
//                                       shows it too, until its first frame
//                                       (run it under hidden_run, on a
//                                       desktop nothing shows)
#include <windows.h>
#include <unknwn.h>
#include <shellapi.h>
#include <werapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#undef GetCurrentTime  // a macro in windows.h, a method in the projection

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <microsoft.ui.xaml.window.h>

#if !WINUI_SPIKE_SELF_CONTAINED
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>
#endif

#include <algorithm>
#include <cstdio>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Microsoft::UI::Dispatching;
using namespace winrt::Microsoft::UI::Input;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Markup;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider;
using winrt::Windows::UI::Xaml::Interop::TypeName;

#if WINUI_SPIKE_SELF_CONTAINED
// Microsoft.WindowsAppRuntime.dll: loading it turns on registration-free
// activation of the runtime's classes from the exe's manifest.
STDAPI WindowsAppRuntime_EnsureIsLoaded();
#endif

namespace
{

// -- timing -------------------------------------------------------------------

// Milliseconds since the process was created, so the loader's share counts.
double since_start()
{
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(now) - ticks(created)) / 10000.0;
}

struct options
{
    std::wstring measure;  // where to write the times; empty: run as an app
    bool activate = false;
};
options opts;
std::vector<std::pair<std::string, double>> marks;

void mark(std::string name)
{
    if (!opts.measure.empty()) marks.emplace_back(std::move(name), since_start());
}

void write_marks()
{
    if (FILE* f = _wfopen(opts.measure.c_str(), L"w"))
    {
        for (auto& [name, ms] : marks) std::fprintf(f, "%-14s %8.1f ms\n", name.c_str(), ms);
        std::fclose(f);
    }
}

void write_marks_and_quit()
{
    write_marks();
    Application::Current().Exit();
}

// -- rows ---------------------------------------------------------------------

// The list's rows as rapport's list_model has them: a count, and each row's
// text asked for when it's shown. The items XAML sees are boxed row numbers,
// made on demand; nothing is held per row.
struct changed_args : implements<changed_args, IVectorChangedEventArgs>
{
    changed_args(CollectionChange change, uint32_t index) : change_(change), index_(index) {}
    CollectionChange CollectionChange() const { return change_; }
    uint32_t Index() const { return index_; }

    winrt::Windows::Foundation::Collections::CollectionChange change_;
    uint32_t index_;
};

struct row_iterator : implements<row_iterator, IIterator<IInspectable>>
{
    row_iterator(uint32_t at, uint32_t n) : at_(at), n_(n) {}
    IInspectable Current() const
    {
        if (at_ >= n_) throw hresult_out_of_bounds();
        return box_value(at_);
    }
    bool HasCurrent() const { return at_ < n_; }
    bool MoveNext()
    {
        if (at_ < n_) ++at_;
        return at_ < n_;
    }
    uint32_t GetMany(array_view<IInspectable> items)
    {
        uint32_t i = 0;
        for (; i < items.size() && at_ < n_; ++i, ++at_) items[i] = box_value(at_);
        return i;
    }

    uint32_t at_, n_;
};

struct row_source
    : implements<row_source, IObservableVector<IInspectable>, IVector<IInspectable>, IIterable<IInspectable>>
{
    explicit row_source(uint32_t n) : n_(n) {}

    IInspectable GetAt(uint32_t i) const
    {
        if (i >= n_) throw hresult_out_of_bounds();
        return box_value(i);
    }
    uint32_t Size() const { return n_; }
    IVectorView<IInspectable> GetView() const { throw hresult_not_implemented(); }
    bool IndexOf(IInspectable const& item, uint32_t& index) const
    {
        auto row = item.try_as<IReference<uint32_t>>();
        if (!row || row.Value() >= n_) return false;
        index = row.Value();
        return true;
    }
    uint32_t GetMany(uint32_t start, array_view<IInspectable> items) const
    {
        uint32_t i = 0;
        for (; i < items.size() && start + i < n_; ++i) items[i] = box_value(start + i);
        return i;
    }
    IIterator<IInspectable> First() const { return make<row_iterator>(0, n_); }

    // Read-only: the model changes, then says so.
    void SetAt(uint32_t, IInspectable const&) { throw hresult_illegal_method_call(); }
    void InsertAt(uint32_t, IInspectable const&) { throw hresult_illegal_method_call(); }
    void RemoveAt(uint32_t) { throw hresult_illegal_method_call(); }
    void Append(IInspectable const&) { throw hresult_illegal_method_call(); }
    void RemoveAtEnd() { throw hresult_illegal_method_call(); }
    void Clear() { throw hresult_illegal_method_call(); }
    void ReplaceAll(array_view<IInspectable const>) { throw hresult_illegal_method_call(); }

    event_token VectorChanged(VectorChangedEventHandler<IInspectable> const& handler)
    {
        return changed_.add(handler);
    }
    void VectorChanged(event_token const& token) noexcept { changed_.remove(token); }

    // list_observer::model_reset.
    void reset(uint32_t n)
    {
        n_ = n;
        changed_(*this, make<changed_args>(CollectionChange::Reset, 0));
    }

    uint32_t n_;
    event<VectorChangedEventHandler<IInspectable>> changed_;
};

std::wstring row_text(uint32_t row, int column)
{
    switch (column)
    {
    case 0: return std::format(L"{}", row + 1);
    case 1: return std::format(L"src/file_{:06}.cpp", row);
    default: return std::format(L"{} KB", (row * 37) % 900 + 1);
    }
}

// A row's look: three cells. Loaded at run time by XamlReader (the XAML
// compiler isn't needed for that); the cells are filled in code as each row
// is shown.
constexpr wchar_t row_template[] =
    LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
          <Grid>
            <Grid.ColumnDefinitions>
              <ColumnDefinition Width="64"/>
              <ColumnDefinition Width="*"/>
              <ColumnDefinition Width="72"/>
            </Grid.ColumnDefinitions>
            <TextBlock Grid.Column="0"/>
            <TextBlock Grid.Column="1" TextTrimming="CharacterEllipsis"/>
            <TextBlock Grid.Column="2" HorizontalAlignment="Right"/>
          </Grid>
        </DataTemplate>)";

// -- splitter -----------------------------------------------------------------

// The bar between the panes. WinUI has no splitter of its own (the Community
// Toolkit's is C#), so it's a Grid with a resize pointer; showing that pointer
// needs ProtectedCursor, so a class of our own derived from Grid.
struct splitter_bar : GridT<splitter_bar>
{
    splitter_bar()
    {
        ProtectedCursor(InputSystemCursor::Create(InputSystemCursorShape::SizeWestEast));
        Background(SolidColorBrush(winrt::Microsoft::UI::Colors::Transparent()));
    }
};

// -- canvas -------------------------------------------------------------------

// A drawn view: Direct3D renders into a composition swap chain the
// SwapChainPanel shows, one swap chain pixel to one of the screen's.
struct canvas_host
{
    SwapChainPanel panel;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain2> chain;
    float shade = 0.25f;

    void create()
    {
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                        device.put(), nullptr, context.put()));
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 8;
        desc.Height = 8;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        com_ptr<IDXGISwapChain1> chain1;
        check_hresult(factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, chain1.put()));
        chain = chain1.as<IDXGISwapChain2>();
        check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));

        panel.SizeChanged([this](auto&&, auto&&) { resize(); });
        panel.CompositionScaleChanged([this](auto&&, auto&&) { resize(); });
    }

    void resize()
    {
        float sx = panel.CompositionScaleX(), sy = panel.CompositionScaleY();
        UINT w = std::max(1u, UINT(panel.ActualWidth() * sx + 0.5));
        UINT h = std::max(1u, UINT(panel.ActualHeight() * sy + 0.5));
        check_hresult(chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0));
        // The panel scales its content by the display's scale; undo that, so
        // the frame is drawn at the screen's pixels.
        DXGI_MATRIX_3X2_F inverse{1.0f / sx, 0, 0, 1.0f / sy, 0, 0};
        check_hresult(chain->SetMatrixTransform(&inverse));
        paint();
    }

    void paint()
    {
        com_ptr<ID3D11Texture2D> buffer;
        check_hresult(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
        com_ptr<ID3D11RenderTargetView> target;
        check_hresult(device->CreateRenderTargetView(buffer.get(), nullptr, target.put()));
        float colour[4] = {shade, 0.35f, 0.55f, 1.0f};
        context->ClearRenderTargetView(target.get(), colour);
        chain->Present(1, 0);
    }
};

// -- the app ------------------------------------------------------------------

// Application, with the metadata provider WinUI's own controls need. That's
// what the XAML compiler would otherwise generate: with no XAML of our own,
// WinUI's provider answers for everything.
struct spike_app : ApplicationT<spike_app, IXamlMetadataProvider>
{
    spike_app()
    {
        // Measuring, a failure is written down with the times, and ends it.
        UnhandledException([](auto&&, UnhandledExceptionEventArgs const& e)
        {
            if (opts.measure.empty()) return;
            marks.emplace_back(std::format("error {:08x} {}", uint32_t(e.Exception().value),
                                           to_string(e.Message())), since_start());
            write_marks();
            ExitProcess(1);
        });
    }

    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        mark("launched");
        Resources().MergedDictionaries().Append(XamlControlsResources());
        mark("resources");

        window_ = Window();
        window_.Title(L"rapport WinUI spike");
        window_.AppWindow().Resize({1000, 640});
        mark("window");

        auto root = build();
        window_.Content(root);
        mark("content");

        if (!opts.measure.empty())
        {
            if (!opts.activate)
            {
                // Never shown: quit once the queue's had a turn.
                DispatcherQueue::GetForCurrentThread().TryEnqueue(DispatcherQueuePriority::Low,
                                                                  [] { write_marks_and_quit(); });
                return;
            }
            root.Loaded([](auto&&, auto&&) { mark("loaded"); });
            rendering_ = CompositionTarget::Rendering(auto_revoke, [this](auto&&, auto&&)
            {
                mark("first_frame");
                rendering_.revoke();
                DispatcherQueue::GetForCurrentThread().TryEnqueue(DispatcherQueuePriority::Low,
                                                                  [] { write_marks_and_quit(); });
            });
        }
        window_.Activate();
        mark("activated");
    }

    IXamlType GetXamlType(TypeName const& type) { return provider_.GetXamlType(type); }
    IXamlType GetXamlType(hstring const& name) { return provider_.GetXamlType(name); }
    com_array<XmlnsDefinition> GetXmlnsDefinitions() { return provider_.GetXmlnsDefinitions(); }

private:
    Grid build()
    {
        Grid root;
        RowDefinition menu_row, body_row;
        menu_row.Height(GridLengthHelper::Auto());
        body_row.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        root.RowDefinitions().Append(menu_row);
        root.RowDefinitions().Append(body_row);

        auto menu = build_menu();
        root.Children().Append(menu);
        mark("  menu");

        // Two panes and the bar between them.
        Grid body;
        Grid::SetRow(body, 1);
        ColumnDefinition left, bar_column, right;
        left.Width(GridLengthHelper::FromPixels(360));
        left.MinWidth(80);
        bar_column.Width(GridLengthHelper::FromPixels(6));
        right.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        body.ColumnDefinitions().Append(left);
        body.ColumnDefinitions().Append(bar_column);
        body.ColumnDefinitions().Append(right);
        root.Children().Append(body);

        list_ = build_list();
        body.Children().Append(list_);
        mark("  list");

        Grid bar = make<splitter_bar>();
        Grid::SetColumn(bar, 1);
        body.Children().Append(bar);
        bar.PointerPressed([this, bar, left, body](auto&&, PointerRoutedEventArgs const& e)
        {
            drag_from_ = e.GetCurrentPoint(body).Position().X;
            drag_width_ = left.ActualWidth();
            dragging_ = bar.CapturePointer(e.Pointer());
            e.Handled(true);
        });
        bar.PointerMoved([this, left, body](auto&&, PointerRoutedEventArgs const& e)
        {
            if (!dragging_) return;
            double x = e.GetCurrentPoint(body).Position().X;
            double most = std::max(80.0, body.ActualWidth() - 80);
            left.Width(GridLengthHelper::FromPixels(std::clamp(drag_width_ + x - drag_from_, 80.0, most)));
        });
        auto end_drag = [this, bar](auto&&, PointerRoutedEventArgs const& e)
        {
            dragging_ = false;
            bar.ReleasePointerCapture(e.Pointer());
        };
        bar.PointerReleased(end_drag);
        bar.PointerCaptureLost([this](auto&&, auto&&) { dragging_ = false; });

        canvas_.create();
        Grid::SetColumn(canvas_.panel, 2);
        body.Children().Append(canvas_.panel);
        mark("  canvas");
        return root;
    }

    MenuBar build_menu()
    {
        using winrt::Windows::System::VirtualKey;
        using winrt::Windows::System::VirtualKeyModifiers;
        auto item = [](hstring text, VirtualKeyModifiers mods, VirtualKey key)
        {
            MenuFlyoutItem item;
            item.Text(text);
            if (key != VirtualKey::None)
            {
                KeyboardAccelerator accel;
                accel.Modifiers(mods);
                accel.Key(key);
                item.KeyboardAccelerators().Append(accel);
            }
            return item;
        };

        MenuBarItem file;
        file.Title(L"File");
        auto quit = item(L"Quit", VirtualKeyModifiers::Control, VirtualKey::Q);
        quit.Click([this](auto&&, auto&&) { window_.Close(); });
        file.Items().Append(item(L"Open...", VirtualKeyModifiers::Control, VirtualKey::O));
        file.Items().Append(MenuFlyoutSeparator());
        file.Items().Append(quit);

        MenuBarItem view;
        view.Title(L"View");
        auto refresh = item(L"Refresh", VirtualKeyModifiers::None, VirtualKey::F5);
        refresh.Click([this](auto&&, auto&&) { refresh_rows(); });
        view.Items().Append(refresh);

        MenuBarItem help;
        help.Title(L"Help");
        auto about = item(L"About", VirtualKeyModifiers::None, VirtualKey::None);
        about.Click([this](auto&&, auto&&) { show_about(); });
        help.Items().Append(about);

        MenuBar menu;
        menu.Items().Append(file);
        menu.Items().Append(view);
        menu.Items().Append(help);
        return menu;
    }

    ListView build_list()
    {
        ListView list;
        list.SelectionMode(ListViewSelectionMode::Extended);
        list.ItemTemplate(XamlReader::Load(row_template).as<DataTemplate>());
        rows_ = make_self<row_source>(100000);
        list.ItemsSource(rows_.as<IInspectable>());
        // Each row's cells are filled as it's realised, from the model.
        list.ContainerContentChanging([](ListViewBase const&, ContainerContentChangingEventArgs const& e)
        {
            if (e.InRecycleQueue()) return;
            uint32_t row = unbox_value<uint32_t>(e.Item());
            auto cells = e.ItemContainer().ContentTemplateRoot().as<Grid>().Children();
            for (int c = 0; c < 3; ++c) cells.GetAt(c).as<TextBlock>().Text(row_text(row, c));
            e.Handled(true);
        });
        return list;
    }

    // ui.post: a worker hands its result back to the UI thread through the
    // window's DispatcherQueue.
    void refresh_rows()
    {
        auto queue = DispatcherQueue::GetForCurrentThread();
        std::thread([queue, rows = rows_, canvas = &canvas_]
        {
            uint32_t n = 50000 + GetTickCount() % 50000;  // the work
            queue.TryEnqueue([rows, canvas, n]
            {
                rows->reset(n);
                canvas->shade = canvas->shade > 0.5f ? 0.25f : 0.75f;
                canvas->paint();
            });
        }).detach();
    }

    // A message box: WinUI's is a ContentDialog, drawn inside the window
    // over a smoke layer, not a window of its own.
    void show_about()
    {
        ContentDialog dialog;
        dialog.XamlRoot(window_.Content().XamlRoot());
        dialog.Title(box_value(L"rapport WinUI spike"));
        dialog.Content(box_value(L"A menu bar, a splitter, 100,000 rows and a Direct3D canvas."));
        dialog.CloseButtonText(L"OK");
        dialog.ShowAsync();
    }

    XamlControlsXamlMetaDataProvider provider_;
    Window window_{nullptr};
    ListView list_{nullptr};
    com_ptr<row_source> rows_;
    canvas_host canvas_;
    CompositionTarget::Rendering_revoker rendering_;
    bool dragging_ = false;
    double drag_from_ = 0, drag_width_ = 0;
};

void parse_options()
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
    {
        std::wstring_view arg = argv[i];
        if (arg == L"--measure" && i + 1 < argc) opts.measure = argv[++i];
        else if (arg == L"--activate") opts.activate = true;
    }
    LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    parse_options();
    mark("main");
    // Measuring is unattended: no error boxes, no crash reporting dialog.
    if (!opts.measure.empty())
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    }

    // COM first: the bootstrapper needs it.
    init_apartment(apartment_type::single_threaded);
#if WINUI_SPIKE_SELF_CONTAINED
    HRESULT hr = WindowsAppRuntime_EnsureIsLoaded();
    if (FAILED(hr)) return hr;
#else
    // The installed framework package, version 2.6 or later; no prompt to
    // install it if it's missing (the default would show one).
    PACKAGE_VERSION min_version{};
    min_version.Version = WINDOWSAPPSDK_RUNTIME_VERSION_UINT64;
    HRESULT hr = MddBootstrapInitialize2(WINDOWSAPPSDK_RELEASE_MAJORMINOR, WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
                                         min_version, MddBootstrapInitializeOptions_None);
    if (FAILED(hr)) return hr;
#endif
    mark("runtime");

    Application::Start([](auto&&) { make<spike_app>(); });

#if !WINUI_SPIKE_SELF_CONTAINED
    MddBootstrapShutdown();
#endif
    return 0;
}
