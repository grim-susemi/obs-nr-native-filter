// SPDX-License-Identifier: GPL-3.0-only
// Veyra-style dual-stage anti-flicker post-filter in linear canonical FP16 space:
//   1) NrTemporal: NVOF motion-compensated temporal stability with 3x3 clamping
//      and disocclusion rejection (calms moving-camera & texture shimmer).
//   2) NrHold: Stage-5 static-pixel output stabilizer comparing 3x3 source luma
//      (locks static background/UI flicker with zero ghosting on moving objects).

cbuffer Params : register(b0) {
    uint width;
    uint height;
    float temporalStrength;
    float holdStrength;
    float holdTolerance;
    uint hasHistory;
    uint pad0;
    uint pad1;
};

Texture2D<float4> curOrigTex : register(t0);
Texture2D<float4> curNrTex : register(t1);
Texture2D<float2> motionTex : register(t2);
Texture2D<float4> prevOrigTex : register(t3);
Texture2D<float4> prevTemporalTex : register(t4);
Texture2D<float4> prevHoldTex : register(t5);

RWTexture2D<float4> outHoldTex : register(u0);
RWTexture2D<float4> outTemporalTex : register(u1);
RWTexture2D<float4> outOrigTex : register(u2);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

bool IsFinite3(float3 v) {
    return all(isfinite(v));
}

bool IsFinite2(float2 v) {
    return all(isfinite(v));
}

float Luma(float3 c) {
    return dot(max(c, 0.0), kLuma);
}

float PhotoError(float3 a, float3 b) {
    float3 d = abs(a - b);
    return max(abs(Luma(a) - Luma(b)), 0.85 * max(d.r, max(d.g, d.b))) /
        max(max(Luma(a), Luma(b)), 0.025);
}

float3 BilinearLoad(Texture2D<float4> tex, float2 centerCoord, int2 maxIdx) {
    float2 sc = clamp(centerCoord - 0.5, float2(0.0, 0.0), float2(maxIdx));
    int2 p00 = clamp(int2(floor(sc)), int2(0, 0), maxIdx);
    int2 p11 = min(p00 + int2(1, 1), maxIdx);
    float2 f = saturate(sc - float2(p00));

    float3 c00 = tex.Load(int3(p00.x, p00.y, 0)).rgb;
    float3 c10 = tex.Load(int3(p11.x, p00.y, 0)).rgb;
    float3 c01 = tex.Load(int3(p00.x, p11.y, 0)).rgb;
    float3 c11 = tex.Load(int3(p11.x, p11.y, 0)).rgb;

    float3 top = lerp(c00, c10, f.x);
    float3 bot = lerp(c01, c11, f.x);
    return lerp(top, bot, f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;

    int2 p = int2(id.xy);
    int2 maxIdx = int2(int(width) - 1, int(height) - 1);

    float4 curOrig4 = curOrigTex.Load(int3(p, 0));
    float4 curNr4 = curNrTex.Load(int3(p, 0));

    float3 curOrig = IsFinite3(curOrig4.rgb) ? curOrig4.rgb : float3(0.0, 0.0, 0.0);
    float3 curNr = IsFinite3(curNr4.rgb) ? curNr4.rgb : curOrig;
    float curAlpha = isfinite(curNr4.a) ? saturate(curNr4.a) : 1.0;

    float3 temporalOut = curNr;
    float2 mv = float2(0.0, 0.0);
    float motionConfidence = 0.0;
    float staticError = 1.0;
    float historyWeight = 0.0;
    float staticMeanChange = 1.0;
    if (hasHistory != 0 && (temporalStrength > 0.0 || holdStrength > 0.0)) {
        mv = motionTex.Load(int3(p, 0)).xy;
        if (IsFinite2(mv)) {
            float divergence = 0.0;
            bool neighborsFinite = true;
            const int2 offsets[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
            [unroll] for (int i = 0; i < 4; ++i) {
                int2 q = clamp(p + offsets[i], int2(0, 0), maxIdx);
                float2 neighbor = motionTex.Load(int3(q, 0)).xy;
                if (!IsFinite2(neighbor)) neighborsFinite = false;
                else divergence = max(divergence, length(neighbor - mv));
            }
            if (neighborsFinite) motionConfidence = 1.0 - smoothstep(0.5, 2.0, divergence);
        }
    }

    if (hasHistory != 0 && motionConfidence > 0.0) {
        float2 prevPos = float2(p) + 0.5 + mv;
        bool inBounds = IsFinite2(mv) &&
            prevPos.x >= 0.5 && prevPos.y >= 0.5 &&
            prevPos.x <= float(width) - 0.5 && prevPos.y <= float(height) - 0.5;

        float staticSum = 0.0;
        float warpedSum = 0.0;
        float sumCur = 0.0;
        float sumPrev = 0.0;
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                int2 q = clamp(p + int2(dx, dy), int2(0, 0), maxIdx);
                float3 current = curOrigTex.Load(int3(q, 0)).rgb;
                float3 stationary = prevOrigTex.Load(int3(q, 0)).rgb;
                float3 warped = inBounds ? BilinearLoad(prevOrigTex, float2(q) + 0.5 + mv, maxIdx) : stationary;
                staticSum += IsFinite3(current) && IsFinite3(stationary) ? PhotoError(current, stationary) : 1.0;
                warpedSum += inBounds && IsFinite3(current) && IsFinite3(warped) ? PhotoError(current, warped) : 1.0;
                sumCur += Luma(IsFinite3(current) ? current : curOrig);
                sumPrev += Luma(IsFinite3(stationary) ? stationary : curOrig);
            }
        }
        staticError = staticSum / 9.0;
        staticMeanChange = abs(sumCur - sumPrev) / max(max(sumCur, sumPrev), 9e-4);
        float warpedError = warpedSum / 9.0;
        // Only suppress subpixel flow: larger vectors must still release the unwarped hold.
        if (length(mv) < 1.0 && staticError <= warpedError) {
            mv = float2(0.0, 0.0);
            prevPos = float2(p) + 0.5;
            inBounds = true;
            warpedError = staticError;
        }
        historyWeight = 1.0 - smoothstep(0.025, 0.11, warpedError);

        if (inBounds && historyWeight > 0.0 && temporalStrength > 0.0) {
            float3 prevResidual = BilinearLoad(prevTemporalTex, prevPos, maxIdx);
            if (IsFinite3(prevResidual)) {
                float3 minResidual = curNr - curOrig;
                float3 maxResidual = minResidual;
                [unroll] for (int dy = -1; dy <= 1; ++dy) {
                    [unroll] for (int dx = -1; dx <= 1; ++dx) {
                        int2 q = clamp(p + int2(dx, dy), int2(0, 0), maxIdx);
                        float3 nr = curNrTex.Load(int3(q, 0)).rgb;
                        float3 source = curOrigTex.Load(int3(q, 0)).rgb;
                        if (IsFinite3(nr) && IsFinite3(source)) {
                            float3 residual = nr - source;
                            minResidual = min(minResidual, residual);
                            maxResidual = max(maxResidual, residual);
                        }
                    }
                }
                float3 clampedPrev = clamp(prevResidual, minResidual, maxResidual);
                float velFade = 1.0 - 0.55 * smoothstep(10.0, 36.0, length(mv));
                float alpha = saturate(temporalStrength * 0.85 * historyWeight * velFade * motionConfidence);
                temporalOut = curOrig + lerp(curNr - curOrig, clampedPrev, alpha);
            }
        }
    }

    float3 holdOut = temporalOut;

    if (hasHistory != 0 && holdStrength > 0.0 && historyWeight > 0.0 && staticMeanChange <= holdTolerance) {
        float4 prevHold4 = prevHoldTex.Load(int3(p, 0));
        float3 prevOrig = prevOrigTex.Load(int3(p, 0)).rgb;
        if (IsFinite3(prevHold4.rgb) && IsFinite3(prevOrig)) {
            // Unwarped hold also carries only the correction, never the old source.
            float staticMotion = 1.0 - smoothstep(0.25, 1.0, length(mv));
            float holdWeight = saturate(holdStrength * motionConfidence * staticMotion * historyWeight *
                (1.0 - smoothstep(0.025, 0.11, staticError)));
            holdOut = curOrig + lerp(temporalOut - curOrig, prevHold4.rgb - prevOrig, holdWeight);
        }
    }

    float3 published = clamp(IsFinite3(holdOut) ? holdOut : curNr, -65504.0, 65504.0);
    outHoldTex[p] = float4(published, curAlpha);
    // History represents exactly the published correction, including hold.
    outTemporalTex[p] = float4(clamp(published - curOrig, -65504.0, 65504.0), curAlpha);
    outOrigTex[p] = float4(curOrig, 1.0);
}
