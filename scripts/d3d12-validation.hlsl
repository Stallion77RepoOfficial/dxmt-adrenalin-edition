cbuffer Globals : register(b0) { float4 tint; };
float4 VSMain(uint id : SV_VertexID) : SV_Position {
 float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
 return float4(p[id],0,1);
}
float4 PSMain() : SV_Target0 { return tint; }
RWStructuredBuffer<uint> output : register(u0);
[numthreads(8,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { output[id.x] = id.x * 17 + 42; }

Texture2D<float4> inputTexture : register(t0);
float4 PSTexture() : SV_Target { return inputTexture.Load(int3(0, 0, 0)); }
