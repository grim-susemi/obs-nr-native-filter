// SPDX-License-Identifier: GPL-3.0-only
Texture2D<float4> sourceImage : register(t0);
float4 vertex(uint id : SV_VertexID) : SV_Position {
 float2 p=float2(float((id<<1)&2),float(id&2));
 return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 pixel(float4 position : SV_Position) : SV_Target {
 // Non-sRGB views: load semantic RGBA; RTV store handles BGRA channel order.
 // No transfer function, alpha division, clipping or color conversion here.
 return sourceImage.Load(int3(int2(position.xy),0));
}
