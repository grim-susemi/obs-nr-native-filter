// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_oracle.h"
#include <cstdio>
#include <stdexcept>
using namespace nr::ghost;
namespace {
unsigned checks=0, blendRejected=0, blurRejected=0, cleanAdmitted=0;
void expect(bool condition,const char* why) {++checks;if(!condition)throw std::runtime_error(why);}
}
int main()
{
    try {
        for(auto color:{FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
            const auto calibrationFixture=fixture(0xA17,color), holdout=fixture(0xB29,color);
            const std::array<Image,2> clean{clean_image(calibrationFixture,8),clean_image(calibrationFixture,64)};
            auto raw=clean;
            raw[0][500].r+=.000002;
            expect(transport(clean[0],clean[0],.000001),"clean analytic transport");
            expect(!transport(clean[0],raw[0],.000001),"wrong raw transport bound admitted");
            const auto calibration=calibrate(.000001,clean,raw);
            expect(calibration.epsilon==calibration.rawMaxError&&calibration.rawMaxError>.000001,"raw-only frozen maximum");
            std::vector<TemporalReport> reports;
            for(unsigned tick=1;tick<Fixture::ticks;++tick) {
                const auto x=clean_image(holdout,tick);
                const auto admitted=temporal(holdout,tick,x,x,calibration);
                expect(admitted.pass,"clean raw added ghost");
                ++cleanAdmitted;
                expect(detail(x,x,detail_edges(holdout,tick),calibration.epsilon),"clean detail admission");
                auto ghost=x;
                const auto prior=clean_image(holdout,tick-1);
                for(size_t p=0;p<ghost.size();++p)
                    ghost[p]={.98*x[p].r+.02*prior[p].r,.98*x[p].g+.02*prior[p].g,.98*x[p].b+.02*prior[p].b};
                auto bad=temporal(holdout,tick,x,ghost,calibration);
                if(holdout.schedule[tick].contentId!=holdout.schedule[tick-1].contentId) {
                    expect(!bad.pass,"2 percent planted blend falsely admitted");
                    ++blendRejected;
                    expect(bad.maximum>calibration.epsilon,"offending maximum lost");
                    if(tick==1) {
                        expect(std::abs(bad.maximum-.02)<1e-12,"independent two-percent analytic G");
                        expect(bad.maximumRaw==0&&std::abs(bad.maximumTotal-.02)<1e-12,"raw/incremental/total split");
                        for(const auto& score:bad.templates)
                            std::printf("CPU_TEMPLATE color=%u tick=%u prior=%u region=%u pixels=%zu denominator=%.17g bound=%.17g incremental=%.17g raw=%.17g total=%.17g zero_contrast=%u pass=%u\n",
                                static_cast<unsigned>(color),score.currentTick,score.priorTick,score.region,score.pixels,
                                score.denominator,score.bound,score.incremental,score.raw,score.total,score.zeroContrast,score.pass);
                    }
                }
                reports.push_back(bad);
                auto blur=x;
                for(unsigned y=8;y<Fixture::height;++y) for(unsigned px=1;px+1<Fixture::width;++px) {
                    const size_t p=y*Fixture::width+px;
                    blur[p]={(x[p-1].r+x[p].r+x[p+1].r)/3,(x[p-1].g+x[p].g+x[p+1].g)/3,
                        (x[p-1].b+x[p].b+x[p+1].b)/3};
                }
                expect(!detail(x,blur,detail_edges(holdout,tick),calibration.epsilon),"1px blur falsely admitted");
                ++blurRejected;
                const auto inherited=inherited_image(holdout,tick);
                const auto attribution=temporal(holdout,tick,inherited,inherited,calibration);
                expect(attribution.pass&&attribution.maximum==0,"inherited trail called plugin trail");
                for(const auto& score:attribution.templates)
                    expect(score.raw==score.total,"raw/total inherited attribution not separate");
            }
            expect(persistence(reports)>0,"brief planted persistence hidden");
            expect(template_persistence(reports,0,0)>0,"region/template persistence hidden");
            std::printf("CPU_PERSISTENCE color=%u maximum=%u first_template=%u reports=%zu\n",static_cast<unsigned>(color),
                persistence(reports),template_persistence(reports,0,0),reports.size());
            // Both static background and moving foreground textured positive ROIs.
            const unsigned tick=32;
            const auto x=clean_image(holdout,tick);
            for(bool moving:{false,true}) {
                std::vector<size_t> roi;
                for(unsigned y=8;y<Fixture::height;++y) for(unsigned px=0;px<Fixture::width;++px)
                    if(moving?foreground(holdout.schedule[tick],px,y):(y>=86&&px<128))
                        roi.push_back(y*Fixture::width+px);
                Image delta(x.size(),{0,0,0}),output=x;
                const double boundedEdit=color==FixtureColor::DarkSdr?-.001:-.01;
                for(unsigned y=8;y<Fixture::height;++y) for(unsigned px=0;px<Fixture::width;++px) {
                    const size_t p=y*Fixture::width+px;
                    delta[p]={boundedEdit,boundedEdit,boundedEdit};
                    output[p]={x[p].r+boundedEdit,x[p].g+boundedEdit,x[p].b+boundedEdit};
                }
                const auto positive=edit(x,output,delta,roi,calibration.epsilon);
                expect(positive.pass&&std::abs(positive.retained-1)<1e-12,"positive bounded edit energy rejected");
                expect(temporal(holdout,tick,x,output,calibration).pass,"edit-positive fixture added trail");
                expect(detail(x,output,detail_edges(holdout,tick),calibration.epsilon),"edit-positive fixture lost signed edge contrast");
                std::printf("CPU_EDIT color=%u moving=%u rms=%.17g retained=%.17g epsilon=%.17g pass=%u\n",
                    static_cast<unsigned>(color),moving,positive.rms,positive.retained,calibration.epsilon,positive.pass);
                expect(!edit(x,x,delta,roi,calibration.epsilon).pass,"permanent bypass falsely positive");
                Image zero(x.size(),{0,0,0});
                expect(!edit(x,output,zero,roi,calibration.epsilon).pass,"zero denominator positive fixture");
                for(auto p:roi) output[p]={x[p].r+boundedEdit/2,x[p].g+boundedEdit/2,x[p].b+boundedEdit/2};
                expect(!edit(x,output,delta,roi,calibration.epsilon).pass,"lost edit energy admitted");
            }
        }
        const std::array<GuidePoint,3> calibrationPoints{{
            {{64.0/255,32.0/255,16.0/255},{63.0/255,32.0/255,16.0/255},1.03125,0,1,0,true},
            {{.25,.5,.75},{.25,.5,.75},-8,0,-8,0,true},
            {{.25,.5,.75},{.25,.5,.75},32,0,32,0,true}}};
        const auto c=calibrate_guides(calibrationPoints);
        for(const auto& p:calibrationPoints) expect(guide_point(p,c),"calibrated guide rejected");
        expect(c.absoluteError==.03125&&c.relativeError==.03125,"independent vector max calibration");
        expect(decoded_vector(-256,1)==-8&&decoded_vector(1,1)==.03125&&decoded_vector(256,.5)==4,"sign/five fraction bits/single rectangle scale");
        auto wrong=calibrationPoints[1];wrong.x=8;
        expect(!guide_point(wrong,c),"wrong sign guide admitted");
        wrong=calibrationPoints[1];wrong.warpedReference={0,0,0};
        expect(!guide_point(wrong,c),"stale photometric guide admitted");
        const std::array<bool,4> supported{true,true,true,true}, invalid{true,false,true,true};
        expect(valid_support(supported)&&!valid_support(invalid),"invalid contributing grid support ignored");
        std::printf("CPU_ORACLE GREEN clean=%u RED blend_2percent=%u blur_1pixel=%u frozen_epsilon_candidate_independent=true\n",
            cleanAdmitted,blendRejected,blurRejected);
        std::printf("PASS ghost_oracle checks=%u CPU_only=true actual_OFA_calibration=false candidate_quality=false\n",checks);
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL ghost_oracle %s\n",e.what());return 1;}
}
