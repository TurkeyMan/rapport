// rapport-gpu's pattern pass.
cbuffer P : register(b0) { float2 size; float2 pad; };
float4 main(float4 pos : SV_Position) : SV_Target
{
    const int2 p = int2(pos.xy);
    if (p.x == 0 || p.y == 0 || p.x == int(size.x) - 1 || p.y == int(size.y) - 1) return float4(1, 0, 0, 1);
    if (p.x < 128 && p.y < 128) return ((p.x ^ p.y) & 1) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);
    return float4(pos.x / size.x, 0.38, pos.y / size.y, 1);
}
