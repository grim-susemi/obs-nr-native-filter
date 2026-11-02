// SPDX-License-Identifier: GPL-3.0-only
#include "recording_scenario.h"
#include "recording_classifier.h"
#include "recording_fixture.h"
#include "recording_challenge.h"
#include "recording_filter_order.h"
#include "recording_decode_join.h"
#include "recording_program_witness.h"
#include "recording_program_draw.h"
#include "recording_decoded_occurrence.h"
#include "nr_bootstrap_images.h"
#include "nr_ghost_scenario.h"
#include "ghost_fixture.h"
#include "nr_recording_encode.h"
#include "nr_recording_cleanup.h"
#include "nr_window_artifacts.h"
#include "obs_native_output.h"
#include <obs.h>
#include <graphics/vec4.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <semaphore>
#include <thread>
namespace {
using namespace nr;
using namespace nr::qa;
namespace image=nr::qa::bootstrap;
using Microsoft::WRL::ComPtr;
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
void checked(HRESULT hr,const char* message) { require(SUCCEEDED(hr),message); }
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Event() { require(value!=nullptr,"bootstrap exact event"); }
    ~Event() { CloseHandle(value); }
    void wait(uint32_t bound=30000) { require(WaitForSingleObject(value,bound)==WAIT_OBJECT_0,"bootstrap exact event timeout"); }
};
uint64_t fixture_instance() { static std::atomic<uint64_t> next{1};return next.fetch_add(1); }
struct Fixture {
    const bool hdr,unrelated;
    const uint32_t ghostSeed,ghostColor,ghostStart;
    gs_texture_t* texture=nullptr;
    uint64_t clock=0,producerIngress=0;
    const uint64_t producerInstance=fixture_instance();
    uint32_t content=0,renders=0,ingress=0;
    bool ghostPrepared=false,ghostArmed=false,ghostMissing=false;
    std::array<std::vector<uint8_t>,3> ghostBytes;
    std::array<std::atomic<unsigned>,3> ghostReady{};
    std::counting_semaphore<4> ghostSpace{3};
    std::atomic<bool> ghostStop{false};
    Event ghostPrimed,ghostExited;
    std::thread ghostProducer;
    Fixture(bool h,bool u,uint32_t seed=0,uint32_t color=0,uint32_t start=0):
        hdr(h),unrelated(u),ghostSeed(seed),ghostColor(color),ghostStart(start) {}
};
const char* source_name(void*) { return "Task16 bounded original source landmarks"; }
void source_state(void*,calldata_t*);
void source_arm(void* opaque,calldata_t*) {
    auto& f=*static_cast<Fixture*>(opaque);f.ghostArmed=true;f.ingress=0;
}
void* source_create(obs_data_t* settings,obs_source_t* source) {
    auto* f=new Fixture(obs_data_get_bool(settings,"hdr"),obs_data_get_bool(settings,"unrelated"),
        static_cast<uint32_t>(obs_data_get_int(settings,"ghost_seed")),
        static_cast<uint32_t>(obs_data_get_int(settings,"ghost_color")),
        static_cast<uint32_t>(obs_data_get_int(settings,"ghost_start")));
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_fixture_state(out int renders, out int content, out int ingress)",source_state,f);
    f->ghostPrepared=f->ghostSeed&&!f->unrelated&&obs_data_get_bool(settings,"ghost_prepare");
    if(f->ghostPrepared) {
        for(auto& ready:f->ghostReady)ready.store(UINT32_MAX);
        f->ghostProducer=std::thread([f] {
            for(unsigned tick=0;tick<256;++tick) {
                f->ghostSpace.acquire();if(f->ghostStop.load(std::memory_order_acquire))break;
                const auto slot=tick%3;
                f->ghostBytes[slot]=nr_ghost_fixture_bytes(f->ghostSeed,f->ghostColor,tick,f->hdr);
                f->ghostReady[slot].store(tick,std::memory_order_release);
                if(tick==2)SetEvent(f->ghostPrimed.value);
            }
            SetEvent(f->ghostExited.value);
        });
        f->ghostPrimed.wait(); // Creation/control path, before binding the graph.
        proc_handler_add(obs_source_get_proc_handler(source),"void nr_fixture_arm()",source_arm,f);
    }
    return f;
}
void source_destroy(void* opaque) {
    auto* f=static_cast<Fixture*>(opaque);
    if(f->ghostProducer.joinable()) {
        f->ghostStop.store(true,std::memory_order_release);f->ghostSpace.release();
        f->ghostExited.wait();f->ghostProducer.join();
    }
    obs_enter_graphics();gs_texture_destroy(f->texture);obs_leave_graphics();delete f;
}
uint32_t source_width(void* opaque) { return static_cast<Fixture*>(opaque)->unrelated?128:image::width; }
uint32_t source_height(void* opaque) { return static_cast<Fixture*>(opaque)->unrelated?128:image::height; }
gs_color_space source_color(void* opaque,size_t,const gs_color_space*) {
    return static_cast<Fixture*>(opaque)->hdr?GS_CS_709_SCRGB:GS_CS_SRGB;
}
void source_state(void* opaque,calldata_t* query) {
    auto& f=*static_cast<Fixture*>(opaque);
    calldata_set_int(query,"renders",f.renders);calldata_set_int(query,"content",f.content);
    calldata_set_int(query,"ingress",f.ingress);
    calldata_set_int(query,"producer_ingress",static_cast<int64_t>(f.producerIngress));
    calldata_set_int(query,"producer_instance",static_cast<int64_t>(f.producerInstance));
    calldata_set_ptr(query,"producer_texture",f.texture);
    calldata_set_bool(query,"prepared",f.ghostPrepared);calldata_set_bool(query,"armed",f.ghostArmed);
    calldata_set_bool(query,"missing",f.ghostMissing);
}
void source_render(void* opaque,gs_effect_t*) {
    auto& f=*static_cast<Fixture*>(opaque);++f.renders;
    const auto clock=obs_get_video_frame_time();
    if(!f.texture)f.texture=gs_texture_create(source_width(&f),source_height(&f),f.hdr?GS_RGBA16F:GS_RGBA_UNORM,1,nullptr,GS_DYNAMIC);
    if(f.clock!=clock) {
        f.clock=clock;
        if(!f.unrelated) {
            ++f.producerIngress;
            if(f.ghostSeed) {
                const auto tick=f.ghostPrepared&&!f.ghostArmed?0u:std::min(f.ghostStart+f.ingress,255u);
                const auto corpus=ghost::fixture(f.ghostSeed,static_cast<ghost::FixtureColor>(f.ghostColor));
                const auto content=corpus.schedule[tick].contentId;
                if(f.ghostPrepared&&f.ghostArmed&&f.ghostReady[tick%3].load(std::memory_order_acquire)!=tick) {
                    f.ghostMissing=true;return;
                }
                // Identical holds keep the same source bytes; ingress still advances.
                if(content!=f.content) {
                    if(f.ghostPrepared) {
                        const auto slot=tick%3;
                        if(f.ghostReady[slot].load(std::memory_order_acquire)!=tick) {
                            f.ghostMissing=true;return; // Explicit refusal, never wait or substitute another ingress.
                        }
                        gs_texture_set_image(f.texture,f.ghostBytes[slot].data(),image::width*(f.hdr?8:4),false);
                    }else {
                        const auto bytes=nr_ghost_fixture_bytes(f.ghostSeed,f.ghostColor,tick,f.hdr);
                        gs_texture_set_image(f.texture,bytes.data(),image::width*(f.hdr?8:4),false);
                    }
                }
                f.content=content;
                if(!f.ghostPrepared||f.ghostArmed) {
                    if(f.ghostPrepared&&tick<255) {
                        f.ghostReady[tick%3].store(UINT32_MAX,std::memory_order_release);f.ghostSpace.release();
                    }
                    ++f.ingress;
                }
            } else {
                f.content=std::min(f.content+1,image::max_content_id);
                const auto bytes=nr::qa::recording_fixture_bytes(f.content,f.hdr);
                gs_texture_set_image(f.texture,bytes.data(),image::width*(f.hdr?8:4),false);
            }
        } else {
            std::vector<uint8_t> bytes(128*128*(f.hdr?8:4));
            for(size_t i=0;i<128*128;++i)for(size_t c=0;c<4;++c) {
                const float value=std::array<float,4>{.25f,.5f,.75f,1}[c];
                if(f.hdr) { const auto half=DirectX::PackedVector::XMConvertFloatToHalf(value);memcpy(bytes.data()+i*8+c*2,&half,2); }
                else bytes[i*4+c]=static_cast<uint8_t>(std::lround(value*255));
            }
            gs_texture_set_image(f.texture,bytes.data(),128*(f.hdr?8:4),false);
        }
    }
    require(f.texture!=nullptr,"actual bounded source texture");
    const bool srgb=gs_framebuffer_srgb_enabled();gs_enable_framebuffer_srgb(false);
    auto* effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),f.texture);
    while(gs_effect_loop(effect,"DrawAlphaDivide"))gs_draw_sprite(f.texture,0,source_width(&f),source_height(&f));
    gs_enable_framebuffer_srgb(srgb);
}
void register_fixture() {
    if(obs_source_get_display_name("nr_task16_fixture"))return;
    obs_source_info info{};info.id="nr_task16_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
    info.get_name=source_name;info.create=source_create;info.destroy=source_destroy;
    info.get_width=source_width;info.get_height=source_height;info.video_render=source_render;info.video_get_color_space=source_color;
    obs_register_source(&info);
}
struct OrderProbe { obs_source_t* source;uint64_t calls=0; };
void* order_create(obs_data_t*,obs_source_t* source) {return new OrderProbe{source};}
void order_destroy(void* opaque) {delete static_cast<OrderProbe*>(opaque);}
const char* order_name(void*) {return "Task16 unchanged filter-order probe";}
void order_render(void* opaque,gs_effect_t*) {
    auto& p=*static_cast<OrderProbe*>(opaque);++p.calls;obs_source_skip_video_filter(p.source);
}
gs_color_space order_color(void* opaque,size_t count,const gs_color_space* spaces) {
    const auto& p=*static_cast<OrderProbe*>(opaque);
    return obs_source_get_color_space(obs_filter_get_target(p.source),count,spaces);
}
void register_order_probe() {
    if(obs_source_get_display_name("nr_task16_order"))return;
    obs_source_info info{};info.id="nr_task16_order";info.type=OBS_SOURCE_TYPE_FILTER;
    info.output_flags=OBS_SOURCE_VIDEO;info.get_name=order_name;info.create=order_create;info.destroy=order_destroy;info.video_render=order_render;
    info.video_get_color_space=order_color;
    obs_register_source(&info);
}
std::vector<uint8_t> read_texture(ID3D11Texture2D* texture,bool hdr,uint32_t width=image::width,uint32_t height=image::height) {
    require(texture!=nullptr,"actual full-size texture");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    require(desc.Width==width && desc.Height==height,"actual full-size capture extent");
    require(hdr?(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS):
        (desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS ||
         desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS),"actual readback numeric format");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> graphics;
    texture->GetDevice(&device);device->GetImmediateContext(&graphics);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;checked(device->CreateTexture2D(&desc,nullptr,&stage),"bounded QA stage");
    graphics->CopyResource(stage.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};checked(graphics->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"bounded QA map");
    const uint32_t row=width*(hdr?8:4);
    std::vector<uint8_t> bytes(static_cast<size_t>(row)*height);
    for(uint32_t y=0;y<height;++y)memcpy(bytes.data()+static_cast<size_t>(y)*row,
        static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,row);
    graphics->Unmap(stage.Get(),0);return bytes;
}
std::vector<uint8_t> read_texture(gs_texture_t* texture,bool hdr) {
    return read_texture(static_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture)),hdr);
}
struct Row {
    uint64_t tick,clock,interval,instance,ingress,selected,epoch;
    uint64_t occurrence=0;uint32_t wire=0;
    uint32_t sourceId,filteredId,programId,kind,renders,programFormat;
    uint64_t rawHash,filteredHash,programHash,unrelatedHash;
    double delta,maximumNits;
    uint64_t alphaErrors=0;
    bool betweenViews=false;
    std::optional<RecordingChallenge> challenge;
    uint64_t previewHash=0,projectorHash=0;
    uint64_t currentHash=0,revision=0;
    double strength=0;
    bool genuine=false,applied=false;
    classified::AdmissionEvidence admission;
    classified::RowEvidence evidence;
    Reason selectionReason=Reason::None;bool selectionResetTemporal=false;
    std::optional<model::Job::ResetDiagnostic> resetOperands;
    std::array<int64_t,8> chronology{};std::array<int64_t,5> qaTiming{};
    uint64_t serial=0,history=0,generation=0;
    uintptr_t module=0;
    std::string modulePath,status;
};
void emit_classified_row(const Row& r,bool hdr) {
    auto* d=obs_data_create();
    obs_data_set_int(d,"lane",hdr?3:1);obs_data_set_int(d,"serial",static_cast<int64_t>(r.serial));
    obs_data_set_int(d,"selection_tick",static_cast<int64_t>(r.tick));
    obs_data_set_int(d,"admission_tick",static_cast<int64_t>(r.admission.tick));
    obs_data_set_string(d,"selection_provenance","selection-tick");
    obs_data_set_int(d,"selection_kind",r.kind);obs_data_set_int(d,"selection_reason",static_cast<int64_t>(r.selectionReason));
    obs_data_set_bool(d,"selection_reset_temporal",r.selectionResetTemporal);
    obs_data_set_string(d,"admission_provenance","admission-derived");
    obs_data_set_bool(d,"admission_tick_witness_matched",r.admission.tickWitnessMatched);
    obs_data_set_bool(d,"predecessor_witness_matched",r.admission.predecessorWitnessMatched);
    const auto cls=classified::reset_class(r.admission);
    obs_data_set_string(d,"admission_reset_class",cls.name.c_str());
    obs_data_set_int(d,"admission_history_delta",static_cast<int64_t>(cls.historyDelta));
    obs_data_set_int(d,"admission_content_delta",static_cast<int64_t>(cls.contentDelta));
    obs_data_set_string(d,"admission_exact_reset_member",cls.exactMember.c_str());
    obs_data_set_string(d,"admission_class_members",cls.name=="history-only"?
        "EvaluationSkipped/EvaluationFailure/DeadlineMiss/SlotsFull-class; exact member UNKNOWN":
        cls.name=="content"?"non-evaluation reset causes; exact member UNKNOWN":"not member-level attribution");
    obs_data_set_bool(d,"preceding_admission_present",r.admission.previous.has_value());
    if(r.admission.key) {
        const auto& k=*r.admission.key;
        obs_data_set_int(d,"content_epoch",static_cast<int64_t>(k.frame.contentEpoch));
        obs_data_set_int(d,"history_epoch",static_cast<int64_t>(k.historyEpoch));
        obs_data_set_int(d,"generation_epoch",static_cast<int64_t>(k.resourceGeneration));
        obs_data_set_int(d,"job_ingress",static_cast<int64_t>(k.frame.ingressSequence));
    }
    if(r.admission.previous) {
        const auto& k=*r.admission.previous;
        obs_data_set_int(d,"preceding_admission_content_epoch",static_cast<int64_t>(k.frame.contentEpoch));
        obs_data_set_int(d,"preceding_admission_history_epoch",static_cast<int64_t>(k.historyEpoch));
        obs_data_set_int(d,"preceding_admission_generation_epoch",static_cast<int64_t>(k.resourceGeneration));
        obs_data_set_int(d,"preceding_admission_ingress",static_cast<int64_t>(k.frame.ingressSequence));
        obs_data_set_int(d,"preceding_admission_serial",static_cast<int64_t>(k.submissionSerial));
    }
    obs_data_set_string(d,"preceding_job_state_provenance","admission-tick ready acquire; evaluated/completion read only if ready");
    obs_data_set_bool(d,"preceding_job_state_observed",r.admission.previousStateObserved);
    obs_data_set_bool(d,"preceding_job_ready",r.admission.previousReady);
    obs_data_set_bool(d,"preceding_job_evaluated",r.admission.previousEvaluated);
    obs_data_set_int(d,"preceding_job_completion_value",static_cast<int64_t>(r.admission.previousCompletion));
    obs_data_set_int(d,"preceding_job_completed_value",static_cast<int64_t>(r.admission.previousCompleted));
    obs_data_set_string(d,"model_provenance","model-side pre-predicate v3 operands");
    obs_data_set_bool(d,"model_operands_present",r.resetOperands.has_value());
    const auto operands=r.resetOperands.value_or(model::Job::ResetDiagnostic{});
    obs_data_set_bool(d,"reset_operands_captured",operands.captured);
    obs_data_set_bool(d,"processor_reset",operands.processorReset);
    obs_data_set_bool(d,"previous_key_present",operands.previousKey.has_value());
    obs_data_set_bool(d,"previous_key_absent",!operands.previousKey);
    obs_data_set_string(d,"reset_setter_cause",operands.setterCause);
    obs_data_set_bool(d,"previous_history_differs",r.evidence.historyMismatch);
    obs_data_set_bool(d,"previous_content_differs",r.evidence.contentMismatch);
    obs_data_set_bool(d,"previous_ingress_plus_one_differs",r.evidence.ingressMismatch);
    obs_data_set_bool(d,"reset_comparisons_available",operands.previousKey.has_value());
    obs_data_set_bool(d,"reference_valid",r.evidence.referenceValid);
    obs_data_set_bool(d,"applied",r.applied);obs_data_set_double(d,"delta",r.delta);
    obs_data_set_double(d,"packing_bound",hdr?.001:.004);
    obs_data_set_string(d,"raw_hash",std::to_string(r.rawHash).c_str());
    obs_data_set_string(d,"filtered_hash",std::to_string(r.filteredHash).c_str());
    const std::array<const char*,8> names{"chronology_create","chronology_encode","chronology_evaluate","chronology_compose",
        "chronology_execute","chronology_submitted","chronology_complete","chronology_status"};
    for(size_t i=0;i<names.size();++i)obs_data_set_int(d,names[i],r.chronology[i]);
    const std::array<const char*,5> timing{"qa_process","qa_producer_ready","qa_module_start","qa_feature_start","qa_feature_return"};
    for(size_t i=0;i<timing.size();++i)obs_data_set_int(d,timing[i],r.qaTiming[i]);
    const auto* failure=r.kind==3?classified::refusal(r.evidence):nullptr;
    obs_data_set_string(d,"classifier_refusal",failure?failure:"none");
    std::cout<<"NR_RECORDING_CLASSIFIED_ROW "<<obs_data_get_json(d)<<std::endl;obs_data_release(d);
}
enum class Action { None,Bind,Stop,Close,Release,Activate,DisconnectRaw };
void preencode(void*,video_data*);
void owner_action(void*);
struct RecordingPublication final:model::ObservationSink {
    std::optional<JobKey> key;bool released=false,armed=false,failed=false;
    std::optional<IngressSample> input;
    std::optional<Selection> selected;
    std::vector<classified::AdmissionEvidence> admissions;
    std::shared_ptr<model::Job> previousJob;
    model::Observation* reserve(const IngressSample& current,const pipeline::TickPlan& plan) noexcept override {
        try {
            input.emplace(current);selected.emplace(obs_adapter::adapter_selection(plan));
            if(plan.admission) {
                classified::AdmissionEvidence a;a.tick=current.canonicalTick;a.key.emplace(plan.admission->job.key);
                if(!admissions.empty()) {
                    a.previous.emplace(*admissions.back().key);
                    a.predecessorWitnessMatched=admissions.back().tickWitnessMatched;
                }
                if(previousJob && a.previous && previousJob->contract.key==*a.previous) {
                    a.previousReady=previousJob->ready.load(std::memory_order_acquire);
                    if(a.previousReady) {
                        a.previousStateObserved=true;a.previousEvaluated=previousJob->evaluated;
                        const auto completion=previousJob->completion();
                        a.previousCompletion=completion.value;
                        a.previousCompleted=completion.fence?completion.fence->GetCompletedValue():0;
                    }
                }
                admissions.push_back(std::move(a));
            }
            previousJob.reset();
        }
        catch(...) {failed=true;}
        if(armed && !key && plan.admission)key.emplace(plan.admission->job.key);return nullptr;
    }
    void capture(model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
    bool publication_available(const JobKey& value) const noexcept override {return released || !key || value!=*key;}
};
std::vector<uint8_t> draw_source(obs_source_t* source,bool hdr) {
    auto* target=gs_texrender_create(hdr?GS_RGBA16F:GS_RGBA,GS_ZS_NONE);
    require(target && gs_texrender_begin_with_color_space(target,image::width,image::height,hdr?GS_CS_709_SCRGB:GS_CS_SRGB),"actual off view target");
    vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);gs_ortho(0,static_cast<float>(image::width),0,static_cast<float>(image::height),-100,100);
    gs_blend_state_push();gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    obs_source_video_render(source);gs_texrender_end(target);gs_blend_state_pop();
    const auto pixels=read_texture(gs_texrender_get_texture(target),hdr);gs_texrender_destroy(target);return pixels;
}
// Graph sample budget equals the bounded source ID range. Measured in the r39 window: lane 0 (NR off) stored
// 15 rows (4 before recording, 8 recorded packets with source IDs 5..12, 3 after the stop); lane 1 (real NR)
// needed at least 17 (7 before arming, 8 packets, stop latency), about 19 expected. 24 leaves about 5 rows of
// margin. Keep in step with RecordingParser.ps1 and image::max_content_id.
constexpr size_t kBoundedGraphSamples=image::max_content_id;
struct Harness {
    const std::filesystem::path root,directory;
    const bool hdr,deterministic;
    Event acted,warm,consumerDone;
    std::atomic<Action> action{Action::None};
    std::atomic<Action> ownerTask{Action::None};
    QueuedTaskCustody custody; // Exact custody of the queued Activate/DisconnectRaw owner task.
    std::atomic<bool> observe{true};
    std::atomic<bool> recordingArmed{false};
    std::shared_ptr<RecordingPublication> publication=std::make_shared<RecordingPublication>();
    std::shared_ptr<model::Job> heldJob;
    std::unique_ptr<Recording> recording;
    bool failed=false,bound=false,closed=false;
    std::string failure;
    obs_source_t* source=nullptr,*filter=nullptr,*unrelated=nullptr;
    std::array<obs_source_t*,2> order{};
    obs_scene_t* scene=nullptr;
    std::vector<Row> rows;
    classified::Window classifiedWindow;
    std::mutex rawMutex;
    std::vector<DecodedFrame> preencode;
    bool rawFailed=false,rawActive=false;
    std::string rawFailure;
    DWORD graphicsOwner=0;uint64_t activationClock=0;
    std::atomic<bool> transportStopping{false};
    ProgramWitness witness;uint64_t interval=0;
    std::unique_ptr<ProgramMarkerDraw> marker;
    Event receiptPublished;
    std::shared_ptr<model::Job> stoppingJob;
    std::shared_ptr<model::QaSignal> exited;
    ComPtr<ID3D11Fence> stoppingFence;
    std::optional<image::Image> rawPicture,filterPicture,programPicture;
    uint32_t pictureId=0;
    explicit Harness(const std::filesystem::path& r,const std::filesystem::path& d,bool h,bool copy):
        root(r),directory(d),hdr(h),deterministic(copy) { rows.reserve(kBoundedGraphSamples);preencode.reserve(16); }
    ~Harness() {
        if(scene)obs_scene_release(scene);
        if(source && filter)obs_source_filter_remove(source,filter);
        obs_source_release(filter);obs_source_release(source);obs_source_release(unrelated);
        for(auto* p:order)obs_source_release(p);
    }
    void execute(Action next) {
        ResetEvent(acted.value);
        const bool queued=next==Action::Activate || next==Action::DisconnectRaw;
        if(queued) {
            ownerTask.store(next,std::memory_order_release);
            custody.begin(); // Published before obs_queue_task: the task owns this lane until its completion is observed.
            obs_queue_task(OBS_TASK_GRAPHICS,owner_action,this,false);
        }else action.store(next,std::memory_order_release);
        acted.wait(); // Control thread only; graphics never waits for publication.
        if(queued)custody.complete(); // Only an observed publication retires the obligation; a timeout leaves it outstanding.
    }
    void create() {
        register_fixture();register_order_probe();
        obs_data_t* settings=obs_data_create();obs_data_set_bool(settings,"hdr",hdr);
        source=obs_source_create_private("nr_task16_fixture","Task16 original",settings);
        obs_data_set_bool(settings,"unrelated",true);
        unrelated=obs_source_create_private("nr_task16_fixture","Task16 unrelated scene source",settings);
        obs_data_release(settings);
        settings=obs_data_create();
        obs_data_set_string(settings,"nr_shader_root",(root/"bin/64bit").generic_string().c_str());
        obs_data_set_string(settings,"nr_data_root",(root/"profile/ngx-data").generic_string().c_str());
        obs_data_set_bool(settings,"nr_enabled",!deterministic);
        obs_data_set_bool(settings,"nr_qa_observe",true);
        filter=obs_source_create_private("susemi_nr_filter","Task16 actual native filter",settings);
        obs_data_release(settings);
        require(source && filter && unrelated,"real source/filter graph creation");
        obs_source_filter_add(source,filter);
        for(size_t i=0;i<order.size();++i) {
            order[i]=obs_source_create_private("nr_task16_order",i?"Task16 after NR":"Task16 before NR",nullptr);
            require(order[i]!=nullptr,"actual filter-order probe creation");obs_source_filter_add(source,order[i]);
        }
        obs_source_filter_set_order(source,order[0],OBS_ORDER_MOVE_TOP);
        obs_source_filter_set_order(source,order[1],OBS_ORDER_MOVE_BOTTOM);
        const int beforeIndex=obs_source_filter_get_index(source,order[0]);
        const int nrIndex=obs_source_filter_get_index(source,filter);
        const int afterIndex=obs_source_filter_get_index(source,order[1]);
        auto* orderDiagnostic=obs_data_create();
        obs_data_set_int(orderDiagnostic,"before_index",beforeIndex);
        obs_data_set_int(orderDiagnostic,"nr_index",nrIndex);
        obs_data_set_int(orderDiagnostic,"after_index",afterIndex);
        obs_data_set_string(orderDiagnostic,"before_name",obs_source_get_name(order[0]));
        obs_data_set_string(orderDiagnostic,"nr_name",obs_source_get_name(filter));
        obs_data_set_string(orderDiagnostic,"after_name",obs_source_get_name(order[1]));
        obs_data_set_string(orderDiagnostic,"before_id",obs_source_get_id(order[0]));
        obs_data_set_string(orderDiagnostic,"nr_id",obs_source_get_id(filter));
        obs_data_set_string(orderDiagnostic,"after_id",obs_source_get_id(order[1]));
        obs_data_set_int(orderDiagnostic,"before_flags",obs_source_get_output_flags(order[0]));
        obs_data_set_int(orderDiagnostic,"nr_flags",obs_source_get_output_flags(filter));
        obs_data_set_int(orderDiagnostic,"after_flags",obs_source_get_output_flags(order[1]));
        obs_data_set_bool(orderDiagnostic,"same_parent",obs_filter_get_parent(order[0])==source &&
            obs_filter_get_parent(filter)==source && obs_filter_get_parent(order[1])==source);
        obs_data_set_bool(orderDiagnostic,"hdr",hdr);
        obs_data_set_bool(orderDiagnostic,"nr_off",deterministic);
        obs_data_set_bool(orderDiagnostic,"pixel_flow_valid",recording_filter_order_valid(beforeIndex,nrIndex,afterIndex));
        std::cout<<"NR_RECORDING_FILTER_ORDER "<<obs_data_get_json(orderDiagnostic)<<std::endl;
        obs_data_release(orderDiagnostic);
        require(recording_filter_order_valid(beforeIndex,nrIndex,afterIndex),"actual filter graph order");
        scene=obs_scene_create_private("Task16 actual program scene");
        require(scene && obs_scene_add(scene,source),"real scene filtered source");
        auto* item=obs_scene_add(scene,unrelated);require(item!=nullptr,"real unrelated scene source");
        vec2 position{1792,952};obs_sceneitem_set_pos(item,&position);
    }
    void stop() {
        publication->previousJob.reset();
        calldata_t query;calldata_init(&query);
        std::shared_ptr<interop::Bridge> bridge;
        calldata_set_ptr(&query,"retain_job",&stoppingJob);calldata_set_ptr(&query,"retain_bridge",&bridge);
        calldata_set_ptr(&query,"retain_exit_signal",&exited);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"native stop subscription snapshot");
        calldata_free(&query);
        if(stoppingJob && stoppingJob->ready.load(std::memory_order_acquire))stoppingJob.reset();
        if(bridge && !stoppingFence) {
            stoppingFence=bridge->consumer_fence();
            uint64_t value=0;for(size_t i=0;i<3;++i)value=std::max(value,bridge->ownership(i).consumer);
            checked(stoppingFence->SetEventOnCompletion(value,consumerDone.value),"pre-stop actual last consumer subscription");
        } else if(!bridge)SetEvent(consumerDone.value);
        // The local Bridge lease ends before the cooperative stop entry point: a lease held
        // across it keeps the very owner under test alive. The retained fence holds its own
        // reference, so the pre-stop subscription survives the lease release.
        enter_stop_released(bridge,[&] {
            calldata_init(&query);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_stop",&query),"native cooperative stop");
            closed=calldata_bool(&query,"closed");calldata_free(&query);
        });
    }
    void release() {
        require(closed,"graph release requires actual native close");
        obs_set_output_source(0,nullptr);bound=false;
        obs_source_filter_remove(source,filter);
        for(auto*& p:order) {obs_source_filter_remove(source,p);obs_source_release(p);p=nullptr;}
        obs_scene_release(scene);scene=nullptr;
        obs_source_release(source);source=nullptr;obs_source_release(filter);filter=nullptr;
        obs_source_release(unrelated);unrelated=nullptr;stoppingJob.reset();stoppingFence.Reset();marker.reset();
    }
    void sample() {
        if(!observe.load(std::memory_order_acquire) || !bound || failed ||
            transportStopping.load(std::memory_order_acquire))return;
        try {
            if(rows.size()>=kBoundedGraphSamples) {
                std::cout<<"NR_RECORDING_WINDOW_LIMIT rows="<<rows.size()
                    <<" stop_observed="<<transportStopping.load(std::memory_order_acquire)
                    <<" armed="<<recordingArmed.load(std::memory_order_acquire)
                    <<" activation_clock="<<activationClock<<std::endl;
            }
            require(rows.size()<kBoundedGraphSamples,"bounded graph samples exceeded");
            calldata_t data;calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&data),"actual canonical filter snapshot");
            Row row{};row.tick=calldata_int(&data,"tick");row.clock=calldata_int(&data,"clock");
            row.interval=calldata_int(&data,"interval");row.instance=calldata_int(&data,"instance");
            row.ingress=calldata_int(&data,"ingress");row.selected=calldata_int(&data,"selected");row.epoch=calldata_int(&data,"epoch");
            const auto sourceIdentity=static_cast<uint64_t>(calldata_int(&data,"source_token"));
            const auto deviceIdentity=static_cast<uint64_t>(calldata_int(&data,"device_token"));
            const auto historyEpoch=static_cast<uint64_t>(calldata_int(&data,"history_epoch"));
            calldata_free(&data);

            std::shared_ptr<model::Job> selectedJob,currentJob;
            std::optional<obs_adapter::TickWitness> tickWitness;
            std::shared_ptr<const obs_adapter::Capture> raw;
            std::shared_ptr<const obs_adapter::Capture> currentRaw;
            std::shared_ptr<interop::Context> context;
            std::shared_ptr<interop::Bridge> selectedBridge;
            calldata_init(&data);calldata_set_ptr(&data,"retain_selected_job",&selectedJob);
            calldata_set_ptr(&data,"retain_job",&currentJob);calldata_set_ptr(&data,"tick_witness",&tickWitness);
            calldata_set_ptr(&data,"retain_raw",&raw);calldata_set_ptr(&data,"retain_context",&context);
            calldata_set_ptr(&data,"retain_current_raw",&currentRaw);
            calldata_set_ptr(&data,"retain_bridge",&selectedBridge);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&data),"actual native image snapshot");
            require(!calldata_bool(&data,"failed") && (deterministic || (raw && context)),"native raw fallback cannot pass");
            auto* output=static_cast<gs_texture_t*>(calldata_ptr(&data,"output_texture"));
            row.kind=static_cast<uint32_t>(calldata_int(&data,"kind"));row.status=calldata_string(&data,"status");
            row.revision=static_cast<uint64_t>(calldata_int(&data,"settings_revision"));
            calldata_free(&data);
            if(!deterministic) {
                require(tickWitness && tickWitness->tick==row.tick,"admission TickWitness missing at canonical tick");
                if(tickWitness->admitted) {
                    require(!publication->admissions.empty() && publication->admissions.back().key==tickWitness->admitted,
                        "admission TickWitness/key disagreement");
                    publication->admissions.back().tickWitnessMatched=true;
                    require(currentJob && currentJob->contract.key==*tickWitness->admitted,"immediately admitted original job missing");
                    publication->previousJob=currentJob;
                }
                currentJob.reset();
                require(publication->selected && publication->selected->canonicalTick==row.tick,"selection-tick provenance missing");
                row.selectionReason=publication->selected->reason;
                row.selectionResetTemporal=publication->selected->resetTemporal;
                if(selectedJob) {
                    const auto found=std::find_if(publication->admissions.begin(),publication->admissions.end(),[&](const auto& a) {
                        return a.key && *a.key==selectedJob->contract.key;
                    });
                    require(found!=publication->admissions.end(),"sampled job admission provenance missing");
                    std::destroy_at(&row.admission);std::construct_at(&row.admission,*found);
                }
            }
            auto* configuration=obs_source_get_settings(filter);row.strength=obs_data_get_double(configuration,"nr_strength");obs_data_release(configuration);
            calldata_t state;calldata_init(&state);
            require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&state),"actual ingress fixture state");
            const auto currentId=static_cast<uint32_t>(calldata_int(&state,"content"));
            const auto fixtureIngress=static_cast<uint64_t>(calldata_int(&state,"producer_ingress"));
            const auto fixtureInstance=static_cast<uint64_t>(calldata_int(&state,"producer_instance"));
            require(!calldata_bool(&state,"missing"),"genuine fixture producer ingress unavailable");
            auto* fixtureTexture=static_cast<gs_texture_t*>(calldata_ptr(&state,"producer_texture"));calldata_free(&state);
            if(deterministic) {
                row.clock=obs_get_video_frame_time();row.interval=obs_get_frame_interval_ns();row.tick=fixtureIngress;
                row.instance=fixtureInstance;row.ingress=fixtureIngress;row.selected=fixtureIngress;row.kind=0;row.epoch=1;
            }
            require(fixtureTexture!=nullptr,"genuine advancing fixture texture");
            const auto currentBytes=currentRaw?read_texture(currentRaw->original11.Get(),hdr):read_texture(fixtureTexture,hdr);
            row.currentHash=image::hash(currentBytes);
            const auto rawBytes=deterministic?currentBytes:read_texture(raw->original11.Get(),hdr);
            const auto outputBytes=deterministic?draw_source(source,hdr):read_texture(output,hdr);
            auto* programTexture=gs_get_render_target();
            const auto programFormat=gs_texture_get_color_format(programTexture);
            row.programFormat=static_cast<uint32_t>(programFormat);
            const bool programBgra=programFormat==GS_BGRA || programFormat==GS_BGRA_UNORM;
            require(hdr?programFormat==GS_RGBA16F:
                (programBgra || programFormat==GS_RGBA || programFormat==GS_RGBA_UNORM),"actual program RGB format");

            auto original=image::rgb_image(rawBytes,hdr,false,"immutable source");
            auto filtered=image::rgb_image(outputBytes,hdr,false,"selected filter");

            row.sourceId=image::content_id(original);row.filteredId=image::content_id(filtered);
            require((deterministic?row.sourceId==currentId:true) && row.filteredId==row.sourceId,"source/filter/program independent image lineage");
            row.rawHash=image::hash(rawBytes);row.filteredHash=image::hash(outputBytes);
            double squared=0;
            for(size_t i=0;i<original.pixels.size();++i) {
                if(original.pixels[i][3]!=filtered.pixels[i][3])++row.alphaErrors;
                for(size_t c=0;c<3;++c) {
                    const double d=filtered.pixels[i][c]-original.pixels[i][c];squared+=d*d;
                    row.maximumNits=std::max(row.maximumNits,static_cast<double>(filtered.pixels[i][c])*(hdr?80:1));
                }
            }
            row.delta=std::sqrt(squared/(image::width*image::height*3));
            require(row.alphaErrors==0,"actual source alpha changed");
            if(deterministic)require(row.delta<=(hdr?.001:.004),"NR-off changed current raw beyond source packing precision");
            if(row.kind==static_cast<unsigned>(OutputKind::DelayedNr)) {
                require(selectedJob && selectedJob->ready.load(std::memory_order_acquire) && selectedJob->published.load() &&
                    selectedJob->contract.key.frame.ingressSequence==row.selected,"exact same-key physical publication");
                row.serial=selectedJob->contract.key.submissionSerial;row.history=selectedJob->contract.key.historyEpoch;
                row.generation=selectedJob->contract.key.resourceGeneration;
                row.genuine=!deterministic && selectedJob->eligible && selectedJob->evaluated &&
                    selectedJob->composed && selectedJob->submitted && !selectedJob->invalid;
                row.module=selectedJob->runtimeModule;row.modulePath=std::filesystem::path(selectedJob->modulePath).generic_string();
                // Lane-1 metadata on pass and refusal; the refusal below remains authoritative.
                if(!deterministic && !hdr) {
                    const runtime::RuntimeIdentity approved{
                        "C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll",
                        "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a"};
                    const auto moduleInspection=runtime::inspect({std::filesystem::path(selectedJob->modulePath),approved.sha256});
                    auto* diagnostic=obs_data_create();
                    obs_data_set_int(diagnostic,"lane",1);
                    obs_data_set_int(diagnostic,"selected_ingress",static_cast<int64_t>(row.selected));
                    obs_data_set_int(diagnostic,"job_ingress",static_cast<int64_t>(selectedJob->contract.key.frame.ingressSequence));
                    obs_data_set_int(diagnostic,"serial",static_cast<int64_t>(row.serial));
                    obs_data_set_bool(diagnostic,"ready",selectedJob->ready.load(std::memory_order_acquire));
                    obs_data_set_bool(diagnostic,"published",selectedJob->published.load());
                    obs_data_set_bool(diagnostic,"genuine",row.genuine);
                    obs_data_set_bool(diagnostic,"eligible",selectedJob->eligible);
                    obs_data_set_bool(diagnostic,"created",selectedJob->created);
                    obs_data_set_bool(diagnostic,"evaluated",selectedJob->evaluated);
                    obs_data_set_bool(diagnostic,"composed",selectedJob->composed);
                    obs_data_set_bool(diagnostic,"submitted",selectedJob->submitted);
                    obs_data_set_int(diagnostic,"invalid",selectedJob->invalid);
                    obs_data_set_double(diagnostic,"delta",row.delta);
                    obs_data_set_int(diagnostic,"runtime_module",static_cast<int64_t>(row.module));
                    obs_data_set_string(diagnostic,"module_path",row.modulePath.c_str());
                    obs_data_set_int(diagnostic,"code",selectedJob->code);
                    obs_data_set_int(diagnostic,"driver_create_code",selectedJob->driverCreateCode);
                    obs_data_set_bool(diagnostic,"driver_handle",selectedJob->driverHandle);
                    obs_data_set_int(diagnostic,"diagnostic_version",3);
                    const auto operands=selectedJob->resetDiagnostic.value_or(decltype(selectedJob->resetDiagnostic)::value_type{});
                    obs_data_set_bool(diagnostic,"reset_operands_captured",operands.captured);
                    obs_data_set_bool(diagnostic,"processor_reset",operands.processorReset);
                    obs_data_set_bool(diagnostic,"previous_key_present",operands.previousKey.has_value());
                    obs_data_set_bool(diagnostic,"previous_key_absent",!operands.previousKey);
                    obs_data_set_string(diagnostic,"reset_setter_cause",operands.setterCause);
                    obs_data_set_bool(diagnostic,"reset_comparisons_available",operands.previousKey.has_value());
                    if(operands.previousKey) {
                        const auto& prior=*operands.previousKey;
                        const auto& current=selectedJob->contract.key;
                        obs_data_set_bool(diagnostic,"previous_history_differs",prior.historyEpoch!=current.historyEpoch);
                        obs_data_set_bool(diagnostic,"previous_content_differs",prior.frame.contentEpoch!=current.frame.contentEpoch);
                        obs_data_set_bool(diagnostic,"previous_ingress_plus_one_differs",prior.frame.ingressSequence+1!=current.frame.ingressSequence);
                        obs_data_set_int(diagnostic,"previous_instance",static_cast<int64_t>(prior.frame.instanceId));
                        obs_data_set_int(diagnostic,"previous_content_epoch",static_cast<int64_t>(prior.frame.contentEpoch));
                        obs_data_set_int(diagnostic,"previous_ingress",static_cast<int64_t>(prior.frame.ingressSequence));
                        obs_data_set_int(diagnostic,"previous_serial",static_cast<int64_t>(prior.submissionSerial));
                        obs_data_set_int(diagnostic,"previous_history",static_cast<int64_t>(prior.historyEpoch));
                        obs_data_set_int(diagnostic,"previous_generation",static_cast<int64_t>(prior.resourceGeneration));
                    } else {
                        obs_data_set_string(diagnostic,"previous_history_differs","not-evaluated-previous-key-absent");
                        obs_data_set_string(diagnostic,"previous_content_differs","not-evaluated-previous-key-absent");
                        obs_data_set_string(diagnostic,"previous_ingress_plus_one_differs","not-evaluated-previous-key-absent");
                    }
                    obs_data_set_bool(diagnostic,"reset",!selectedJob->flow || selectedJob->flow->reset);
                    obs_data_set_bool(diagnostic,"reference_valid",selectedJob->referenceValid);
                    obs_data_set_double(diagnostic,"resolve_strength",selectedJob->contract.input.settings.strength.value);
                    // Per-pass creation handle is retained through successful evaluation; no new handle query.
                    obs_data_set_bool(diagnostic,"evaluation_driver_handle_valid",selectedJob->evaluated && selectedJob->driverHandle);
                    obs_data_set_string(diagnostic,"evaluation_path",!selectedJob->evaluated ? "not-established" :
                        (selectedJob->driverHandle ? "driver" : "direct-fallback"));
                    obs_data_set_string(diagnostic,"evaluation_path_basis","retained per-pass creation handle plus evaluated; not a new dispatch observation");
                    obs_data_set_string(diagnostic,"evaluation_branch","task-13/shared-sdk-lifetime-repair-20261005/overlay/src/nr_runtime.cpp:584-586; dispatch:626; api:544");
                    obs_data_set_int(diagnostic,"selected_instance",static_cast<int64_t>(selectedJob->contract.key.frame.instanceId));
                    obs_data_set_int(diagnostic,"selected_content_epoch",static_cast<int64_t>(selectedJob->contract.key.frame.contentEpoch));
                    obs_data_set_int(diagnostic,"selected_history",static_cast<int64_t>(row.history));
                    obs_data_set_int(diagnostic,"selected_generation",static_cast<int64_t>(row.generation));
                    obs_data_set_bool(diagnostic,"reference_present",selectedJob->guideReferenceKey.has_value());
                    if(selectedJob->guideReferenceKey) {
                        const auto& reference=*selectedJob->guideReferenceKey;
                        obs_data_set_int(diagnostic,"reference_instance",static_cast<int64_t>(reference.frame.instanceId));
                        obs_data_set_int(diagnostic,"reference_content_epoch",static_cast<int64_t>(reference.frame.contentEpoch));
                        obs_data_set_int(diagnostic,"reference_ingress",static_cast<int64_t>(reference.frame.ingressSequence));
                        obs_data_set_int(diagnostic,"reference_serial",static_cast<int64_t>(reference.submissionSerial));
                        obs_data_set_int(diagnostic,"reference_history",static_cast<int64_t>(reference.historyEpoch));
                        obs_data_set_int(diagnostic,"reference_generation",static_cast<int64_t>(reference.resourceGeneration));
                    }
                    obs_data_set_int(diagnostic,"source_content_id",row.sourceId);
                    obs_data_set_int(diagnostic,"filtered_content_id",row.filteredId);
                    obs_data_set_string(diagnostic,"raw_hash",std::to_string(row.rawHash).c_str());
                    obs_data_set_string(diagnostic,"filtered_hash",std::to_string(row.filteredHash).c_str());
                    obs_data_set_string(diagnostic,"module_sha256",moduleInspection.sha256.c_str());
                    obs_data_set_string(diagnostic,"module_inspection",moduleInspection.reason.c_str());
                    obs_data_set_string(diagnostic,"approved_runtime_path",approved.canonicalPath.generic_string().c_str());
                    obs_data_set_string(diagnostic,"approved_runtime_sha256",approved.sha256.c_str());
                    obs_data_set_bool(diagnostic,"module_path_match",std::filesystem::path(selectedJob->modulePath)==approved.canonicalPath);
                    obs_data_set_bool(diagnostic,"module_hash_match",moduleInspection.sha256==approved.sha256);
                    // File identity only, not an in-memory module or SDK capability attestation.
                    std::cout<<"NR_RECORDING_MODEL_GATE_DIAGNOSTIC "<<obs_data_get_json(diagnostic)<<std::endl;
                    obs_data_release(diagnostic);
                }
                if(!deterministic) {
                    if(selectedJob->resetDiagnostic)row.resetOperands.emplace(*selectedJob->resetDiagnostic);
                    row.chronology=selectedJob->chronology;row.qaTiming=selectedJob->qaTiming;
                    auto& proof=row.evidence;proof.referenceValid=selectedJob->referenceValid;
                    proof.genuine=row.genuine;proof.module=row.module!=0;proof.delta=row.delta;
                    proof.packingBound=hdr?.001:.004;proof.rawHash=row.rawHash;proof.filteredHash=row.filteredHash;
                    proof.admission=classified::reset_class(row.admission);
                    proof.predecessorWitnessMatched=row.admission.predecessorWitnessMatched;
                    proof.admissionMatched=row.admission.tickWitnessMatched && row.admission.predecessorWitnessMatched &&
                        row.admission.key && row.admission.previous;
                    proof.previousUnevaluatedObserved=row.admission.previousStateObserved && row.admission.previousReady &&
                        !row.admission.previousEvaluated;
                    if(row.resetOperands) {
                        const auto& reset=*row.resetOperands;proof.operandsCaptured=reset.captured;
                        proof.processorReset=reset.processorReset;proof.previousKeyAbsent=!reset.previousKey;
                        if(reset.previousKey) {
                            const auto& prior=*reset.previousKey;const auto& current=selectedJob->contract.key;
                            proof.historyMismatch=prior.historyEpoch!=current.historyEpoch;
                            proof.contentMismatch=prior.frame.contentEpoch!=current.frame.contentEpoch;
                            proof.ingressMismatch=prior.frame.ingressSequence+1!=current.frame.ingressSequence;
                        }
                    }
                    row.applied=proof.referenceValid && !classified::refusal(proof);
                }
            }
            if(!deterministic) {
                emit_classified_row(row,hdr);
                classifiedWindow.observe(row.kind==static_cast<unsigned>(OutputKind::DelayedNr),row.evidence);
            }
            require(row.clock==obs_get_video_frame_time() && row.interval==interval && row.ingress>0,"actual OBS scheduled clock and SDK interval");
            if(!rows.empty()) {
                require(row.tick==rows.back().tick+1 && row.ingress==rows.back().ingress+1,"missing/reordered canonical sample");

            }
            // Freeze on the actual main resource path before secondary draws.
            row.occurrence=rows.size()+1;
            std::optional<IngressSample> selectedInput,currentInput;
            std::optional<Selection> selectedDecision;std::optional<JobContract> appliedJob;
            std::optional<FixtureProducerContract> fixtureKey;bool resourceAgreement=true,completionAgreement=true;
            if(deterministic) {
                fixtureKey.emplace(FixtureProducerContract{{fixtureInstance,1,fixtureIngress},
                    {{image::width,image::height},{0,0,image::width,image::height},
                        hdr?PixelFormat::Rgba16Float:PixelFormat::Rgba8,
                        hdr?ColorSpace::ScRgbLinear:ColorSpace::Rec709Srgb,AlphaMode::Premultiplied,80},
                    fixtureInstance,reinterpret_cast<uintptr_t>(gs_get_device_obj()),true});
                require(!selectedJob && !raw && !currentRaw,"OFF fabricated native producer owner");
                resourceAgreement=gs_texture_get_width(fixtureTexture)==image::width &&
                    gs_texture_get_height(fixtureTexture)==image::height &&
                    gs_texture_get_color_format(fixtureTexture)==(hdr?GS_RGBA16F:GS_RGBA_UNORM);
            }else {
                require(!publication->failed && publication->selected && publication->input && raw && currentRaw,
                    "actual selected immutable producer decision missing");
                selectedInput.emplace(raw->input);currentInput.emplace(currentRaw->input);
                selectedDecision.emplace(*publication->selected);
                require(*publication->input==*currentInput && selectedDecision->canonicalTick==row.tick &&
                    currentInput->sourceIdentity==sourceIdentity && currentInput->deviceIdentity==deviceIdentity &&
                    static_cast<uint32_t>(selectedDecision->kind)==row.kind,"actual immutable decision publication mismatch");
                row.revision=selectedInput->settings.revision.value;row.strength=selectedInput->settings.strength.value;
                D3D11_TEXTURE2D_DESC selectedDesc{},currentDesc{};
                raw->original11->GetDesc(&selectedDesc);currentRaw->original11->GetDesc(&currentDesc);
                ComPtr<ID3D11Device> selectedDevice,currentDevice;
                raw->original11->GetDevice(&selectedDevice);currentRaw->original11->GetDevice(&currentDevice);
                const auto expectedFormat=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
                resourceAgreement=output && selectedDesc.Width==image::width && selectedDesc.Height==image::height &&
                    currentDesc.Width==image::width && currentDesc.Height==image::height &&
                    selectedDesc.Format==expectedFormat && currentDesc.Format==expectedFormat &&
                    selectedDevice.Get()==currentDevice.Get() && selectedDevice.Get()==gs_get_device_obj() &&
                    selectedInput->descriptor.allocation.width==selectedDesc.Width &&
                    selectedInput->descriptor.allocation.height==selectedDesc.Height;
                if(selectedJob) {
                    appliedJob.emplace(selectedJob->contract);const auto completion=selectedJob->completion();
                    const auto completed=completion.fence?completion.fence->GetCompletedValue():0;
                    completionAgreement=selectedJob->ready.load(std::memory_order_acquire) && selectedJob->published.load() &&
                        selectedJob->contract.key.historyEpoch==historyEpoch &&
                        completion.fence && completed!=UINT64_MAX && completed>=completion.value;
                    resourceAgreement=resourceAgreement && selectedJob->frame &&
                        selectedJob->frame->graphicsLease.get()==raw.get() &&
                        selectedJob->frame->original.Get()==raw->original12.Get();
                }
            }
            auto producer=std::make_shared<const RecordingProducer>(RecordingProducer{selectedInput,currentInput,
                selectedDecision,appliedJob,fixtureKey,row.filteredId,true,resourceAgreement,completionAgreement});
            std::optional<JobKey> completedTicket,copiedTicket;uint32_t slot=0;
            uint64_t producerValue=0,workerValue=0,consumerValue=0,producerDone=0,workerDone=0,copiedConsumerDone=0;
            uintptr_t completedOutput=0;
            if(selectedJob) {
                require(selectedBridge!=nullptr && selectedJob->output!=nullptr,"selected physical output transport");
                unsigned matches=0;
                for(size_t candidate=0;candidate<3;++candidate) {
                    const auto own=selectedBridge->ownership(candidate);
                    if(own.key && *own.key==selectedJob->contract.key) {
                        require(own.phase==interop::WorkPhase::Submitted && !own.failed && !own.cancelled &&
                            !own.consumerOpen && own.consumerLease && own.consumer,"selected actual consumer copy receipt");
                        ++matches;slot=static_cast<uint32_t>(candidate);
                        completedTicket.emplace(selectedJob->contract.key);copiedTicket.emplace(*own.key);
                        producerValue=own.producer;workerValue=own.worker;consumerValue=own.consumer;
                        producerDone=own.producerDone;workerDone=own.workerDone;copiedConsumerDone=own.consumerDone;
                        completedOutput=reinterpret_cast<uintptr_t>(selectedJob->output);
                    }
                }
                require(matches==1,"unique selected completed output ticket");
            }
            const auto ingress=deterministic?fixtureIngress:raw->input.key.ingressSequence;
            const auto phase=deterministic?0u:row.kind==static_cast<unsigned>(OutputKind::PrimingRaw)?1u:
                row.kind==static_cast<unsigned>(OutputKind::CurrentSource)?2u:0u;
            row.wire=ProgramMarker::wire(1,static_cast<uint32_t>(ingress),phase);
            const ProgramPhysicalReceipt programPhysical{completedTicket,copiedTicket,slot,producerValue,workerValue,consumerValue,
                producerDone,workerDone,copiedConsumerDone,
                deterministic?reinterpret_cast<uintptr_t>(gs_texture_get_obj(fixtureTexture)):reinterpret_cast<uintptr_t>(raw->original12.Get()),
                completedOutput,deterministic?reinterpret_cast<uintptr_t>(gs_texture_get_obj(fixtureTexture)):
                    reinterpret_cast<uintptr_t>(gs_texture_get_obj(output)),reinterpret_cast<uintptr_t>(programTexture),true};
            {std::lock_guard lock(rawMutex);witness.seal({row.wire,1,row.occurrence,row.clock,std::move(producer),programPhysical});}
            marker->draw(row.wire,reinterpret_cast<uintptr_t>(programTexture));
            const auto programBytes=read_texture(programTexture,hdr);
            auto program=image::rgb_image(programBytes,hdr,programBgra,"actual marked main program");
            row.programId=image::content_id(program);row.programHash=image::hash(programBytes);
            require(row.programId==row.sourceId,"actual marked program independent content changed");
            row.unrelatedHash=14695981039346656037ull;
            const uint32_t pixelBytes=hdr?8u:4u,stride=image::width*pixelBytes;
            for(uint32_t y=968;y<1064;++y)for(uint32_t x=1808u*pixelBytes;x<1904u*pixelBytes;++x)
                row.unrelatedHash=(row.unrelatedHash^programBytes[static_cast<size_t>(y)*stride+x])*1099511628211ull;
            if(!rows.empty())require(row.unrelatedHash==rows.front().unrelatedHash,"unchanged central unrelated control");
            require(SetEvent(receiptPublished.value)!=FALSE,"positive program witness signal");
            require(GetCurrentThreadId()==graphicsOwner,"producer GPU lease graphics owner");
            // All independent reads and metadata publication are positive. No GPU lease enters the ledger.
            retire_recording_producer(true,true,GetCurrentThreadId()==graphicsOwner,
                selectedJob,raw,currentRaw,context,selectedBridge);
            // Two extra real source renders use the same production filter latch.
            calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&data),"fixture source-render observation");
            row.renders=static_cast<uint32_t>(calldata_int(&data,"renders"));calldata_free(&data);
            for(unsigned view=0;view<2;++view) {
                if(view==1 && heldJob && publication->key && heldJob->contract.key==*publication->key &&
                    row.selected==publication->key->frame.ingressSequence && !publication->released) {
                    require(heldJob->ready.load(std::memory_order_acquire),"late exact job not ready");
                    row.challenge.emplace(heldJob->contract.key);
                    auto& c=*row.challenge;
                    c.ready=true;c.eligible=heldJob->eligible;c.evaluated=heldJob->evaluated;
                    c.composed=heldJob->composed;c.submitted=heldJob->submitted;c.invalid=heldJob->invalid;
                    c.module=heldJob->runtimeModule;c.modulePath=std::filesystem::path(heldJob->modulePath).generic_string();
                    const auto physical=heldJob->completion();c.fence=physical.fence!=nullptr;
                    c.completionValue=physical.value;c.completedValue=physical.fence?physical.fence->GetCompletedValue():0;
                    require(genuine_recording_challenge(c,*publication->key),"late exact completed job is not genuine eligible NR");
                    require(row.kind==static_cast<unsigned>(OutputKind::DelayedRaw),"withheld cutoff did not choose raw");
                    publication->released=true;row.betweenViews=true;
                }
                auto* target=gs_texrender_create(hdr?GS_RGBA16F:GS_RGBA,GS_ZS_NONE);
                require(target && gs_texrender_begin_with_color_space(target,image::width,image::height,hdr?GS_CS_709_SCRGB:GS_CS_SRGB),"actual preview/projector target");
                gs_blend_state_push();gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
                vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);
                gs_ortho(0,static_cast<float>(image::width),0,static_cast<float>(image::height),-100,100);
                obs_source_video_render(source);gs_texrender_end(target);gs_blend_state_pop();
                const auto viewed=read_texture(gs_texrender_get_texture(target),hdr);gs_texrender_destroy(target);
                require(image::content_id(image::rgb_image(viewed,hdr,false,"secondary view"))==row.filteredId,"view-only or between-view selection changed");
                (view?row.projectorHash:row.previewHash)=image::hash(viewed);
                if(!deterministic) {
                    calldata_t latch;calldata_init(&latch);
                    require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&latch),"actual view key snapshot");
                    require(static_cast<uint64_t>(calldata_int(&latch,"selected"))==row.selected &&
                        static_cast<uint64_t>(calldata_int(&latch,"epoch"))==row.epoch &&
                        static_cast<uint64_t>(calldata_int(&latch,"tick"))==row.tick &&
                        static_cast<uint32_t>(calldata_int(&latch,"kind"))==row.kind,"between-view exact latch changed");
                    calldata_free(&latch);
                }
            }
            calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&data),"source count after views");
            if(!deterministic)require(static_cast<uint32_t>(calldata_int(&data,"renders"))==row.renders,"duplicate-view recapture");
            calldata_free(&data);
            if(recordingArmed.load(std::memory_order_acquire) && !rawPicture && row.sourceId>=3 && (deterministic || row.applied)) {
                pictureId=row.sourceId;rawPicture.emplace(std::move(original));
                filterPicture.emplace(std::move(filtered));programPicture.emplace(std::move(program));
            }
            if(!deterministic && publication->key && !heldJob) {
                calldata_t q;calldata_init(&q);std::shared_ptr<model::Job> job;calldata_set_ptr(&q,"retain_job",&job);
                require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&q),"exact held original job snapshot");
                calldata_free(&q);if(job && job->contract.key==*publication->key)heldJob=std::move(job);
            }
            rows.push_back(std::move(row));
            if(!deterministic && rows.size()==kBoundedGraphSamples) {
                if(const auto* error=classifiedWindow.refusal_at_end())require(false,error);
            }
            if(rows.size()>=3 && (deterministic || (classifiedWindow.applied>=2 && !classifiedWindow.rowFailure)))SetEvent(warm.value);
        }catch(const std::exception& e) { failed=true;failure=e.what();SetEvent(warm.value); }
    }
};
std::unique_ptr<Harness> retainedRecordingGraph;
void action(void* opaque,uint32_t,uint32_t) {
    auto& h=*static_cast<Harness*>(opaque);const auto next=h.action.exchange(Action::None);
    if(next==Action::None)return;
    try {
        if(next==Action::Bind) { h.graphicsOwner=GetCurrentThreadId();obs_set_output_source(0,obs_scene_get_source(h.scene));h.bound=true; }
        else if(next==Action::Activate) {
            require(!obs_video_active(),"owned inactive transport before first activation");
            h.activationClock=obs_get_video_frame_time();
            obs_add_raw_video_callback(nullptr,preencode,&h);h.rawActive=true;
            h.recording->start();
        }
        else if(next==Action::DisconnectRaw) {
            obs_remove_raw_video_callback(preencode,&h);h.rawActive=false;

        }
        else if(next==Action::Release)h.release();
        else h.stop();
    }catch(const std::exception& e) { h.failed=true;h.failure=e.what(); }
    SetEvent(h.acted.value);
}
void rendered(void* opaque) { static_cast<Harness*>(opaque)->sample(); }
void owner_action(void* opaque) {
    auto& h=*static_cast<Harness*>(opaque);
    // A retired obligation means the lane was already resolved without this task; never
    // touch a target whose completion was not observed by the queueing caller.
    if(!h.custody.act()) {SetEvent(h.acted.value);return;}
    // execute_graphics_tasks runs after output_frames releases mixes_mutex.
    // Public video/encoder lifecycle calls must not run under that mutex.
    h.action.store(h.ownerTask.exchange(Action::None),std::memory_order_release);
    action(opaque,0,0);
}
void preencode(void* opaque,video_data* data) {
    auto& h=*static_cast<Harness*>(opaque);std::lock_guard lock(h.rawMutex);
    const auto* info=video_output_get_info(obs_get_video());
    if(!info || info->width!=image::width || info->height!=image::height ||
        info->format!=(h.hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12) || info->cache_size!=6 ||
        info->fps_num!=1 || info->fps_den!=1) {h.rawFailed=true;return;}
    if(h.preencode.size()>=16) { h.rawFailed=true;return; }
    DecodedFrame frame{};frame.width=image::width;frame.height=image::height;frame.timestamp=data->timestamp;
    frame.format=h.hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;frame.transfer=h.hdr?VIDEO_TRC_PQ:VIDEO_TRC_DEFAULT;
    frame.fullRange=false;frame.rowBytes={image::width*(h.hdr?2:1),image::width*(h.hdr?2:1),0};
    frame.rows={image::height,image::height/2,0};
    for(size_t p=0;p<2;++p) {
        if(!data->data[p] || data->linesize[p]<frame.rowBytes[p]) { h.rawFailed=true;return; }
        frame.planes[p].resize(static_cast<size_t>(frame.rowBytes[p])*frame.rows[p]);
        for(uint32_t y=0;y<frame.rows[p];++y)memcpy(frame.planes[p].data()+static_cast<size_t>(y)*frame.rowBytes[p],
            data->data[p]+static_cast<size_t>(y)*data->linesize[p],frame.rowBytes[p]);
    }
    try {
        const auto occurrence=h.witness.raw(frame,
            {reinterpret_cast<uintptr_t>(data->data[0]),reinterpret_cast<uintptr_t>(data->data[1])},
            [](const DecodedFrame& f,size_t p,uint32_t x,uint32_t y){return image::code(f,p,x,y);});
        require(occurrence==h.preencode.size(),"raw plane and receipt occurrence cardinality");
        h.preencode.push_back(std::move(frame));
    }catch(const std::exception& e) {h.rawFailed=true;h.rawFailure=e.what();}
}
void emit(const std::string& prefix,obs_data_t* data) {
    std::cout<<prefix<<obs_data_get_json(data)<<std::endl;obs_data_release(data);
}
void emit_rows(const Harness& h,unsigned lane) {
    for(const auto& r:h.rows) {
        auto* data=obs_data_create();
        for(const auto& pair:std::initializer_list<std::pair<const char*,uint64_t>>{
            {"lane",lane},{"tick",r.tick},{"clock",r.clock},{"interval",r.interval},{"instance",r.instance},
            {"ingress",r.ingress},{"selected",r.selected},{"content_epoch",r.epoch},{"kind",r.kind},
            {"source_id",r.sourceId},{"filter_id",r.filteredId},{"program_id",r.programId},{"width",image::width},
            {"height",image::height},{"source_renders",r.renders},{"alpha_errors",r.alphaErrors},{"program_gs_format",r.programFormat},
            {"submission_serial",r.serial},{"history_epoch",r.history},{"resource_generation",r.generation},{"module_base",r.module}})
            obs_data_set_int(data,pair.first,static_cast<int64_t>(pair.second));
        obs_data_set_bool(data,"hdr",h.hdr);obs_data_set_bool(data,"deterministic",false);obs_data_set_bool(data,"nr_off",h.deterministic);
        obs_data_set_int(data,"fps_numerator",1);obs_data_set_int(data,"fps_denominator",1);
        obs_data_set_string(data,"current_raw_hash",std::to_string(r.currentHash).c_str());
        obs_data_set_string(data,"source_alpha","premultiplied");
        obs_data_set_double(data,"source_reference_white_nits",80);
        obs_data_set_double(data,"output_sdr_white_nits",300);obs_data_set_double(data,"output_peak_nits",1000);
        obs_data_set_int(data,"settings_revision",static_cast<int64_t>(r.revision));obs_data_set_double(data,"settings_strength",r.strength);
        obs_data_set_bool(data,"filter_order_preserved",true);
        obs_data_set_int(data,"transport_occurrence",static_cast<int64_t>(r.occurrence));
        obs_data_set_int(data,"wire_id",r.wire);
        obs_data_set_bool(data,"native_producer_present",!h.deterministic);
        if(r.challenge) {
            const auto& c=*r.challenge;auto* proof=obs_data_create();
            for(const auto& pair:std::initializer_list<std::pair<const char*,uint64_t>>{
                {"instance",c.key.frame.instanceId},{"content_epoch",c.key.frame.contentEpoch},{"ingress",c.key.frame.ingressSequence},
                {"history_epoch",c.key.historyEpoch},{"resource_generation",c.key.resourceGeneration},{"submission_serial",c.key.submissionSerial},
                {"invalid",c.invalid},{"module_base",c.module},{"completion_value",c.completionValue},{"completed_value",c.completedValue}})
                obs_data_set_int(proof,pair.first,static_cast<int64_t>(pair.second));
            obs_data_set_bool(proof,"ready",c.ready);obs_data_set_bool(proof,"eligible",c.eligible);
            obs_data_set_bool(proof,"evaluated",c.evaluated);obs_data_set_bool(proof,"composed",c.composed);
            obs_data_set_bool(proof,"submitted",c.submitted);obs_data_set_bool(proof,"fence_present",c.fence);
            obs_data_set_string(proof,"module_path",c.modulePath.c_str());
            obs_data_set_obj(data,"challenge_job",proof);obs_data_release(proof);
        }
        obs_data_set_bool(data,"between_views_completion",r.betweenViews);
        obs_data_set_string(data,"preview_hash",std::to_string(r.previewHash).c_str());
        obs_data_set_string(data,"projector_hash",std::to_string(r.projectorHash).c_str());
        obs_data_set_bool(data,"genuine_nr",r.genuine);obs_data_set_bool(data,"same_latch_views",true);
        obs_data_set_double(data,"same_key_nr_rms",r.delta);obs_data_set_double(data,"maximum_linear_nits",r.maximumNits);
        obs_data_set_string(data,"raw_hash",std::to_string(r.rawHash).c_str());
        obs_data_set_string(data,"filter_hash",std::to_string(r.filteredHash).c_str());
        obs_data_set_string(data,"program_hash",std::to_string(r.programHash).c_str());
        obs_data_set_string(data,"unrelated_hash",std::to_string(r.unrelatedHash).c_str());
        obs_data_set_string(data,"module_path",r.modulePath.c_str());obs_data_set_string(data,"status",r.status.c_str());
        emit("NR_RECORDING_FRAME ",data);
    }
}
void stop_graph(Harness& h) {
    h.observe.store(false,std::memory_order_release);h.execute(Action::Stop);
    if(h.stoppingJob && !h.stoppingJob->ready.load(std::memory_order_acquire)) {
        require(WaitForSingleObject(h.stoppingJob->observed,30000)==WAIT_OBJECT_0,"normal pending model completion");
        h.stoppingJob.reset();
        h.execute(Action::Stop);
    }
    h.consumerDone.wait();
    // Host-held Job/Frame GPU references die while the NR runtime is still loaded; ~Frame after Close faults in the driver.
    h.heldJob.reset();h.stoppingJob.reset();
    h.execute(Action::Close); // Actual completed consumers permit input-lease collection before worker stop.
    if(h.exited)require(h.exited->owner_module() && h.exited->wait_for(30000),"subscribed module-owned worker normal exit");
    h.execute(Action::Close);require(h.closed,"normal producer/worker/consumer/feature retirement");
    release_after_final_close(h.exited,h.closed); // The exit-signal hold ends with the final Close confirmation.
    h.execute(Action::Release);require(!h.heldJob,"host-held job released before runtime close");
}
}
bool nr_run_recording_scenario(const std::filesystem::path& root,const std::filesystem::path& directory,bool& retained) {
    if(!nr::qa::artifact_directory_allowed(root,directory,nr::qa::ArtifactScenario::RecordingOutput)) {
        std::cout<<"NR_RECORDING_FAILURE native evidence path refused"<<std::endl;return false;
    }
    std::array<std::vector<DecodedFrame>,2> baselines;
    for(unsigned lane=0;lane<4;++lane) {
        const bool hdr=lane>=2,deterministic=(lane%2)==0;
        const auto laneDirectory=directory/("lane-"+std::to_string(lane));std::filesystem::create_directory(laneDirectory);
        auto h=std::make_unique<Harness>(root,laneDirectory,hdr,deterministic);
        bool callbacks=false,rawConnected=false;
        try {
            obs_video_info video{};require(obs_get_video_info(&video),"owned video info");
            video.fps_num=1;video.fps_den=1;video.base_width=video.output_width=image::width;
            video.base_height=video.output_height=image::height;video.gpu_conversion=true;
            video.output_format=hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;
            video.colorspace=hdr?VIDEO_CS_2100_PQ:VIDEO_CS_709;video.range=VIDEO_RANGE_PARTIAL;
            video.scale_type=OBS_SCALE_DISABLE;
            require(obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"full-size SDR/HDR owned video reset");
            obs_set_video_levels(300,1000);
            require(obs_get_video_sdr_white_level()==300 && obs_get_video_hdr_nominal_peak_level()==1000,
                "explicit owned OBS SDR-white/HDR-peak levels");
            h->create();
            require(obs_canvas_get_video(obs_get_main_canvas())==obs_get_video(),"owned main video root");
            h->interval=obs_get_frame_interval_ns();h->witness.begin(1);
            h->marker=std::make_unique<ProgramMarkerDraw>(hdr);
            if(!deterministic) {
                obs_enter_graphics();calldata_t q;calldata_init(&q);
                std::shared_ptr<model::ObservationSink> sink=h->publication;calldata_set_ptr(&q,"observer",&sink);
                require(proc_handler_call(obs_source_get_proc_handler(h->filter),"nr_native_observe",&q),"immutable decision observer");
                calldata_free(&q);obs_leave_graphics();
            }
            h->recording=std::make_unique<Recording>(laneDirectory/"program.hevc",8,hdr);
            h->recording->observe_stop([](void* p){static_cast<Harness*>(p)->transportStopping.store(true,std::memory_order_release);},h.get());
            obs_add_main_render_callback(action,h.get());obs_add_main_rendered_callback(rendered,h.get());callbacks=true;
            h->execute(Action::Bind);h->warm.wait();require(!h->failed,h->failure.c_str());
            if(!deterministic) {
                obs_enter_graphics();calldata_t q;calldata_init(&q);
                h->publication->armed=true;
                std::shared_ptr<model::ObservationSink> sink=h->publication;calldata_set_ptr(&q,"observer",&sink);
                require(proc_handler_call(obs_source_get_proc_handler(h->filter),"nr_native_observe",&q),"recording publication observation seam");
                calldata_free(&q);obs_leave_graphics();
            }
            {
                auto& recording=*h->recording; // Already owned before observation and activation.
                h->execute(Action::Activate);rawConnected=h->rawActive;
                require(!h->failed,h->failure.c_str());
                recording.first_packet();
                h->recordingArmed.store(true,std::memory_order_release);
                const auto packets=recording.finish();
                const auto packetTransport=recording.transport();
                h->execute(Action::DisconnectRaw);rawConnected=false;
                h->observe.store(false,std::memory_order_release);
                require(packetTransport.size()==packets.size(),"actual packet transport cardinality");
                for(size_t i=0;i<packetTransport.size();++i) {
                    const auto& p=packetTransport[i];
                    require(p.occurrence==i && p.pts==static_cast<int64_t>(i) && p.dts==p.pts &&
                        p.timebaseNumerator==1 && p.timebaseDenominator==1 && p.ownedEncoder && p.singlePicture &&
                        p.keyframe==p.idrPicture && (i || p.keyframe) && p.bytes,"actual packet input/picture manifest");
                }
                h->recording.reset();
                h->observe.store(false,std::memory_order_release);
                stop_graph(*h);
                obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get());callbacks=false;
                require(!h->failed,h->failure.c_str());require(!h->rawFailed,h->rawFailure.c_str());
                if(!deterministic)if(const auto* error=h->classifiedWindow.refusal_at_end())require(false,error);
                require(h->rawPicture && h->filterPicture && h->programPicture,"actual source/filter/program image artifacts missing");
                image::save_bmp(laneDirectory/"source.bmp",*h->rawPicture);
                image::save_bmp(laneDirectory/"filter.bmp",*h->filterPicture);
                image::save_bmp(laneDirectory/"program.bmp",*h->programPicture);
                emit_rows(*h,lane); // Written partial image proof survives a later decoder failure.
                const auto decoded=decode_recording(laneDirectory/"program.hevc",hdr,static_cast<uint32_t>(packets.size()));
                require(decoded.size()==packets.size() && decoded.size()>=8,"actual encoded/decoded frame count");
                uint32_t previousId=0;uint64_t previousTimestamp=0,previousPreencodeTimestamp=0;
                size_t packetIndex=0;
                std::vector<uint32_t> decodedIds;std::vector<size_t> associations;
                unsigned recordedPositive=0;
                for(const auto& frame:decoded) {
                    float matrix[16]{},minimum[3]{},maximum[3]{};
                    require(video_format_get_parameters_for_format(hdr?VIDEO_CS_2100_PQ:VIDEO_CS_709,
                        VIDEO_RANGE_PARTIAL,frame.format,matrix,minimum,maximum),"decoder explicit color matrix");
                    for(size_t i=0;i<16;++i)require(std::abs(matrix[i]-frame.matrix[i])<=1e-6,"actual decoder primaries matrix changed");
                    auto decodedPicture=image::decoded_image(frame,hdr);
                    const auto id=image::content_id(decodedPicture);
                    require(image::content_id(frame)==id,"decoded luma/RGB landmark disagreement");
                    // Actual decoded wire chooses the producer; all clocks/content remain checks.
                    require(!previousId || packetIndex>0,"decoded occurrence order");
                    previousId=id;decodedIds.push_back(id);
                    const auto deliveryIndex=h->witness.decoded(frame,packetIndex,decoded.size(),
                        [](const DecodedFrame& f,size_t p,uint32_t x,uint32_t y){return image::code(f,p,x,y);});
                    associations.push_back(deliveryIndex);h->witness.healthy(associations);
                    const auto& delivery=h->witness.deliveries[deliveryIndex];
                    const auto wire=ProgramMarker::read(frame,[](const DecodedFrame& f,size_t p,uint32_t x,uint32_t y){return image::code(f,p,x,y);});
                    const auto before=h->preencode.begin()+static_cast<ptrdiff_t>(deliveryIndex);
                    const auto selected=h->rows.begin()+static_cast<ptrdiff_t>(delivery.use);
                    const auto rawWire=ProgramMarker::read(*before,
                        [](const DecodedFrame& f,size_t p,uint32_t x,uint32_t y){return image::code(f,p,x,y);});
                    require(rawWire==wire && delivery.use>0,"independent raw/decoded wire and startup manifest");
                    const auto predecessor=h->witness.uses[delivery.use-1].clock;
                    require(before->timestamp==predecessor &&
                        packetTransport[packetIndex].systemDtsUsec==static_cast<int64_t>(predecessor/1000),
                        "post-wire exact original raw/packet timestamp provenance");
                    require(image::content_id(*before)==id && selected->programId==id,"associated independent decoded content mismatch");
                    require(selected!=h->rows.end() && (deterministic?selected->kind==0:
                        (selected->kind==3 && selected->genuine) || (selected->kind==2 && selected->betweenViews)),
                        "preview-only, stale key or unexplained recording bypass cannot pass");
                    const StreamRate rate{1,1};
                    if(packetIndex) {
                        require(rate.timestamp_delta(frame.timestamp-previousTimestamp,1) &&
                            rate.timestamp_delta(before->timestamp-previousPreencodeTimestamp,1),"actual decoder/preencode rational timestamp drift");
                        require(packets[packetIndex].pts==packets[packetIndex-1].pts+1 &&
                            packets[packetIndex].dts==packets[packetIndex-1].dts+1,"actual encoded key order changed");
                    }
                    const auto& packet=packets[packetIndex];
                    require(packet.timebaseNumerator==1 && packet.timebaseDenominator==1 && packet.bytes>0,
                        "declared rational encoded packet timebase");
                    previousTimestamp=frame.timestamp;previousPreencodeTimestamp=before->timestamp;++packetIndex;
                    const auto [errors,checkedCodes]=recording_lossless_codes(frame,*before,
                        [](const DecodedFrame& f,size_t p,uint32_t x,uint32_t y){return image::code(f,p,x,y);});
                    require(errors==0,"real lossless recording differs from actual preencode plane codes");
                    double decodedDelta=0;
                    if(!deterministic) {
                        const auto& baseline=baselines[hdr?1:0];
                        const auto raw=std::find_if(baseline.begin(),baseline.end(),[&](const DecodedFrame& p){return image::content_id(p)==id;});
                        if(raw!=baseline.end()) {
                            for(uint32_t y=0;y<image::height;++y)for(uint32_t x=0;x<image::width;++x) {
                                const double delta=static_cast<double>(image::code(frame,0,x,y))-image::code(*raw,0,x,y);
                                decodedDelta+=delta*delta;
                            }
                            decodedDelta=std::sqrt(decodedDelta/(image::width*image::height));
                            if(selected->applied) {
                                require(decodedDelta>0,"actual independently decoded same-ID NR no-op");++recordedPositive;
                            }
                        }
                    }
                    double peak=0;for(const auto& p:decodedPicture.pixels)for(size_t c=0;c<3;++c)peak=std::max(peak,static_cast<double>(p[c])*(hdr?80:1));
                    if(hdr)require(peak>200,"HDR numeric highlight coerced to SDR");
                    if(id==h->pictureId)image::save_bmp(laneDirectory/"decoded.bmp",decodedPicture);
                    auto* row=obs_data_create();obs_data_set_int(row,"lane",lane);obs_data_set_int(row,"source_id",id);
                    obs_data_set_int(row,"tick",static_cast<int64_t>(selected->tick));
                    obs_data_set_int(row,"transport_occurrence",static_cast<int64_t>(selected->occurrence));
                    obs_data_set_int(row,"delivery_occurrence",static_cast<int64_t>(delivery.occurrence));
                    obs_data_set_int(row,"render_clock_ns",static_cast<int64_t>(selected->clock));
                    obs_data_set_int(row,"wire_id",wire);
                    obs_data_set_int(row,"raw_wire_id",rawWire);
                    obs_data_set_int(row,"observed_predecessor_clock_ns",static_cast<int64_t>(predecessor));
                    obs_data_set_int(row,"packet_sys_dts_usec",packetTransport[packetIndex-1].systemDtsUsec);
                    obs_data_set_int(row,"instance",static_cast<int64_t>(selected->instance));
                    obs_data_set_int(row,"content_epoch",static_cast<int64_t>(selected->epoch));
                    obs_data_set_int(row,"selected",static_cast<int64_t>(selected->selected));
                    obs_data_set_int(row,"history_epoch",static_cast<int64_t>(selected->history));
                    obs_data_set_int(row,"resource_generation",static_cast<int64_t>(selected->generation));
                    obs_data_set_int(row,"submission_serial",static_cast<int64_t>(selected->serial));
                    obs_data_set_int(row,"preencode_timestamp_ns",static_cast<int64_t>(before->timestamp));
                    obs_data_set_int(row,"decoder_timestamp_ns",static_cast<int64_t>(frame.timestamp));
                    obs_data_set_int(row,"packet_pts",packet.pts);obs_data_set_int(row,"packet_dts",packet.dts);
                    obs_data_set_int(row,"packet_timebase_numerator",packet.timebaseNumerator);
                    obs_data_set_int(row,"packet_timebase_denominator",packet.timebaseDenominator);
                    for(size_t plane=0;plane<3;++plane)if(!frame.planes[plane].empty()) {
                        const auto prefix="decoded-"+std::to_string(id)+"-plane-"+std::to_string(plane);
                        std::ofstream file(laneDirectory/(prefix+".bin"),std::ios::binary);
                        file.write(reinterpret_cast<const char*>(frame.planes[plane].data()),static_cast<std::streamsize>(frame.planes[plane].size()));
                        require(static_cast<bool>(file),"actual decoder plane artifact write");file.close();
                        obs_data_set_int(row,(prefix+"-row-bytes").c_str(),frame.rowBytes[plane]);
                        obs_data_set_int(row,(prefix+"-rows").c_str(),frame.rows[plane]);
                    }
                    for(size_t plane=0;plane<2;++plane) {
                        std::ofstream file(laneDirectory/("preencode-"+std::to_string(id)+"-plane-"+std::to_string(plane)+".bin"),std::ios::binary);
                        file.write(reinterpret_cast<const char*>(before->planes[plane].data()),static_cast<std::streamsize>(before->planes[plane].size()));
                        require(static_cast<bool>(file),"actual preencode plane artifact write");file.close();
                    }
                    obs_data_set_int(row,"width",frame.width);obs_data_set_int(row,"height",frame.height);
                    obs_data_set_int(row,"bit_depth",hdr?10:8);obs_data_set_int(row,"format",frame.format);
                    obs_data_set_int(row,"transfer",frame.transfer);obs_data_set_bool(row,"full_range",frame.fullRange);
                    obs_data_set_bool(row,"primaries_matrix_preserved",true);obs_data_set_int(row,"codes_compared",static_cast<int64_t>(checkedCodes));
                    obs_data_set_int(row,"code_errors",static_cast<int64_t>(errors));obs_data_set_double(row,"decoded_peak_linear_nits",peak);
                    obs_data_set_double(row,"same_id_raw_decoded_nr_rms_codes",decodedDelta);
                    emit("NR_RECORDING_DECODED ",row);
                }
                if(deterministic)baselines[hdr?1:0]=decoded;
                else require(recordedPositive>=3,"missing independently decoded same-ID raw/NR positive set");
                if(!deterministic)require(std::count_if(h->rows.begin(),h->rows.end(),[](const Row& r){return r.betweenViews;})==1,"missing exact completed-between-views challenge");
                auto* summary=obs_data_create();obs_data_set_int(summary,"lane",lane);
                obs_data_set_bool(summary,"hdr",hdr);obs_data_set_bool(summary,"nr_off",deterministic);
                obs_data_set_int(summary,"packets",static_cast<int64_t>(packets.size()));
                obs_data_set_int(summary,"decoded_frames",static_cast<int64_t>(decoded.size()));
                obs_data_set_int(summary,"picture_content_id",h->pictureId);
                obs_data_set_string(summary,"artifacts",laneDirectory.generic_string().c_str());
                obs_data_set_bool(summary,"display_tonemapped_only",hdr);
                obs_data_set_int(summary,"decoder_observation_fps",60);
                obs_data_set_int(summary,"decoder_elementary_stream_fps",1);
                emit("NR_RECORDING_LANE ",summary);
            }
            auto* close=obs_data_create();obs_data_set_int(close,"lane",lane);obs_data_set_bool(close,"closed",true);
            obs_data_set_bool(close,"worker_exit",true);obs_data_set_bool(close,"retained",false);
            emit("NR_RECORDING_CLEANUP ",close);
        }catch(const std::exception& e) {
            std::cout<<"NR_RECORDING_FAILURE lane="<<lane<<" "<<e.what()<<std::endl;
            h->observe.store(false,std::memory_order_release);
            if(h->custody.must_retain()) { // The finite wait is not a cancellation: the queued owner task still owns this lane.
                retained=true;
                std::cout<<"NR_RECORDING_RETAINED lane="<<lane<<" queued owner-task completion unobserved; recording/harness/raw registration preserved"<<std::endl;
                retainedRecordingGraph=std::move(h);
                return false;
            }
            if(retain_recording_failure(h->recording)) {
                retained=true;
                std::cout<<"NR_RECORDING_RETAINED lane="<<lane<<" exact encoder stop unresolved; graph/callback/session owners preserved"<<std::endl;
                retainedRecordingGraph=std::move(h);
                return false;
            }
            if(rawConnected)obs_remove_raw_video_callback(preencode,h.get());
            try { if(callbacks && h->source)stop_graph(*h); }
            catch(const std::exception& cleanup) {
                std::cout<<"NR_RECORDING_RETAINED lane="<<lane<<" "<<cleanup.what()<<std::endl;retained=true;
            }
            if(!retained && callbacks) { obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get()); }
            if(retained)retainedRecordingGraph=std::move(h);
            return false;
        }
    }
    std::cout<<"NR_RECORDING_SCOPE real_filter=true NR_off_current_raw=true Feature18=true SDR=true HDR=true recording=true independently_decoded=true quality=false performance=false cleanup=complete"<<std::endl;
    return true;
}

