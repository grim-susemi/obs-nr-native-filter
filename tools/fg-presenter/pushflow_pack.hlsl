// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): This work is based on PushFPS, licensed under GPL-3.0-or-later; port modified 2026-11-21.
// PushFlow (PushFPS's own frame generation, src/pushflow/PushFlowBackend.cpp): the two real frames and
// the time of the frame to make, packed into the network's input tensor [1, C, padH, padW] (planar
// float: previous RGB, current RGB, timestep). C = 7 for models that make their own sampling grid;
// C = 11 for the older layout that also takes the flow scale (2/(W-1), 2/(H-1)) and the sampling
// grid (-1..1 across, -1..1 down) as planes. The pictures are resampled to the model size (the
// quality tier's scale), padded by repeating the edge, and encoded the way the network was trained
// (sRGB-like values in 0..1): encoding 0 = already encoded, 1 = linear SDR -> sRGB, 2 = scRGB -> PQ.
Texture2D<float4> previous : register(t0);
Texture2D<float4> current : register(t1);
RWStructuredBuffer<float> tensor : register(u0);
cbuffer Constants : register(b0) {
    uint srcW, srcH, padW, padH;
    uint modelW, modelH, encoding, channels;
    float timestep;
    uint timestepOnly;      // 1: the pair is already packed (2nd/3rd subframe): only the time plane changes
    uint unused2, unused3;
};
float3 encodeColor(float3 c) {
    if (encoding == 1) {
        c = saturate(c);
        return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
    }
    if (encoding == 2) {
        const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        const float3 y = pow(saturate(max(c, 0.0) * (80.0 / 10000.0)), m1);
        return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
    }
    return saturate(c);
}
float3 tap(Texture2D<float4> t, float2 p) {
    const float2 q = clamp(p - 0.5, 0.0, float2(srcW - 1, srcH - 1));
    const uint2 a = uint2(q);
    const uint2 b = min(a + 1, uint2(srcW - 1, srcH - 1));
    const float2 f = q - float2(a);
    return lerp(lerp(t[a].rgb, t[uint2(b.x, a.y)].rgb, f.x), lerp(t[uint2(a.x, b.y)].rgb, t[b].rgb, f.x), f.y);
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= padW || id.y >= padH) return;
    // The padding repeats the last model row / column.
    const uint plane = padW * padH, i = id.y * padW + id.x;
    if (timestepOnly != 0) { tensor[6 * plane + i] = timestep; return; }
    const float2 m = float2(min(id.x, modelW - 1), min(id.y, modelH - 1)) + 0.5;
    const float2 s = m * float2(srcW, srcH) / float2(modelW, modelH);
    const float3 a = encodeColor(tap(previous, s));
    const float3 b = encodeColor(tap(current, s));
    tensor[i] = a.r; tensor[plane + i] = a.g; tensor[2 * plane + i] = a.b;
    tensor[3 * plane + i] = b.r; tensor[4 * plane + i] = b.g; tensor[5 * plane + i] = b.b;
    tensor[6 * plane + i] = timestep;
    if (channels == 11) {
        tensor[7 * plane + i] = 2.0 / float(padW - 1);
        tensor[8 * plane + i] = 2.0 / float(padH - 1);
        tensor[9 * plane + i] = 2.0 * float(id.x) / float(padW - 1) - 1.0;
        tensor[10 * plane + i] = 2.0 * float(id.y) / float(padH - 1) - 1.0;
    }
}

