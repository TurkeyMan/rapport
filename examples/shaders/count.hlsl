// rapport-gpu's compute test: a histogram of the source's red channel (256
// bins of bytes) added into u0, a group-local histogram first and one atomic
// per bin into the buffer, as a real histogram should.
cbuffer P : register(b0) { uint width; uint height; uint2 pad; };
Texture2D<float4> src : register(t0);
RWByteAddressBuffer bins : register(u0);

groupshared uint local_bins[256];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    local_bins[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < width && id.y < height)
    {
        const uint v = uint(src.Load(int3(id.xy, 0)).r * 255.0 + 0.5);
        InterlockedAdd(local_bins[v], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (local_bins[index] != 0)
        bins.InterlockedAdd(index * 4, local_bins[index]);
}
