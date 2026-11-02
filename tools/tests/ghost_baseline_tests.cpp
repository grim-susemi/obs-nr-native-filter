// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_baseline.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
using namespace nr::ghost;
using namespace nr::ghost::baseline;
namespace {
unsigned checks=0;
void expect(bool condition,const char* why) {++checks;if(!condition)throw std::runtime_error(why);}
template<class F> void refused(F&& action,const char* why) {
    ++checks;
    try {action();}
    catch(const std::invalid_argument& error) {
        if(std::string_view(error.what()).rfind("ghost baseline refused",0)==0)return;
        throw std::runtime_error(std::string("wrong refusal for ")+why+": "+error.what());
    }
    throw std::runtime_error(std::string("not refused: ")+why);
}

FreezePolicy policy() {
    FreezePolicy p;
    p.calibrationSeed=2583;p.holdoutSeed=2857;p.ticks=256;
    p.rateNumerators={60,60000};p.rateDenominators={1,1001};
    p.width=192;p.height=96;p.visibleTop=8;p.physicalWidth=1920;p.physicalHeight=1080;
    p.editRetention=.8;p.temporalFactor=temporal_epsilon_factor;p.detailFactor=detail_epsilon_factor;
    p.editFactor=edit_epsilon_factor;p.staticEdit={256,128,32,32};p.movingInset=8;
    for(size_t i=0;i<freeze_hashes.size();++i)p.hashes[i]=freeze_hashes[i];
    p.manifestHash="d8674dc102d4f1c36b3ffdb6001c27363b65face0dd8e66a4c32f1430a1cd213";
    return p;
}
const std::string hostHash(64,'a'),sourceHash(64,'b');
std::array<PhysicalLane,8> lanes() {
    std::array<PhysicalLane,8> result{};
    for(size_t i=0;i<result.size();++i) {
        auto& lane=result[i];
        lane.rawSamples=256;lane.guideSamples=200;lane.guidePoints=1500;lane.modelSamples=200;lane.codecFrames=32;
        lane.codecErrors=0;lane.closed=true;lane.offCurrentSamples=1;lane.offInstance=7+i;lane.offIngress=300;lane.offTick=300;
        lane.formatBound=1e-6*static_cast<double>(i+1);lane.rawMaximum=2e-6;lane.guideMismatch=.01*static_cast<double>(i+1);
        lane.absoluteVector=.03125;lane.relativeVector=.03125;
    }
    return result;
}
std::array<PhysicalControl,9> controls() {
    std::array<PhysicalControl,9> result{};
    for(auto& c:result){c.cases=8;c.rejected=true;c.actualPixels=true;}
    return result;
}
struct Docs {std::string calibration,controls;};
Docs documents(const FreezePolicy& p,const std::array<PhysicalLane,8>& laneSet,const std::array<PhysicalControl,9>& controlSet,uint64_t anchors) {
    std::ostringstream a,b;
    write_calibration(a,p,laneSet,hostHash,sourceHash);
    write_controls(b,p,controlSet,anchors,hostHash,sourceHash);
    return {a.str(),b.str()};
}
void replace(std::string& text,std::string_view from,std::string_view to) {
    const auto at=text.find(from);
    if(at==std::string::npos)throw std::runtime_error("test document lacks "+std::string(from));
    text.replace(at,from.size(),to);
}
AcceptedCalibration accept(const Docs& d,const FreezePolicy& p) {
    return accept_calibration(d.calibration,d.controls,sha256_hex(d.calibration),sha256_hex(d.controls),p);
}

// A per-tick replay of the real oracle, shared by every synthetic lane.
struct Replay {
    std::vector<TemporalReport> reports;
    std::vector<double> edge;
};
Replay replay(const Fixture& f,const Calibration& calibration,bool blend,bool blur) {
    Replay result;
    result.reports.resize(Fixture::ticks);
    result.edge.assign(Fixture::ticks,0);
    for(unsigned tick=0;tick<Fixture::ticks;++tick) {
        const auto x=clean_image(f,tick);
        auto y=x;
        if(blend&&tick>0) {
            const auto prior=clean_image(f,tick-1);
            for(size_t p=0;p<y.size();++p)
                y[p]={.98*x[p].r+.02*prior[p].r,.98*x[p].g+.02*prior[p].g,.98*x[p].b+.02*prior[p].b};
        }
        if(blur)
            for(unsigned row=8;row<Fixture::height;++row)for(unsigned px=1;px+1<Fixture::width;++px) {
                const size_t p=row*Fixture::width+px;
                y[p]={(x[p-1].r+x[p].r+x[p+1].r)/3,(x[p-1].g+x[p].g+x[p+1].g)/3,(x[p-1].b+x[p].b+x[p+1].b)/3};
            }
        result.reports[tick]=temporal(f,tick,x,y,calibration);
        const auto edges=detail_edges(f,tick);
        result.edge[tick]=edge_loss(x,y,edges);
    }
    return result;
}
void off_frame(Tally& t,const Fixture& f,unsigned tick,const Image& current,const Image& output) {
    const auto pack=[](const Image& image) {
        std::vector<uint8_t> bytes(image.size()*4,255);
        for(size_t i=0;i<image.size();++i) {
            const std::array<double,3> rgb{image[i].r,image[i].g,image[i].b};
            for(size_t c=0;c<3;++c)bytes[i*4+c]=static_cast<uint8_t>(std::lround(std::clamp(rgb[c],0.0,1.0)*255));
        }
        return bytes;
    };
    const auto a=pack(current),b=pack(output);
    const auto clean=clean_image(f,tick);
    double maximum=0;
    for(size_t p=0;p<output.size();++p)maximum=std::max({maximum,std::abs(output[p].r-clean[p].r),
        std::abs(output[p].g-clean[p].g),std::abs(output[p].b-clean[p].b)});
    const auto read=[](const void* pixels,unsigned x,unsigned y){return static_cast<const Image*>(pixels)->at(y*Fixture::width+x);};
    const nr::FrameKey key{9,1,1000+tick};
    t.off_frame({f,tick,key,key,2000+tick,2000+tick,2000+tick,1,false,false,nr::OutputKind::CurrentSource,
        {&current,{Fixture::width,Fixture::height},read},{&output,{Fixture::width,Fixture::height},read},a,b,4,t.epsilon},maximum);
}
Tally make(Rung rung,unsigned rate,unsigned mode,const Replay& r,double epsilon) {
    Tally t(rung,rate,mode,epsilon);
    const auto f=fixture(0xB29,FixtureColor::Sdr);
    for(unsigned tick=0;tick<Fixture::ticks;++tick) {
        const uint64_t ingress=1000+tick;
        const auto selectedTick=rung==Rung::Raw||tick==0?tick:tick-1;
        if(rung==Rung::Raw) {
            const auto current=clean_image(f,tick),output=current;
            off_frame(t,f,tick,current,output);
        }else t.frame(tick==0?1u:3u,ingress,tick==0?ingress:ingress-1,9);
        t.temporal(selectedTick,r.reports[selectedTick]);
        t.edge(selectedTick,r.edge[selectedTick],100);
        if(rung==Rung::UnprotectedNr&&tick>8) {
            t.edit(tick,false,true,1,1);
            t.edit(tick,true,true,1,1);
        }
    }
    t.codecFrames=32;t.codecErrors=0;
    t.nativeGuides=10;t.guidePoints=100;t.modelSamples=10;
    t.guideBound={1e-6,1e-6,1,1,1};
    t.guideMismatch=.5;t.absoluteVector=.5;t.relativeVector=.5;
    return t;
}
Summary summary(const Tally& raw,const Tally& binary,const Tally& nr) {
    Summary s;
    s.hostSha256=hostHash;s.hostSourceSha256=sourceHash;s.policy=policy();
    for(unsigned rate=0;rate<2;++rate)for(unsigned mode=0;mode<4;++mode)
        for(const Tally* t:{&raw,&binary,&nr}) {
            auto copy=*t;copy.rateIndex=rate;copy.mode=mode;s.tallies.push_back(std::move(copy));
        }
    s.controls=controls();s.anchors=8;
    s.accepted.calibrationSha256=std::string(64,'c');s.accepted.controlsSha256=std::string(64,'d');
    s.accepted.hostSha256=hostHash;s.accepted.hostSourceSha256=std::string(64,'e');
    return s;
}
const char* find_value(const Json& j,const char* key) {
    const auto* v=j.find(key);
    return v&&v->kind==Json::Kind::String?v->text.c_str():"";
}
}

int main(int argc,char** argv) {
    try {
        // SHA-256 vectors (FIPS 180-4) and the multi-block padding boundary.
        expect(sha256_hex(std::string_view{})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","sha256 empty");
        expect(sha256_hex(std::string_view{"abc"})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","sha256 abc");
        expect(sha256_hex(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","sha256 two-block message");
        expect(sha256_hex(std::string(64,'a'))=="ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb","sha256 exactly one block");
        expect(sha256_hex(std::string(55,'a'))=="9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318","sha256 55 bytes");
        expect(sha256_hex(std::string(56,'a'))=="b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a","sha256 56 bytes");

        // Accepted-calibration gate: GREEN on a fully measured Task26 document pair.
        const auto p=policy();
        const auto good=documents(p,lanes(),controls(),8);
        const auto accepted=accept(good,p);
        expect(accepted.calibrationSha256==sha256_hex(good.calibration)&&accepted.controlsSha256==sha256_hex(good.controls),"accepted hashes recorded");
        expect(accepted.hostSha256==hostHash&&accepted.hostSourceSha256==sourceHash,"calibration host binding recorded");
        for(size_t i=0;i<8;++i) {
            expect(accepted.lanes[i].formatBound==1e-6*static_cast<double>(i+1)&&accepted.lanes[i].rawMaximum==2e-6,"lane thresholds round trip bit exactly");
            expect(accepted.lanes[i].guideMismatch==.01*static_cast<double>(i+1)&&accepted.lanes[i].absoluteVector==.03125,"guide thresholds round trip");
        }
        // RED: holdout refused without the accepted hash.
        refused([&]{accept_calibration(good.calibration,good.controls,"",sha256_hex(good.controls),p);},"empty calibration hash argument");
        refused([&]{accept_calibration(good.calibration,good.controls,sha256_hex(good.calibration),"",p);},"empty controls hash argument");
        refused([&]{accept_calibration(good.calibration,good.controls,std::string(64,'0'),sha256_hex(good.controls),p);},"wrong calibration hash");
        refused([&]{accept_calibration(good.calibration,good.controls,sha256_hex(good.calibration),std::string(64,'0'),p);},"wrong controls hash");
        auto upper=sha256_hex(good.calibration);for(auto& c:upper)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        refused([&]{accept_calibration(good.calibration,good.controls,upper,sha256_hex(good.controls),p);},"malformed (uppercase) hash argument");
        {auto edited=good;edited.calibration.back()=' ';edited.calibration+=" ";
         refused([&]{accept_calibration(edited.calibration,edited.controls,sha256_hex(good.calibration),sha256_hex(good.controls),p);},"calibration bytes changed after acceptance");}
        {auto bad=p;bad.hashes[3][0]=bad.hashes[3][0]=='0'?'1':'0';
         refused([&]{accept(good,bad);},"loaded policy differs from the compiled Task26 freeze");}
        {auto bad=p;bad.holdoutSeed=2583;
         refused([&]{accept(good,bad);},"calibration seed reused as holdout");}
        {auto d=good;replace(d.calibration,"\"measurement_status\":\"measured\"","\"measurement_status\":\"unmeasured\"");
         refused([&]{accept(d,p);},"unmeasured calibration");}
        {auto d=good;replace(d.calibration,"\"raw_only_frozen\":true","\"raw_only_frozen\":false");
         refused([&]{accept(d,p);},"calibration not raw-only frozen");}
        {auto d=good;replace(d.calibration,"\"qualification_allowed\":false","\"qualification_allowed\":true");
         refused([&]{accept(d,p);},"calibration claiming qualification");}
        {auto d=good;replace(d.calibration,"\"manifest_sha256\":\""+p.manifestHash+"\"","\"manifest_sha256\":\""+std::string(64,'1')+"\"");
         refused([&]{accept(d,p);},"calibration manifest differs");}
        {auto d=good;replace(d.calibration,std::string("\"")+freeze_files[1]+"\":\""+freeze_hashes[1]+"\"",std::string("\"")+freeze_files[1]+"\":\""+std::string(64,'2')+"\"");
         refused([&]{accept(d,p);},"calibration binds a different holdout.json");}
        {auto laneSet=lanes();laneSet[5].guideMismatch.reset();
         refused([&]{accept(documents(p,laneSet,controls(),8),p);},"one lane has no measured guide mismatch");}
        {auto laneSet=lanes();laneSet[2].codecErrors=1;
         refused([&]{accept(documents(p,laneSet,controls(),8),p);},"codec errors in calibration");}
        {auto controlSet=controls();controlSet[4].rejected=false;
         refused([&]{accept(documents(p,lanes(),controlSet,8),p);},"dropped control not rejected");}
        {auto controlSet=controls();controlSet[0].actualPixels=false;
         refused([&]{accept(documents(p,lanes(),controlSet,8),p);},"control without actual pixels");}
        refused([&]{accept(documents(p,lanes(),controls(),7),p);},"seven of eight positive anchors");
        {auto d=good;replace(d.controls,std::string(64,'a'),std::string(64,'f'));
         refused([&]{accept(d,p);},"controls from a different host");}
        refused([&]{accept_calibration("not json","{}",sha256_hex("not json"),sha256_hex("{}"),p);},"non-json calibration");

        // Freeze files are read-only here: names and the Task26 evidence tree are unwritable.
        for(const char* name:freeze_files)refused([&]{require_baseline_output(std::filesystem::path("out")/name);},name);
        refused([&]{require_baseline_output("out/bad-controls.json");},"bad-controls.json");
        refused([&]{require_baseline_output("out/Calibration.JSON");},"case-insensitive calibration.json");
        refused([&]{require_baseline_output("E/task-26/ghost-artifacts/attempt-1/anything.json");},"write inside task-26 evidence");
        refused([&]{require_baseline_output("E/task-26-verifier/x.json");},"write inside task-26-verifier");
        require_baseline_output("E/task-27/ghost-artifacts/attempt-1/ghost-baseline.json");
        require_baseline_output("E/task-27/ghost-artifacts/attempt-1/pass-1-rate-0-mode-0/templates.tsv");
        ++checks;
        const auto temp=argc==3&&std::string_view(argv[1])=="--emit"?std::filesystem::path(argv[2])/"scratch":
            std::filesystem::temp_directory_path()/("ghost_baseline_tests_output_"+sha256_hex(std::string_view(argv[0])).substr(0,16));
        std::filesystem::remove_all(temp);std::filesystem::create_directories(temp);
        refused([&]{auto out=open_output(temp/"calibration.json");},"open_output on calibration.json");
        expect(!std::filesystem::exists(temp/"calibration.json"),"refused freeze write created a file");
        {auto out=open_output(temp/"ghost-baseline.json");out<<"{}";}
        expect(std::filesystem::exists(temp/"ghost-baseline.json"),"allowed output not created");

        // Planted controls: raw and binary admitted; 2% blend, 1px blur, frozen, extra-delayed, dropped rejected.
        const Calibration calibration{1e-6,1e-6,1e-6};
        for(auto color:{FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
            const auto holdout=fixture(0xB29,color);
            const auto tick=plantable_tick(holdout);
            expect(tick.has_value(),"holdout has a plantable tick");
            const auto planted=plant_controls(holdout,*tick,calibration);
            expect(planted[0].admission.pass(),"raw control admitted");
            expect(planted[1].admission.pass(),"binary control admitted");
            expect(!planted[2].admission.trail&&!planted[2].admission.pass(),"2 percent blend admitted");
            expect(!planted[3].admission.detail&&!planted[3].admission.pass(),"1 pixel blur admitted");
            expect(!planted[4].admission.identity&&!planted[4].admission.pass(),"frozen sample admitted");
            expect(!planted[5].admission.identity&&!planted[5].admission.pass(),"extra delayed sample admitted");
            expect(!planted[6].admission.identity&&!planted[6].admission.pass(),"dropped sample admitted");
            std::printf("CPU_PLANTED color=%u tick=%u raw=%u binary=%u blend_rejected=%u blur_rejected=%u frozen_rejected=%u extra_delay_rejected=%u dropped_rejected=%u\n",
                static_cast<unsigned>(color),*tick,planted[0].admission.pass(),planted[1].admission.pass(),
                !planted[2].admission.pass(),!planted[3].admission.pass(),!planted[4].admission.pass(),
                !planted[5].admission.pass(),!planted[6].admission.pass());
        }

        // Tallies replay the real oracle on the holdout fixture.
        const auto holdout=fixture(0xB29,FixtureColor::Sdr);
        const double epsilon=1e-6;
        const auto cleanReplay=replay(holdout,calibration,false,false);
        const auto blendReplay=replay(holdout,calibration,true,false);
        const auto blurReplay=replay(holdout,calibration,false,true);
        const auto raw=make(Rung::Raw,0,0,cleanReplay,epsilon),binary=make(Rung::Binary,0,0,cleanReplay,epsilon),
            nr=make(Rung::UnprotectedNr,0,0,cleanReplay,epsilon);
        expect(raw.pass()&&binary.pass()&&nr.pass(),"clean raw/binary/unprotected controls pass");
        expect(raw.templates>0&&raw.templateFailures==0&&raw.maxRatio==0,"clean trail scored every template");
        expect(raw.kinds[0]==256&&raw.kinds[3]==0&&raw.offCurrentSamples==256&&nr.kinds[0]==0&&nr.kinds[3]==255,
            "raw OFF keys copied from delayed NR");
        expect(raw.templates!=nr.templates,"current raw and n-1 NR incorrectly scored at the same phase");
        // RED: a byte-for-byte reproduction of the old copied-key tally must never pass as raw.
        Tally copiedRaw(Rung::Raw,0,0,epsilon);
        for(unsigned tick=0;tick<Fixture::ticks;++tick) {
            const uint64_t ingress=1000+tick;
            copiedRaw.frame(tick==0?1u:3u,ingress,tick==0?ingress:ingress-1,9);
            copiedRaw.raw_transport(tick,0,true);
            const auto selectedTick=tick==0?0:tick-1;
            copiedRaw.temporal(selectedTick,cleanReplay.reports[selectedTick]);
            copiedRaw.edge(selectedTick,cleanReplay.edge[selectedTick],100);
        }
        copiedRaw.codecFrames=32;
        expect(!copiedRaw.pass()&&copiedRaw.keyAnomalies>0,"copied delayed NR raw tally accepted");
        // Stronger counter-case: NR's selected n-1 image is clean, but actual OFF/current output is a 2% trail.
        // Feeding NR's clean tally instead would lose the only offending pixels.
        const auto differentTick=*plantable_tick(holdout);
        Tally independentRaw(Rung::Raw,0,0,epsilon);
        for(unsigned tick=0;tick<differentTick;++tick) {
            const auto current=clean_image(holdout,tick);
            off_frame(independentRaw,holdout,tick,current,current);
        }
        const auto current=clean_image(holdout,differentTick),prior=clean_image(holdout,differentTick-1);
        auto offOutput=current;
        for(size_t pixel=0;pixel<offOutput.size();++pixel)offOutput[pixel]={.98*current[pixel].r+.02*prior[pixel].r,
            .98*current[pixel].g+.02*prior[pixel].g,.98*current[pixel].b+.02*prior[pixel].b};
        const auto delayedNr=admit_sample(holdout,differentTick-1,prior,prior,calibration);
        bool offRejected=false;
        try{off_frame(independentRaw,holdout,differentTick,current,offOutput);}
        catch(const std::runtime_error&){offRejected=true;}
        expect(delayedNr.pass()&&offRejected,"independent current output was replaced with clean delayed NR");
        const auto offTrail=temporal(holdout,differentTick,offOutput,offOutput,calibration);
        expect(offTrail.maximumRaw>.019&&cleanReplay.reports[differentTick-1].maximumRaw==0,
            "divergent raw/NR trail regression has no independently measured difference");
        std::printf("CPU_RAW_INDEPENDENCE current=%llu delayed_nr=%llu current_templates=%llu nr_templates=%llu copied_rejected=%u nr_clean=%u off_blend_rejected=%u raw_G=%.17g\n",
            static_cast<unsigned long long>(raw.kinds[0]),static_cast<unsigned long long>(nr.kinds[3]),
            static_cast<unsigned long long>(raw.templates),static_cast<unsigned long long>(nr.templates),!copiedRaw.pass(),
            delayedNr.pass(),offRejected,offTrail.maximumRaw);
        const auto badBlend=make(Rung::UnprotectedNr,0,0,blendReplay,epsilon);
        expect(!badBlend.pass()&&badBlend.templateFailures>0&&badBlend.longestPersistence>=1,"planted 2 percent blend passes as unprotected NR");
        expect(badBlend.maxIncremental>.019&&badBlend.maxRatio>1,"blend G at least the analytic two percent and above the bound");
        const auto badBlur=make(Rung::Binary,0,0,blurReplay,epsilon);
        expect(!badBlur.pass()&&badBlur.edgeFailures>0&&badBlur.maxEdgeLoss>detail_epsilon_factor*epsilon,"planted 1px blur passes as binary");
        {auto t=binary;t.edge(9,1e-3,1);expect(!t.pass(),"edge loss over 2*epsilon accepted");}
        {auto t=raw;t.raw_transport(9,2e-6,true);expect(!t.pass(),"raw error over epsilon accepted");}
        {auto t=raw;t.raw_transport(9,0,false);expect(!t.pass(),"raw scene identity failure accepted");}
        {auto t=nr;t.codecErrors=1;expect(!t.pass(),"codec error accepted");}
        {auto t=raw;t.codecFrames=0;expect(!t.pass(),"raw without independent codec cohort accepted");}
        {auto t=raw;t.offCurrentSamples=1;expect(!t.pass(),"single OFF witness substituted for finite raw cohort");}
        {auto t=nr;t.edit(300,true,true,1e-9,1);expect(!t.pass()&&t.editFailures==1,"sub-8-epsilon edit accepted");}
        {auto t=nr;t.edit(300,false,true,1,.5);expect(!t.pass(),"edit retaining 50 percent accepted");}
        {auto t=nr;t.edit(300,false,false,0,0);expect(t.pass()&&t.editSamples==nr.editSamples+1,"restore-invalid D sample must not count as a failure");}
        {auto t=nr;t.guideMismatch=2;expect(!t.pass(),"guide mismatch above frozen bound accepted");}
        {auto t=nr;t.frame(3,5000,4999,9);expect(!t.pass(),"extra tick accepted");}
        {Tally t(Rung::Binary,0,0,epsilon);for(unsigned tick=0;tick<255;++tick)t.frame(3,1000+tick,999+tick,9);expect(!t.complete(),"dropped sample not detected in cadence");}
        {Tally t(Rung::Binary,0,0,epsilon);t.frame(3,1000,999,9);t.frame(3,1002,1001,9);expect(t.keyAnomalies==1,"missing source ID not detected");}
        {Tally t(Rung::Binary,0,0,epsilon);t.frame(3,1000,999,9);t.frame(3,1000,999,9);expect(t.keyAnomalies==1,"repeated source ID not detected");}

        // Summary verdicts and the machine DEFECTS list.
        auto good24=summary(raw,binary,nr);
        expect(baseline_valid(good24)&&nr_pass(good24)&&collect_defects(good24).empty(),"clean ladder not valid");
        expect(std::string_view(result_label(good24))=="BASELINE_VALID_NR_PASS","clean label");
        auto defects24=summary(raw,binary,badBlend);
        expect(baseline_valid(defects24)&&!nr_pass(defects24),"unprotected NR failure must keep the baseline valid");
        const auto defects=collect_defects(defects24);
        expect(!defects.empty()&&std::all_of(defects.begin(),defects.end(),[](const Defect& d){return d.forTask28&&d.rung=="unprotected_nr";}),"defects not all Task28 NR");
        expect(std::any_of(defects.begin(),defects.end(),[](const Defect& d){return d.category=="TEMPORAL_TRAIL"&&d.persistence>=1&&d.occurrences>0&&d.measured>d.bound;}),"temporal defect lacks measurement");
        expect(std::string_view(result_label(defects24))=="BASELINE_VALID_NR_DEFECTS","defects label");
        auto badRaw=summary(raw,binary,nr);badRaw.tallies[0].raw_transport(1,1,true);
        expect(!baseline_valid(badRaw)&&std::string_view(result_label(badRaw))=="BASELINE_INVALID","raw failure must invalidate");
        auto badBinary=summary(raw,badBlur,nr);
        expect(!baseline_valid(badBinary),"binary failure must invalidate");
        auto noControls=summary(raw,binary,nr);noControls.controls[3].rejected=false;
        expect(!baseline_valid(noControls)&&!controls_valid(noControls),"unrejected bad control must invalidate");
        auto faulted=summary(raw,binary,nr);faulted.faults.push_back("lane fault");
        expect(!baseline_valid(faulted),"fault label must invalidate");
        auto shortLadder=summary(raw,binary,nr);shortLadder.tallies.pop_back();
        expect(!baseline_valid(shortLadder)&&!nr_pass(shortLadder),"incomplete ladder accepted");

        // Receipt: parseable, complete, and it states that nothing frozen was written.
        for(unsigned mode=0;mode<4;++mode) {
            const auto inherited=attribute_inherited(fixture(0xB29,static_cast<FixtureColor>(mode)),calibration);
            expect(inherited.ticks==Fixture::ticks-1&&inherited.templates>0,"inherited scored count is not exactly ticks 1..255");
            defects24.inherited.push_back(inherited);good24.inherited.push_back(inherited);
        }
        expect(!defects24.inherited[0].pluginAttributed&&defects24.inherited[0].rawEqualsTotal&&defects24.inherited[0].maxIncremental==0,"inherited trail attributed to the plugin");
        std::ostringstream receipt;write_summary(receipt,defects24);
        const auto parsed=parse_json(receipt.str());
        expect(std::string_view(find_value(parsed,"result"))=="BASELINE_VALID_NR_DEFECTS","receipt result");
        expect(parsed.find("baseline_valid")->boolean&&!parsed.find("unprotected_nr_pass")->boolean&&parsed.find("raw_pass")->boolean&&parsed.find("binary_pass")->boolean,"receipt verdict flags");
        expect(parsed.find("rungs")->items.size()==24&&parsed.find("bad_controls")->items.size()==9,"receipt rung/control counts");
        expect(parsed.find("defects")->items.size()==defects.size(),"receipt defects count");
        expect(parsed.find("freeze")->find("files_written")->number==0&&parsed.find("freeze")->find("read_only")->boolean,"receipt freeze claim");
        expect(std::string_view(find_value(*parsed.find("accepted_calibration"),"calibration_sha256"))==std::string(64,'c'),"receipt accepted hash");
        std::printf("CPU_BASELINE GREEN raw/binary/unprotected controls=%u RED blend=%llu blur_edge=%llu defects=%zu\n",
            raw.pass()&&binary.pass()&&nr.pass(),static_cast<unsigned long long>(badBlend.templateFailures),
            static_cast<unsigned long long>(badBlur.edgeFailures),defects.size());

        if(argc==3&&std::string_view(argv[1])=="--emit") {
            const std::filesystem::path out(argv[2]);
            std::filesystem::create_directories(out);
            const auto write=[&](const char* name,const std::string& text){std::ofstream(out/name,std::ios::binary)<<text;};
            write("accepted-calibration.json",good.calibration);write("accepted-controls.json",good.controls);
            write("ghost-baseline-defects.json",receipt.str());
            std::ostringstream pass;write_summary(pass,good24);write("ghost-baseline-pass.json",pass.str());
            std::ostringstream invalid;write_summary(invalid,badRaw);write("ghost-baseline-invalid.json",invalid.str());
            write("hashes.txt",sha256_hex(good.calibration)+"\n"+sha256_hex(good.controls)+"\n"+sha256_hex(receipt.str())+"\n");
        }
        std::printf("PASS ghost_baseline checks=%u CPU_only=true physical_measurements=false freeze_written=false\n",checks);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL ghost_baseline %s (after %u checks)\n",error.what(),checks);
        return 1;
    }
}
