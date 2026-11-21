// SPDX-License-Identifier: GPL-3.0-only
Texture2D<float4> Previous : register(t0);
Texture2D<float4> Current : register(t1);
RWByteAddressBuffer Changed : register(u0);
groupshared uint tileChanged;

// Each of the 64 lanes tests its part of a 64x16 tile. Out-of-bounds
// coordinates are skipped; every in-bounds pixel is covered unless a
// difference already proves this tile changed.
[numthreads(16, 4, 1)]
void main(uint3 tile : SV_GroupID, uint3 lane : SV_GroupThreadID, uint i : SV_GroupIndex)
{
    if (i == 0) tileChanged = 0;
    GroupMemoryBarrierWithGroupSync();

    uint width, height;
    Current.GetDimensions(width, height);
    const uint2 origin = tile.xy * uint2(64, 16) + lane.xy;
    bool mismatch = false;
    [loop]
    for (uint k = 0; k < 16 && !mismatch; ++k) {
        const uint2 p = origin + uint2((k % 4) * 16, (k / 4) * 4);
        if (p.x < width && p.y < height) {
            // R8G8B8A8_UNORM loads are injective for all 256 byte values per
            // channel. No filtering, gamma conversion, thresholds or hashing.
            mismatch = any(Previous.Load(int3(p, 0)) != Current.Load(int3(p, 0)));
        }
    }
    if (mismatch) InterlockedOr(tileChanged, 1);
    GroupMemoryBarrierWithGroupSync();

    // Shard the result over 64 uints to avoid a single contended GPU address.
    if (i == 0 && tileChanged != 0)
        Changed.InterlockedOr(((tile.x ^ (tile.y * 17)) & 63) * 4, 1);
}
