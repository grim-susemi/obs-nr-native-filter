// SPDX-License-Identifier: GPL-3.0-only
// Ported from source-lock shader f97fb3dffc2e305a4edbd83d00942849d222a2f21547d880788a897fdfdd80b5.
// Default full-size soft-knee encode and RenoDX composition; no game routing.
/*
RenoDX
======

Project:  RenoDX by clshortfuse
Source:   https://github.com/clshortfuse/renodx

What is derived from it
-----------------------

The colour composition in OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl is taken
from RenoDX's DLSS 5 Neural Rendering addon. Specifically:

  * UpgradeToneMap -- the two-branch luminance ratio: below the proxy's luminance
    the original's own luminance is the target, and above it the difference is
    treated as headroom the proxy could not represent and handed back on top of
    the model's answer.

  * The OkLab hue correction applied to the rescaled result, which takes the hue
    and chroma direction from the model's own output and only the chroma
    magnitude from the scaled colour.

  * The final blend between a luminance-only result (the original's hue, the
    model's light) and the fully upgraded colour.

  * The reversible neutral-axis gamut compression: the D65 adaptation state
    carried in a Hunt-Pointer-Estevez LMS basis, the largest scale toward the
    neutral colour of equal luminance that leaves no channel negative, and the
    compression itself. Ours previously clamped per channel in AP1, which bends
    the hue of exactly the saturated pixels most likely to breach; theirs scales
    the whole colour by one factor so only saturation gives way, and is the
    identity when nothing is out of gamut.

This is their design. It was reimplemented here with different names and
comments, which does not make it original work, and it is credited as theirs.

What is not derived from it
---------------------------

The OkLab conversion matrices are Bjorn Ottosson's published constants. The AP1
gamut matrices, the sRGB transfer functions and the SMPTE ST.2084 (PQ) transfer
are standard colour science and are not specific to RenoDX.

Licence
-------

MIT License

Copyright (c) 2025 Carlos Lopez Jr.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
cbuffer Params : register(b0) {
    uint gMode; float gWhitePoint; uint gWidth; uint gHeight;
    float gTransferStrength; float gColourStrength; float gMaxRatio; uint gPassthrough;
    float gStrength; float3 gPadding;
};
// Hue-preserving gamut compression toward the D65 neutral axis.
// Adapted from clshortfuse/RenoDX (https://github.com/clshortfuse/renodx).
// See Licenses/RenoDX_ATTRIBUTION.txt.

float SanitizeFinite(float v, float fallback) { return isfinite(v) ? v : fallback; }

// Approximate skin-colour selection, not a face/skin segmentation network. Warm
// materials may be selected and coloured lighting can hide skin. The preview is
// deliberately exposed so users can check this before relying on protection.
float SkinColourWeight(float3 rgb)
{
    rgb = saturate(rgb);
    float y = dot(rgb, float3(0.299, 0.587, 0.114));
    float cb = (rgb.b - y) * 0.564 + 0.5;
    float cr = (rgb.r - y) * 0.713 + 0.5;
    float2 distance = (float2(cb, cr) - float2(0.405, 0.600)) / float2(0.090, 0.110);
    float chroma = max(rgb.r, max(rgb.g, rgb.b)) - min(rgb.r, min(rgb.g, rgb.b));
    return (1.0 - smoothstep(0.55, 1.35, length(distance))) * smoothstep(0.02, 0.10, chroma);
}

float3 SanitizeFinite3(float3 v, float3 fallback)
{
    return float3(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y),
                  SanitizeFinite(v.z, fallback.z));
}

float SafeDivide(float numerator, float denominator, float fallback)
{
    return abs(denominator) > 1e-8 ? numerator / denominator : fallback;
}

// Hunt-Pointer-Estevez LMS over linear BT.709, carrying the fixed D65 adaptation state the
// compression is defined against. The signal itself never leaves BT.709.
float3 LMSToBT709(float3 color)
{
    const float3x3 m = { 5.62059812, -4.57145756, 0.15577924,
                         -1.15555585, 2.25800438, -0.15415806,
                         0.03059913, -0.19018011, 1.06820532 };
    return mul(m, color);
}

float3 BT709ToLMS(float3 color)
{
    const float3x3 m = { 0.30569589, 0.62271286, 0.04528636,
                         0.15776262, 0.76968599, 0.08807030,
                         0.01933082, 0.11919478, 0.95053215 };
    return mul(m, color);
}

// The neutral colour of the same luminance as what is being compressed -- the point everything is
// pulled toward, so that pulling changes saturation and not hue.
float3 D65NeutralBT709(float3 adaptiveStateLms, float luminance)
{
    float3 d65 = LMSToBT709(max(adaptiveStateLms, 1e-8));
    float d65Y = max(dot(d65, float3(0.2126, 0.7152, 0.0722)), 1e-8);
    return d65 * (luminance / d65Y);
}

// The largest scale toward the neutral axis that leaves no channel negative. One for a colour that
// was already representable, which is why this is safe to run on every pixel.
float GamutCompressionScale(float3 color, float3 adaptiveStateLms)
{
    color = SanitizeFinite3(color, float3(0.0, 0.0, 0.0));

    const float y = dot(color, float3(0.2126, 0.7152, 0.0722));

    if (!(y > 1e-8))
        return 1.0;

    const float3 neutral = D65NeutralBT709(adaptiveStateLms, y);
    float scale = 1.0;

    if (color.r < 0.0 && neutral.r > color.r)
        scale = min(scale, SafeDivide(neutral.r, neutral.r - color.r, 1.0));

    if (color.g < 0.0 && neutral.g > color.g)
        scale = min(scale, SafeDivide(neutral.g, neutral.g - color.g, 1.0));

    if (color.b < 0.0 && neutral.b > color.b)
        scale = min(scale, SafeDivide(neutral.b, neutral.b - color.b, 1.0));

    return saturate(SanitizeFinite(scale, 1.0));
}

float3 ClampAp1(float3 color)
{
    const float3 adaptiveStateLms = BT709ToLMS(float3(0.18, 0.18, 0.18));
    const float scale = GamutCompressionScale(color, adaptiveStateLms);

    // Nothing was out of gamut. Leave the colour exactly as it arrived.
    if (scale >= 1.0)
        return color;

    const float y = dot(color, float3(0.2126, 0.7152, 0.0722));
    const float3 neutral = D65NeutralBT709(adaptiveStateLms, y);

    return SanitizeFinite3(neutral + (color - neutral) * scale, max(neutral, 0.0));
}

// ---------------------------------------------------------------------------------------------
// The composition below (UpgradeToneMap's two-branch ratio, the OkLab hue correction, and the blend
// between a luminance-only result and the model's own colour) is taken from RenoDX's DLSS 5 addon by
// clshortfuse -- https://github.com/clshortfuse/renodx. It is their design, not ours; see
// Licenses/RenoDX_LICENSE.txt. The OkLab matrices are Bjorn Ottosson's published constants and the
// AP1, sRGB and PQ transforms are standard colour science.
// ---------------------------------------------------------------------------------------------

// Use OkLab to retain the model's hue while adjusting chroma magnitude.
float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 color)
{
    const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                  0.2119034982, 0.6806995451, 0.1073969566,
                                  0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                  1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab)
{
    const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
                                  1.0, -0.1055613458, -0.0638541728,
                                  1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
                                  -1.2684380046, 2.6097574011, -0.3413193965,
                                  -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(lab_to_lms, lab);
    return mul(lms_to_rgb, lms * lms * lms);
}

// Take hue direction from correct and chroma magnitude from incorrect.
float3 HueOkLab(float3 incorrect, float3 correct)
{
    float3 incorrectLab = ToOkLab(incorrect);
    const float3 correctLab = ToOkLab(correct);
    const float incorrectChroma = length(incorrectLab.yz);
    const float correctChroma = length(correctLab.yz);

    // Normalize hue direction before scaling; near-grey chroma must not amplify numerical noise.
    const float2 hueDirection = correctChroma > 1e-5 ? correctLab.yz / correctChroma : float2(0.0, 0.0);

    incorrectLab.yz = hueDirection * incorrectChroma;

    return ClampAp1(FromOkLab(incorrectLab));
}

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gModel : register(t1);
Texture2D<float4> gOriginal : register(t2);
RWTexture2D<float4> gTarget : register(u0);
RWTexture2D<float4> gKeep : register(u1);
SamplerState gLinear : register(s0);
float WhitePoint() { return max(gWhitePoint, 1e-4); }
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// Soft-knee proxy shared by encoding and matched-residual reconstruction.
float3 SoftKnee(float3 display)
{
    if (gPassthrough != 0)
        return display;

    float displayLuma = dot(display, kLuma);

    if (displayLuma > 0.75)
    {
        float rolled = 0.75 + 0.25 * (1.0 - exp(-(displayLuma - 0.75) / 0.25));
        display *= rolled / displayLuma;
    }

    // Per-channel headroom, with the hue kept.
    //
    // The roll-off above is on luminance, and luminance is a weighted sum in which blue counts for
    // seven percent. A saturated blue can therefore sit at B = 2 with a luminance of 0.14, pass the
    // knee untouched, and be clipped per channel by the saturate in LinearToSrgb -- and clipping one
    // channel of a triple is a hue rotation, so blue arrives as cyan. That was the green cast over
    // every blue thing in GTA V at colour strength 1: the sky, the denim, the minimap. The model was
    // shown a cyan proxy, answered in cyan, and at colour strength 1 its hue is the frame's hue.
    //
    // One scalar on the whole triple cannot move hue, so the peak channel is brought to 1 that way.
    // Only pixels that were already being clipped are touched, so everything else is bit-identical
    // to before, and the resolve's reconstruction of this proxy stays exact because it goes through
    // this same function.
    float peak = max(display.r, max(display.g, display.b));

    if (peak > 1.0)
        display /= peak;

    return display;
}

[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gWidth || id.y >= gHeight) return;
    if (gMode == 0) {
        float4 source = gSource.Load(int3(id.xy,0));
        float3 frame = max(source.rgb,0.0);
        // Preserve exact source alpha and negative HDR channels in the raw copy.
        gKeep[id.xy] = source;
        gTarget[id.xy] = gPassthrough != 0 ? float4(frame,source.a)
            : float4(LinearToSrgb(SoftKnee(frame / WhitePoint())),source.a);
        return;
    }
    float2 cmpUv = (float2(id.xy)+0.5) / float2(gWidth,gHeight);
    float4 proxySample = gSource.SampleLevel(gLinear,cmpUv,0);
    float4 modelSample = gModel.SampleLevel(gLinear,cmpUv,0);
    float3 proxy = gPassthrough != 0 ? proxySample.rgb : SrgbToLinear(proxySample.rgb);
    float3 model = gPassthrough != 0 ? modelSample.rgb : SrgbToLinear(modelSample.rgb);
    float4 originalSample = gOriginal.Load(int3(id.xy,0));
    if (gStrength == 0.0) { gTarget[id.xy] = originalSample; return; }
    const float normScale = gPassthrough != 0 ? 1.0 : WhitePoint();
    float3 original = originalSample.rgb / normScale;
    float originalLuma = dot(original,kLuma);
    float proxyLuma = dot(proxy,kLuma);
    // Rescale the model answer to the original luminance and restore headroom lost by the proxy.
    float modelLuma = dot(model, kLuma);
    float3 upgraded;

    if (modelLuma <= 1e-5)
    {
        // The model can return an empty frame for an input it cannot read. Rescaling that collapses
        // the picture to black, so the frame is handed back untouched instead.
        upgraded = original;
    }
    else
    {
        float ratio;

        if (originalLuma < proxyLuma)
        {
            // Below what the proxy showed: the frame's own luminance is the target.
            ratio = originalLuma / max(proxyLuma, 1e-6);
        }
        else
        {
            // Above it, the difference is headroom the proxy could not represent -- brightness the
            // frame really has and the model never saw. It is handed back on top of the model's own
            // answer rather than scaled away, which is what kept highlights from being muted.
            ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;
        }

        // Keep the RGB blend within [0,1]; strength above 1 amplifies the bounded luminance ratio below.
        upgraded = lerp(original, HueOkLab(model * ratio, model), saturate(gTransferStrength));
    }

    // Detail strength decides how much of the model's picture is reached at all; colour strength
    // decides whether its colour comes with it. At 0 the frame keeps the game's own hue exactly and
    // only its light carries the model's verdict; at 1 the model's colour arrives as well.
    float upgradedLuma = dot(upgraded, kLuma);

    // A common luminance floor suppresses unstable ratios in near-black pixels.
    const float kRatioFloor = 1.0 / 512.0;
    float lumaRatio = (upgradedLuma + kRatioFloor) / (originalLuma + kRatioFloor);

    // Amplify detail through a luminance-ratio power, preserving neutral edits and two-sided bounds.
    const float amplified = pow(max(lumaRatio, 1e-6), 1.0 + max(gTransferStrength - 1.0, 0.0));

    // Bound both brightening and darkening. A single luminance-derived scale preserves hue.
    const float guard = max(gMaxRatio, 1.0);
    float boundedRatio = clamp(amplified, 1.0 / guard, guard);

    // Exactly one while the ratio is already inside the guard, so a frame that never needed bounding
    // is untouched rather than rounded, and strength zero stays bit-identical.
    upgraded *= boundedRatio / max(lumaRatio, 1e-6);

    // Both blend endpoints obey the luminance guard. Above colour strength 1, boost OkLab chroma
    // while preserving lightness/hue, then compress out-of-gamut colours toward neutral.
    float3 result = lerp(original * boundedRatio, upgraded, min(gColourStrength, 1.0));

    if (gColourStrength > 1.0)
        result = ClampAp1(FromOkLab(float3(1.0, gColourStrength, gColourStrength) * ToOkLab(max(result, 0.0))));

    result *= normScale;
    float3 bounded = max(result,0.0);
    gTarget[id.xy] = float4(originalSample.rgb + gStrength * (bounded-originalSample.rgb), originalSample.a);
}
