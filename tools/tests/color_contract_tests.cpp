// SPDX-License-Identifier: GPL-3.0-only
#include "nr_color.h"
#include <cstdio>
#include <stdexcept>
using namespace nr;
void expect(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        const FrameDescriptor d{{16,8},{0,0,16,8},PixelFormat::Rgba16Float,ColorSpace::ScRgbLinear,AlphaMode::Straight,80};
        const auto plan=color_plan(d,{1,1},true);
        expect(plan.has_value(),"scRGB admission");
        const ColorPixel raw{-0.125f,2,12,0.5f};
        const auto canonical=color_prepare(*plan,raw);
        expect(canonical.has_value(),"finite HDR preparation");
        const auto output=color_restore(*plan,raw,*canonical);
        expect(output && *output==raw,"unchanged HDR/negative/alpha roundtrip");
        auto changed=*canonical;changed.g=3;changed.r=1;
        const auto edited=color_restore(*plan,raw,changed);
        expect(edited&&edited->r==raw.r&&edited->g==3&&edited->a==raw.a,"negative source channel/alpha lost on edit");
        const auto protectedRaw=color_restore(*plan,raw,changed,false);
        expect(protectedRaw&&*protectedRaw==raw,"missing actual reference changed same-key HDR raw/negative/alpha");
        expect(color_constants(*plan,true,false).referenceValid==0&&
            color_constants(*plan,true,true).referenceValid==1,"restore validity was not binary or default changed");
        const auto exposed=color_plan(d,{2,4},true);
        expect(exposed&&exposed->linearScale==2&&exposed->whitePoint==2,"exposure/preExposure ordering");
        expect(color_prepare(*exposed,raw)->g==4,"explicit scene exposure");
        for(const auto color:{ColorSpace::Rec709Srgb,ColorSpace::ScRgbLinear,ColorSpace::Rec2020Linear,ColorSpace::Rec2020Pq}) {
            const auto format=color==ColorSpace::Rec709Srgb?PixelFormat::Rgba8:
                color==ColorSpace::Rec2020Pq?PixelFormat::Rgb10A2:PixelFormat::Rgba16Float;
            for(const auto alpha:{AlphaMode::Opaque,AlphaMode::Straight,AlphaMode::Premultiplied}) {
                const FrameDescriptor desc{{16,8},{2,1,8,4},format,color,alpha,203};
                const auto cp=color_plan(desc,{1,1},true);expect(cp.has_value(),"supported descriptor narrowed");
                for(unsigned i=0;i<=255;++i) {
                    const float a=static_cast<float>(i)/255;
                    const float factor=alpha==AlphaMode::Premultiplied?a:1;
                    const ColorPixel input{0.1f*factor,0.3f*factor,0.7f*factor,a};
                    const auto can=color_prepare(*cp,input);expect(can.has_value(),"ramp rejected");
                    const auto back=color_restore(*cp,input,*can);expect(back&&*back==input,"exact identity ramp/alpha");
                    auto edit=*can;edit.g*=0.95f;edit.a=1;
                    const auto out=color_restore(*cp,input,edit);expect(out&&out->a==input.a,"model alpha overwrote source");
                    const auto protectedInput=color_restore(*cp,input,edit,false);
                    expect(protectedInput&&*protectedInput==input,"cold/reset g=0 changed admitted source color/alpha");
                }
                const auto k=color_constants(*cp,false);
                expect(k.width==8&&k.height==4&&k.originX==2&&k.originY==1,"active subrectangle scaled/rebased incorrectly");
            }
        }
        const FrameDescriptor wide{{1,1},{0,0,1,1},PixelFormat::Rgba16Float,ColorSpace::Rec2020Linear,AlphaMode::Straight,80};
        const auto wp=color_plan(wide,{1,1},true);const auto red=color_prepare(*wp,{1,0,0,1});
        expect(red&&std::abs(red->r-1.660491f)<0.000002f&&std::abs(red->g+0.1245505f)<0.000002f&&
            std::abs(red->b+0.0181508f)<0.000002f,"Rec2020 primary conversion oracle");
        const FrameDescriptor pq{{1,1},{0,0,1,1},PixelFormat::Rgb10A2,ColorSpace::Rec2020Pq,AlphaMode::Straight,203};
        const auto pp=color_plan(pq,{1,1},true);
        // ST2084 analytic anchors: 100 nits at 0.5080784215; 10000 at 1.
        const auto anchor=color_prepare(*pp,{0.5080784215f,0.5080784215f,0.5080784215f,1});
        expect(anchor&&std::abs(anchor->r-1.25f)<0.00005f,"PQ absolute nits anchor");
        expect(std::abs(color_prepare(*pp,{1,1,1,1})->g-125)<0.001f,"PQ highlight clipped to SDR");
        const float nan=std::numeric_limits<float>::quiet_NaN();
        expect(!color_plan(d,{0,1},true)&&!color_plan(d,{1,nan},true)&&!color_plan(d,{1,1},false),"exposure/capability admission");
        const FrameDescriptor unknown{{1,1},{0,0,1,1},PixelFormat::Rgba16Float,ColorSpace::Unknown,AlphaMode::Straight,80};
        expect(!color_plan(unknown,{1,1},true),"unknown primaries admitted");
        expect(!color_prepare(*plan,{nan,1,1,1})&&!color_restore(*plan,raw,{nan,1,1,1}),"nonfinite claimed valid");
        expect(!color_restore(*plan,raw,{nan,1,1,1},false)&&
            !color_restore(*plan,{nan,1,1,1},changed,false),"g=0 disguised invalid source/NR answer as protected success");
        expect(!color_prepare(*pp,{1.1f,0,0,1}),"invalid normalized PQ accepted");
        for(const auto f:{PixelFormat::Unknown,PixelFormat::Bgra8,PixelFormat::Rgba8,PixelFormat::Rgba16Float,PixelFormat::Rgb10A2})
            for(const auto cs:{ColorSpace::Unknown,ColorSpace::Rec709Srgb,ColorSpace::ScRgbLinear,ColorSpace::Rec2020Linear,ColorSpace::Rec2020Pq})
                for(const auto a:{AlphaMode::Unknown,AlphaMode::Opaque,AlphaMode::Straight,AlphaMode::Premultiplied}) {
                    const FrameDescriptor boundary{{16,8},{0,0,16,8},f,cs,a,80};
                    expect(color_plan(boundary,{1,1},true).has_value()==boundary.valid(),"frozen descriptor admission widened/narrowed");
                }
        bool nullRejected=false;try {D3D12ColorPass invalid(nullptr,{});}catch(const std::invalid_argument&){nullRejected=true;}
        expect(nullRejected,"null pass initialized default hardware");
        const auto overflow=color_plan(d,{1,2},true);
        expect(!color_prepare(*overflow,{65504,65504,65504,1}),"canonical half storage overflow falsely valid");
        const auto repeat=color_plan(d,{1,1},true);
        expect(color_prepare(*repeat,raw)==canonical,"cached exposure contaminated subsequent plan");
        const FrameDescriptor sdr{{1,1},{0,0,1,1},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80};
        const auto sp=color_plan(sdr,{1,1},true);const ColorPixel sraw{0.5f,0.25f,0.75f,1};
        const auto over=color_restore(*sp,sraw,{1.25f,0.25f,-0.5f,1});const auto clamped=color_restore(*sp,sraw,{1,0.25f,0,1});
        expect(over&&clamped&&*over==*clamped&&over->r==1&&over->b==0,"SDR finite overshoot rejected or not clamped like the GPU restore");
        expect(!color_restore(*sp,sraw,{nan,0.25f,0.5f,1})&&!color_restore(*sp,sraw,{0.5f,0.25f,0.5f,1.5f}),"SDR nonfinite/alpha NR answer admitted");
        std::puts("PASS color_contract CPU: ramps HDR white/exposure PQ primaries negative alpha invalid controls stateless resume"); return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}
