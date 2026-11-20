// SPDX-License-Identifier: GPL-3.0-only
// AMD FidelityFX CAS (Contrast Adaptive Sharpening) + linear/transfer bridge for DLSS SR.
// Copyright (c) 2020 Advanced Micro Devices, Inc. for the CAS algorithm.
// See provenance/source-notices/AMD-FidelityFX-CAS-MIT.txt.
// mode 0: prepare/resize (t0 -> u0 FP16, optional bilinear upscale + optional sRGB->Linear)
// mode 1: CAS + encode (t0 linear FP16 + t1 original -> u0 output texture)
// mode 2: CAS-only 1:1 (t0 FP16 -> u0 FP16, optional sRGB->Linear->CAS->Linear->sRGB)
cbuffer CasConstants : register(b0) {
    uint mode, srcW, srcH, outW;
    uint outH, colorSpace, alphaMode, srgbTransfer;
    float sharpness, linearScale, pad0, pad1;
};
Texture2D<float4> srcTex : register(t0);
Texture2D<float4> origTex : register(t1);
RWTexture2D<float4> outTex : register(u0);

float SrgbToLinear1(float x) {
    float v = max(x, 0.0);
    return v <= 0.04045 ? (v / 12.92) : pow((v + 0.055) / 1.055, 2.4);
}
float LinearToSrgb1(float x) {
    float v = max(x, 0.0);
    return v <= 0.0031308 ? (v * 12.92) : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
}
float3 SrgbToLinear(float3 c) { return float3(SrgbToLinear1(c.r), SrgbToLinear1(c.g), SrgbToLinear1(c.b)); }
float3 LinearToSrgb(float3 c) { return float3(LinearToSrgb1(c.r), LinearToSrgb1(c.g), LinearToSrgb1(c.b)); }
float3 To2020(float3 v) {
    return mul(float3x3(0.627403896,0.329283038,0.043313066,
        0.069097289,0.919540395,0.011362316,0.016391439,0.088013308,0.895595253),v);
}
float3 PqEncode(float3 v) {
    float3 p = pow(saturate(v / 10000.0), 2610.0 / 16384.0);
    return pow(abs((3424.0 / 4096.0 + 2413.0 / 128.0 * p) / (1.0 + 2392.0 / 128.0 * p)), 2523.0 / 32.0);
}
float4 SampleBilinear(Texture2D<float4> tex, uint2 p, uint w, uint h, uint ow, uint oh) {
    float4 result = float4(0, 0, 0, 0);
    if (w == ow && h == oh) {
        result = tex.Load(int3(p, 0));
    } else {
        float2 pos = (float2(p) + 0.5) * float2(w, h) / float2(ow, oh) - 0.5;
        int2 q = int2(floor(pos));
        float2 f = frac(pos);
        int2 hi = int2(int(w) - 1, int(h) - 1);
        float4 c00 = tex.Load(int3(clamp(q, int2(0,0), hi), 0));
        float4 c10 = tex.Load(int3(clamp(q + int2(1,0), int2(0,0), hi), 0));
        float4 c01 = tex.Load(int3(clamp(q + int2(0,1), int2(0,0), hi), 0));
        float4 c11 = tex.Load(int3(clamp(q + int2(1,1), int2(0,0), hi), 0));
        result = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
    }
    return result;
}
float3 LoadLinear(int2 p, int2 hi) {
    float3 v = srcTex.Load(int3(clamp(p, int2(0,0), hi), 0)).rgb;
    if (srgbTransfer == 1) v = SrgbToLinear(v);
    return v;
}
float3 RunCas(uint2 id, uint w, uint h) {
    int2 ip = int2(id);
    int2 hi = int2(int(w) - 1, int(h) - 1);
    float3 c = LoadLinear(ip, hi);
    if (sharpness <= 0.0001) return c;
    float3 a = LoadLinear(ip + int2(0, -1), hi);
    float3 b = LoadLinear(ip + int2(-1, 0), hi);
    float3 d = LoadLinear(ip + int2(1, 0), hi);
    float3 e = LoadLinear(ip + int2(0, 1), hi);
    // Tonemap softly for HDR safety so highlights never ring or clip CAS weights.
    float3 cN = max(c, 0.0), aN = max(a, 0.0), bN = max(b, 0.0), dN = max(d, 0.0), eN = max(e, 0.0);
    float3 minRGB = min(min(bN, dN), min(aN, eN));
    minRGB = min(minRGB, cN);
    float3 maxRGB = max(max(bN, dN), max(aN, eN));
    maxRGB = max(maxRGB, cN);
    float3 scaleMax = max(maxRGB, 1.0);
    float3 rcpM = rcp(max(maxRGB, 1e-4));
    float3 amp = saturate(min(minRGB, max(2.0 * scaleMax - maxRGB, 0.0)) * rcpM);
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
    float3 wt = amp * peak;
    float3 rcpWt = rcp(1.0 + 4.0 * wt);
    float3 outC = (bN * wt + dN * wt + aN * wt + eN * wt + cN) * rcpWt;
    outC = max(outC, 0.0);
    if (any(c < 0.0)) outC = float3(c.r < 0.0 ? c.r : outC.r, c.g < 0.0 ? c.g : outC.g, c.b < 0.0 ? c.b : outC.b);
    return outC;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 p = tid.xy;
    if (p.x >= outW || p.y >= outH) return;
    if (mode == 0) {
        float4 v = SampleBilinear(srcTex, p, srcW, srcH, outW, outH);
        if (srgbTransfer != 0) v.rgb = SrgbToLinear(v.rgb);
        outTex[p] = v;
        return;
    }
    if (mode == 2) {
        float3 c = RunCas(p, outW, outH);
        if (srgbTransfer != 0) c = LinearToSrgb(saturate(c));
        float a = srcTex.Load(int3(p, 0)).a;
        outTex[p] = float4(c, a);
        return;
    }
    // mode == 1: CAS on outW x outH linear FP16 + color space encode + alpha from origTex
    float3 c = RunCas(p, outW, outH);
    float origAlpha = SampleBilinear(origTex, p, srcW, srcH, outW, outH).a;
    float factor = alphaMode == 3 ? origAlpha : 1.0;
    float3 enc = c;
    if (colorSpace == 1) {
        enc = LinearToSrgb(saturate(c));
    } else {
        enc = c / max(linearScale, 1e-6);
        if (colorSpace == 3 || colorSpace == 4) enc = To2020(enc);
        if (colorSpace == 4) enc = PqEncode(enc);
    }
    enc *= factor;
    if (colorSpace == 1 || colorSpace == 4) enc = clamp(enc, 0.0, factor);
    outTex[p] = float4(enc, origAlpha);
}
