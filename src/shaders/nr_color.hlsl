// SPDX-License-Identifier: GPL-3.0-only
// Explicit color boundary only. Frozen nr_codec remains the model proxy/resolve.
cbuffer Params:register(b0) {
    uint mode,color,alpha,width;
    uint height,originX,originY,referenceValid;
    float linearScale; float3 reserved;
};
Texture2D<float4> original:register(t0);
Texture2D<float4> canonical:register(t1);
RWTexture2D<float4> outputImage:register(u0);
RWByteAddressBuffer invalidStatus:register(u1);
float3 To709(float3 v) {
    return mul(float3x3(1.660491002,-0.587641139,-0.072849863,
        -0.124550475,1.132899897,-0.008349422,-0.018150763,-0.100578898,1.118729661),v);
}
float3 To2020(float3 v) {
    return mul(float3x3(0.627403896,0.329283038,0.043313066,
        0.069097289,0.919540395,0.011362316,0.016391439,0.088013308,0.895595253),v);
}
// PQ near-black inversion amplifies the intermediate pow rounding error before
// the wide-primary matrix cancellation. Basic double arithmetic with bounded,
// range-reduced series avoids driver pow precision, with no lookup/tone mapping.
// SM5 basic doubles do not guarantee extended double division instructions.
// Refine a float reciprocal using only supported double add/multiply.
double Reciprocal(double x) {
    double r=double(rcp(float(x)));
    r=r*(2-x*r);
    return r*(2-x*r);
}
double LogPositive(double x) {
    int e=int((asuint(float(x))>>23)&255)-127;
    double m=x*double(exp2(float(-e)));
    double t=(m-1)*Reciprocal(m+1);
    double t2=t*t;
    double term=t;
    double sum=t;
    [unroll] for(int n=3;n<=25;n+=2) {term*=t2;sum+=term*Reciprocal(double(n));}
    return 2*sum+double(e)*0.6931471805599453094L;
}
double ExpReal(double x) {
    int e=int(floor(float(x*1.4426950408889634074L)));
    double r=x-double(e)*0.6931471805599453094L,term=1,sum=1;
    [unroll] for(int n=1;n<=16;++n) {term*=r*Reciprocal(double(n));sum+=term;}
    return sum*double(exp2(float(e)));
}
double PqNits(double code) {
    double result=0;
    if(code>0) {
        double p=ExpReal(LogPositive(code)*(32.0L/2523.0L));
        double numerator=p-3424.0L/4096.0L;
        if(numerator>0) result=10000*ExpReal(LogPositive(numerator*Reciprocal(2413.0L/128.0L-2392.0L/128.0L*p))*(16384.0L/2610.0L));
    }
    return result;
}
float3 PqEncode(float3 v) {
    float3 p=pow(saturate(v/10000),2610.0/16384.0);
    return pow(abs((3424.0/4096.0+2413.0/128.0*p)/(1+2392.0/128.0*p)),2523.0/32.0);
}
bool Valid(float4 v,bool normalized) {
    return all(isfinite(v)) && v.a>=0 && v.a<=1 && (!normalized || (all(v.rgb>=0)&&all(v.rgb<=1)));
}
float3 Decode(float4 raw) {
    float3 v=raw.rgb;
    if(alpha==3 && raw.a>0) v/=raw.a;
    if(color==4) {
        double r=PqNits(double(v.r));
        double g=PqNits(double(v.g));
        double b=PqNits(double(v.b));
        v.r=float(1.660491002L*r-0.587641139L*g-0.072849863L*b);
        v.g=float(-0.124550475L*r+1.132899897L*g-0.008349422L*b);
        v.b=float(-0.018150763L*r-0.100578898L*g+1.118729661L*b);
    }
    if(color==3) v=To709(v);
    return v*linearScale;
}
float3 Encode(float3 v) {
    v/=linearScale;
    if(color==3||color==4) v=To2020(v);
    if(color==4) v=PqEncode(v);
    return v;
}
void Invalid(uint2 at,float4 raw,uint reason) {
    uint old;invalidStatus.InterlockedOr(0,reason,old);outputImage[at]=raw;
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width||id.y>=height) return;
    float4 raw=original.Load(int3(id.xy+uint2(originX,originY),0));
    bool normalized=color==1||color==4;
    if(!Valid(raw,normalized)) { Invalid(id.xy,raw,1);return; }
    if(normalized&&alpha==3&&raw.a>0&&any(raw.rgb>raw.a)) { Invalid(id.xy,raw,2);return; }
    float3 prepared=alpha==3&&raw.a==0?float3(0,0,0):Decode(raw);
    if(!Valid(float4(prepared,raw.a),color==1)||any(abs(prepared)>65504)) { Invalid(id.xy,raw,4);return; }
    if(mode==0) { outputImage[id.xy]=float4(prepared,raw.a);return; }
    float4 resolved=canonical.Load(int3(id.xy,0));
    if(!all(isfinite(resolved))||resolved.a<0||resolved.a>1) { Invalid(id.xy,raw,8);return; }
    // A finite SDR answer can overshoot [0,1] slightly at sharp edges once history is carried. Clamp that pixel
    // instead of discarding the whole frame: one such pixel cancelled every history job (copy OBS 2026-11-03, bit 8).
    if(color==1) resolved.rgb=saturate(resolved.rgb);
    // No real prior guide pair exists on cold/reset. Preserve exact source,
    // after validating the real unprotected NR answer; D remains independently observed.
    if(referenceValid==0) { outputImage[id.xy]=raw;return; }
    // Frozen core strength-zero returns its untouched FP16 canonical input.
    // Compare in that declared representation, not against unquantized math.
    float3 storedPrepared=f16tof32(f32tof16(prepared));
    if((alpha==3&&raw.a==0)||all(prepared==resolved.rgb)||all(storedPrepared==resolved.rgb)) { outputImage[id.xy]=raw;return; }
    float factor=alpha==3?raw.a:1;
    float3 result=raw.rgb+factor*(Encode(resolved.rgb)-Encode(prepared));
    result=float3(raw.r<0?raw.r:result.r,raw.g<0?raw.g:result.g,raw.b<0?raw.b:result.b);
    if(normalized) result=clamp(result,0,factor);
    if(!Valid(float4(result,raw.a),normalized)||(!normalized&&any(abs(result)>65504))) { Invalid(id.xy,raw,16);return; }
    outputImage[id.xy]=float4(result,raw.a);
}
