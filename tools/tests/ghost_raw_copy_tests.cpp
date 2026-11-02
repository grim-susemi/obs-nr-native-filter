// SPDX-License-Identifier: GPL-3.0-only
// Common-API regression: this exact source runs against both the reviewed old and revised libraries.
#include "ghost_baseline.h"
#include <cstdio>
using namespace nr::ghost;
using namespace nr::ghost::baseline;
int main() {
    const auto f=fixture(0xB29,FixtureColor::Sdr);
    const Calibration calibration{1e-6,1e-6,1e-6};
    const auto tick=*plantable_tick(f);
    const auto clean=clean_image(f,tick),prior=clean_image(f,tick-1);
    auto off=clean;
    for(size_t p=0;p<off.size();++p)off[p]={.98*clean[p].r+.02*prior[p].r,
        .98*clean[p].g+.02*prior[p].g,.98*clean[p].b+.02*prior[p].b};
    const auto rawReport=temporal(f,tick,off,off,calibration);
    const auto nrReport=temporal(f,tick-1,prior,prior,calibration);
    Tally current(Rung::Raw,0,0,calibration.epsilon),copied(Rung::Raw,0,0,calibration.epsilon);
    for(unsigned t=0;t<Fixture::ticks;++t) {
        const uint64_t ingress=1000+t;
        current.frame(static_cast<unsigned>(nr::OutputKind::CurrentSource),ingress,ingress,9);
        copied.frame(t==0?1u:3u,ingress,t==0?ingress:ingress-1,9);
        current.raw_transport(t,0,true);copied.raw_transport(t,0,true);
        current.edge(t,0,1);copied.edge(t,0,1);
    }
    current.temporal(tick,rawReport);
    copied.temporal(tick-1,nrReport);
    current.codecFrames=copied.codecFrames=32;
    const bool regressionClosed=current.complete()&&!copied.pass()&&current.templateFailures>0&&
        rawReport.maximumRaw>.019&&nrReport.maximumRaw==0;
    std::printf("RAW_COPY_REGRESSION genuine_OFF_complete=%u OFF_key_anomalies=%llu copied_NR_pass=%u copied_key_anomalies=%llu raw_G=%.17g NR_raw_G=%.17g closed=%u\n",
        current.complete(),static_cast<unsigned long long>(current.keyAnomalies),copied.pass(),
        static_cast<unsigned long long>(copied.keyAnomalies),rawReport.maximumRaw,nrReport.maximumRaw,regressionClosed);
    return regressionClosed?0:1;
}
