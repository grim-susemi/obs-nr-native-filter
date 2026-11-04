// SPDX-License-Identifier: GPL-3.0-only
#include "obs_filter.h"
#include "obs_native_output.h"
#include "interop.h"
#include "nr_properties.h"
#include "capture_exclusion.h"
#include <obs-module.h>
#include <graphics/vec4.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
using nr::sampler::Admission;
std::atomic<uint64_t> nextInstance{1};
std::atomic<uint64_t> nextLifetime{1};
struct Filter;
struct Callback {
    std::atomic<Filter*> instance{nullptr};
    std::atomic<bool> cut{false};
};
struct Filter {
    obs_source_t *source;
    const uint64_t instance;
    nr::PerInstanceTick ticks;
    gs_texrender_t *capture = nullptr;
    gs_texrender_t *linearize = nullptr; // sRGB staging for drawing into GS_CS_SRGB_16F targets
    gs_texture_t *output = nullptr;
    std::unique_ptr<nr::obs_adapter::NativeOutput> native;
    nr::model::Config config;
    std::string initializationReason;
    std::string admittedRuntimeText,initialAdmissionReason;
    std::unique_ptr<Callback> callback=std::make_unique<Callback>();
    obs_weak_source_t* targetLifetime=nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> deviceLifetime;
    std::atomic<std::shared_ptr<const nr::properties::Configuration>> configuration;
    std::atomic<std::shared_ptr<const nr::properties::StatusSnapshot>> publicStatus{
        std::make_shared<const nr::properties::StatusSnapshot>(nr::properties::StatusSnapshot{"priming","awaitingObservation"})};
    std::atomic<bool> cut{false};
    uint64_t sourceToken=0,deviceToken=0,sourceCut=0;
    std::atomic<bool> placeholder{false};
    bool physicalSignalKnown=false;
    bool validityMetadataPresent=false;
    nr::SourceValidity sourceValidity=nr::SourceValidity::Unknown;
    nr::Reason lifecycleReason=nr::Reason::None;
    gs_color_format captureFormat=GS_UNKNOWN;
    bool stopping = false, nativeFailed = false, deterministicQa = false;
    bool destroyed = false; // Graphics-owned: destroy() ran; deletion waits for the graphics thread.
    bool capacityEvidence = false;
    bool captureExclusion = false; // Graphics-owned: holds nr::capture_exclusion (parent is a shown display capture).
    HANDLE capacityEvent = nullptr; // Non-owning; caller keeps it alive through unsubscribe.
    uint64_t capacityGeneration = 0;
    gs_color_space color = GS_CS_SRGB;
    uint64_t ingress = 0, clock = 0, interval = 0, appliedRevision = 0;
    uint32_t width = 0, height = 0;
    std::string loggedState,loggedReason;unsigned statusLogLines=0; // change-only status log (capped)
    int64_t lastTickQpc=0,tickStartQpc=0;std::string trace;unsigned gapN=0,admitN=0;double gapSum=0,gapMin=1e9,gapMax=0,admitSum=0,admitMax=0;
    uint64_t heldLogged=0;
    std::vector<std::pair<std::string,unsigned>> windowCounts; // frames per state/reason in the current 5 s window
    uint64_t windowStart=0,lastCountTime=0;unsigned windowFrames=0,windowLines=0;
    std::atomic<Admission> admission{Admission::NoTarget};
    explicit Filter(obs_source_t *s) : source(s), instance(nextInstance.fetch_add(1)), ticks(instance)
    {
        callback->instance.store(this,std::memory_order_release);
    }
};
struct Retiring {
    std::unique_ptr<nr::obs_adapter::NativeOutput> output;
    std::unique_ptr<Callback> callback;
};
std::vector<Retiring> retiring;
// Inert shells of destroyed filters (every owner already released by destroy()), freed by
// the graphics thread's next main render pass. video_tick runs on that thread, so a tick
// dispatched before destroy() has returned by then. Graphics-owned, like reclaimScheduled.
std::vector<std::unique_ptr<Filter>> destroyedFilters;
bool reclaimScheduled=false;
nr::lifecycle::CloseSnapshotStore lastClosure;
void retire(void*,uint32_t,uint32_t);
void retirement_snapshot(void*,calldata_t* data)
{
    // Primitive-only module state survives source procedure destruction. The
    // graphics mutex protects this copy; it owns no resource/callback lease.
    obs_enter_graphics();
    if(calldata_bool(data,"retry"))retire(nullptr,0,0);
    const auto snapshot=lastClosure.read(retiring.size());
    if(auto* out=static_cast<nr::lifecycle::CloseSnapshot*>(calldata_ptr(data,"snapshot")))*out=snapshot;
    calldata_set_bool(data,"normal_closed",nr::lifecycle::close_complete(snapshot));
    obs_leave_graphics();
}
bool close_owned(nr::obs_adapter::NativeOutput& output,bool detached)
{
    bool closed=false;
    try {closed=output.try_close();}
    catch(...) {
        auto snapshot=output.close_diagnostics(detached);
        snapshot.block=nr::lifecycle::CloseBlock::Exception;
        lastClosure.record(snapshot);
        return false;
    }
    lastClosure.record(output.close_diagnostics(detached));
    return closed;
}
void discontinuity(void* opaque,calldata_t*)
{
    // Signals can originate outside graphics. They touch only the retained
    // callback lifetime, never a Filter pointer that may be destroyed/reused.
    static_cast<Callback*>(opaque)->cut.store(true,std::memory_order_release);
}
void detach_target(Filter& f)
{
    if(f.targetLifetime) {
        if(auto* target=obs_weak_source_get_source(f.targetLifetime)) {
            for(const auto* signal:{"media_started","media_ended"})
                signal_handler_disconnect(obs_source_get_signal_handler(target),signal,discontinuity,f.callback.get());
            obs_source_release(target);
        }
        obs_weak_source_release(f.targetLifetime);f.targetLifetime=nullptr;
    }
    f.sourceToken=0;f.sourceCut=0;
}
Filter* callback(void* opaque) { return static_cast<Callback*>(opaque)->instance.load(std::memory_order_acquire); }
const nr::Selection* selection(const Filter& f,uint64_t tick)
{
    if(f.placeholder || f.callback->cut.load(std::memory_order_acquire))return nullptr;
    const auto* selected=f.ticks.peek(tick);
    if(selected && selected->frame && f.native && selected->frame->contentEpoch!=f.native->epochs().contentEpoch)return nullptr;
    return selected;
}
std::vector<std::byte> bytes(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("native filter shader unavailable");
    const auto length=file.tellg();
    if(length<=0) throw std::runtime_error("native filter shader empty");
    std::vector<std::byte> result(static_cast<size_t>(length));file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),length);
    if(!file) throw std::runtime_error("native filter shader read failed");
    return result;
}
void retire(void*,uint32_t,uint32_t)
{
    retiring.erase(std::remove_if(retiring.begin(),retiring.end(),[](const auto& owner) {
        return close_owned(*owner.output,true);
    }),retiring.end());
    if(retiring.empty())obs_remove_main_render_callback(retire,nullptr);
}
void reclaim(void*,uint32_t,uint32_t)
{
    destroyedFilters.clear();reclaimScheduled=false;
    obs_remove_main_render_callback(reclaim,nullptr);
}
const char *status(Admission value)
{
    switch (value) {
    case Admission::Ready: return "Sampler only: pass-through; NR inference unavailable";
    case Admission::NoTarget: return "Bypassed: no target";
    case Admission::ZeroExtent: return "Bypassed: zero extent";
    case Admission::UnsupportedRenderer: return "Bypassed: unsupported renderer (D3D11 required)";
    case Admission::UnsupportedColor: return "Bypassed: unsupported sampler color space";
    case Admission::CaptureFailed: return "Bypassed: capture failed";
    default: return "Bypassed: OBS video clock unavailable";
    }
}
const char *status(const Filter& f)
{
    if(f.nativeFailed) return "Error: native processing failed; original source shown";
    if(f.placeholder || f.callback->cut.load(std::memory_order_acquire)) {
        if(f.lifecycleReason==nr::Reason::RetirementBudget)return "Bypassed: retired generation budget exhausted";
        if(f.lifecycleReason==nr::Reason::SlotsFull)return "Bypassed: physical snapshot retirement pending";
        if(!obs_source_enabled(f.source))return "Disabled: current source shown";
        return "Bypassed: invalid or unsupported source/settings; current source shown";
    }
    if(!f.native || f.admission!=Admission::Ready) return status(f.admission);
    if(f.stopping) return f.native->closed()?"Stopped: resources retired":"Stopping: physical resources retained";
    const auto* selected=selection(f,nr::sampler::canonical_tick(f.clock,f.interval));
    if(selected && selected->status==nr::Status::Applied)
        return f.deterministicQa?"QA transport only: exact prior sample":"Applied: native NR exact prior sample";
    if(selected && selected->kind==nr::OutputKind::CurrentSource && selected->reason==nr::Reason::SlotsFull)
        return "Bypassed: native processing slots full; current source shown";
    return selected && selected->kind==nr::OutputKind::DelayedRaw?
        "Bypassed: untouched prior sample; processing not ready":"Priming: current raw sample";
}
void native_snapshot(void* opaque,calldata_t* data)
{
    auto* owner=callback(opaque);if(!owner){calldata_set_bool(data,"closed",false);return;}auto& f=*owner;
    // Exported owners (shared_ptr control blocks, future state, QaSignal) are released
    // by the host, possibly after a successful Close: keep this module mapped for them.
    for(const auto* field:{"retain_job","retain_selected_job","retain_raw","retain_current_raw","retain_context",
        "retain_bridge","retain_exit","retain_exit_signal","retain_dispatch_signals","diagnostics"})
        if(calldata_ptr(data,field)) {
            if(!nr::model::pin_module_for_export())OutputDebugStringA("NR export module pin failed");
            break;
        }
    if(auto* witness=static_cast<std::optional<nr::obs_adapter::TickWitness>*>(calldata_ptr(data,"tick_witness"))) {
        witness->reset();
        if(f.native)if(const auto value=f.native->tick_witness())witness->emplace(*value);
    }
    if(auto* counters=static_cast<nr::pipeline::Counters*>(calldata_ptr(data,"counters")))
        *counters=f.native?f.native->counters():nr::pipeline::Counters{};
    if(auto* diagnostics=static_cast<std::optional<nr::obs_adapter::Diagnostics>*>(calldata_ptr(data,"diagnostics")))
        diagnostics->emplace(f.native?f.native->diagnostics():nr::obs_adapter::Diagnostics{});
    if(auto* job=static_cast<std::shared_ptr<nr::model::Job>*>(calldata_ptr(data,"retain_job")))
        *job=f.native?f.native->current_job():nullptr;
    if(auto* context=static_cast<std::shared_ptr<nr::interop::Context>*>(calldata_ptr(data,"retain_context")))
        *context=f.native?f.native->context():nullptr;
    if(auto* job=static_cast<std::shared_ptr<nr::model::Job>*>(calldata_ptr(data,"retain_selected_job")))
        *job=f.native?f.native->selected_job():nullptr;
    if(auto* raw=static_cast<std::shared_ptr<const nr::obs_adapter::Capture>*>(calldata_ptr(data,"retain_raw")))
        *raw=f.native?f.native->selected_raw():nullptr;
    if(auto* raw=static_cast<std::shared_ptr<const nr::obs_adapter::Capture>*>(calldata_ptr(data,"retain_current_raw")))
        *raw=f.native?f.native->current_raw():nullptr;
    if(auto* bridge=static_cast<std::shared_ptr<nr::interop::Bridge>*>(calldata_ptr(data,"retain_bridge")))
        *bridge=f.native?f.native->transport():nullptr;
    if(auto* exited=static_cast<std::shared_future<void>*>(calldata_ptr(data,"retain_exit")))
        *exited=f.native?f.native->worker_exit():std::shared_future<void>{};
    if(auto* exited=static_cast<std::shared_ptr<nr::model::QaSignal>*>(calldata_ptr(data,"retain_exit_signal")))
        *exited=f.native?f.native->worker_exit_signal():nullptr;
    if(auto* signals=static_cast<std::array<std::shared_ptr<nr::model::QaSignal>,3>*>(calldata_ptr(data,"retain_dispatch_signals")))
        *signals=f.native?f.native->dispatch_signals():std::array<std::shared_ptr<nr::model::QaSignal>,3>{};
    calldata_set_ptr(data,"output_texture",f.output);
    calldata_set_ptr(data,"observation",f.native?f.native->observation():nullptr);
    calldata_set_string(data,"status",status(f));
    const auto publicStatus=f.publicStatus.load(std::memory_order_acquire);
    calldata_set_string(data,"public_state",publicStatus->state.c_str());
    calldata_set_string(data,"public_reason",publicStatus->reason.c_str());
    calldata_set_bool(data,"deterministic",f.deterministicQa);
    calldata_set_bool(data,"closed",!f.native || f.native->closed());
    calldata_set_bool(data,"failed",f.nativeFailed);
    calldata_set_int(data,"retiring",static_cast<int64_t>(retiring.size()));
    calldata_set_int(data,"resource_generation",f.native?f.native->epochs().resourceGeneration:0);
    calldata_set_int(data,"history_epoch",f.native?f.native->epochs().historyEpoch:0);
    calldata_set_int(data,"retired_generations",f.native?static_cast<int64_t>(f.native->retired_generations()):0);
    calldata_set_bool(data,"placeholder",f.placeholder || f.callback->cut.load(std::memory_order_acquire));
    calldata_set_bool(data,"physical_signal_known",f.physicalSignalKnown);
    calldata_set_bool(data,"validity_metadata_present",f.validityMetadataPresent);
    calldata_set_int(data,"source_validity",static_cast<int64_t>(f.sourceValidity));
    const auto control=f.configuration.load(std::memory_order_acquire);
    const auto configured=control?control->settings:nullptr;
    calldata_set_int(data,"settings_revision",configured?static_cast<int64_t>(configured->revision.value):0);
    calldata_set_int(data,"applied_settings_revision",static_cast<int64_t>(f.appliedRevision));
    const auto* selected=selection(f,nr::sampler::canonical_tick(f.clock,f.interval));
    calldata_set_int(data,"kind",selected?static_cast<int64_t>(selected->kind):0);
}
void capacity_watch(void* opaque,calldata_t* data)
{
    calldata_set_bool(data,"subscribed",false);
    obs_enter_graphics();
    if(auto* owner=callback(opaque)) {
        auto& f=*owner;
        f.capacityEvent=nullptr;f.capacityGeneration=0;
        const auto generation=calldata_int(data,"min_generation");
        const auto event=static_cast<HANDLE>(calldata_ptr(data,"event"));
        if(event && generation>0 && f.capacityEvidence && !f.stopping) {
            f.capacityEvent=event;f.capacityGeneration=static_cast<uint64_t>(generation);
            calldata_set_bool(data,"subscribed",true);
        }
    }
    obs_leave_graphics();
}
void native_stop(void* opaque,calldata_t* data)
{
    auto* owner=callback(opaque);if(!owner){calldata_set_bool(data,"closed",false);return;}auto& f=*owner;f.stopping=true;
    if(f.native) { f.native->request_stop();calldata_set_bool(data,"closed",close_owned(*f.native,false)); }
    else calldata_set_bool(data,"closed",true);
}
void native_observe(void* opaque,calldata_t* data)
{
    auto* owner=callback(opaque);if(!owner)return;auto& f=*owner;
    auto* observer=static_cast<std::shared_ptr<nr::model::ObservationSink>*>(calldata_ptr(data,"observer"));
    // Envelope signals get this module's future state bound into them: same export pin.
    if(observer && !nr::model::pin_module_for_export())OutputDebugStringA("NR export module pin failed");
    if(f.native&&observer)f.native->observe_with(*observer);
}
void original_marker_release(void* opaque,calldata_t* data)
{
    calldata_set_bool(data,"accepted",false);
    auto* owner=callback(opaque);if(!owner || !owner->native)return;
    const auto* key=static_cast<const nr::JobKey*>(calldata_ptr(data,"key"));
    if(key)calldata_set_bool(data,"accepted",owner->native->release_original_marker(*key));
}
// Read-only observation of production state, on the graphics owner thread.
void snapshot(void *opaque, calldata_t *data)
{
    const auto* owner=callback(opaque);if(!owner)return;const auto& f=*owner;
    calldata_set_int(data, "instance", f.instance);
    calldata_set_int(data, "tick", nr::sampler::canonical_tick(f.clock, f.interval));
    calldata_set_int(data, "clock", f.clock);
    calldata_set_int(data, "interval", f.interval);
    calldata_set_int(data, "ingress", f.ingress);
    calldata_set_int(data, "width", f.width);
    calldata_set_int(data, "height", f.height);
    calldata_set_int(data, "admission", static_cast<int>(f.admission.load()));
    calldata_set_string(data, "status", status(f));
    const auto *selected = selection(f,nr::sampler::canonical_tick(f.clock, f.interval));
    calldata_set_int(data, "selected", selected && selected->frame ? selected->frame->ingressSequence : 0);
    calldata_set_int(data, "kind", selected ? static_cast<int64_t>(selected->kind) : 0);
    calldata_set_int(data, "epoch", selected && selected->frame ? selected->frame->contentEpoch : 0);
    calldata_set_string(data, "color", f.admission == Admission::Ready ?
        (f.color==GS_CS_709_SCRGB?"ScRgbLinear":"Rec709Srgb") : "Unspecified");
    calldata_set_string(data, "alpha", f.admission == Admission::Ready ? "Premultiplied" : "Unspecified");
    calldata_set_int(data,"source_token",f.sourceToken);
    calldata_set_int(data,"device_token",f.deviceToken);
    calldata_set_int(data,"resource_generation",f.native?f.native->epochs().resourceGeneration:1);
    calldata_set_int(data,"history_epoch",f.native?f.native->epochs().historyEpoch:1);
    calldata_set_int(data,"content_epoch",f.native?f.native->epochs().contentEpoch:1);
    calldata_set_int(data,"lifecycle_reason",static_cast<int64_t>(f.lifecycleReason));
    const auto control=f.configuration.load(std::memory_order_acquire);
    const auto configured=control?control->settings:nullptr;
    calldata_set_int(data,"settings_revision",configured?static_cast<int64_t>(configured->revision.value):0);
    calldata_set_int(data,"applied_settings_revision",static_cast<int64_t>(f.appliedRevision));
    calldata_set_bool(data,"placeholder",f.placeholder || f.callback->cut.load(std::memory_order_acquire));
    calldata_set_bool(data,"physical_signal_known",f.physicalSignalKnown);
    calldata_set_bool(data,"validity_metadata_present",f.validityMetadataPresent);
    calldata_set_int(data,"source_validity",static_cast<int64_t>(f.sourceValidity));
}
void update(void* opaque,obs_data_t* values)
{
    auto& f=*static_cast<Filter*>(opaque);
    auto decoded=nr::properties::decode(values,f.admittedRuntimeText,f.config.runtime.canonicalPath.generic_string());
    const auto before=f.configuration.load(std::memory_order_acquire);
    const auto old=before?before->settings:nullptr;
    if(!old && !f.initialAdmissionReason.empty())decoded.reason=f.initialAdmissionReason;
    if(old && !f.native && !f.nativeFailed && decoded.reason.empty())decoded.reason="filterRecreateRequired";
    const auto next=nr::properties::updated(old,decoded);
    f.configuration.store(std::make_shared<const nr::properties::Configuration>(nr::properties::Configuration{
        next,decoded.reason}),std::memory_order_release);
    if(next!=old || !before || before->reason!=decoded.reason)f.callback->cut.store(true,std::memory_order_release);
    if(!decoded.reason.empty())f.publicStatus.store(std::make_shared<const nr::properties::StatusSnapshot>(
        nr::properties::StatusSnapshot{"bypassed",decoded.reason}),std::memory_order_release);
    else if(f.nativeFailed)f.publicStatus.store(std::make_shared<const nr::properties::StatusSnapshot>(
        nr::properties::StatusSnapshot{"error",f.initializationReason}),std::memory_order_release);
}
std::filesystem::path module_path(bool config,const char* relative)
{
    char* raw=config?obs_module_config_path(relative):obs_module_file(relative);
    const auto path=raw?nr::properties::utf8_path(raw):std::filesystem::path{};
    bfree(raw);
    return path;
}
// A driver GPU reset (TDR) removes the D3D12 device. D3D12 hands that same removed device to every later
// filter in this OBS process, so only an OBS restart recovers; name that cause instead of a generic failure.
// A running filter reads its device removed reason; a filter made after the reset has no active device yet,
// so the thrown device-lost HRESULT names it.
void note_gpu_reset(Filter& f,const std::exception& failure)
{
    const auto context=f.native?f.native->context():nullptr;
    ID3D12Device* device=context?context->device():nullptr;
    const HRESULT removed=nr::interop::device_lost_cause(device,failure);
    if(removed==S_OK)return;
    f.initializationReason="gpuDeviceReset";
    blog(LOG_WARNING,"[obs-nr-filter] GPU device removed by the driver: reason 0x%08X",static_cast<unsigned>(removed));
    for(const auto& line:nr::interop::dred_report(device,context?context->queue():nullptr))
        blog(LOG_WARNING,"[obs-nr-filter] %s",line.c_str());
}
void *create(obs_data_t *settings, obs_source_t *source)
{
    auto *f = new Filter(source);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void nr_sampler_snapshot(out int instance, out int tick, out int clock, out int interval, out int ingress, out int selected, out int kind, out int epoch, out int width, out int height, out int admission, out string status, out string color, out string alpha, out int source_token, out int device_token, out int resource_generation, out int history_epoch, out int content_epoch, out int lifecycle_reason, out bool placeholder, out bool physical_signal_known, out bool validity_metadata_present, out int source_validity)", snapshot, f->callback.get());
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_native_snapshot(in ptr retain_job, in ptr retain_selected_job, in ptr retain_raw, in ptr retain_current_raw, in ptr retain_context, in ptr retain_bridge, in ptr retain_exit, in ptr retain_exit_signal, in ptr retain_dispatch_signals, in ptr diagnostics, in ptr tick_witness, in ptr counters, out ptr output_texture, out ptr observation, out bool closed, out bool failed, out int retiring, out int kind, out bool deterministic, out string status, out string public_state, out string public_reason, out int resource_generation, out int history_epoch, out int retired_generations, out bool placeholder, out bool physical_signal_known)",native_snapshot,f->callback.get());
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_qa_capacity_watch(in ptr event, in int min_generation, out bool subscribed)",capacity_watch,f->callback.get());
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_native_stop(out bool closed)",native_stop,f->callback.get());
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_native_observe(in ptr observer)",native_observe,f->callback.get());
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_qa_original_marker_release(in ptr key, out bool accepted)",original_marker_release,f->callback.get());
    signal_handler_connect(obs_source_get_signal_handler(source),"enable",discontinuity,f->callback.get());
    const auto shaderOverride=obs_data_get_string(settings,"nr_shader_root");
    const auto dataOverride=obs_data_get_string(settings,"nr_data_root");
    const auto shaderRoot=*shaderOverride?nr::properties::utf8_path(shaderOverride):module_path(false,"shaders");
    f->admittedRuntimeText=obs_data_get_string(settings,"nr_runtime_path");
    const auto decoded=nr::properties::admit(settings);
    f->initialAdmissionReason=decoded.reason;
    if(decoded.reason.empty()) {
        try {
            f->deterministicQa=obs_data_get_bool(settings,"nr_qa_deterministic");
            f->capacityEvidence=(f->deterministicQa || obs_data_get_bool(settings,"nr_qa_observe")) &&
                obs_data_get_bool(settings,"nr_qa_capacity_evidence");
            f->config.runtime={std::filesystem::path(decoded.runtimePath),nr::properties::runtime_sha256};
            f->config.driver=nr::runtime::resolve_driver().identity.canonicalPath;
            const auto dataRoot=*dataOverride?nr::properties::utf8_path(dataOverride):module_path(true,"runtime");
            if(dataRoot.empty())throw std::runtime_error("module configuration directory unavailable");
            std::filesystem::create_directories(dataRoot);
            f->config.data=std::filesystem::canonical(dataRoot);
            f->config.codec=bytes(shaderRoot/"nr_codec.dxil");f->config.color=bytes(shaderRoot/"nr_color.dxil");
            f->config.guides=bytes(shaderRoot/"nr_guides.cso");
            f->config.frameIntervalNs=obs_get_frame_interval_ns();
            f->config.qaObservations=obs_data_get_bool(settings,"nr_qa_observe");
            f->config.qaPreparation=obs_data_get_bool(settings,"nr_qa_prepare");
            f->native=std::make_unique<nr::obs_adapter::NativeOutput>(f->instance,f->config,
                bytes(shaderRoot/"nr_interop_vertex.cso"),bytes(shaderRoot/"nr_interop_pixel.cso"),
                bytes(shaderRoot/"nr_obs_analysis.cso"),f->deterministicQa);
        } catch(const std::exception& e) { f->nativeFailed=true;f->initializationReason=e.what();blog(LOG_WARNING,"[obs-nr-filter] create failed: %s",e.what());note_gpu_reset(*f,e); }
    }
    blog(LOG_INFO,"[obs-nr-filter] create: admissionReason='%s' native=%d failed=%d initReason='%s'",f->initialAdmissionReason.c_str(),(int)(f->native!=nullptr),(int)f->nativeFailed,f->initializationReason.c_str());
    update(f,settings);
    return f;
}
void destroy(void *opaque)
{
    auto *f = static_cast<Filter *>(opaque);
    // libobs has no proc_handler_remove. Tombstone the procedure owner before
    // destruction; source-owned procedure storage ends with this source lifetime.
    f->callback->instance.store(nullptr,std::memory_order_release);
    signal_handler_disconnect(obs_source_get_signal_handler(f->source),"enable",discontinuity,f->callback.get());
    detach_target(*f);
    obs_enter_graphics();
    if(f->native) {
        f->native->request_stop();
        if(!close_owned(*f->native,true)) {
            if(retiring.empty())obs_add_main_render_callback(retire,nullptr);
            retiring.push_back({std::move(f->native),std::move(f->callback)});
            // A retained vendor callback/worker may still execute plugin code.
            // Pin this module rather than unload live code on unresolved close.
            HMODULE module=nullptr;
            (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&retire),&module);
        }
    }
    gs_texture_destroy(f->output);f->output=nullptr;
    gs_texrender_destroy(f->capture);f->capture=nullptr;
    gs_texrender_destroy(f->linearize);f->linearize=nullptr;
    // A video_tick already dispatched on the graphics thread may still be before its
    // graphics entry; it then reads only `destroyed`. Release every owner here, as before,
    // and let only the inert Filter shell wait for that thread's next render pass.
    const bool exclusion=std::exchange(f->captureExclusion,false);
    f->destroyed=true;
    f->native.reset();f->callback.reset();f->deviceLifetime.Reset();
    f->configuration.store(nullptr,std::memory_order_release);
    f->publicStatus.store(nullptr,std::memory_order_release);
    const bool schedule=!reclaimScheduled;reclaimScheduled=true;
    destroyedFilters.emplace_back(f);
    obs_leave_graphics();
    if(exclusion)nr::capture_exclusion::leave();
    if(schedule)obs_add_main_render_callback(reclaim,nullptr);
}
obs_properties_t *properties(void *opaque)
{
    const auto value=opaque?static_cast<Filter*>(opaque)->publicStatus.load(std::memory_order_acquire):
        std::make_shared<const nr::properties::StatusSnapshot>(nr::properties::StatusSnapshot{"priming","awaitingObservation"});
    nr::properties::Labels labels;
    labels.enabled=obs_module_text("NR.Enabled");labels.runtime=obs_module_text("NR.Runtime");
    labels.strength=obs_module_text("NR.Strength");labels.preset=obs_module_text("NR.Preset");
    labels.style=obs_module_text("NR.Style");labels.styleStandard=obs_module_text("NR.Style.Standard");
    labels.styleNatural=obs_module_text("NR.Style.Natural");labels.styleCinematic=obs_module_text("NR.Style.Cinematic");
    labels.intensity=obs_module_text("NR.Intensity");labels.localStructure=obs_module_text("NR.LocalStructure");
    labels.localTone=obs_module_text("NR.LocalTone");labels.skinStructure=obs_module_text("NR.SkinStructure");
    labels.autoMask=obs_module_text("NR.AutoMask");
    // Display only: public state/reason codes stay stable (host tests and QA read them); translate here.
    const auto localized=[](const char* prefix,std::string code)->std::string {
        struct Sentence{const char* text;const char* id;};
        static const Sentence sentences[]{
            {"Bypassed: no target","noTarget"},{"Bypassed: zero extent","zeroExtent"},
            {"Bypassed: unsupported renderer (D3D11 required)","unsupportedRenderer"},
            {"Bypassed: unsupported sampler color space","unsupportedColor"},
            {"Bypassed: capture failed","captureFailed"},{"Bypassed: OBS video clock unavailable","videoClockUnavailable"}};
        for(const auto& s:sentences)if(code==s.text){code=s.id;break;}
        const std::string key=std::string(prefix)+code;
        const char* text=obs_module_text(key.c_str());
        return text && key!=text?std::string(text)+" ("+code+")":code;
    };
    const auto statusText=localized("NR.State.",value->state);
    const auto reasonText=localized("NR.Reason.",value->reason);
    labels.statusText=statusText.c_str();labels.reasonText=reasonText.c_str();
    return nr::properties::controls(*value,labels);
}
void publish_status(Filter& f)
{
    nr::properties::StatusSnapshot value{"priming","awaitingSample"};
    const auto control=f.configuration.load(std::memory_order_acquire);
    const auto configured=control?control->settings:nullptr;
    if(control && !control->reason.empty())value={"bypassed",control->reason};
    else if(f.nativeFailed)value={"error",f.initializationReason.empty()?"nativeProcessingFailed":f.initializationReason};
    else if(configured && !nr::properties::effective_enabled(configured->enabled,obs_source_enabled(f.source)))
        value={"bypassed","disabled"};
    else if(f.stopping)value={"bypassed","stopping"};
    else if(f.placeholder && (f.lifecycleReason==nr::Reason::RetirementBudget || f.lifecycleReason==nr::Reason::SlotsFull))
        value=nr::properties::status(nr::Status::Bypassed,f.lifecycleReason);
    else if(f.native && configured && f.appliedRevision!=configured->revision.value)value={"bypassed","awaitingSettingsObservation"};
    else if(f.placeholder)value=nr::properties::status(nr::Status::Bypassed,f.lifecycleReason);
    else if(f.admission!=Admission::Ready)value={"bypassed",status(f.admission)};
    else if(const auto* selected=selection(f,nr::sampler::canonical_tick(f.clock,f.interval)))
        value=f.deterministicQa && selected->status==nr::Status::Applied?
            nr::properties::StatusSnapshot{"bypassed","qaTransportOnly"}:nr::properties::status(selected->status,selected->reason);
    if(f.statusLogLines<40 && (value.state!=f.loggedState || value.reason!=f.loggedReason)) {
        f.loggedState=value.state;f.loggedReason=value.reason;++f.statusLogLines;
        blog(LOG_INFO,"[obs-nr-filter] status #%u: %s / %s (instance %llu, %ux%u, frame interval %llu ns, ingress %llu)%s",
            f.statusLogLines,value.state.c_str(),value.reason.c_str(),static_cast<unsigned long long>(f.instance),
            f.width,f.height,static_cast<unsigned long long>(f.interval),static_cast<unsigned long long>(f.ingress),
            f.statusLogLines==40?" - later single changes are not logged; see window lines":"");
    }
    if(const auto now=obs_get_video_frame_time(); now && now!=f.lastCountTime) {
        f.lastCountTime=now;
        if(!f.windowStart)f.windowStart=now;
        const std::string key=value.state+"/"+value.reason;
        bool found=false;
        for(auto& e:f.windowCounts)if(e.first==key){++e.second;found=true;break;}
        if(!found)f.windowCounts.emplace_back(key,1u);
        ++f.windowFrames;
        if(f.windowLines<240)f.trace.push_back(key=="ready/none"?'R':key=="bypassed/deadlineMiss"?'M':key=="bypassed/slotsFull"?'S':value.state=="priming"?'P':key=="bypassed/keyMismatch"?'K':key=="bypassed/disabled"?'D':'?');
        if(now-f.windowStart>=5000000000ull && f.windowLines<240) {
            std::string parts;const auto tm=f.gapN?nr::model::take_job_timing():nr::model::JobTimingWindow{};
            const double gapMean=f.gapN?f.gapSum/f.gapN:0.0,admitMean=f.admitN?f.admitSum/f.admitN:0.0;
            for(const auto& e:f.windowCounts)parts+=" "+e.first+"="+std::to_string(e.second);
            {const uint64_t held=f.native?f.native->held_frames():0;parts+=" held="+std::to_string(held>=f.heldLogged?held-f.heldLogged:held);f.heldLogged=held;}
            if(f.native)parts+=f.native->diag();
            blog(LOG_INFO,"[obs-nr-filter] window %.1fs instance %llu %ux%u interval %llu ns frames %u jobs %u submit->done mean %.1f max %.1f ms, encode->done mean %.1f max %.1f ms, admit->done mean %.1f max %.1f ms late %u, tick gap mean %.1f min %.1f max %.1f ms n %u, tick->admit mean %.1f max %.1f ms, stages queue %.1f/%.1f producer %.1f/%.1f prep %.1f record %.1f gpu %.1f ms, creation jobs %u mean %.1f max %.1f ms, trace %s, lastTick %llu, jobs [%s]:%s",
                static_cast<double>(now-f.windowStart)/1e9,static_cast<unsigned long long>(f.instance),f.width,f.height,
                static_cast<unsigned long long>(f.interval),f.windowFrames,tm.jobs,tm.submitMeanMs,tm.submitMaxMs,tm.encodeMeanMs,tm.encodeMaxMs,tm.admitMeanMs,tm.admitMaxMs,tm.late,gapMean,f.gapMin>1e8?0.0:f.gapMin,f.gapMax,f.gapN,admitMean,f.admitMax,tm.queueMeanMs,tm.queueMaxMs,tm.prodMeanMs,tm.prodMaxMs,tm.prepMeanMs,tm.recMeanMs,tm.gpuMeanMs,tm.creationJobs,tm.creationMeanMs,tm.creationMaxMs,(f.trace.size()>96?f.trace.substr(f.trace.size()-96):f.trace).c_str(),static_cast<unsigned long long>(nr::sampler::canonical_tick(f.clock,f.interval)),nr::model::take_job_trace().c_str(),parts.c_str());
            ++f.windowLines;f.windowCounts.clear();f.windowFrames=0;f.windowStart=now;
            f.gapSum=0;f.gapMin=1e9;f.gapMax=0;f.gapN=0;f.admitSum=0;f.admitMax=0;f.admitN=0;f.trace.clear();
        }
    }
    f.publicStatus.store(std::make_shared<const nr::properties::StatusSnapshot>(std::move(value)),std::memory_order_release);
}
void bypass(Filter &f)
{
    // OBS skip dereferences the parent; unattached filters draw nothing.
    if (obs_filter_get_target(f.source) && obs_filter_get_parent(f.source)) obs_source_skip_video_filter(f.source);
}
std::optional<nr::lifecycle::Observation> observe(Filter& f,uint64_t tick)
{
    auto* target=obs_filter_get_target(f.source);
    const bool attached=target && obs_filter_get_parent(f.source);
    if(!attached)detach_target(f);
    else if(!f.targetLifetime || !obs_weak_source_references_source(f.targetLifetime,target)) {
        detach_target(f);
        f.targetLifetime=obs_source_get_weak_source(target);
        f.sourceToken=nextLifetime.fetch_add(1);
        for(const auto* signal:{"media_started","media_ended"})
            signal_handler_connect(obs_source_get_signal_handler(target),signal,discontinuity,f.callback.get());
    }
    auto* device=gs_get_device_type()==GS_DEVICE_DIRECT3D_11?static_cast<ID3D11Device*>(gs_get_device_obj()):nullptr;
    if(device && (!f.deviceLifetime || !nr::interop::same_identity(device,f.deviceLifetime.Get()))) {
        f.deviceLifetime=device;f.deviceToken=nextLifetime.fetch_add(1);
    } else if(!device) { f.deviceLifetime.Reset();f.deviceToken=0; }
    f.width=attached?obs_source_get_width(target):0;f.height=attached?obs_source_get_height(target):0;
    const gs_color_space preferred[]{GS_CS_709_SCRGB,GS_CS_SRGB};
    f.color=attached?obs_source_get_color_space(target,2,preferred):GS_CS_SRGB;
    f.admission=nr::sampler::admit(attached,device!=nullptr,f.width,f.height,
        f.color==GS_CS_SRGB || (f.native && f.color==GS_CS_709_SCRGB));
    nr::SourceValidity validity=attached?nr::SourceValidity::Valid:nr::SourceValidity::Invalid;
    f.physicalSignalKnown=false;
    f.validityMetadataPresent=false;
    if(attached) {
        calldata_t signal;calldata_init(&signal);
        if(proc_handler_call(obs_source_get_proc_handler(target),"nr_source_lifecycle",&signal)) {
            const auto value=calldata_int(&signal,"validity");
            validity=value==1?nr::SourceValidity::Valid:value==2?nr::SourceValidity::Invalid:nr::SourceValidity::Unknown;
            f.validityMetadataPresent=true;
            f.physicalSignalKnown=value==1 || value==2;
            const auto serial=static_cast<uint64_t>(calldata_int(&signal,"cut"));
            if(serial!=f.sourceCut) { f.cut.store(true);f.sourceCut=serial; }
        }
        calldata_free(&signal);
    }
    f.sourceValidity=validity;
    if(!f.native || f.nativeFailed)return {};
    const auto control=f.configuration.load(std::memory_order_acquire);
    const auto configured=control->settings;
    const bool hdr=f.color==GS_CS_709_SCRGB;
    const nr::FrameDescriptor descriptor{{f.width,f.height},{0,0,f.width,f.height},
        f.admission==Admission::Ready?(hdr?nr::PixelFormat::Rgba16Float:nr::PixelFormat::Rgba8):nr::PixelFormat::Unknown,
        hdr?nr::ColorSpace::ScRgbLinear:nr::ColorSpace::Rec709Srgb,nr::AlphaMode::Premultiplied,80};
    const nr::lifecycle::Observation observation{tick,f.sourceToken,f.deviceToken,validity,descriptor,
        {configured->revision,nr::properties::effective_enabled(configured->enabled,obs_source_enabled(f.source)),
         !control->reason.empty()?std::string{}:configured->runtimePath,configured->strength,configured->tuning}};
    const bool cut=f.cut.exchange(false);
    const bool signalled=f.callback->cut.exchange(false,std::memory_order_acq_rel);
    const auto prepared=f.native->prepare(observation,device,cut || signalled);
    f.placeholder=prepared.placeholder.has_value();
    f.lifecycleReason=prepared.placeholder?prepared.placeholder->reason:nr::Reason::None;
    if(!retiring.empty()) { f.placeholder=true;f.lifecycleReason=nr::Reason::RetirementBudget; }
    if(f.capacityEvent && f.native->epochs().resourceGeneration>=f.capacityGeneration &&
        f.native->retired_generations()==0 && !f.placeholder && !f.nativeFailed && !f.stopping) {
        const auto event=f.capacityEvent;
        f.capacityEvent=nullptr;f.capacityGeneration=0;
        SetEvent(event);
    }
    return observation;
}
// A display capture of the monitor that shows an OBS projector re-captures this filter's own
// output every frame; keep OBS windows out of screen capture while that can happen.
void hold_capture_exclusion(Filter& f)
{
    obs_source_t* parent=obs_filter_get_parent(f.source);
    const char* kind=parent?obs_source_get_unversioned_id(parent):nullptr;
    const bool want=kind && std::strcmp(kind,"monitor_capture")==0 && obs_source_showing(parent);
    if(want==f.captureExclusion) { if(want)nr::capture_exclusion::refresh();return; }
    f.captureExclusion=want;
    if(want)nr::capture_exclusion::enter();else nr::capture_exclusion::leave();
}
void video_tick(void* opaque,float)
{
    auto& f=*static_cast<Filter*>(opaque);
    // destroy() mutates this state under graphics; read it only there.
    obs_enter_graphics();
    if(!f.destroyed) hold_capture_exclusion(f);
    if(f.destroyed || !f.native || f.stopping || f.nativeFailed) { obs_leave_graphics();return; }
    try {
        if(f.native->latch_qa_failure()) {
            f.nativeFailed=true;f.placeholder=true;
            f.initializationReason="handledAdapterFailure";blog(LOG_WARNING,"[obs-nr-filter] handled adapter failure latched");
        } else (void)observe(f,nr::sampler::canonical_tick(obs_get_video_frame_time(),obs_get_frame_interval_ns()));
    }
    catch(const std::exception& e) { f.nativeFailed=true;blog(LOG_WARNING,"[obs-nr-filter] video_tick failed: %s",e.what());note_gpu_reset(f,e); }
    publish_status(f);
    obs_leave_graphics();
}
void filter_remove(void* opaque,obs_source_t*)
{
    auto& f=*static_cast<Filter*>(opaque);
    obs_enter_graphics();
    detach_target(f);f.cut.store(true);f.placeholder=true;
    // A control-thread removal invalidates adapter publication immediately.
    // The next graphics observation applies the cut; no accepting pipeline or
    // Bridge operation may be created or mutated on the control thread.
    const bool exclusion=std::exchange(f.captureExclusion,false);
    obs_leave_graphics();
    if(exclusion)nr::capture_exclusion::leave();
}
void render(void *opaque, gs_effect_t *)
{
    auto &f = *static_cast<Filter *>(opaque);
    struct PublishOnReturn {Filter& filter;~PublishOnReturn(){publish_status(filter);}} publish{f};
    if(f.stopping) {
        if(f.native) (void)close_owned(*f.native,false);
        bypass(f);return;
    }
    const auto time = obs_get_video_frame_time();
    const auto interval = obs_get_frame_interval_ns();
    const auto tick = nr::sampler::canonical_tick(time, interval);
    // Off-owner renders (screenshot requests) may only reuse a tick the owner already latched; they must not claim one.
    if(f.native && !f.native->on_graphics_owner() && !f.ticks.peek(tick)) { bypass(f);return; }
    const auto action = f.ticks.claim(tick);
    if (action == nr::TickAction::CaptureOnce) {
        f.clock = time;
        f.interval = interval;
        {const auto q=nr::model::qpc_now();f.tickStartQpc=q;
         if(f.lastTickQpc){const double g=nr::model::qpc_ms(q-f.lastTickQpc);f.gapSum+=g;f.gapMin=g<f.gapMin?g:f.gapMin;f.gapMax=g>f.gapMax?g:f.gapMax;++f.gapN;}
         f.lastTickQpc=q;}
        std::optional<nr::lifecycle::Observation> observed;
        try { if(const auto value=observe(f,tick))observed.emplace(*value); }
        catch(const std::exception& e) { f.nativeFailed=true;f.placeholder=true;blog(LOG_WARNING,"[obs-nr-filter] observe in render failed: %s",e.what());note_gpu_reset(f,e); }
        const auto space=f.color;
        const auto control=f.configuration.load(std::memory_order_acquire);
        const auto configured=control?control->settings:nullptr;
        const bool enabled=configured && nr::properties::effective_enabled(configured->enabled,obs_source_enabled(f.source));
        if (f.admission == Admission::Ready && !f.nativeFailed && !f.placeholder && enabled && control->reason.empty()) {
            const auto format=space==GS_CS_SRGB?GS_RGBA:GS_RGBA16F;
            if(f.captureFormat!=format) {
                gs_texrender_destroy(f.capture);f.capture=nullptr;gs_texture_destroy(f.output);f.output=nullptr;
                f.captureFormat=format;
            }
            if (!f.capture) f.capture = gs_texrender_create(format, GS_ZS_NONE);
            gs_texrender_reset(f.capture);
            gs_blend_state_push();
            const bool srgb = gs_framebuffer_srgb_enabled();
            gs_enable_framebuffer_srgb(false);
            if (gs_texrender_begin_with_color_space(f.capture, f.width, f.height, space)) {
                vec4 clear{};
                gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
                gs_ortho(0, static_cast<float>(f.width), 0, static_cast<float>(f.height), -100, 100);
                gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
                obs_source_skip_video_filter(f.source);
                gs_texrender_end(f.capture);
                ++f.ingress;
            } else f.admission = Admission::CaptureFailed;
            gs_enable_framebuffer_srgb(srgb);
            gs_blend_state_pop();
            if(f.admission==Admission::CaptureFailed && f.native && observed) {
                const auto& old=*observed;
                try {
                    (void)f.native->prepare({tick,old.sourceIdentity,old.deviceIdentity,nr::SourceValidity::Invalid,
                        old.descriptor,old.settings},f.deviceLifetime.Get());
                }catch(const std::exception& e) { f.nativeFailed=true;blog(LOG_WARNING,"[obs-nr-filter] capture-failed prepare failed: %s",e.what());note_gpu_reset(f,e); }
                f.placeholder=true;f.lifecycleReason=nr::Reason::InvalidSource;
            }
        }
        nr::Selection selected{tick,
            f.admission == Admission::Ready && !f.placeholder ? nr::OutputKind::PrimingRaw : nr::OutputKind::CurrentSource,
            obs_source_enabled(f.source)?nr::Status::Bypassed:nr::Status::Disabled, f.lifecycleReason,
            f.admission == Admission::Ready && !f.placeholder ? std::optional<nr::FrameKey>{{f.instance, 1, f.ingress}} : std::nullopt,
            std::nullopt, false};
        if(f.native && f.admission==Admission::Ready && !f.placeholder && !f.nativeFailed) {
            try {
                const bool hdr=space==GS_CS_709_SCRGB;
                if(f.output && (gs_texture_get_width(f.output)!=f.width || gs_texture_get_height(f.output)!=f.height)) {
                    gs_texture_destroy(f.output);f.output=nullptr;
                }
                if(!f.output) f.output=gs_texture_create(f.width,f.height,hdr?GS_RGBA16F:GS_RGBA_UNORM,1,nullptr,0);
                if(!f.output) throw std::runtime_error("native filter output texture unavailable");
                const auto epochs=f.native->epochs();
                const nr::IngressSample input{{f.instance,epochs.contentEpoch,f.ingress},tick,tick-1,
                    observed->sourceIdentity,observed->deviceIdentity,
                    epochs.resourceGeneration,observed->validity,observed->descriptor,observed->settings};
                {const auto q=nr::model::qpc_now();nr::model::note_admit(tick,q);const double o=nr::model::qpc_ms(q-f.tickStartQpc);
             f.admitSum+=o;f.admitMax=o>f.admitMax?o:f.admitMax;++f.admitN;}
            const auto result=f.native->tick(input,
                    static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(f.capture))),
                    static_cast<ID3D11Texture2D*>(gs_texture_get_obj(f.output)));
                f.appliedRevision=input.settings.revision.value;
                std::destroy_at(&selected);std::construct_at(&selected,result);
            } catch(const std::exception& e) { f.nativeFailed=true;blog(LOG_WARNING,"[obs-nr-filter] native tick failed: %s",e.what());note_gpu_reset(f,e); }
        }
        f.ticks.latch(tick, selected);
    }
    if (!tick) f.admission = Admission::ClockUnavailable;
    const auto* latched=selection(f,tick);
    const auto control=f.configuration.load(std::memory_order_acquire);
    const auto configured=control?control->settings:nullptr;
    if ((action != nr::TickAction::CaptureOnce && action != nr::TickAction::ReuseLatch) ||
        !configured || !nr::properties::effective_enabled(configured->enabled,obs_source_enabled(f.source)) ||
        (f.native && configured && f.appliedRevision!=configured->revision.value) ||
        (control && !control->reason.empty()) || f.admission != Admission::Ready || f.placeholder || !latched || latched->kind==nr::OutputKind::CurrentSource) {
        bypass(f);
        return;
    }
    auto *texture = f.native && !f.nativeFailed?f.output:gs_texrender_get_texture(f.capture);
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    auto *image = gs_effect_get_param_by_name(effect, "image");
    // A GS_CS_SRGB_16F target (a projector on a >8-bit SDR monitor) stores linear light, but the
    // sRGB output holds encoded values. Un-premultiply into an sRGB texrender, then sample it
    // through its sRGB view, as libobs does for its own color-space conversions.
    if (f.color == GS_CS_SRGB && gs_get_color_space() == GS_CS_SRGB_16F) {
        if (!f.linearize) f.linearize = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        gs_texrender_reset(f.linearize);
        gs_blend_state_push();
        gs_enable_blending(false);
        if (gs_texrender_begin_with_color_space(f.linearize, f.width, f.height, GS_CS_SRGB)) {
            gs_ortho(0, static_cast<float>(f.width), 0, static_cast<float>(f.height), -100, 100);
            gs_effect_set_texture(image, texture);
            while (gs_effect_loop(effect, "DrawAlphaDivide")) gs_draw_sprite(texture, 0, f.width, f.height);
            gs_texrender_end(f.linearize);
            texture = gs_texrender_get_texture(f.linearize);
        } else texture = nullptr;
        gs_blend_state_pop();
        if (texture) {
            gs_enable_framebuffer_srgb(true);
            gs_effect_set_texture_srgb(image, texture);
            while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(texture, 0, f.width, f.height);
        } else bypass(f);
        gs_enable_framebuffer_srgb(srgb);
        return;
    }
    gs_effect_set_texture(image, texture);
    // The private capture is premultiplied; OBS's normal scene blend expects straight RGB.
    while (gs_effect_loop(effect, "DrawAlphaDivide")) gs_draw_sprite(texture, 0, f.width, f.height);
    gs_enable_framebuffer_srgb(srgb);
}
gs_color_space color_space(void* opaque,size_t count,const gs_color_space* preferred)
{
    auto& f=*static_cast<Filter*>(opaque);
    auto* target=obs_filter_get_target(f.source);
    return target?obs_source_get_color_space(target,count,preferred):GS_CS_SRGB;
}
const char *name(void *) { return "Native NR"; }
}
extern "C" void nr_register_obs_filter(void)
{
    // A previous libobs instance in this process dropped its render callbacks; no source of
    // this type exists yet, so its remaining inert shells are unreachable.
    destroyedFilters.clear();reclaimScheduled=false;
    proc_handler_add(obs_get_proc_handler(),"void nr_retirement_snapshot(in ptr snapshot, out bool normal_closed)",retirement_snapshot,nullptr);
    obs_source_info info{};
    info.id = "susemi_nr_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.video_tick = video_tick;
    info.filter_remove = filter_remove;
    info.get_properties = properties;
    info.get_defaults = nr::properties::defaults;
    info.video_render = render;
    info.video_get_color_space = color_space;
    obs_register_source(&info);
}
