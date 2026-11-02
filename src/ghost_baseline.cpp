// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_baseline.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace nr::ghost::baseline {
namespace {
[[noreturn]] void refuse(const std::string& why) {throw std::invalid_argument("ghost baseline refused: "+why);}
void need(bool condition,const char* why) {if(!condition)refuse(why);}

// --- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------------------
constexpr std::array<uint32_t,64> kRound{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
uint32_t rotr(uint32_t value,unsigned bits) {return (value>>bits)|(value<<(32-bits));}
void compress(std::array<uint32_t,8>& state,const uint8_t* block) {
    std::array<uint32_t,64> w{};
    for(size_t i=0;i<16;++i)
        w[i]=(uint32_t{block[i*4]}<<24)|(uint32_t{block[i*4+1]}<<16)|(uint32_t{block[i*4+2]}<<8)|uint32_t{block[i*4+3]};
    for(size_t i=16;i<64;++i) {
        const uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        const uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    auto v=state;
    for(size_t i=0;i<64;++i) {
        const uint32_t s1=rotr(v[4],6)^rotr(v[4],11)^rotr(v[4],25);
        const uint32_t choose=(v[4]&v[5])^(~v[4]&v[6]);
        const uint32_t t1=v[7]+s1+choose+kRound[i]+w[i];
        const uint32_t s0=rotr(v[0],2)^rotr(v[0],13)^rotr(v[0],22);
        const uint32_t majority=(v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]);
        const uint32_t t2=s0+majority;
        v[7]=v[6];v[6]=v[5];v[5]=v[4];v[4]=v[3]+t1;v[3]=v[2];v[2]=v[1];v[1]=v[0];v[0]=t1+t2;
    }
    for(size_t i=0;i<8;++i)state[i]+=v[i];
}

// --- JSON reader --------------------------------------------------------------------------------------------
class Reader {
public:
    explicit Reader(std::string_view text):s_(text) {}
    Json document() {
        auto value=parse(0);
        skip();
        if(at_!=s_.size())throw std::invalid_argument("json trailing bytes");
        return value;
    }
private:
    std::string_view s_;
    size_t at_=0;
    void skip() {while(at_<s_.size()&&(s_[at_]==' '||s_[at_]=='\n'||s_[at_]=='\r'||s_[at_]=='\t'))++at_;}
    bool take(char c) {skip();if(at_<s_.size()&&s_[at_]==c){++at_;return true;}return false;}
    void expect(char c) {if(!take(c))throw std::invalid_argument("json syntax");}
    bool literal(std::string_view word) {
        if(s_.substr(at_,word.size())!=word)return false;
        at_+=word.size();return true;
    }
    std::string string() {
        expect('"');
        std::string out;
        while(at_<s_.size()&&s_[at_]!='"') {
            char c=s_[at_++];
            if(c=='\\') {
                if(at_>=s_.size())throw std::invalid_argument("json escape");
                c=s_[at_++];
                switch(c) {
                case '"':case '\\':case '/':break;
                case 'b':c='\b';break;
                case 'f':c='\f';break;
                case 'n':c='\n';break;
                case 'r':c='\r';break;
                case 't':c='\t';break;
                case 'u':
                    if(at_+4>s_.size())throw std::invalid_argument("json unicode");
                    at_+=4;c='?';break;
                default:throw std::invalid_argument("json escape");
                }
            }
            out.push_back(c);
        }
        if(at_>=s_.size())throw std::invalid_argument("json unterminated string");
        ++at_;
        return out;
    }
    Json parse(unsigned depth) {
        if(depth>32)throw std::invalid_argument("json depth");
        skip();
        if(at_>=s_.size())throw std::invalid_argument("json eof");
        Json value;
        const char c=s_[at_];
        if(c=='{') {
            ++at_;value.kind=Json::Kind::Object;
            if(take('}'))return value;
            do {
                auto name=string();expect(':');
                value.members.emplace_back(std::move(name),parse(depth+1));
            } while(take(','));
            expect('}');
        }else if(c=='[') {
            ++at_;value.kind=Json::Kind::Array;
            if(take(']'))return value;
            do value.items.push_back(parse(depth+1)); while(take(','));
            expect(']');
        }else if(c=='"') {
            value.kind=Json::Kind::String;value.text=string();
        }else if(literal("true")) {
            value.kind=Json::Kind::Bool;value.boolean=true;
        }else if(literal("false")) {
            value.kind=Json::Kind::Bool;
        }else if(literal("null")) {
            value.kind=Json::Kind::Null;
        }else {
            const size_t begin=at_;
            while(at_<s_.size()&&(std::isdigit(static_cast<unsigned char>(s_[at_]))||s_[at_]=='-'||s_[at_]=='+'||
                s_[at_]=='.'||s_[at_]=='e'||s_[at_]=='E'))++at_;
            if(begin==at_)throw std::invalid_argument("json value");
            double number=0;
            const auto result=std::from_chars(s_.data()+begin,s_.data()+at_,number);
            if(result.ec!=std::errc{}||result.ptr!=s_.data()+at_||!std::isfinite(number))throw std::invalid_argument("json number");
            value.kind=Json::Kind::Number;value.number=number;
        }
        return value;
    }
};

bool hex64(std::string_view value) {
    return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
const Json& member(const Json& object,const char* key,Json::Kind kind) {
    need(object.kind==Json::Kind::Object,"measured document is not an object");
    const auto* value=object.find(key);
    if(!value||value->kind!=kind)refuse(std::string("measured document field absent or mistyped: ")+key);
    return *value;
}
double number(const Json& object,const char* key) {
    const double value=member(object,key,Json::Kind::Number).number;
    if(!std::isfinite(value)||value<0)refuse(std::string("measured number out of range: ")+key);
    return value;
}
bool flag(const Json& object,const char* key) {return member(object,key,Json::Kind::Bool).boolean;}
const std::string& text(const Json& object,const char* key) {return member(object,key,Json::Kind::String).text;}

void binding(const Json& document,const FreezePolicy& policy,const char* what) {
    need(number(document,"schema")==2,what);
    need(text(document,"identity_digest_algorithm")=="sha256-chunk-root-v1",what);
    need(text(document,"evidence_class")=="physical",what);
    need(hex64(text(document,"host_sha256"))&&hex64(text(document,"host_source_sha256")),what);
    const auto& hashes=member(document,"policy_hashes",Json::Kind::Object);
    need(hashes.members.size()==freeze_files.size(),"policy hash set is not the five-file freeze");
    for(size_t i=0;i<freeze_files.size();++i) {
        const auto* value=hashes.find(freeze_files[i]);
        need(value&&value->kind==Json::Kind::String&&value->text==policy.hashes[i],
            "accepted calibration policy hash differs from the compiled/loaded freeze");
    }
    need(text(document,"manifest_sha256")==policy.manifestHash,"accepted calibration manifest hash differs");
}
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value;
}
unsigned slot(unsigned prior,unsigned region) {return prior*2+region;}
void put(std::ostream& out,double value) {
    if(!std::isfinite(value))throw std::runtime_error("ghost baseline receipt: nonfinite measurement");
    out<<value;
}
}

std::string sha256_hex(std::span<const uint8_t> data) {
    std::array<uint32_t,8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const size_t whole=data.size()/64;
    for(size_t block=0;block<whole;++block)compress(state,data.data()+block*64);
    std::array<uint8_t,128> tail{};
    const size_t rest=data.size()-whole*64;
    for(size_t i=0;i<rest;++i)tail[i]=data[whole*64+i];
    tail[rest]=0x80;
    const size_t total=rest+1+8<=64?64:128;
    const uint64_t bits=static_cast<uint64_t>(data.size())*8;
    for(size_t i=0;i<8;++i)tail[total-1-i]=static_cast<uint8_t>(bits>>(8*i));
    for(size_t offset=0;offset<total;offset+=64)compress(state,tail.data()+offset);
    static constexpr char digits[]="0123456789abcdef";
    std::string out;out.reserve(64);
    for(auto word:state)for(int shift=28;shift>=0;shift-=4)out.push_back(digits[(word>>shift)&15u]);
    return out;
}

const Json* Json::find(std::string_view key) const {
    for(const auto& entry:members)if(entry.first==key)return &entry.second;
    return nullptr;
}
Json parse_json(std::string_view text) {return Reader(text).document();}

AcceptedCalibration accept_calibration(std::string_view calibrationJson,std::string_view controlsJson,
    std::string_view acceptedCalibrationSha256,std::string_view acceptedControlsSha256,const FreezePolicy& policy) {
    need(hex64(acceptedCalibrationSha256)&&hex64(acceptedControlsSha256),
        "accepted Task26 calibration/controls hash argument absent or malformed");
    const auto calibrationHash=sha256_hex(calibrationJson),controlsHash=sha256_hex(controlsJson);
    need(calibrationHash==acceptedCalibrationSha256,"calibration bytes differ from the accepted Task26 hash");
    need(controlsHash==acceptedControlsSha256,"controls bytes differ from the accepted Task26 hash");
    try {validate_freeze(policy);}
    catch(const std::exception& error) {refuse(std::string("policy: ")+error.what());}
    Json calibration,controls;
    try {calibration=parse_json(calibrationJson);controls=parse_json(controlsJson);}
    catch(const std::exception& error) {refuse(std::string("measured document is not JSON: ")+error.what());}
    binding(calibration,policy,"calibration is not the schema-2 physical Task26 document");
    need(text(calibration,"measurement_status")=="measured"&&flag(calibration,"raw_only_frozen")&&
        flag(calibration,"holdout_disjoint")&&!flag(calibration,"qualification_allowed")&&
        flag(calibration,"all_prior_templates")&&flag(calibration,"image_decoded_source_identity")&&
        flag(calibration,"actual_loaded_runtime")&&flag(calibration,"normal_cleanup")&&
        number(calibration,"off_current_samples")>0,"Task26 calibration is not measured and raw-only frozen");
    need(number(calibration,"calibration_seed")==policy.calibrationSeed&&number(calibration,"holdout_seed")==policy.holdoutSeed,
        "calibration seeds differ from the frozen cohort");
    const auto& lanes=member(calibration,"lanes",Json::Kind::Array);
    need(lanes.items.size()==8,"calibration lane set is not 2 rates x 4 modes");
    AcceptedCalibration accepted;
    for(size_t i=0;i<lanes.items.size();++i) {
        const auto& lane=lanes.items[i];
        need(number(lane,"rate_index")==static_cast<double>(i/4)&&number(lane,"mode")==static_cast<double>(i%4),
            "calibration lane order");
        need(flag(lane,"measured")&&number(lane,"raw_samples")==256&&number(lane,"codec_frames")==32&&
            number(lane,"codec_code_errors")==0&&number(lane,"guide_points")>0&&number(lane,"model_samples")>0&&
            number(lane,"off_current_samples")>0,"calibration lane unmeasured");
        auto& threshold=accepted.lanes[i];
        threshold.formatBound=number(lane,"format_bound");
        threshold.rawMaximum=number(lane,"raw_maximum");
        threshold.guideMismatch=number(lane,"guide_mismatch");
        threshold.absoluteVector=number(lane,"absolute_vector_error");
        threshold.relativeVector=number(lane,"relative_vector_error");
        need(number(lane,"epsilon")==threshold.epsilon(),"calibration epsilon is not max(format,raw)");
    }
    binding(controls,policy,"controls is not the schema-2 physical Task26 document");
    need(text(controls,"measurement_status")=="measured"&&number(controls,"positive_anchors")==8&&
        flag(controls,"qualification_allowed"),"Task26 controls unmeasured or not all rejected");
    need(text(controls,"host_sha256")==text(calibration,"host_sha256")&&
        text(controls,"host_source_sha256")==text(calibration,"host_source_sha256"),
        "controls and calibration come from different hosts");
    const auto& list=member(controls,"controls",Json::Kind::Array);
    need(list.items.size()==physical_control_names.size(),"Task26 control set is not the nine planted controls");
    for(size_t i=0;i<list.items.size();++i) {
        const auto& control=list.items[i];
        need(text(control,"name")==physical_control_names[i]&&number(control,"cases")==8&&
            flag(control,"actual_pixels")&&flag(control,"rejected"),"a planted Task26 control was not rejected on real pixels");
    }
    accepted.calibrationSha256=calibrationHash;
    accepted.controlsSha256=controlsHash;
    accepted.hostSha256=text(calibration,"host_sha256");
    accepted.hostSourceSha256=text(calibration,"host_source_sha256");
    return accepted;
}

bool frozen_file_name(std::string_view filename) {
    const auto name=lower(std::string(filename));
    if(name=="bad-controls.json")return true;
    return std::any_of(freeze_files.begin(),freeze_files.end(),[&](const char* frozen){return name==frozen;});
}
void require_baseline_output(const std::filesystem::path& file) {
    if(file.filename().empty()||frozen_file_name(file.filename().string()))
        throw std::invalid_argument("ghost baseline refused: freeze file write");
    for(const auto& part:file) {
        const auto name=lower(part.string());
        if(name.rfind("task-26",0)==0||name=="foundation-20261014")
            throw std::invalid_argument("ghost baseline refused: write inside Task26 freeze evidence");
    }
}
std::ofstream open_output(const std::filesystem::path& file) {
    require_baseline_output(file);
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    if(!out.good())throw std::runtime_error("ghost baseline output unavailable");
    return out;
}

const char* rung_name(Rung rung) {
    switch(rung) {
    case Rung::Raw:return "raw";
    case Rung::Binary:return "binary";
    case Rung::UnprotectedNr:return "unprotected_nr";
    }
    return "unknown";
}

void Tally::frame(unsigned kind,uint64_t ingress,uint64_t selected,uint64_t instanceId) {
    ++ticks;
    if(kind<kinds.size())++kinds[kind];else ++keyAnomalies;
    if(rung==Rung::Raw?kind!=static_cast<unsigned>(OutputKind::CurrentSource):kind<1||kind>3)++keyAnomalies;
    if(!ingress||!instanceId)++keyAnomalies;
    if(ticks==1){firstIngress=ingress;instance=instanceId;}
    else if(ingress!=lastIngress+1||instanceId!=instance)++keyAnomalies;
    lastIngress=ingress;
    if(kind<=1?selected!=ingress:selected+1!=ingress)++keyAnomalies;
}
void Tally::off_frame(const OffObservation& off,double maximumError) {
    if(rung!=Rung::Raw||off.sourceTick!=ticks||off.epsilon!=epsilon)
        throw std::runtime_error("raw OFF episode phase or frozen epsilon mismatch");
    PhysicalLane proof;
    proof.offCurrentSamples=offCurrentSamples;proof.offInstance=instance;
    proof.offIngress=lastIngress;proof.offTick=lastOffTick;
    collect_off(proof,off);
    frame(static_cast<unsigned>(off.kind),off.currentKey.ingressSequence,off.outputKey.ingressSequence,off.currentKey.instanceId);
    offCurrentSamples=proof.offCurrentSamples;lastOffTick=proof.offTick;
    raw_transport(off.sourceTick,maximumError,true); // collect_off independently checks both actual current images.
}
void Tally::temporal(unsigned tick,const TemporalReport& report) {
    bool failedTick=false;
    for(const auto& score:report.templates) {
        ++templates;
        if(score.zeroContrast){++zeroContrast;continue;}
        auto& stat=stats.at(slot(score.priorTick,score.region));
        ++stat.scored;
        const double judged=gateRaw?score.raw:score.incremental;
        const bool good=judged<=score.bound;
        const double ratio=score.bound>0?judged/score.bound:(judged>0?std::numeric_limits<double>::max():0);
        maxIncremental=std::max(maxIncremental,score.incremental);
        maxRaw=std::max(maxRaw,score.raw);
        maxTotal=std::max(maxTotal,score.total);
        if(ratio>=maxRatio) {
            maxRatio=ratio;
            worst={tick,score.priorTick,score.region,score.incremental,score.raw,score.total,score.bound};
        }
        stat.maxIncremental=std::max(stat.maxIncremental,score.incremental);
        stat.maxRaw=std::max(stat.maxRaw,score.raw);
        stat.maxTotal=std::max(stat.maxTotal,score.total);
        if(judged>=(gateRaw?stat.maxRaw:stat.maxIncremental))stat.boundAtMax=score.bound;
        if(good){stat.current=0;continue;}
        failedTick=true;
        ++templateFailures;
        if(!stat.failures)stat.firstFailureTick=tick;
        ++stat.failures;
        stat.longest=std::max(stat.longest,++stat.current);
        longestPersistence=std::max(longestPersistence,stat.longest);
        if(failures.size()<kListedFailures)
            failures.push_back({tick,score.priorTick,score.region,score.incremental,score.raw,score.total,score.bound});
    }
    if(failedTick)++failedTicks;
}
void Tally::raw_transport(unsigned,double maximumError,bool sceneIdentity) {
    ++rawChecks;
    maxRawError=std::max(maxRawError,maximumError);
    if(!sceneIdentity||!(maximumError<=epsilon))++rawFailures;
}
void Tally::edge(unsigned tick,double maximumLoss,uint64_t checks) {
    edgeChecks+=checks;
    maxEdgeLoss=std::max(maxEdgeLoss,maximumLoss);
    if(!(maximumLoss<=detail_epsilon_factor*epsilon)) {
        if(!edgeFailures)firstEdgeFailureTick=tick;
        ++edgeFailures;
    }
}
void Tally::edit(unsigned tick,bool moving,bool referenceValid,double rms,double retained) {
    ++editSamples;
    maxEditRms=std::max(maxEditRms,rms);
    if(!referenceValid)return;
    ++editValid;
    minValidRetention=std::min(minValidRetention,retained);
    if(rms>edit_epsilon_factor*epsilon&&retained>=.8){if(moving)++movingPositive;else ++staticPositive;return;}
    if(!editFailures)firstEditFailureTick=tick;
    ++editFailures;
}
bool Tally::pass() const {
    if(!complete()||templateFailures)return false;
    if(gateRaw&&(rawFailures||rawChecks!=ticks||offCurrentSamples!=ticks||kinds[0]!=ticks))return false;
    if(edgeFailures||!edgeChecks)return false;
    if(gateEdit&&(editFailures||!editValid||!staticPositive||!movingPositive))return false;
    if(codecRequired&&(codecFrames!=32||codecErrors))return false;
    if(gateGuides&&(!nativeGuides||!guidePoints||guideMismatch>guideBound.guideMismatch||
        absoluteVector>guideBound.absoluteVector||relativeVector>guideBound.relativeVector))return false;
    return true;
}

Inherited attribute_inherited(const Fixture& f,const Calibration& calibration) {
    Inherited result;
    result.mode=static_cast<unsigned>(f.color);
    for(unsigned tick=1;tick<Fixture::ticks;++tick) {
        const auto inherited=inherited_image(f,tick);
        const auto report=temporal(f,tick,inherited,inherited,calibration);
        ++result.ticks;
        result.templates+=report.templates.size();
        result.maxIncremental=std::max(result.maxIncremental,report.maximum);
        result.maxRaw=std::max(result.maxRaw,report.maximumRaw);
        result.maxTotal=std::max(result.maxTotal,report.maximumTotal);
        if(!report.pass||report.maximum!=0)result.pluginAttributed=true;
        for(const auto& score:report.templates)if(score.raw!=score.total)result.rawEqualsTotal=false;
    }
    return result;
}

bool controls_valid(const Summary& summary) {
    return summary.anchors==8&&std::all_of(summary.controls.begin(),summary.controls.end(),[](const PhysicalControl& c){
        return c.cases==8&&c.rejected&&*c.rejected&&c.actualPixels;});
}
bool rung_pass(const Summary& summary,Rung rung) {
    unsigned seen=0;
    for(const auto& tally:summary.tallies) {
        if(tally.rung!=rung)continue;
        ++seen;
        if(!tally.pass())return false;
    }
    return seen==8;
}
bool baseline_valid(const Summary& summary) {
    return summary.faults.empty()&&summary.tallies.size()==24&&rung_pass(summary,Rung::Raw)&&
        rung_pass(summary,Rung::Binary)&&controls_valid(summary);
}
bool nr_pass(const Summary& summary) {return rung_pass(summary,Rung::UnprotectedNr);}
const char* result_label(const Summary& summary) {
    if(!baseline_valid(summary))return "BASELINE_INVALID";
    return nr_pass(summary)?"BASELINE_VALID_NR_PASS":"BASELINE_VALID_NR_DEFECTS";
}

std::vector<Defect> collect_defects(const Summary& summary) {
    std::vector<Defect> out;
    for(const auto& t:summary.tallies) {
        if(t.pass())continue;
        const std::string where=std::string(rung_name(t.rung))+"-r"+std::to_string(t.rateIndex)+"m"+std::to_string(t.mode);
        const bool task28=t.rung==Rung::UnprotectedNr;
        const auto add=[&](std::string id,const char* category,std::string detail,unsigned tick,unsigned prior,unsigned region,
            unsigned persistence,uint64_t occurrences,double measured,double bound) {
            Defect d;
            d.id=std::move(id);d.rung=rung_name(t.rung);d.category=category;d.detail=std::move(detail);
            d.rateIndex=t.rateIndex;d.mode=t.mode;d.tick=tick;d.prior=prior;d.region=region;d.persistence=persistence;
            d.occurrences=occurrences;d.measured=measured;d.bound=bound;d.forTask28=task28;
            out.push_back(std::move(d));
        };
        if(!t.complete())
            add("CK-"+where,"CADENCE_KEYS","episode is not 256 ordered keyed rows",0,0,0,0,t.keyAnomalies+(t.ticks==256?0:1),
                static_cast<double>(t.ticks),256);
        for(size_t key=0;key<t.stats.size();++key) {
            const auto& s=t.stats[key];
            if(!s.failures)continue;
            const unsigned prior=static_cast<unsigned>(key/2),region=static_cast<unsigned>(key%2);
            add("TT-"+where+"-p"+std::to_string(prior)+"g"+std::to_string(region),
                t.gateRaw?"RAW_TRAIL":"TEMPORAL_TRAIL",
                t.gateRaw?"raw trail G exceeds 2*epsilon*norm1/norm2^2":"incremental G exceeds 2*epsilon*norm1/norm2^2",
                s.firstFailureTick,prior,region,s.longest,s.failures,t.gateRaw?s.maxRaw:s.maxIncremental,s.boundAtMax);
        }
        if(t.gateRaw&&t.rawFailures)
            add("RT-"+where,"RAW_TRANSPORT","raw scene identity or per-channel error outside epsilon",0,0,0,0,t.rawFailures,
                t.maxRawError,t.epsilon);
        if(t.edgeFailures||!t.edgeChecks)
            add("EL-"+where,"EDGE_LOSS","signed 1px channel contrast loss exceeds 2*epsilon",t.firstEdgeFailureTick,0,0,0,
                t.edgeFailures,t.maxEdgeLoss,detail_epsilon_factor*t.epsilon);
        if(t.gateEdit&&t.editFailures)
            add("EE-"+where,"EDIT_ENERGY","edit RMS or retained D energy below 8*epsilon / 0.8",t.firstEditFailureTick,0,0,0,
                t.editFailures,t.minValidRetention,.8);
        if(t.gateEdit&&(!t.editValid||!t.staticPositive||!t.movingPositive))
            add("EP-"+where,"EDIT_NO_POSITIVE","no edit-positive static/moving cohort",0,0,0,0,
                t.editValid,static_cast<double>(std::min(t.staticPositive,t.movingPositive)),1);
        if(t.codecRequired&&(t.codecFrames!=32||t.codecErrors))
            add("CD-"+where,"CODEC","decoded output differs from preencode beyond the codec cohort",0,0,0,0,t.codecErrors,
                static_cast<double>(t.codecErrors),0);
        if(t.gateGuides) {
            if(!t.nativeGuides||!t.guidePoints)
                add("GN-"+where,"GUIDE_ABSENT","no ordinary native guide cohort",0,0,0,0,t.nativeGuides,0,1);
            if(t.guideMismatch>t.guideBound.guideMismatch)
                add("GM-"+where,"GUIDE_MISMATCH","visible-surface mismatch above the frozen residual",0,0,0,0,t.guidePoints,
                    t.guideMismatch,t.guideBound.guideMismatch);
            if(t.absoluteVector>t.guideBound.absoluteVector||t.relativeVector>t.guideBound.relativeVector)
                add("GV-"+where,"GUIDE_VECTOR","SDK vector error above the frozen calibration",0,0,0,0,t.guidePoints,
                    std::max(t.absoluteVector,t.relativeVector),std::max(t.guideBound.absoluteVector,t.guideBound.relativeVector));
        }
    }
    return out;
}

namespace {
class Out {
public:
    explicit Out(std::ostream& stream):o_(stream) {o_<<std::setprecision(17);}
    void begin_object() {sep();o_<<'{';first_.push_back(true);}
    void end_object() {o_<<'}';first_.pop_back();}
    void begin_array() {sep();o_<<'[';first_.push_back(true);}
    void end_array() {o_<<']';first_.pop_back();}
    void key(std::string_view name) {sep();quote(name);o_<<':';key_=true;}
    void str(std::string_view value) {sep();quote(value);}
    void num(double value) {sep();put(o_,value);}
    void u64(uint64_t value) {sep();o_<<value;}
    void boolean(bool value) {sep();o_<<(value?"true":"false");}
    void null() {sep();o_<<"null";}
    template<class V> void field(std::string_view name,V value) {
        key(name);
        if constexpr(std::is_same_v<V,bool>)boolean(value);
        else if constexpr(std::is_floating_point_v<V>)num(value);
        else if constexpr(std::is_integral_v<V>)u64(static_cast<uint64_t>(value));
        else str(value);
    }
private:
    std::ostream& o_;
    std::vector<bool> first_;
    bool key_=false;
    void sep() {
        if(key_){key_=false;return;}
        if(first_.empty())return;
        if(first_.back())first_.back()=false;else o_<<',';
    }
    void quote(std::string_view value) {
        o_<<'"';
        for(char c:value) {
            if(c=='"'||c=='\\')o_<<'\\'<<c;
            else if(static_cast<unsigned char>(c)<0x20)o_<<' ';
            else o_<<c;
        }
        o_<<'"';
    }
};
void write_tally(Out& j,const Tally& t) {
    j.begin_object();
    j.field("rung",rung_name(t.rung));j.field("rate_index",t.rateIndex);j.field("mode",t.mode);
    j.field("pass",t.pass());j.field("epsilon",t.epsilon);
    j.field("ticks",t.ticks);j.field("key_anomalies",t.keyAnomalies);j.field("instance",t.instance);
    j.field("first_ingress",t.firstIngress);j.field("last_ingress",t.lastIngress);
    j.key("kinds");j.begin_object();
    j.field("current_source",t.kinds[0]);
    j.field("priming_raw",t.kinds[1]);j.field("delayed_raw",t.kinds[2]);j.field("delayed_nr",t.kinds[3]);
    j.end_object();
    j.key("temporal");j.begin_object();
    j.field("judged",t.gateRaw?"raw_trail":"incremental_trail");
    j.field("templates",t.templates);j.field("zero_contrast",t.zeroContrast);j.field("failures",t.templateFailures);
    j.field("failed_ticks",t.failedTicks);j.field("maximum_incremental",t.maxIncremental);
    j.field("maximum_raw",t.maxRaw);j.field("maximum_total",t.maxTotal);j.field("maximum_ratio",t.maxRatio);
    j.field("longest_persistence",t.longestPersistence);
    j.key("worst");j.begin_object();
    j.field("tick",t.worst.tick);j.field("prior",t.worst.prior);j.field("region",t.worst.region);
    j.field("incremental",t.worst.incremental);j.field("raw",t.worst.raw);j.field("total",t.worst.total);
    j.field("bound",t.worst.bound);j.end_object();
    j.key("first_failures");j.begin_array();
    for(const auto& f:t.failures) {
        j.begin_object();
        j.field("tick",f.tick);j.field("prior",f.prior);j.field("region",f.region);
        j.field("incremental",f.incremental);j.field("raw",f.raw);j.field("total",f.total);j.field("bound",f.bound);
        j.end_object();
    }
    j.end_array();j.end_object();
    j.key("raw_transport");j.begin_object();
    j.field("checks",t.rawChecks);j.field("failures",t.rawFailures);j.field("maximum_error",t.maxRawError);
    j.field("off_current_samples",t.offCurrentSamples);
    j.end_object();
    j.key("edge");j.begin_object();
    j.field("checks",t.edgeChecks);j.field("failures",t.edgeFailures);j.field("maximum_loss",t.maxEdgeLoss);
    j.field("bound",detail_epsilon_factor*t.epsilon);j.end_object();
    j.key("edit");j.begin_object();
    j.field("samples",t.editSamples);j.field("valid",t.editValid);j.field("failures",t.editFailures);
    j.field("static_positive",t.staticPositive);j.field("moving_positive",t.movingPositive);
    j.key("minimum_retention");
    if(std::isfinite(t.minValidRetention))j.num(t.minValidRetention);else j.null();
    j.field("maximum_rms",t.maxEditRms);j.end_object();
    j.key("codec");j.begin_object();
    j.field("required",t.codecRequired);j.field("frames",t.codecFrames);j.field("code_errors",t.codecErrors);
    j.end_object();
    j.key("guides");j.begin_object();
    j.field("native",t.nativeGuides);j.field("points",t.guidePoints);j.field("model_samples",t.modelSamples);
    j.field("mismatch",t.guideMismatch);j.field("absolute_vector_error",t.absoluteVector);
    j.field("relative_vector_error",t.relativeVector);
    j.field("frozen_mismatch",t.guideBound.guideMismatch);j.field("frozen_absolute_vector_error",t.guideBound.absoluteVector);
    j.field("frozen_relative_vector_error",t.guideBound.relativeVector);j.end_object();
    j.end_object();
}
}

void write_summary(std::ostream& stream,const Summary& s) {
    Out j(stream);
    const bool valid=baseline_valid(s);
    j.begin_object();
    j.field("schema",1u);j.field("kind","GHOST_BASELINE");j.field("evidence_class","physical");
    j.field("test_not_release",true);j.field("identity_digest_algorithm","sha256-chunk-root-v1");
    j.field("host_sha256",s.hostSha256);j.field("host_source_sha256",s.hostSourceSha256);
    j.field("holdout_seed",s.policy.holdoutSeed);j.field("calibration_seed_measured_here",false);
    j.key("freeze");j.begin_object();
    j.key("policy_hashes");j.begin_object();
    for(size_t i=0;i<freeze_files.size();++i)j.field(freeze_files[i],s.policy.hashes[i]);
    j.end_object();
    j.field("manifest_sha256",s.policy.manifestHash);j.field("read_only",true);j.field("files_written",0u);
    j.end_object();
    j.key("accepted_calibration");j.begin_object();
    j.field("calibration_sha256",s.accepted.calibrationSha256);j.field("controls_sha256",s.accepted.controlsSha256);
    j.field("host_sha256",s.accepted.hostSha256);j.field("host_source_sha256",s.accepted.hostSourceSha256);
    j.end_object();
    j.field("result",result_label(s));j.field("baseline_valid",valid);
    j.field("raw_pass",rung_pass(s,Rung::Raw));j.field("binary_pass",rung_pass(s,Rung::Binary));
    j.field("controls_pass",controls_valid(s));j.field("unprotected_nr_pass",nr_pass(s));
    j.key("faults");j.begin_array();
    for(const auto& fault:s.faults)j.str(fault);
    j.end_array();
    j.key("rungs");j.begin_array();
    for(const auto& t:s.tallies)write_tally(j,t);
    j.end_array();
    j.key("bad_controls");j.begin_array();
    for(size_t i=0;i<s.controls.size();++i) {
        const auto& c=s.controls[i];
        j.begin_object();
        j.field("name",physical_control_names[i]);j.field("cases",c.cases);j.field("actual_pixels",c.actualPixels);
        j.key("rejected");
        if(c.cases&&c.rejected)j.boolean(*c.rejected);else j.null();
        j.end_object();
    }
    j.end_array();
    j.field("positive_anchors",s.anchors);
    j.key("inherited_trail_attribution");j.begin_array();
    for(const auto& a:s.inherited) {
        j.begin_object();
        j.field("label","analytic_inherited_trail_X=0.9C_t+0.1C_t-1");j.field("mode",a.mode);j.field("ticks",a.ticks);
        j.field("episode_ticks",Fixture::ticks);j.field("first_scored_tick",1u);j.field("last_scored_tick",Fixture::ticks-1);
        j.field("templates",a.templates);j.field("maximum_incremental",a.maxIncremental);
        j.field("maximum_raw",a.maxRaw);j.field("maximum_total",a.maxTotal);
        j.field("raw_equals_total",a.rawEqualsTotal);j.field("attributed_to_plugin",a.pluginAttributed);
        j.end_object();
    }
    j.end_array();
    j.key("defects");j.begin_array();
    for(const auto& d:collect_defects(s)) {
        j.begin_object();
        j.field("id",d.id);j.field("rung",d.rung);j.field("category",d.category);j.field("detail",d.detail);
        j.field("rate_index",d.rateIndex);j.field("mode",d.mode);j.field("tick",d.tick);j.field("prior",d.prior);
        j.field("region",d.region);j.field("persistence",d.persistence);j.field("occurrences",d.occurrences);
        j.field("measured",d.measured);j.field("bound",d.bound);j.field("for_task28",d.forTask28);
        j.end_object();
    }
    j.end_array();
    j.end_object();
    stream<<'\n';
    if(!stream.good())throw std::runtime_error("ghost baseline receipt serialization failed");
}

double edge_loss(const Image& clean,const Image& output,std::span<const Edge> edges) {
    if(clean.size()!=output.size())throw std::invalid_argument("edge loss image extent mismatch");
    double worst=0;
    for(const auto& edge:edges) {
        if(edge.a>=clean.size()||edge.b>=clean.size())throw std::invalid_argument("edge loss extent");
        const std::array<double,3> before{clean[edge.a].r-clean[edge.b].r,clean[edge.a].g-clean[edge.b].g,clean[edge.a].b-clean[edge.b].b};
        const std::array<double,3> after{output[edge.a].r-output[edge.b].r,output[edge.a].g-output[edge.b].g,output[edge.a].b-output[edge.b].b};
        for(size_t channel=0;channel<3;++channel)
            if(before[channel]!=0)worst=std::max(worst,std::abs(before[channel])-(before[channel]>0?after[channel]:-after[channel]));
    }
    return worst;
}
Admission admit_sample(const Fixture& f,unsigned expectedTick,const Image& raw,const Image& output,const Calibration& calibration) {
    Admission result;
    result.identity=decoded_landmarks(f,expectedTick,output,calibration.epsilon);
    result.trail=temporal(f,expectedTick,raw,output,calibration).pass;
    result.detail=detail(clean_image(f,expectedTick),output,detail_edges(f,expectedTick),calibration.epsilon);
    return result;
}
std::optional<unsigned> plantable_tick(const Fixture& f) {
    for(unsigned tick=3;tick+1<Fixture::ticks;++tick) {
        const std::array<unsigned,4> ids{f.schedule[tick].contentId,f.schedule[tick-1].contentId,
            f.schedule[tick-2].contentId,f.schedule[tick+1].contentId};
        bool distinct=true;
        for(size_t a=0;a<ids.size();++a)for(size_t b=a+1;b<ids.size();++b)distinct=distinct&&ids[a]!=ids[b];
        if(distinct)return tick;
    }
    return std::nullopt;
}
std::array<PlantedControl,7> plant_controls(const Fixture& f,unsigned tick,const Calibration& calibration) {
    if(tick<3||tick+1>=Fixture::ticks)throw std::invalid_argument("planted control tick");
    const auto x=clean_image(f,tick),prior=clean_image(f,tick-1);
    auto blend=x,blur=x;
    for(size_t p=0;p<blend.size();++p)
        blend[p]={.98*x[p].r+.02*prior[p].r,.98*x[p].g+.02*prior[p].g,.98*x[p].b+.02*prior[p].b};
    for(unsigned y=8;y<Fixture::height;++y)for(unsigned px=1;px+1<Fixture::width;++px) {
        const size_t p=y*Fixture::width+px;
        blur[p]={(x[p-1].r+x[p].r+x[p+1].r)/3,(x[p-1].g+x[p].g+x[p+1].g)/3,(x[p-1].b+x[p].b+x[p+1].b)/3};
    }
    return {{
        {"raw_current",admit_sample(f,tick,x,x,calibration)},
        {"binary_delayed",admit_sample(f,tick-1,prior,prior,calibration)},
        {"blend_2percent",admit_sample(f,tick,x,blend,calibration)},
        {"blur_1pixel",admit_sample(f,tick,x,blur,calibration)},
        {"frozen",admit_sample(f,tick,x,prior,calibration)},
        {"extra_delay",admit_sample(f,tick,x,clean_image(f,tick-2),calibration)},
        {"dropped",admit_sample(f,tick,x,clean_image(f,tick+1),calibration)}}};
}
}
