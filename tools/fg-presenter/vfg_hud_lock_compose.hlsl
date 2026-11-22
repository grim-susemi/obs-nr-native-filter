// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// Modified 2026-11-22 from PushFPS PushHudLockCompose.hlsl, commit 4e5c0491.
Texture2D<float4> generatedTex : register(t0);
Texture2D<float4> heldTex : register(t1);
Texture2D<float4> stateTex : register(t2);
Texture2D<float4> shareTex : register(t3);
RWTexture2D<float4> outputTex : register(u0);
cbuffer Constants : register(b0) {
    uint width, height, outputMode, flags;
    uint pad1, pad2, pad3, pad4;
};
float stillOf(float2 s) { return smoothstep(0.80, 0.95, s.x); }
float edgeOf(float2 s) {
    const float edge = outputMode == 1 ? 0.03 : 0.04;
    return smoothstep(edge, edge * 2.5, s.y);
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const float4 here = stateTex[id.xy];
    const int2 last = int2(width - 1, height - 1);
    float w = 0;
    if ((flags & 1) != 0) {
        const float still = stillOf(here.xy);
        float edge = 0;
        if (still > 0) {
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x) {
                    const float2 q = stateTex[clamp(int2(id.xy) + int2(x, y), int2(0, 0), last)].xy;
                    edge = max(edge, edgeOf(q) * stillOf(q));
                }
        }
        w = still * edge;
    }
    bool keep = false;
    if ((flags & 2) != 0 && here.z > 0.5 && shareTex[id.xy / 16].x < 0.5) {
        uint beside = 0, around = 0;
        [unroll] for (int y = -2; y <= 2; ++y)
            [unroll] for (int x = -2; x <= 2; ++x) {
                const uint same = stateTex[clamp(int2(id.xy) + int2(x, y), int2(0, 0), last)].z > 0.5 ? 1 : 0;
                around += same;
                if (abs(x) <= 1 && abs(y) <= 1) beside += same;
            }
        keep = beside >= 3 && around >= 5;
    }
    if ((flags & 4) != 0) { if (keep) outputTex[id.xy] = heldTex[id.xy]; return; }
    const float4 g = generatedTex[id.xy];
    if (keep) outputTex[id.xy] = heldTex[id.xy];
    else outputTex[id.xy] = w > 0 ? float4(lerp(g.rgb, heldTex[id.xy].rgb, w), g.a) : g;
}
