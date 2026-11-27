// SPDX-License-Identifier: GPL-3.0-only
// Both analysis inputs cover the full working extent. No crop/resize or second MV scaling.
// Mode 0: Fast Veyra-style NvofDensify cost-gated 5-cell cross photometric upsample
//         with static-pixel zero-motion protection (stops 4x4 block wobble at minimal GPU cost).
// Mode 1: reset, Mode 2: predictor broadcast, Mode 3: bilinear upsample, Mode 4: zero motion.
//
// `sanitize` is the Shared NVOF export path, compiled separately into
// nr_flow_export.h. The private `main` above keeps Feature 18's NaN marker
// outside activeRect; the exported shared ring never exposes that marker, so a
// consumer on another device/process can sample the texture without filtering.
cbuffer Parameters : register(b0) {
    uint width, height, grid, resetFrame;
    uint activeX, activeY, activeWidth, activeHeight;
};
Texture2D<int2> hardwareFlow : register(t0);
Texture2D<uint> costFlow : register(t1);
Texture2D<float4> curFrame : register(t2);
Texture2D<float4> prevFrame : register(t3);
RWTexture2D<float2> motion : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<int2> externalHints : register(u2);

float PatchSad5(int2 p, float2 mv, int2 maxIdx) {
    int2 q = clamp(int2(round(float2(p) + mv)), int2(0, 0), maxIdx);
    int2 pm = max(p - int2(1, 1), int2(0, 0)), pp = min(p + int2(1, 1), maxIdx);
    int2 qm = max(q - int2(1, 1), int2(0, 0)), qp = min(q + int2(1, 1), maxIdx);
    float3 d = abs(curFrame.Load(int3(p, 0)).rgb - prevFrame.Load(int3(q, 0)).rgb);
    d += abs(curFrame.Load(int3(pm.x, p.y, 0)).rgb - prevFrame.Load(int3(qm.x, q.y, 0)).rgb);
    d += abs(curFrame.Load(int3(pp.x, p.y, 0)).rgb - prevFrame.Load(int3(qp.x, q.y, 0)).rgb);
    d += abs(curFrame.Load(int3(p.x, pm.y, 0)).rgb - prevFrame.Load(int3(q.x, qm.y, 0)).rgb);
    d += abs(curFrame.Load(int3(p.x, pp.y, 0)).rgb - prevFrame.Load(int3(q.x, qp.y, 0)).rgb);
    return (d.r + d.g + d.b) * (1.0 / 15.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 p = tid.xy;
    if (p.x >= width || p.y >= height) return;
    if (resetFrame == 2) { externalHints[p] = hardwareFlow.Load(int3(0, 0, 0)); return; }
    depth[p] = 1.0;
    if (resetFrame == 1 || resetFrame == 4) { motion[p] = float2(0, 0); return; }
    if (p.x < activeX || p.y < activeY || p.x >= activeX + activeWidth || p.y >= activeY + activeHeight) {
        motion[p] = float2(asfloat(0x7fc00000), asfloat(0x7fc00000));
        return;
    }
    uint2 count = uint2((width + grid - 1) / grid, (height + grid - 1) / grid);
    if (resetFrame == 3) {
        float2 position = clamp((float2(p) + 0.5) / float(grid) - 0.5, 0.0, float2(count - 1));
        uint2 a = uint2(floor(position)), b = min(a + 1, count - 1);
        float2 f = position - float2(a);
        float2 top = lerp(float2(hardwareFlow.Load(int3(a, 0))), float2(hardwareFlow.Load(int3(b.x, a.y, 0))), f.x);
        float2 bottom = lerp(float2(hardwareFlow.Load(int3(a.x, b.y, 0))), float2(hardwareFlow.Load(int3(b, 0))), f.x);
        motion[p] = lerp(top, bottom, f.y) / 32.0;
        return;
    }

    int2 ip = int2(p);
    int2 maxIdx = int2(int(width) - 1, int(height) - 1);
    int2 cell = int2(min(p / grid, count - 1));

    float zeroSad = PatchSad5(ip, float2(0.0, 0.0), maxIdx);
    if (zeroSad <= 0.004) {
        motion[p] = float2(0.0, 0.0);
        return;
    }

    static const int2 kOffsets[5] = { int2(0,0), int2(-1,0), int2(1,0), int2(0,-1), int2(0,1) };
    float2 bestMv = float2(hardwareFlow.Load(int3(cell, 0))) / 32.0;
    float bestScore = 1e9;
    float bestSad = 1.0;

    [unroll] for (int i = 0; i < 5; ++i) {
        int2 nc = clamp(cell + kOffsets[i], int2(0, 0), int2(count) - 1);
        float2 mv = float2(hardwareFlow.Load(int3(nc, 0))) / 32.0;
        float2 prevPos = float2(ip) + mv;
        if (prevPos.x < 0.0 || prevPos.y < 0.0 || prevPos.x > float(maxIdx.x) || prevPos.y > float(maxIdx.y))
            continue;
        uint c = costFlow.Load(int3(nc, 0));
        float sad = PatchSad5(ip, mv, maxIdx);
        float costPen = saturate(float(c) / 255.0) * 0.025;
        float distPen = i == 0 ? 0.0 : 0.0015;
        float score = sad + costPen + distPen;
        if (score < bestScore) {
            bestScore = score;
            bestMv = mv;
            bestSad = sad;
        }
    }

    if (zeroSad <= bestSad + 0.002 && zeroSad <= 0.012) {
        motion[p] = float2(0.0, 0.0);
        return;
    }
    if (bestSad > 0.16) {
        bestMv *= saturate(1.0 - (bestSad - 0.16) / 0.14);
    }
    motion[p] = bestMv;
}

cbuffer ExportParameters : register(b0) {
    uint exportWidth, exportHeight, exportActiveX, exportActiveY;
    uint exportActiveWidth, exportActiveHeight, exportGrid, exportReserved;
};
Texture2D<float2> privateMotion : register(t0);
RWTexture2D<float2> exportedMotion : register(u0);

// Copy the private guide texture into the broker's shared slot. Pixels outside
// activeRect and any non-finite vector become (0,0); this is the only place a
// consumer's motion is allowed to come from.
[numthreads(8, 8, 1)]
void sanitize(uint3 tid : SV_DispatchThreadID) {
    uint2 p = tid.xy;
    if (p.x >= exportWidth || p.y >= exportHeight) return;
    float2 mv = privateMotion.Load(int3(p, 0));
    const bool inside = p.x >= exportActiveX && p.y >= exportActiveY &&
        p.x < exportActiveX + exportActiveWidth && p.y < exportActiveY + exportActiveHeight;
    if (!inside || !all(isfinite(mv))) {
        exportedMotion[p] = float2(0, 0);
        return;
    }
    exportedMotion[p] = mv;
}
