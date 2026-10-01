// SPDX-License-Identifier: GPL-3.0-only
// Both analysis inputs cover the full working extent. No crop/resize or second MV scaling.
cbuffer Parameters : register(b0) {
    uint width,height,grid,resetFrame;
    uint activeX,activeY,activeWidth,activeHeight;
};
Texture2D<int2> hardwareFlow : register(t0);
RWTexture2D<float2> motion : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<int2> externalHints : register(u2);
[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 p=tid.xy;
    if(p.x>=width || p.y>=height) return;
    // Predictor pass: broadcast actual native global S10.5 unchanged at the admitted hint grid.
    // No image estimation, filtering, scaling, rounding or CPU feedback; never publish this field.
    if(resetFrame==2) { externalHints[p]=hardwareFlow.Load(int3(0,0,0)); return; }
    depth[p]=1.0;
    if(resetFrame!=0) { motion[p]=float2(0,0); return; }
    if(p.x<activeX || p.y<activeY || p.x>=activeX+activeWidth || p.y>=activeY+activeHeight) {
        motion[p]=float2(asfloat(0x7fc00000),asfloat(0x7fc00000)); return;
    }
    // Grid vectors locate grid-cell centers. Edge-clamped bilinear upsample, decode ONCE /32.
    uint2 count=uint2((width+grid-1)/grid,(height+grid-1)/grid);
    float2 position=clamp((float2(p)+0.5)/float(grid)-0.5,0.0,float2(count-1));
    uint2 a=uint2(floor(position)),b=min(a+1,count-1);
    float2 f=position-float2(a);
    float2 top=lerp(float2(hardwareFlow.Load(int3(a,0))),float2(hardwareFlow.Load(int3(b.x,a.y,0))),f.x);
    float2 bottom=lerp(float2(hardwareFlow.Load(int3(a.x,b.y,0))),float2(hardwareFlow.Load(int3(b,0))),f.x);
    motion[p]=lerp(top,bottom,f.y)/32.0;
}
