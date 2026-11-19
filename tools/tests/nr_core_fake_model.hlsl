// SPDX-License-Identifier: GPL-3.0-only
// Test-only evaluator: copies its input, optionally adding an edit on the right half.
cbuffer Params : register(b0) { uint width; uint height; float gain; };
Texture2D<float4> input : register(t0);
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(id.x>=width || id.y>=height) return;
    float4 p=input.Load(int3(id.xy,0));
    output[id.xy]=float4(p.rgb+(id.x>=width/2 ? gain : 0.0),p.a);
}
