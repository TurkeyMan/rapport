// rap::gpu on Direct3D 11, for the Win32 backend: a device over the
// backend's own D3D11 device, and a texture's D3D11 resource.
#pragma once

#include "gpu.h"

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID2D1Multithread;

namespace rap::gpu::d3d11
{

// A device over `d3d` (kept referenced). `lock`: the lock of the Direct2D
// factory (made multithreaded) whose device draws on `d3d` too; the device
// holds it around its own use of the immediate context, so Direct2D's and
// the app's work never interleave. Null when nothing else uses `d3d`.
std::shared_ptr<device> wrap(ID3D11Device* d3d, ID2D1Multithread* lock);

// The texture's resource when it's `dev`'s, else null.
ID3D11Texture2D* native(const device& dev, const texture&);

}  // namespace rap::gpu::d3d11
