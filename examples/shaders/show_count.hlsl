// rapport-gpu's pass reading a buffer: bin x's count, as red (count / 1024).
ByteAddressBuffer bins : register(t8);
float4 main(float4 pos : SV_Position) : SV_Target
{
    const uint count = bins.Load(uint(pos.x) * 4);
    return float4(min(count, 1024u) / 1024.0, 0, 0, 1);
}
