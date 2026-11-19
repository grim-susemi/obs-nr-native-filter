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

    if (hasHistory != 0 && temporalStrength > 0.0) {
        float2 mv = motionTex.Load(int3(p, 0)).xy;
        float2 prevPos = float2(p) + 0.5 + mv;
        bool inBounds = IsFinite2(mv) &&
            prevPos.x >= 0.5 && prevPos.y >= 0.5 &&
            prevPos.x <= float(width) - 0.5 && prevPos.y <= float(height) - 0.5;

        if (inBounds) {
            float3 prevOrig = BilinearLoad(prevOrigTex, prevPos, maxIdx);
            float3 prevTemp = BilinearLoad(prevTemporalTex, prevPos, maxIdx);

            if (IsFinite3(prevOrig) && IsFinite3(prevTemp)) {
                float3 minNr = curNr;
                float3 maxNr = curNr;
                [unroll] for (int dy = -1; dy <= 1; ++dy) {
                    [unroll] for (int dx = -1; dx <= 1; ++dx) {
                        int2 q = clamp(p + int2(dx, dy), int2(0, 0), maxIdx);
                        float3 s = curNrTex.Load(int3(q, 0)).rgb;
                        if (IsFinite3(s)) {
                            minNr = min(minNr, s);
                            maxNr = max(maxNr, s);
                        }
                    }
                }
                float3 clampedPrev = clamp(prevTemp, minNr, maxNr);

                float lCur = Luma(curOrig);
                float lPrev = Luma(prevOrig);
                float3 rgbDiff = abs(curOrig - prevOrig);
                float maxCh = max(rgbDiff.r, max(rgbDiff.g, rgbDiff.b));
                float relErr = max(abs(lCur - lPrev), maxCh * 0.85) / max(max(lCur, lPrev), 0.025);
                float velFade = 1.0 - 0.55 * smoothstep(10.0, 36.0, length(mv));
                float occWeight = (1.0 - smoothstep(0.025, 0.11, relErr)) * velFade;
                float alpha = saturate(temporalStrength * 0.85 * occWeight);
                temporalOut = lerp(curNr, clampedPrev, alpha);
            }
        }
    }

    float3 holdOut = temporalOut;

    if (hasHistory != 0 && holdStrength > 0.0) {
        float4 prevHold4 = prevHoldTex.Load(int3(p, 0));
        if (IsFinite3(prevHold4.rgb)) {
            float sumCur = 0.0;
            float sumPrev = 0.0;
            [unroll] for (int dy = -1; dy <= 1; ++dy) {
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    int2 q = clamp(p + int2(dx, dy), int2(0, 0), maxIdx);
                    float3 sc = curOrigTex.Load(int3(q, 0)).rgb;
                    float3 sp = prevOrigTex.Load(int3(q, 0)).rgb;
                    sumCur += Luma(IsFinite3(sc) ? sc : curOrig);
                    sumPrev += Luma(IsFinite3(sp) ? sp : curOrig);
                }
            }
            float meanCur = sumCur * (1.0 / 9.0);
            float meanPrev = sumPrev * (1.0 / 9.0);
            float relDiff = abs(meanCur - meanPrev) / max(max(meanCur, meanPrev), 1e-4);
            if (relDiff <= holdTolerance) {
                holdOut = lerp(temporalOut, prevHold4.rgb, saturate(holdStrength));
            }
        }
    }

    outHoldTex[p] = float4(IsFinite3(holdOut) ? holdOut : curNr, curAlpha);
    outTemporalTex[p] = float4(IsFinite3(temporalOut) ? temporalOut : curNr, curAlpha);
    outOrigTex[p] = float4(curOrig, 1.0);
}
