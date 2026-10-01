// SPDX-License-Identifier: GPL-3.0-only
// Separate full-size OFA analysis only. Original HDR/alpha is never overwritten.
cbuffer Params:register(b0) { uint color,alpha,padding0,padding1; };
Texture2D<float4> original:register(t0);
float4 main(float4 position:SV_Position):SV_Target {
    float4 raw=original.Load(int3(uint2(position.xy),0));
    float3 rgb=alpha==3 && raw.a>0?raw.rgb/raw.a:raw.rgb;
    if(alpha==3 && raw.a==0) rgb=0;
    if(color!=1) {
        rgb=max(rgb,0);
        rgb=rgb/(1+max(rgb.r,max(rgb.g,rgb.b)));
    }
    return float4(saturate(rgb),1);
}
