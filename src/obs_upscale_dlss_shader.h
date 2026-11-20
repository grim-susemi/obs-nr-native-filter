// SPDX-License-Identifier: GPL-3.0-only
#pragma once

constexpr char shader[] = R"hlsl(
Texture2D<float4> first : register(t0);
Texture2D<float4> second : register(t1);
Texture2D<int2> flow : register(t2);
Texture2D<uint> cost : register(t3);
RWTexture2D<float4> dest : register(u0);
RWTexture2D<uint> cut : register(u1);
SamplerState linearClamp : register(s0);
cbuffer MotionUnits : register(b0) { float2 outputPerInput; };

float lin(float x) { return x <= 0.04045 ? x / 12.92 : pow(max(x + 0.055, 0.0) / 1.055, 2.4); }
void prepare(uint2 p, bool srgb) {
    uint w, h;
    dest.GetDimensions(w, h);
    if (any(p >= uint2(w, h))) return;
    float4 c = first.Load(int3(p, 0));
    if (srgb) c.rgb = float3(lin(c.r), lin(c.g), lin(c.b));
    c.a = saturate(c.a);
    dest[p] = float4(c.a > 0.0001 ? saturate(c.rgb / c.a) : 0.0, c.a);
}
[numthreads(8, 8, 1)]
void prepare_srgb(uint3 p : SV_DispatchThreadID) { prepare(p.xy, true); }
[numthreads(8, 8, 1)]
void prepare_linear(uint3 p : SV_DispatchThreadID) { prepare(p.xy, false); }

float4 flow_vertex(uint id : SV_VertexID) : SV_Position {
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
}
float3 encode_srgb(float3 x) {
    return lerp(12.92 * x, 1.055 * pow(max(x, 0.0), 1.0 / 2.4) - 0.055, step(0.0031308, x));
}
float4 flow_input(float4 position : SV_Position) : SV_Target {
    const float4 c = first.Load(int3(int2(position.xy), 0));
    return float4(encode_srgb(c.rgb * c.a), 1.0);
}

float luma(float4 c) { return dot(c.rgb, float3(0.213, 0.715, 0.072)) * c.a; }
[numthreads(8, 8, 1)]
void densify(uint3 p : SV_DispatchThreadID) {
    uint w, h;
    dest.GetDimensions(w, h);
    if (any(p.xy >= uint2(w, h))) return;
    const int3 cell = int3(p.xy / 4u, 0);
    const float2 size = float2(w, h);
    const float2 d = float2(flow.Load(cell)) * (1.0 / 32.0);
    const float2 at = float2(p.xy) + d;
    float confidence = 0.0;
    if (cost.Load(cell) < 32 && all(at >= 0.0) && all(at <= size - 1.0)) {
        const float a = luma(first.Load(int3(p.xy, 0)));
        const float b = luma(second.SampleLevel(linearClamp, (at + 0.5) / size, 0));
        confidence = 1.0 - smoothstep(0.03, 0.15, abs(a - b));
    }
    dest[p.xy] = float4(d * outputPerInput * confidence, 0, 0);
}

// A small RGB sample detects obvious cuts/fades without reading back either
// image. Require widespread, large changes rather than local transients.
#ifdef COMPUTE
groupshared uint changed[256];
[numthreads(16, 16, 1)]
void detect_cut(uint3 p : SV_GroupThreadID, uint index : SV_GroupIndex) {
    const float2 uv = (float2(p.xy) + 0.5) / 16.0;
    const float4 a = first.SampleLevel(linearClamp, uv, 0);
    const float4 b = second.SampleLevel(linearClamp, uv, 0);
    const float3 d = abs(a.rgb * a.a - b.rgb * b.a);
    changed[index] = max(max(d.r, d.g), d.b) >= 0.24 ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) changed[index] += changed[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0) cut[uint2(0, 0)] = changed[0] >= 166u ? 1u : 0u;
}
#endif

[numthreads(8, 8, 1)]
void compose(uint3 p : SV_DispatchThreadID) {
    uint w, h;
    dest.GetDimensions(w, h);
    if (any(p.xy >= uint2(w, h))) return;
    float a = saturate(second.SampleLevel(linearClamp, (float2(p.xy) + 0.5) / float2(w, h), 0).a);
    float3 rgb = saturate(first.Load(int3(p.xy, 0)).rgb);
    dest[p.xy] = float4(rgb * a, a);
}
)hlsl";
