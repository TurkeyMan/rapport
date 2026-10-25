// rapport-gpu's scale pass.
cbuffer P : register(b0) { float gain; float3 pad; };
Texture2D<float4> src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
    // Rounded to a byte here: the hardware's own conversion may be 0.6 of a
    // step out, as Direct3D allows.
    const float4 v = src.Load(int3(pos.xy, 0));
    return floor(float4(saturate(v.rgb * gain), v.a) * 255.0 + 0.5) / 255.0;
}
