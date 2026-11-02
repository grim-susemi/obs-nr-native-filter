// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_freeze.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <string_view>

namespace nr::ghost {
namespace {
bool sha256(std::string_view value) {
    return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
void value(std::ostream& out,std::optional<double> number) {
    if(!number){out<<"null";return;}
    if(!std::isfinite(*number)||*number<0)throw std::invalid_argument("nonfinite physical measurement");
    out<<std::setprecision(17)<<*number;
}
void binding(std::ostream& out,const FreezePolicy& policy,const std::string& host,const std::string& source) {
    validate_freeze(policy);
    if(!sha256(host)||!sha256(source))throw std::invalid_argument("physical source binding absent");
    out<<"\"schema\":2,\"identity_digest_algorithm\":\"sha256-chunk-root-v1\","
        "\"evidence_class\":\"physical\",\"host_sha256\":\""<<host<<"\",\"host_source_sha256\":\""<<source
        <<"\",\"policy_hashes\":{";
    for(size_t i=0;i<freeze_files.size();++i) {
        if(i)out<<',';
        out<<'"'<<freeze_files[i]<<"\":\""<<policy.hashes[i]<<'"';
    }
    out<<"},\"manifest_sha256\":\""<<policy.manifestHash<<'"';
}
}
void validate_freeze(const FreezePolicy& p) {
    for(size_t i=0;i<freeze_hashes.size();++i)
        if(p.hashes[i]!=freeze_hashes[i])throw std::invalid_argument("ghost freeze hash mismatch");
    if(p.calibrationSeed!=2583||p.holdoutSeed!=2857||p.calibrationSeed==p.holdoutSeed||p.ticks!=256||
        p.rateNumerators!=std::array<uint32_t,2>{60,60000}||p.rateDenominators!=std::array<uint32_t,2>{1,1001}||
        p.width!=192||p.height!=96||p.visibleTop!=8||p.physicalWidth!=1920||p.physicalHeight!=1080||
        p.editRetention!=.8||p.temporalFactor!=temporal_epsilon_factor||p.detailFactor!=detail_epsilon_factor||
        p.editFactor!=edit_epsilon_factor||p.staticEdit!=std::array<uint32_t,4>{256,128,32,32}||p.movingInset!=8||
        p.manifestHash!="d8674dc102d4f1c36b3ffdb6001c27363b65face0dd8e66a4c32f1430a1cd213")
        throw std::invalid_argument("ghost freeze cohort/mask/threshold mismatch");
}
bool measured(const PhysicalLane& lane) {
    const std::array<std::optional<double>,5> values{
        lane.formatBound,lane.rawMaximum,lane.guideMismatch,lane.absoluteVector,lane.relativeVector};
    return lane.rawSamples==256&&lane.guideSamples>0&&lane.guidePoints>0&&lane.modelSamples>0&&
        lane.codecFrames==32&&lane.codecErrors==0&&lane.closed&&lane.offCurrentSamples>0&&lane.offInstance>0&&
        std::all_of(values.begin(),values.end(),[](const auto& v){return v&&std::isfinite(*v)&&*v>=0;});
}
void collect_off(PhysicalLane& lane,const OffObservation& off) {
    const auto extent=off.current.extent;
    const auto bytes=static_cast<size_t>(extent.width)*extent.height*off.bytesPerPixel;
    if(off.configurationEnabled||off.filterEnabled||off.kind!=OutputKind::CurrentSource||
        !off.settingsRevision||!off.currentKey.valid()||off.outputKey!=off.currentKey||
        !off.expectedTick||off.currentTick!=off.expectedTick||off.outputTick!=off.expectedTick||
        (lane.offInstance&&lane.offInstance!=off.currentKey.instanceId)||
        off.currentTick<=lane.offTick||off.currentKey.ingressSequence<=lane.offIngress||
        !off.current.pixels||!off.output.pixels||!off.current.read||!off.output.read||
        extent!=off.output.extent||extent.width<Fixture::width||extent.height<Fixture::height||
        (off.bytesPerPixel!=4&&off.bytesPerPixel!=8)||off.currentBytes.size()!=bytes||
        off.outputBytes.size()!=bytes||!std::equal(off.currentBytes.begin(),off.currentBytes.end(),off.outputBytes.begin())||
        off.sourceTick>=Fixture::ticks||!std::isfinite(off.epsilon)||off.epsilon<0)
        throw std::runtime_error("disabled current-output OFF proof absent or mismatched");
    Image current,output;
    current.reserve(Fixture::width*Fixture::height);output.reserve(Fixture::width*Fixture::height);
    for(unsigned y=0;y<Fixture::height;++y)for(unsigned x=0;x<Fixture::width;++x) {
        current.push_back(off.current.read(off.current.pixels,x,y));
        output.push_back(off.output.read(off.output.pixels,x,y));
    }
    if(!decoded_landmarks(off.fixture,off.sourceTick,current,off.epsilon)||
        !decoded_landmarks(off.fixture,off.sourceTick,output,off.epsilon))
        throw std::runtime_error("OFF current-source pixel identity absent");
    lane.offInstance=off.currentKey.instanceId;lane.offIngress=off.currentKey.ingressSequence;
    lane.offTick=off.currentTick;++lane.offCurrentSamples;
}
void require_calibration(const std::array<PhysicalLane,8>& lanes) {
    if(!std::all_of(lanes.begin(),lanes.end(),measured))
        throw std::runtime_error("unmeasured physical calibration; holdout refused");
}
void require_controls(const std::array<PhysicalControl,9>& controls,uint64_t anchors) {
    if(anchors!=8||!std::all_of(controls.begin(),controls.end(),[](const auto& c){
        return c.cases==8&&c.rejected&&*c.rejected&&c.actualPixels;}))
        throw std::runtime_error("unmeasured or failed physical controls; holdout refused");
}
std::array<PhysicalControl,9> calibration_controls(const ControlInput& in) {
    if(in.tick<2||in.tick>=Fixture::ticks||in.fixture.schedule[in.tick].held||
        in.current.size()!=Fixture::width*Fixture::height||in.previous.size()!=in.current.size()||
        in.guideCurrent.size()!=in.current.size()||in.guidePrevious.size()!=in.current.size()||
        !decoded_landmarks(in.fixture,in.tick,in.current,in.calibration.epsilon)||
        !decoded_landmarks(in.fixture,in.tick-1,in.previous,in.calibration.epsilon)||
        decode_content(in.guideCurrent)!=in.fixture.schedule[in.tick].contentId||
        decode_content(in.guidePrevious)!=in.fixture.schedule[in.tick-1].contentId||
        in.graphicsMask!=63||in.selectedKey.instanceId!=in.currentKey.instanceId||
        in.selectedKey.contentEpoch!=in.currentKey.contentEpoch||
        in.selectedKey.ingressSequence+1!=in.currentKey.ingressSequence||
        in.completedKey!=in.expectedCompleted||in.guideKey.frame!=in.currentKey||
        in.referenceKey.frame.instanceId!=in.currentKey.instanceId||
        in.referenceKey.frame.contentEpoch!=in.currentKey.contentEpoch||
        in.referenceKey.frame.ingressSequence+1!=in.currentKey.ingressSequence||
        in.referenceKey.historyEpoch!=in.guideKey.historyEpoch||
        in.referenceKey.resourceGeneration!=in.guideKey.resourceGeneration)
        throw std::invalid_argument("physical control positive anchor absent");
    std::array<PhysicalControl,9> result{};
    auto record=[&](size_t index,bool rejected){result[index]={1,rejected,in.physicalPixels};};
    auto blend=in.current,blur=in.current,poststamp=in.previous;
    for(size_t p=0;p<blend.size();++p)
        blend[p]={.98*in.current[p].r+.02*in.previous[p].r,
            .98*in.current[p].g+.02*in.previous[p].g,.98*in.current[p].b+.02*in.previous[p].b};
    record(0,!temporal(in.fixture,in.tick,in.current,blend,in.calibration).pass);
    for(unsigned y=8;y<Fixture::height;++y)for(unsigned x=1;x+1<Fixture::width;++x) {
        const size_t p=y*Fixture::width+x;
        blur[p]={(in.current[p-1].r+in.current[p].r+in.current[p+1].r)/3,
            (in.current[p-1].g+in.current[p].g+in.current[p+1].g)/3,
            (in.current[p-1].b+in.current[p].b+in.current[p+1].b)/3};
    }
    record(1,!detail(in.current,blur,detail_edges(in.fixture,in.tick),in.calibration.epsilon));
    record(2,!decoded_landmarks(in.fixture,in.tick,in.previous,in.calibration.epsilon));
    const FrameKey extraDelay{in.selectedKey.instanceId,in.selectedKey.contentEpoch,in.selectedKey.ingressSequence-1};
    record(3,extraDelay!=in.selectedKey||extraDelay.ingressSequence+1!=in.currentKey.ingressSequence);
    record(4,(in.graphicsMask&~uint64_t{1})!=63);
    record(5,decode_content(in.guidePrevious)!=in.fixture.schedule[in.tick].contentId);
    const JobKey relabeled{in.guideKey.frame,in.referenceKey.historyEpoch,
        in.referenceKey.resourceGeneration,in.referenceKey.submissionSerial};
    record(6,relabeled.frame.ingressSequence+1!=in.guideKey.frame.ingressSequence);
    const JobKey reordered{in.completedKey.frame,in.completedKey.historyEpoch,
        in.completedKey.resourceGeneration,in.completedKey.submissionSerial+1};
    record(7,reordered!=in.expectedCompleted);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<Fixture::width;++x)
        poststamp[y*Fixture::width+x]=in.current[y*Fixture::width+x];
    record(8,!decoded_landmarks(in.fixture,in.tick,poststamp,in.calibration.epsilon));
    return result;
}
void write_calibration(std::ostream& out,const FreezePolicy& p,const std::array<PhysicalLane,8>& lanes,
    const std::string& host,const std::string& source) {
    const bool complete=std::all_of(lanes.begin(),lanes.end(),measured);
    uint64_t offSamples=0;
    for(const auto& lane:lanes)offSamples+=lane.offCurrentSamples;
    out<<'{';binding(out,p,host,source);
    out<<",\"calibration_seed\":"<<p.calibrationSeed<<",\"holdout_seed\":"<<p.holdoutSeed
        <<",\"holdout_disjoint\":true,\"raw_only_frozen\":"<<(complete?"true":"false")
        <<",\"measurement_status\":\""<<(complete?"measured":"unmeasured")<<"\",\"qualification_allowed\":false"
        <<",\"all_prior_templates\":true,\"image_decoded_source_identity\":"<<(complete?"true":"false")
        <<",\"actual_loaded_runtime\":"<<(complete?"true":"false")
        <<",\"normal_cleanup\":"<<(complete?"true":"false")<<",\"off_current_samples\":"<<offSamples<<",\"lanes\":[";
    for(size_t i=0;i<lanes.size();++i) {
        if(i)out<<',';
        const auto& lane=lanes[i];
        out<<"{\"rate_index\":"<<i/4<<",\"mode\":"<<i%4<<",\"measured\":"<<(measured(lane)?"true":"false")
            <<",\"raw_samples\":"<<lane.rawSamples<<",\"guide_samples\":"<<lane.guideSamples
            <<",\"off_current_samples\":"<<lane.offCurrentSamples<<",\"off_instance\":"<<lane.offInstance
            <<",\"off_ingress\":"<<lane.offIngress<<",\"off_tick\":"<<lane.offTick
            <<",\"guide_points\":"<<lane.guidePoints<<",\"model_samples\":"<<lane.modelSamples
            <<",\"codec_frames\":"<<lane.codecFrames<<",\"codec_code_errors\":";
        if(lane.codecFrames)out<<lane.codecErrors;else out<<"null";
        out<<",\"format_bound\":";value(out,lane.formatBound);
        out<<",\"raw_maximum\":";value(out,lane.rawMaximum);
        out<<",\"epsilon\":";
        value(out,lane.formatBound&&lane.rawMaximum?std::optional<double>{std::max(*lane.formatBound,*lane.rawMaximum)}:std::nullopt);
        out<<",\"guide_mismatch\":";value(out,lane.guideMismatch);
        out<<",\"absolute_vector_error\":";value(out,lane.absoluteVector);
        out<<",\"relative_vector_error\":";value(out,lane.relativeVector);out<<'}';
    }
    out<<"]}\n";
    if(!out.good())throw std::runtime_error("physical calibration serialization failed");
}
void write_controls(std::ostream& out,const FreezePolicy& p,const std::array<PhysicalControl,9>& controls,
    uint64_t anchors,const std::string& host,const std::string& source) {
    const bool complete=anchors==8&&std::all_of(controls.begin(),controls.end(),[](const auto& c){
        return c.cases==8&&c.rejected.has_value()&&c.actualPixels;});
    const bool passed=complete&&std::all_of(controls.begin(),controls.end(),[](const auto& c){return *c.rejected;});
    out<<'{';binding(out,p,host,source);
    out<<",\"measurement_status\":\""<<(complete?"measured":"unmeasured")
        <<"\",\"positive_anchors\":"<<anchors<<",\"physical_pixel_mutations_not_gpu_submissions\":true"
        <<",\"control_evidence_log_prefix\":\"NR_GHOST_PHYSICAL_CONTROL \""
        <<",\"qualification_allowed\":"<<(passed?"true":"false")<<",\"controls\":[";
    for(size_t i=0;i<controls.size();++i) {
        if(i)out<<',';
        const auto& c=controls[i];
        out<<"{\"name\":\""<<physical_control_names[i]<<"\",\"cases\":"<<c.cases
            <<",\"actual_pixels\":"<<(c.actualPixels?"true":"false")<<",\"rejected\":";
        if(c.cases&&c.rejected)out<<(*c.rejected?"true":"false");else out<<"null";
        out<<'}';
    }
    out<<"]}\n";
    if(!out.good())throw std::runtime_error("physical controls serialization failed");
}
}
