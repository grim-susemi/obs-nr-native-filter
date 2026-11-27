// SPDX-License-Identifier: GPL-3.0-only
// Shared NVOF consumer pass. NR exports motion in analysis pixels
// (current -> previous); a consumer that renders at a different extent resamples
// it to its own extent and pre-multiplies the pixel displacement so the vendor
// consumer can keep a unit motion scale. Non-finite vectors become zero.
cbuffer ScaleParameters : register(b0) {
    float scaleX, scaleY;
    uint width, height;
};

Texture2D<float2> sourceMotion : register(t0);
SamplerState sourceSampler : register(s0);
RWTexture2D<float2> scaledMotion : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    const float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float2 mv = sourceMotion.SampleLevel(sourceSampler, uv, 0) * float2(scaleX, scaleY);
    if (!all(isfinite(mv))) mv = float2(0, 0);
    scaledMotion[tid.xy] = mv;
}
