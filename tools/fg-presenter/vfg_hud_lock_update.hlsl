// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// Modified 2026-11-22 from PushFPS PushHudLockUpdate.hlsl, commit 4e5c0491.
Texture2D<float4> currentTex : register(t0);
Texture2D<float4> previousTex : register(t1);
RWTexture2D<float4> stateTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, reset, outputMode;
    uint lockOn, pad1, pad2, pad3;
};
float3 perceptual(float3 v) { return outputMode == 1 ? sqrt(max(v, 0) / 2.5) : v; }
float luma(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }
float3 at(int2 p) { return perceptual(currentTex[clamp(p, int2(0, 0), int2(width - 1, height - 1))].rgb); }
static const float kRise = 0.06;
static const float kStill = 0.02;
static const float kFade = 0.97;
static const float kPairStill = 2.0 / 255.0 + 1e-4;
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    if (reset != 0) { stateTex[id.xy] = float4(0, 0, 0, 0); return; }
    const int2 p = int2(id.xy);
    const float3 c = at(p);
    const float3 d = abs(c - perceptual(previousTex[id.xy].rgb));
    const float change = max(d.r, max(d.g, d.b));
    const float same = change <= kPairStill ? 1.0 : 0.0;
    if (lockOn == 0) { stateTex[id.xy] = float4(0, 0, same, 0); return; }
    const float l = luma(c);
    const float contrast = max(max(abs(l - luma(at(p + int2(1, 0)))), abs(l - luma(at(p - int2(1, 0))))),
                               max(abs(l - luma(at(p + int2(0, 1)))), abs(l - luma(at(p - int2(0, 1))))));
    float2 s = stateTex[id.xy].xy;
    s.x = change < kStill ? s.x + (1 - s.x) * kRise : 0;
    s.y = change < kStill ? max(contrast, s.y * kFade) : contrast;
    stateTex[id.xy] = float4(s, same, 0);
}
