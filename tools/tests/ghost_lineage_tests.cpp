// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_fixture.h"
#include <cstdio>
#include <stdexcept>
#include <string_view>
using namespace nr;
using namespace nr::ghost;
namespace {
unsigned checks = 0;
void expect(bool condition, const char* why) { ++checks; if (!condition) throw std::runtime_error(why); }
FrameDescriptor descriptor(const Fixture& f)
{
    const bool sdr = f.color == FixtureColor::Sdr || f.color == FixtureColor::DarkSdr;
    return {{Fixture::width,Fixture::height},{0,0,Fixture::width,Fixture::height},
        sdr ? PixelFormat::Rgba8 : PixelFormat::Rgba16Float,
        sdr ? ColorSpace::Rec709Srgb : f.color == FixtureColor::Hdr709 ? ColorSpace::ScRgbLinear : ColorSpace::Rec2020Linear,
        AlphaMode::Opaque,static_cast<float>(f.referenceWhiteNits)};
}
std::vector<Capture> trace(const Fixture& f, unsigned input)
{
    std::vector<Capture> result;
    const FrameKey key{41,1,input+1};
    const JobKey job{key,1,1,input+1};
    for (Stage stage : {Stage::Raw,Stage::ModelInput,Stage::GuideCurrent,Stage::GuideReference,
        Stage::Unprotected,Stage::Completed,Stage::Selected,Stage::Preencode,Stage::Decoded}) {
        const bool cold=input==0||f.schedule[input].cut;
        const unsigned t = stage == Stage::GuideReference && !cold ? input-1 : input;
        result.push_back({stage,{41,1,t+1},job,descriptor(f),1,clean_image(f,t),1,"CPU_ANALYTIC"});
    }
    return result;
}
bool check_trace(const Fixture& f,unsigned tick,OutputKind kind,std::span<const Capture> captures)
{
    const bool delayed=kind==OutputKind::DelayedRaw||kind==OutputKind::DelayedNr;
    const unsigned input=tick-(delayed?1:0);
    const unsigned reference=input&&!f.schedule[input].cut?input-1:input;
    return lineage(f,tick,kind,captures,0,{41,1,input+1},{41,1,reference+1});
}
}
#include "ghost_scheduler_cpu.h"
int main(int argc,char** argv)
{
    try {
        if(argc==2&&std::string_view(argv[1])=="--manifest") {
            std::printf("{\"schema\":1,\"scope\":\"CPU_ANALYTIC_NOT_PHYSICAL\",\"width\":192,\"height\":96,"
                "\"ticks\":256,\"xorshift32\":[13,17,5],\"seeds\":[2583,2857],"
                "\"reference_white_nits\":{\"SDR\":80,\"HDR\":203},\"exposure\":1,"
                "\"normalized_by\":\"fixed_fixture_white\",\"transport_bound\":\"StageB_raw_only_calibration_required\","
                "\"holds\":[[64,79],[192,207]],\"cuts\":[128,224],"
                "\"visibility\":{\"exclude_rows\":[0,7],\"current_foreground_excluded\":true},"
                "\"silhouette\":{\"head_inset\":8,\"head_height\":8},"
                "\"texture\":{\"state\":\"seed^((phase+1)*0x9E3779B9)^((y+1)*0x85EBCA6B)\","
                "\"phase\":\"(x+pan+8192)%%8192\",\"generator\":\"xorshift32(13,17,5)\",\"amplitude\":\"(state&255)/255*0.125\","
                "\"low\":[0.125,0.25,0.1875],\"contrast\":0.125},"
                "\"dark_SDR_scale\":0.0625,\"foreground_checker_contrast\":0.0625,"
                "\"HUD\":{\"y\":80,\"height\":1,\"rgb\":[1,1,1]},"
                "\"HDR_highlight\":{\"x\":160,\"y\":84,\"rgb\":[4,2,1]},"
                "\"same_colored_reveal\":{\"ticks\":[144,175],\"rect\":[16,28,128,32],\"inherits_foreground_checker\":true},"
                "\"content_header\":{\"rect\":[0,0,64,4],\"bits\":16,\"bit_width\":4,\"levels\":[0.125,0.875],\"complement_rows\":[2,3]},"
                "\"spatial_landmarks\":{\"y\":6,\"x\":[83,127,179],\"code\":\"(content_id*(x+1)+seed)&255\",\"rgb\":\"[code/256,(255-code)/256,0.375]\"},"
                "\"inherited_trail\":[0.9,0.1],\"episodes\":[");
            bool first=true;
            for(auto seed:{0xA17u,0xB29u}) for(auto color:{FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
                const auto f=fixture(seed,color);
                std::printf("%s{\"seed\":%u,\"color\":%u,\"ticks\":[",first?"":",",seed,static_cast<unsigned>(color));
                first=false;
                for(unsigned t=0;t<256;++t) {
                    const auto& v=f.schedule[t];
                    std::printf("%s{\"tick\":%u,\"ingress\":%u,\"content_id\":%u,\"pan\":%d,"
                        "\"bar\":[%d,%d,%d,%d],\"silhouette\":[%d,%d,%d,%d],\"foreground\":[%.17g,%.17g,%.17g],"
                        "\"cut\":%s,\"held\":%s,\"same_colored_reveal\":%s}",t?",":"",t,v.ingress,v.contentId,v.pan,
                        v.bar.x,v.bar.y,v.bar.width,v.bar.height,v.silhouette.x,v.silhouette.y,v.silhouette.width,v.silhouette.height,
                        v.foreground.r,v.foreground.g,v.foreground.b,v.cut?"true":"false",v.held?"true":"false",v.sameColoredReveal?"true":"false");
                }
                std::printf("]}");
            }
            std::printf("]}\n");
            return 0;
        }
        if(argc!=1) throw std::invalid_argument("unknown CPU lineage command");
        ghost_scheduler_cpu::run();
        for (auto seed : {0xA17u,0xB29u}) for (auto color : {FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
            const auto f = fixture(seed,color);
            expect(f.schedule.size()==256,"finite episode");
            for (unsigned t=0;t<256;++t) {
                const auto image=clean_image(f,t);
                expect(decode_content(image)==f.schedule[t].contentId,"independent source ID decode");
                expect(decoded_landmarks(f,t,image,0),"analytic independent spatial landmarks");
                auto captures=trace(f,t);
                expect(check_trace(f,t,OutputKind::CurrentSource,captures),"OFF current raw image lineage");
                if (t<255) expect(check_trace(f,t+1,OutputKind::DelayedNr,captures),"exact n-1 model/guide/program/decoded lineage");
                if (f.schedule[t].held) {
                    expect(image==clean_image(f,t-1),"identical display hold changed pixels");
                    expect(f.schedule[t].ingress==f.schedule[t-1].ingress+1,"hold failed immutable ingress advance");
                }
                for (auto rate : {60u,60000u}) if (t)
                    expect(canonical_time(t,rate,rate==60?1:1001)>canonical_time(t-1,rate,rate==60?1:1001),"59.94/60 canonical tick progression");
            }
            for(unsigned start:{64u,192u}) {
                expect(f.schedule[start].contentId!=f.schedule[start-1].contentId,"hold anchor not distinct");
                expect(f.schedule[start].contentId==f.schedule[start+15].contentId,"16tick hold content ID");
                expect(f.schedule[start].contentId!=f.schedule[start+16].contentId,"hold lasted beyond 16ticks");
            }
            auto good=trace(f,40);
            expect(check_trace(f,41,OutputKind::DelayedRaw,good),"delayed raw control");
            expect(!check_trace(f,42,OutputKind::DelayedNr,good),"extra delay admitted");
            auto stale=trace(f,40);
            stale[3].pixels=clean_image(f,38);
            expect(!check_trace(f,41,OutputKind::DelayedNr,stale),"stale guide image matched bookkeeping falsely");
            auto stamp=trace(f,40);
            stamp[6].pixels=clean_image(f,38);
            const auto clean=clean_image(f,40);
            for(unsigned y=0;y<4;++y) for(unsigned x=0;x<64;++x)
                stamp[6].pixels[y*Fixture::width+x]=clean[y*Fixture::width+x];
            for(unsigned x:{83u,127u,179u}) stamp[6].pixels[6*Fixture::width+x]=clean[6*Fixture::width+x];
            expect(!check_trace(f,41,OutputKind::DelayedNr,stamp),"postcomposition ID stamp proved stale image falsely");
            auto frozen=trace(f,40);
            frozen[6].pixels=clean_image(f,20);
            expect(!check_trace(f,41,OutputKind::DelayedNr,frozen),"frozen selected pixels admitted");
            auto reorder=trace(f,40);
            reorder[5].job.emplace(JobKey{{41,1,39},1,1,39});
            expect(!check_trace(f,41,OutputKind::DelayedNr,reorder),"reordered completion admitted");
            auto differentlyBuilt=trace(f,40);
            differentlyBuilt[4].buildIdentity="UNRELATED_BUILD";
            expect(!check_trace(f,41,OutputKind::DelayedNr,differentlyBuilt),"D from another build admitted");
            auto differentlyStrengthened=trace(f,40);
            differentlyStrengthened[4].settingsRevision=2;
            expect(!check_trace(f,41,OutputKind::DelayedNr,differentlyStrengthened),"D from another settings revision admitted");
            std::vector<Capture> dropped;
            for(const auto& capture:good) if(capture.stage!=Stage::Selected) dropped.push_back(capture);
            expect(!check_trace(f,41,OutputKind::DelayedNr,dropped),"dropped selected sample admitted");
        }
        std::printf("PASS ghost_lineage checks=%u episodes=8 ticks=2048 CPU_only=true production_capture=false\n",checks);
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL ghost_lineage %s\n",e.what());return 1;}
}
