// rapport-gpu's fill pass.
cbuffer P : register(b0) { float4 colour; };
float4 main(float4 pos : SV_Position) : SV_Target { return colour; }
