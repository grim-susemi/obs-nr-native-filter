// SPDX-License-Identifier: GPL-3.0-only
Texture2D<float4> inputColor : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    const float2 xy = float2((id << 1) & 2, id & 2);
    return float4(xy * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps_main(float4 position : SV_Position) : SV_Target
{
    // The BGRA RTV encodes the layout. The shader returns logical RGBA.
    return inputColor.Load(int3(int2(position.xy), 0));
}
