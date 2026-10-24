// rap::sys::decode_image: the platform's own decoders, which are maintained
// and hardened with the OS. WIC on Windows; elsewhere gdk-pixbuf, loaded on
// first use like libcurl in http.cpp, so the core neither builds nor installs
// against it.
#include "rapport/sys.h"
#include "rapport/widgets.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincodec.h>
#endif

namespace rap::sys
{
namespace
{

constexpr uint64_t k_max_side = 8192, k_max_pixels = uint64_t(64) << 20;

bool acceptable(uint64_t w, uint64_t h)
{
    return w > 0 && h > 0 && w <= k_max_side && h <= k_max_side && w * h <= k_max_pixels;
}

}  // namespace

#if defined(_WIN32)

namespace
{

template <class T>
struct com
{
    T* p = nullptr;
    ~com()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

}  // namespace

std::optional<decoded_image> decode_image(std::span<const uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > 0xffffffffu) return std::nullopt;
    // WIC needs COM on this thread; a thread that has it already keeps its own.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct uninit
    {
        bool owed;
        ~uninit()
        {
            if (owed) CoUninitialize();
        }
    } guard{SUCCEEDED(init)};

    com<IWICImagingFactory> factory;
    com<IWICStream> stream;
    com<IWICBitmapDecoder> decoder;
    com<IWICBitmapFrameDecode> frame;
    com<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), DWORD(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !acceptable(w, h) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;
    decoded_image out{int(w), int(h), pixel_format::bgra8_premultiplied, {}};
    out.pixels.resize(size_t(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(out.pixels.size()), out.pixels.data())))
        return std::nullopt;
    return out;
}

#else  // gdk-pixbuf

namespace
{

// The few calls used, as gdk-pixbuf's and GObject's headers have them.
struct GdkPixbuf;
struct GdkPixbufLoader;
struct GError;
using gboolean = int;

struct pixbuf_api
{
    GdkPixbufLoader* (*loader_new)();
    gboolean (*loader_write)(GdkPixbufLoader*, const uint8_t*, size_t, GError**);
    gboolean (*loader_close)(GdkPixbufLoader*, GError**);
    GdkPixbuf* (*loader_get_pixbuf)(GdkPixbufLoader*);
    int (*get_width)(const GdkPixbuf*);
    int (*get_height)(const GdkPixbuf*);
    int (*get_rowstride)(const GdkPixbuf*);
    int (*get_n_channels)(const GdkPixbuf*);
    int (*get_bits_per_sample)(const GdkPixbuf*);
    const uint8_t* (*read_pixels)(const GdkPixbuf*);
    unsigned long (*signal_connect_data)(void*, const char*, void (*)(), void*, void*, int);
    void (*object_unref)(void*);
    void (*error_free)(GError*);
    bool loaded = false;
};

const pixbuf_api& pixbuf()
{
    static pixbuf_api api = []
    {
        pixbuf_api a{};
        auto lib = library::open("libgdk_pixbuf-2.0.so.0");
        if (!lib) return a;
        // Symbols are looked up through the library's dependencies too, so
        // GObject's and GLib's come from the copies it loaded.
        a.loader_new = lib->function<GdkPixbufLoader*()>("gdk_pixbuf_loader_new");
        a.loader_write = lib->function<gboolean(GdkPixbufLoader*, const uint8_t*, size_t, GError**)>(
            "gdk_pixbuf_loader_write");
        a.loader_close = lib->function<gboolean(GdkPixbufLoader*, GError**)>("gdk_pixbuf_loader_close");
        a.loader_get_pixbuf = lib->function<GdkPixbuf*(GdkPixbufLoader*)>("gdk_pixbuf_loader_get_pixbuf");
        a.get_width = lib->function<int(const GdkPixbuf*)>("gdk_pixbuf_get_width");
        a.get_height = lib->function<int(const GdkPixbuf*)>("gdk_pixbuf_get_height");
        a.get_rowstride = lib->function<int(const GdkPixbuf*)>("gdk_pixbuf_get_rowstride");
        a.get_n_channels = lib->function<int(const GdkPixbuf*)>("gdk_pixbuf_get_n_channels");
        a.get_bits_per_sample = lib->function<int(const GdkPixbuf*)>("gdk_pixbuf_get_bits_per_sample");
        a.read_pixels = lib->function<const uint8_t*(const GdkPixbuf*)>("gdk_pixbuf_read_pixels");
        a.signal_connect_data =
            lib->function<unsigned long(void*, const char*, void (*)(), void*, void*, int)>(
                "g_signal_connect_data");
        a.object_unref = lib->function<void(void*)>("g_object_unref");
        a.error_free = lib->function<void(GError*)>("g_error_free");
        a.loaded = a.loader_new && a.loader_write && a.loader_close && a.loader_get_pixbuf &&
                   a.get_width && a.get_height && a.get_rowstride && a.get_n_channels &&
                   a.get_bits_per_sample && a.read_pixels && a.signal_connect_data &&
                   a.object_unref && a.error_free;
        if (a.loaded) lib->release();  // for the life of the process
        return a;
    }();
    return api;
}

// "size-prepared": the header has been read, before any pixels are.
void size_prepared(GdkPixbufLoader*, int w, int h, void* refused)
{
    *static_cast<bool*>(refused) = !acceptable(uint64_t(std::max(w, 0)), uint64_t(std::max(h, 0)));
}

}  // namespace

std::optional<decoded_image> decode_image(std::span<const uint8_t> bytes)
{
    const pixbuf_api& g = pixbuf();
    if (!g.loaded || bytes.empty()) return std::nullopt;
    GdkPixbufLoader* loader = g.loader_new();
    if (!loader) return std::nullopt;
    bool refused = false;
    g.signal_connect_data(loader, "size-prepared", reinterpret_cast<void (*)()>(&size_prepared),
                          &refused, nullptr, 0);
    // In pieces, so a refused size stops it before the pixels are decoded.
    bool ok = true;
    GError* error = nullptr;
    for (size_t at = 0; ok && !refused && at < bytes.size(); at += 16384)
        ok = g.loader_write(loader, bytes.data() + at, std::min<size_t>(16384, bytes.size() - at),
                            &error);
    ok = g.loader_close(loader, error ? nullptr : &error) && ok && !refused;
    if (error) g.error_free(error);
    std::optional<decoded_image> out;
    const GdkPixbuf* pb = ok ? g.loader_get_pixbuf(loader) : nullptr;
    if (pb && g.get_bits_per_sample(pb) == 8 && acceptable(uint64_t(g.get_width(pb)),
                                                             uint64_t(g.get_height(pb))))
    {
        const int w = g.get_width(pb), h = g.get_height(pb), stride = g.get_rowstride(pb),
                  channels = g.get_n_channels(pb);
        const uint8_t* src = g.read_pixels(pb);
        if (src && (channels == 3 || channels == 4))
        {
            out = decoded_image{w, h, pixel_format::rgba8, {}};
            out->pixels.resize(size_t(w) * size_t(h) * 4);
            uint8_t* dst = out->pixels.data();
            for (int y = 0; y < h; ++y)
            {
                const uint8_t* row = src + size_t(y) * size_t(stride);
                for (int x = 0; x < w; ++x, dst += 4, row += channels)
                {
                    std::memcpy(dst, row, 3);
                    dst[3] = channels == 4 ? row[3] : 255;
                }
            }
        }
    }
    g.object_unref(loader);
    return out;
}

#endif

}  // namespace rap::sys
