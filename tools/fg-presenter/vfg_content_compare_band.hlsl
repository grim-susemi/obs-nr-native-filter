// SPDX-License-Identifier: GPL-3.0-only
// Band-tolerant content comparison. mode 0 keeps the exact byte-equality
// contract of vfg_content_compare.hlsl; any nonzero mode counts every RGB
// component that differs by more than `band` 8-bit steps and reports the
// largest such step. The host decides repeat when overCount fits its budget.
Texture2D<float4> Previous : register(t0);
Texture2D<float4> Current : register(t1);
RWByteAddressBuffer Changed : register(u0);

cbuffer Params : register(b0)
{
    uint mode;    // 0 exact, nonzero band
    uint band;    // per-component LSB tolerance in band mode
    uint maxOver; // host budget: overCount <= maxOver means unchanged
    uint pad;
};

groupshared uint tileChanged;
groupshared uint tileOver;
groupshared uint tileMax;

// Each of the 64 lanes covers 16 pixels of a 64x16 tile: lane.x selects one of
// four column quarters per step, lane.y one of four row quarters.
[numthreads(16, 4, 1)]
void main(uint3 tile : SV_GroupID, uint3 lane : SV_GroupThreadID, uint i : SV_GroupIndex)
{
    uint width, height;
    Current.GetDimensions(width, height);
    const uint2 origin = tile.xy * uint2(64, 16) + lane.xy;

    if (mode == 0) {
        // Exact mode only needs existence, so the first unequal pixel ends the tile.
        if (i == 0) tileChanged = 0;
        GroupMemoryBarrierWithGroupSync();
        bool mismatch = false;
        [loop]
        for (uint k = 0; k < 16 && !mismatch; ++k) {
            const uint2 p = origin + uint2((k % 4) * 16, (k / 4) * 4);
            if (p.x < width && p.y < height)
                mismatch = any(Previous.Load(int3(p, 0)) != Current.Load(int3(p, 0)));
        }
        if (mismatch) InterlockedOr(tileChanged, 1);
        GroupMemoryBarrierWithGroupSync();
        // Shard the result over 64 uints to avoid a single contended GPU address.
        if (i == 0 && tileChanged != 0)
            Changed.InterlockedOr(((tile.x ^ (tile.y * 17)) & 63) * 4, 1);
        return;
    }

    // Band mode visits every in-bounds pixel: an early exit would undercount the
    // motion area and report a real cut as a tolerated blend. R8G8B8A8_UNORM
    // loads are injective, so round(x * 255) recovers the stored byte exactly.
    if (i == 0) {
        tileOver = 0;
        tileMax = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const int bandSteps = int(band);
    uint over = 0;
    uint maxDiff = 0;
    [loop]
    for (uint k = 0; k < 16; ++k) {
        const uint2 p = origin + uint2((k % 4) * 16, (k / 4) * 4);
        if (p.x >= width || p.y >= height) continue;
        const int3 before = int3(round(Previous.Load(int3(p, 0)).rgb * 255.0));
        const int3 after = int3(round(Current.Load(int3(p, 0)).rgb * 255.0));
        const int3 delta = abs(before - after);
        over += (delta.r > bandSteps) ? 1u : 0u;
        over += (delta.g > bandSteps) ? 1u : 0u;
        over += (delta.b > bandSteps) ? 1u : 0u;
        maxDiff = max(maxDiff, uint(max(delta.r, max(delta.g, delta.b))));
    }
    if (over != 0) InterlockedAdd(tileOver, over);
    if (maxDiff != 0) InterlockedMax(tileMax, maxDiff);
    GroupMemoryBarrierWithGroupSync();
    if (i == 0) {
        if (tileOver != 0) Changed.InterlockedAdd(0, tileOver);
        if (tileMax != 0) Changed.InterlockedMax(4, tileMax);
    }
}
