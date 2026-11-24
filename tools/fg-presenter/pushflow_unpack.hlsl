// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): This work is based on PushFPS, licensed under GPL-3.0-or-later; port modified 2026-11-21.
// PushFlow: the network's output tensor [1, 3, padH, padW] back to a picture at the graph's size,
// resampled from the model size and decoded back to the graph's colours (see PushFlowPack.hlsl).
RWStructuredBuffer<float> tensor : register(u0);
RWTexture2D<float4> output : register(u1);
cbuffer Constants : register(b0) {
    uint srcW, srcH, padW, padH;
    uint modelW, modelH, encoding, unused0;
};
float3 decodeColor(float3 e) {
    e = saturate(e);
    if (encoding == 1) return lerp(e / 12.92, pow((e + 0.055) / 1.055, 2.4), step(0.04045, e));
    if (encoding == 2) {
        const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        const float3 p = pow(e, 1.0 / m2);
        return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * (10000.0 / 80.0);
    }
    return e;
}
float3 texel(uint2 p) {
    const uint plane = padW * padH, i = p.y * padW + p.x;
    return float3(tensor[i], tensor[plane + i], tensor[2 * plane + i]);
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= srcW || id.y >= srcH) return;
    const float2 q = clamp((float2(id.xy) + 0.5) * float2(modelW, modelH) / float2(srcW, srcH) - 0.5, 0.0, float2(modelW - 1, modelH - 1));
    const uint2 a = uint2(q);
    const uint2 b = min(a + 1, uint2(modelW - 1, modelH - 1));
    const float2 f = q - float2(a);
    const float3 c = lerp(lerp(texel(a), texel(uint2(b.x, a.y)), f.x), lerp(texel(uint2(a.x, b.y)), texel(b), f.x), f.y);
    output[id.xy] = float4(decodeColor(c), 1.0);
}
