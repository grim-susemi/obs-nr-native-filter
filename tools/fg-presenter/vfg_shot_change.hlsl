// SPDX-License-Identifier: GPL-3.0-only
// Adapted from PushFPS VfgShotChange.hlsl, modified 2026-11-22.
cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint parity;
    uint compare;
};

Texture2D<float4> previousFrame : register(t0);
Texture2D<float4> currentFrame : register(t1);
RWByteAddressBuffer flags : register(u0);

static const uint kColumns = 64;
static const uint kRows = 36;
static const uint kSamples = kColumns * kRows;
groupshared uint histogramPrevious[256];
groupshared uint histogramCurrent[256];
groupshared uint sadSum;
groupshared uint histogramL1;

uint lumaAt(Texture2D<float4> frame, uint x, uint y)
{
    const uint3 rgb = uint3(round(frame.Load(int3(x, y, 0)).rgb * 255.0));
    return (54u * rgb.r + 183u * rgb.g + 19u * rgb.b + 128u) >> 8;
}

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    histogramPrevious[index] = 0;
    histogramCurrent[index] = 0;
    if (index == 0) {
        sadSum = 0;
        histogramL1 = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (compare != 0) {
        for (uint i = index; i < kSamples; i += 256) {
            const uint x = (i % kColumns) * width / kColumns;
            const uint y = (i / kColumns) * height / kRows;
            const uint a = lumaAt(previousFrame, x, y);
            const uint b = lumaAt(currentFrame, x, y);
            InterlockedAdd(histogramPrevious[a], 1);
            InterlockedAdd(histogramCurrent[b], 1);
            InterlockedAdd(sadSum, a > b ? a - b : b - a);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint p = histogramPrevious[index], c = histogramCurrent[index];
    InterlockedAdd(histogramL1, p > c ? p - c : c - p);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        const float sad = float(sadSum) / (255.0 * kSamples);
        const float histogram = float(histogramL1) / (2.0 * kSamples);
        const bool flash = sad > 0.5 && histogram < 0.1;
        const bool cut = compare != 0 && !flash &&
            ((sad > 0.3 && histogram > 0.5) || (sad > 0.08 && histogram > 0.9));
        flags.Store2(parity * 8, uint2(cut ? 1u : 0u, 0u));
        flags.Store4(16 + parity * 16, uint4(asuint(sad), asuint(histogram), compare, flash ? 1u : 0u));
        if (cut) {
            uint previousCount;
            flags.InterlockedAdd(48, 1, previousCount);
        }
    }
}
