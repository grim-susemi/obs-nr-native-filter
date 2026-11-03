// SPDX-License-Identifier: GPL-3.0-only
#include "obs_native_output.h"
#include <obs.h>
#include <algorithm>
#include <list>
#include <stdexcept>
#include <cstring>
namespace nr::obs_adapter {
namespace {
using Microsoft::WRL::ComPtr;
void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
void checked(HRESULT value,const char* why) { require(SUCCEEDED(value),why); }
struct Record {
    interop::Ticket ticket;
    std::optional<interop::Consumer> pin;
    std::shared_ptr<model::Job> job;
    std::shared_ptr<interop::Bridge> bridge;
};
// Explicit QA transport processor, never a model success. Uses the SAME production
// scheduler, Bridge, capture pin and output consumer as real model processing.
struct CopyProcessor final:pipeline::Processor {
    std::atomic<bool> closed{false};
    pipeline::Processed process(const pipeline::Work& work,std::stop_token) override {
        auto job=std::static_pointer_cast<model::Job>(work.binding.processorLease);
        const auto view=work.binding.transport->begin_worker(work.binding.ticket);
        require(job && job->contract==work.job && view.has_value(),"QA exact copy binding");
        auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
            view->commands->ResourceBarrier(1,&b);
        };
        transition(view->input,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(view->output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        view->commands->CopyResource(view->output,view->input);
        transition(view->input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(view->output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        checked(work.binding.transport->worker_fence()->SetEventOnCompletion(view->completionValue,job->gpu),
            "QA copy pre-submit physical event");
        job->output=view->output;
        job->submitted=work.binding.transport->submit_worker(work.binding.ticket);
        require(job->submitted && WaitForSingleObject(job->gpu,15000)==WAIT_OBJECT_0 &&
            interop::completion(view->completionValue,work.binding.transport->worker_fence()->GetCompletedValue())==
                interop::Completion::Complete,"QA actual copy completion");
        job->eligible=true;job->ready.store(true,std::memory_order_release);
        if(auto* observation=job->observation) {
            observation->completionFence=work.binding.transport->worker_fence();
            observation->completionValue=view->completionValue;
            observation->submitted=job->submitted;
            job->observation=nullptr;
            observation->finished.store(true,std::memory_order_release);
            SetEvent(observation->observed);
        }
        SetEvent(job->observed);
        return {work.job,true,job}; // Processor result, NOT Feature18 evaluated/composed.
    }
    void worker_stopping() noexcept override { closed.store(true,std::memory_order_release); }
};
}
struct NativeOutput::Impl {
    struct Generation final:pipeline::Processor {
    const uint64_t id;
    const FrameDescriptor descriptor;
    ComPtr<ID3D11Device> renderer;
    ComPtr<ID3D11DeviceContext4> graphics;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> analysis;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3DDeviceContextState> analysisState;
    std::shared_ptr<interop::Context> context;
    std::shared_ptr<interop::Bridge> bridge;
    std::shared_ptr<pipeline::Processor> processor;
    std::shared_ptr<model::Processor> model;
    std::shared_ptr<CopyProcessor> copy;
    ComPtr<ID3D11Fence> graphicsFence;
    ComPtr<ID3D11Texture2D> targetLease;
    uint64_t graphicsSerial=0;
    std::atomic<bool> closeRequested{false},closed{false};
    lifecycle::SnapshotRetirement cutoffRaw;
    Generation(uint64_t generation,ID3D11Device* device,const FrameDescriptor& frame,
        const model::Config& config,uint64_t instance,bool deterministicQa,
        const std::vector<std::byte>& vertexBytes,const std::vector<std::byte>& pixelBytes,
        const std::vector<std::byte>& analysisBytes):id(generation),descriptor(frame),renderer(device),cutoffRaw(generation)
    {
        ComPtr<ID3D11DeviceContext> immediate;renderer->GetImmediateContext(&immediate);
        checked(immediate.As(&graphics),"native graphics owner context");
        context=interop::Context::create(renderer.Get());
        bridge=std::make_shared<interop::Bridge>(context,id,descriptor,vertexBytes,pixelBytes);
        if(deterministicQa) { copy=std::make_shared<CopyProcessor>();processor=copy; }
        else { model=std::make_shared<model::Processor>(context,config,instance);processor=model; }
        checked(renderer->CreateVertexShader(vertexBytes.data(),vertexBytes.size(),nullptr,&vertex),"native analysis vertex shader");
        checked(renderer->CreatePixelShader(analysisBytes.data(),analysisBytes.size(),nullptr,&analysis),"native analysis pixel shader");
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(renderer->CreateBuffer(&cb,nullptr,&constants),"native analysis constants");
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        checked(renderer->CreateRasterizerState(&rs,&raster),"native analysis raster");
        ComPtr<ID3D11Device1> stateDevice;checked(renderer.As(&stateDevice),"native analysis state device");
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL chosen{};
        checked(stateDevice->CreateDeviceContextState(0,levels,2,D3D11_SDK_VERSION,__uuidof(ID3D11Device),
            &chosen,&analysisState),"native analysis isolated graphics state");
        ComPtr<ID3D11Device5> fenceDevice;checked(renderer.As(&fenceDevice),"native snapshot fence device");
        checked(fenceDevice->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&graphicsFence)),"native snapshot ownership fence");
    }
    pipeline::Processed process(const pipeline::Work& work,std::stop_token stop) override
    {
        require(work.job.key.resourceGeneration==id && work.binding.transport==bridge,"native exact generation dispatch");
        return processor->process(work,stop);
    }
    void worker_stopping() noexcept override
    {
        processor->worker_stopping();
        closed.store(model?model->closed():copy->closed.load(std::memory_order_acquire),std::memory_order_release);
    }
    void worker_service() noexcept override { processor->worker_service(); }
    bool graphics_complete() const
    {
        return interop::completion(graphicsSerial,graphicsFence->GetCompletedValue())==interop::Completion::Complete;
    }
    void signal_graphics()
    {
        require(graphicsSerial<UINT64_MAX-1,"native snapshot fence exhausted");
        checked(graphics->Signal(graphicsFence.Get(),++graphicsSerial),"native snapshot ownership signal");
    }
    };
    struct Maintenance final:pipeline::Processor {
        std::array<std::atomic<std::shared_ptr<Generation>>,3> generations;
        pipeline::Processed process(const pipeline::Work&,std::stop_token) override
        {
            throw std::runtime_error("native dispatch missing physical generation");
        }
        void worker_service() noexcept override
        {
            for(auto& cell:generations)if(auto g=cell.load(std::memory_order_acquire)) {
                g->worker_service(); // Live unready owners need release before closeRequested.
                if(g->closeRequested.load(std::memory_order_acquire) && !g->closed.load(std::memory_order_acquire))
                    g->worker_stopping();
            }
        }
        void worker_stopping() noexcept override { worker_service(); }
        bool worker_stop_complete() const noexcept override {
            for(const auto& cell:generations)if(auto g=cell.load(std::memory_order_acquire))
                if(!g->closed.load(std::memory_order_acquire))return false;
            return true;
        }
    };
    const uint64_t instance;
    const model::Config config;
    const bool deterministicQa;
    const std::vector<std::byte> vertexBytes,pixelBytes,analysisBytes;
    std::shared_ptr<Maintenance> maintenance=std::make_shared<Maintenance>();
    std::array<std::shared_ptr<Generation>,3> generations;
    std::shared_ptr<Generation> active;
    std::unique_ptr<pipeline::FramePipeline> pipeline;
    std::unique_ptr<lifecycle::Observer> lifecycle;
    std::list<Record> records;
    std::shared_ptr<Capture> previousRaw;
    std::optional<FrameKey> previousKey;
    ID3D11Texture2D* nrTarget=nullptr; // output texture whose content is an NR frame (deadline-miss hold)
    uint64_t nrTargetTick=0;unsigned nrHeld=0;uint64_t heldFrames=0;
    uint64_t diagAdmit=0,diagRefused=0,diagNoAdmit=0;unsigned diagLogged=0;uint64_t diagReason[32]{}; // N22 wedge diagnostics
    std::optional<IngressSample> lastInput;
    std::shared_ptr<model::Job> selectedJob;
    std::optional<TickWitness> tickWitness;
    std::shared_ptr<const Capture> selectedRaw;
    std::shared_future<void> exited;
    std::shared_ptr<model::QaSignal> exitSignal;
    std::array<std::shared_ptr<model::QaSignal>,3> dispatchSignals;
    std::shared_ptr<model::ObservationSink> observer;
    model::Observation* observation=nullptr;
    std::optional<JobWitness> firstCompleted,firstOrdinary;
    DWORD graphicsOwner=0;
    lifecycle::CloseSnapshot closeProof{};
    bool stopping=false,stopSent=false,closed=false;
    Impl(uint64_t id,model::Config c,std::vector<std::byte> v,std::vector<std::byte> p,std::vector<std::byte> a,bool qa)
        :instance(id),config(std::move(c)),deterministicQa(qa),vertexBytes(std::move(v)),pixelBytes(std::move(p)),analysisBytes(std::move(a))
    {}
    std::shared_ptr<Capture> capture(ID3D11Texture2D* borrowed,const IngressSample& input)
    {
        auto& g=*active;
        auto* renderer=g.renderer.Get();auto* graphics=g.graphics.Get();
        const auto& context=g.context;
        const auto& analysisState=g.analysisState;
        const auto& constants=g.constants;const auto& raster=g.raster;
        const auto& vertex=g.vertex;const auto& analysis=g.analysis;
        const auto& frame=input.descriptor;
        auto owned=std::make_shared<Capture>(input);
        auto allocate=[&](DXGI_FORMAT format,ComPtr<ID3D11Texture2D>& texture,ComPtr<ID3D12Resource>& opened) {
            D3D11_TEXTURE2D_DESC d{};d.Width=frame.allocation.width;d.Height=frame.allocation.height;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.Usage=D3D11_USAGE_DEFAULT;
            d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            checked(renderer->CreateTexture2D(&d,nullptr,&texture),"native immutable allocation");
            ComPtr<IDXGIResource1> shared;checked(texture.As(&shared),"native immutable NT share");
            HANDLE handle=nullptr;
            checked(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
                nullptr,&handle),"native immutable NT handle");
            const auto hr=interop::open_shared_resource(context->device(),handle,&opened);
            CloseHandle(handle);checked(hr,"native immutable private open");
        };
        allocate(interop::source_format(frame.format),owned->original11,owned->original12);
        allocate(DXGI_FORMAT_B8G8R8A8_UNORM,owned->analysis11,owned->analysis12);
        graphics->CopyResource(owned->original11.Get(),borrowed);
        ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11RenderTargetView> rtv;
        checked(renderer->CreateShaderResourceView(owned->original11.Get(),nullptr,&srv),"native original analysis view");
        checked(renderer->CreateRenderTargetView(owned->analysis11.Get(),nullptr,&rtv),"native BGRA analysis target");
        struct Restore {
            ID3D11DeviceContext4* graphics;
            ComPtr<ID3DDeviceContextState> saved;
            ~Restore() { if(saved) graphics->SwapDeviceContextState(saved.Get(),nullptr); }
        } restore{graphics,{}};
        graphics->SwapDeviceContextState(analysisState.Get(),&restore.saved);
        const uint32_t params[]{static_cast<uint32_t>(frame.color),static_cast<uint32_t>(frame.alpha),0,0};
        graphics->UpdateSubresource(constants.Get(),0,nullptr,params,0,0);
        ID3D11RenderTargetView* target=rtv.Get();graphics->OMSetRenderTargets(1,&target,nullptr);
        graphics->OMSetBlendState(nullptr,nullptr,UINT_MAX);graphics->OMSetDepthStencilState(nullptr,0);
        graphics->RSSetState(raster.Get());
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(frame.allocation.width),static_cast<float>(frame.allocation.height),0,1};
        graphics->RSSetViewports(1,&viewport);
        graphics->IASetInputLayout(nullptr);graphics->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        graphics->VSSetShader(vertex.Get(),nullptr,0);graphics->PSSetShader(analysis.Get(),nullptr,0);
        ID3D11ShaderResourceView* image=srv.Get();graphics->PSSetShaderResources(0,1,&image);
        ID3D11Buffer* cb=constants.Get();graphics->PSSetConstantBuffers(0,1,&cb);
        graphics->Draw(3,0);
        return owned;
    }
    JobWitness witness(const Record& r) const
    {
        JobWitness value{r.ticket.key,r.bridge->ownership(r.ticket.slot)};
        const auto state=pipeline->state(r.ticket.key);
        value.ready=r.job->ready.load(std::memory_order_acquire);
        value.dispatchReturned=state && state->dispatchReturned;
        if(state)value.publication=static_cast<uint32_t>(state->publication);
        if(value.ready && value.dispatchReturned) {
            value.flow=r.job->flow.has_value();value.reset=value.flow && r.job->flow->reset;
            value.evaluated=r.job->evaluated;value.composed=r.job->composed;value.submitted=r.job->submitted;
            value.code=r.job->code;value.chronology=r.job->chronology;
            const auto bytes=std::min(r.job->diagnostic.size(),value.failure.size()-1);
            memcpy(value.failure.data(),r.job->diagnostic.data(),bytes);
            value.failureBytes=static_cast<uint32_t>(bytes);
            value.failureTruncated=bytes!=r.job->diagnostic.size();
        }
        return value;
    }
    void observe()
    {
        for(auto& r:records) {
            if(!r.job->ready.load(std::memory_order_acquire)) continue;
            const auto state=pipeline->state(r.ticket.key);
            if(!state || !state->dispatchReturned)continue;
            if(config.qaPreparation) {
                const auto value=witness(r);
                if(value.dispatchReturned) {
                    if(!firstCompleted)firstCompleted.emplace(value);
                    if(!firstOrdinary && value.flow && !value.reset && value.evaluated && value.composed && value.submitted)
                        firstOrdinary.emplace(value);
                }
            }
            const auto physical=r.bridge->ownership(r.ticket.slot);
            if(!physical.key || *physical.key!=r.ticket.key) continue;
            if(r.job->composed && r.job->submitted && !r.job->eligible) {
                require(pipeline->cancel(r.ticket.key) && r.bridge->cancel(r.ticket),"native invalid completion cancellation");
            }
            require(model::observe_publication(*pipeline,r.ticket,r.bridge->ownership(r.ticket.slot),
                observer.get(),stopping),"native exact ownership observation");
        }
    }
    void end_pins()
    {
        // Each final ending reserves its own completion target in submission order.
        // An independently held older lease can end later without reusing a target.
        for(auto& r:records) if(r.pin) {
            require(static_cast<bool>(r.bridge->end_consumer(*r.pin)),"native capture pin end");
            r.pin.reset();
        }
    }
    void clear_views()
    {
        if(active) {
            require(active->cutoffRaw.retire({previousRaw,selectedRaw},active->graphicsSerial),
                "native bounded snapshot cutoff cannot replace unresolved owners");
        }
        previousRaw.reset();previousKey.reset();selectedRaw.reset();selectedJob.reset();
        observation=nullptr;
    }
    void collect()
    {
        observe();pipeline->collect();
        records.remove_if([&](const Record& r) { return !r.pin && !pipeline->state(r.ticket.key); });
        for(size_t i=0;i<generations.size();++i)if(auto g=generations[i]) {
            auto& proof=closeProof.generations[i];
            if(proof.id!=g->id)proof={};
            proof.id=g->id;proof.requested=g->closeRequested.load(std::memory_order_acquire);
            proof.hasModel=static_cast<bool>(g->model);
            proof.closed=g->closed.load(std::memory_order_acquire);
            proof.graphics=g->graphicsSerial;proof.graphicsDone=g->graphicsFence->GetCompletedValue();
            proof.cutoffLeases=g->cutoffRaw.leases();proof.quiescent=g->bridge->quiescent();
            if(g->model)proof.model=g->model->close_diagnostics();
            (void)g->cutoffRaw.collect(g->id,g->graphicsFence->GetCompletedValue());
            proof.cutoffLeases=g->cutoffRaw.leases();
            if(g==active && !stopping)continue;
            const bool jobs=std::any_of(records.begin(),records.end(),[&](const Record& r) {
                return r.ticket.key.resourceGeneration==g->id;
            });
            for(auto& slot:proof.slots)slot.occupied=false;
            for(const auto& record:records)if(record.ticket.key.resourceGeneration==g->id) {
                const auto state=pipeline->state(record.ticket.key);
                const auto physical=record.bridge->ownership(record.ticket.slot);
                proof.slots[record.ticket.slot]={record.ticket.key.submissionSerial,
                    physical.producer,physical.producerDone,physical.worker,physical.workerDone,
                    physical.consumer,physical.consumerDone,true,record.job->ready.load(std::memory_order_acquire),
                    state && state->dispatchReturned,record.pin.has_value()};
            }
            struct Actions {
                Impl& owner;
                std::shared_ptr<Generation>& generation;
                void release_inputs() {
                    require(generation->cutoffRaw.capture_allowed(),"native snapshot owners require exact completion before session close");
                    generation->analysisState.Reset();generation->targetLease.Reset();
                }
                void request_model() { generation->closeRequested.store(true,std::memory_order_release); }
                bool model_closed() const { return generation->closed.load(std::memory_order_acquire); }
                lifecycle::ModelClose model_receipt() const {
                    return generation->model?generation->model->close_diagnostics():lifecycle::ModelClose{};
                }
                void retry_model() { owner.pipeline->service(); }
                bool close_bridge() { return generation->bridge->close(); }
                bool complete_generation() { return owner.pipeline->complete_generation(generation->id); }
            } actions{*this,g};
            const auto block=lifecycle::close_generation_steps(proof,jobs,actions);
            if(block!=lifecycle::CloseBlock::None) {
                closeProof.refusingGeneration=g->id;closeProof.block=block;continue;
            }
            require(lifecycle::close_generation(proof)==lifecycle::CloseBlock::None,"native close proof agrees with actual receipts");
            maintenance->generations[i].store({},std::memory_order_release);
            if(active==g)active.reset();
            generations[i].reset();
        }
        pipeline->collect();
    }
};
NativeOutput::NativeOutput(uint64_t id,model::Config c,std::vector<std::byte> v,std::vector<std::byte> p,std::vector<std::byte> a,bool qa)
    :impl_(std::make_unique<Impl>(id,std::move(c),std::move(v),std::move(p),std::move(a),qa)) {}
NativeOutput::~NativeOutput()
{
    if(!impl_->closed) (void)impl_.release(); // Unresolved physical owners are never freed.
}
lifecycle::Decision NativeOutput::prepare(const lifecycle::Observation& observation,ID3D11Device* renderer,bool cut)
{
    auto& p=*impl_;require(!p.stopping,"native lifecycle detached");
    // OBS creates filters on its UI/control thread. Bind the accepting pipeline
    // only at the first graphics-owned observation, before any capture or job.
    if(!p.pipeline) {
        p.graphicsOwner=GetCurrentThreadId();
        p.pipeline=std::make_unique<pipeline::FramePipeline>(Epochs{p.instance,1,1,1},p.maintenance);
        p.lifecycle=std::make_unique<nr::lifecycle::Observer>(*p.pipeline);
    }
    require(p.graphicsOwner==GetCurrentThreadId(),"native prepare requires its graphics owner");
    if(p.tickWitness && p.tickWitness->tick!=observation.canonicalTick)p.tickWitness.reset();
    p.collect();
    const auto decision=p.lifecycle->prepare(observation,cut,
        observation.settings.runtimePath==p.config.runtime.canonicalPath.generic_string()?Reason::None:Reason::InvalidSettings);
    if(decision.resetTemporal && decision.cause &&
        *decision.cause!=ResetCause::DeadlineMiss && *decision.cause!=ResetCause::EvaluationSkipped &&
        *decision.cause!=ResetCause::EvaluationFailure) {
        p.clear_views();p.end_pins();p.collect();
    }
    if(decision.placeholder)return decision;
    if(!p.active || p.active->id!=decision.epochs.resourceGeneration) {
        size_t index=0;for(;index<p.generations.size() && p.generations[index];++index) {}
        require(index<p.generations.size(),"native bounded physical generation budget");
        auto replacement=std::make_shared<Impl::Generation>(decision.epochs.resourceGeneration,renderer,
            observation.descriptor,p.config,p.instance,p.deterministicQa,p.vertexBytes,p.pixelBytes,p.analysisBytes);
        require(p.pipeline->hold_generation(replacement->id,replacement),"native exact feature/upload generation hold");
        p.generations[index]=replacement;
        p.maintenance->generations[index].store(replacement,std::memory_order_release);
        p.active=std::move(replacement);
        p.collect();
    }
    if(!p.active->cutoffRaw.capture_allowed())
        return {decision.epochs,decision.cause,decision.resetTemporal,
            Selection{observation.canonicalTick,OutputKind::CurrentSource,Status::Bypassed,Reason::SlotsFull,{},{},false}};
    // The accepted direct runtime owns one module's import/initialization lease.
    // Replacement allocation may exist, but never dispatch a second model until
    // the preceding generation has its real session/module close receipt.
    if(!p.deterministicQa && std::any_of(p.generations.begin(),p.generations.end(),[&](const auto& g) {
        return g && g!=p.active;
    }))return {decision.epochs,decision.cause,decision.resetTemporal,
        Selection{observation.canonicalTick,OutputKind::CurrentSource,Status::Bypassed,Reason::RetirementBudget,{},{},false}};
    return decision;
}
Selection NativeOutput::tick(const IngressSample& input,ID3D11Texture2D* borrowed,ID3D11Texture2D* target)
{
    auto& p=*impl_;require(!p.stopping,"native output is stopping");
    require(p.active && p.active->id==input.resourceGeneration && p.active->descriptor==input.descriptor,
        "native capture requires prepared physical generation");
    auto& g=*p.active;
    require(g.cutoffRaw.capture_allowed(),"native capture forbidden while snapshot cutoff is pending");
    const auto& bridge=g.bridge;
    p.lastInput.emplace(input);
    p.observe();
    const auto plan=p.pipeline->begin(input,input.canonicalTick+1);
    require(plan.action==TickAction::CaptureOnce,"native immutable capture once per canonical tick");
    p.tickWitness.emplace(TickWitness{input.canonicalTick,plan.admissionReason,{},p.pipeline->counters()});
    if(plan.admission)++p.diagAdmit;else{++p.diagNoAdmit;const auto rr=static_cast<size_t>(plan.admissionReason);if(rr<32)++p.diagReason[rr];}
    if(plan.selection.kind==OutputKind::CurrentSource && plan.admissionReason!=Reason::SlotsFull) {
        p.clear_views();p.end_pins();p.nrTarget=nullptr;
        require(p.pipeline->finish({}),"native capture-free scheduler placeholder");
        p.collect();p.tickWitness->counters=p.pipeline->counters();
        return p.pipeline->view(input.canonicalTick)->selection;
    }
    const auto immutable=p.capture(borrowed,input);
    const auto selected=adapter_selection(plan);
    p.observation=p.observer?p.observer->reserve(input,plan):nullptr;
    if(p.observation) {
        require(p.observation->returned!=nullptr,"prepared module-owned dispatch signal");
        if(plan.admission)p.observation->returned->bind(plan.admission->returned);
        else p.observation->returned->bind({});
    }
    p.selectedJob.reset();
    const auto ticket=plan.admission?
        bridge->capture_at(plan.admission->slot,plan.admission->job.key,borrowed):std::nullopt;
    if(plan.admission&&!ticket){++p.diagRefused;if(p.diagLogged<6){++p.diagLogged;const auto o=bridge->ownership(plan.admission->slot);
        blog(LOG_INFO,"[obs-nr-filter] diag capture refused instance %llu tick %llu slot %zu serial %llu sel %d/%d | slot serial %llu phase %d prod %llu/%llu work %llu/%llu cons %llu/%llu open %d cancel %d fail %d",
            (unsigned long long)p.instance,(unsigned long long)input.canonicalTick,plan.admission->slot,(unsigned long long)plan.admission->job.key.submissionSerial,
            (int)selected.kind,(int)selected.reason,(unsigned long long)(o.key?o.key->submissionSerial:0),(int)o.phase,
            (unsigned long long)o.producer,(unsigned long long)o.producerDone,(unsigned long long)o.worker,(unsigned long long)o.workerDone,
            (unsigned long long)o.consumer,(unsigned long long)o.consumerDone,(int)o.consumerOpen,(int)o.cancelled,(int)o.failed);}}
    p.end_pins();
    const bool holdNr=!p.deterministicQa && hold_previous_nr(selected,p.nrTarget==target,p.nrTargetTick,p.nrHeld);
    // The readiness cutoff is latched by begin, not by completion during capture.
    if(selected.kind==OutputKind::DelayedNr) {
        const auto old=std::find_if(p.records.begin(),p.records.end(),[&](const Record& r) {
            return selected.job && r.ticket.key==*selected.job;
        });
        require(old!=p.records.end(),"native exact selected NR resource owner");
        const auto consumer=old->bridge->acquire_output(old->ticket);
        require(consumer && old->bridge->copy_to(*consumer,target),"native selected output consumer copy");
        require(static_cast<bool>(old->bridge->end_consumer(*consumer)),"native selected output consumer end");
        if(g.model) require(g.model->publish(old->job,selected,old->bridge->ownership(old->ticket.slot)),"native exact applied publication");
        else old->job->published.store(true,std::memory_order_release);
        p.selectedJob=old->job;
    } else if(selected.kind==OutputKind::DelayedRaw) {
        require(p.previousRaw && p.previousKey==selected.frame,"native independent delayed raw owner");
        if(!holdNr) g.graphics->CopyResource(target,p.previousRaw->original11.Get()); // else target keeps the previous NR frame
    } else g.graphics->CopyResource(target,immutable->original11.Get());
    p.nrHeld=holdNr?p.nrHeld+1:0;p.heldFrames+=holdNr?1:0;
    p.nrTarget=selected.kind==OutputKind::DelayedNr||holdNr?target:nullptr;p.nrTargetTick=input.canonicalTick;
    auto raw=std::make_shared<const pipeline::RawFrame>(pipeline::RawFrame{input,immutable});
    if(ticket) {
        const auto pin=bridge->acquire_raw(*ticket);require(pin.has_value(),"native new capture pin reservation");
        const auto physical=bridge->ownership(ticket->slot);
        auto frame=std::make_shared<model::Frame>(immutable->original12.Get(),immutable->analysis12.Get(),immutable,
            NV_OF_FENCE_POINT{bridge->producer_fence(),physical.producer});
        const auto fault=p.observer?p.observer->qa_fault(plan.admission->job.key):model::Fault::None;
        auto job=std::make_shared<model::Job>(frame,plan.admission->job,ColorExposure{1,1},fault);
        job->observation=p.observation;
        const pipeline::WorkBinding binding{*ticket,physical,bridge,job,pin,p.active};
        require(p.pipeline->finish(raw,&binding),"native exact model dispatch");
        p.tickWitness->admitted.emplace(ticket->key);
        p.records.push_back({*ticket,pin,std::move(job),bridge});
    } else require(p.pipeline->finish(raw),"native independent raw admission skip");
    const auto* published=p.pipeline->view(input.canonicalTick);
    p.selectedRaw=selected.kind==OutputKind::CurrentSource?immutable:
        (published->raw?std::static_pointer_cast<const Capture>(published->raw->image):immutable);
    p.previousRaw=immutable;p.previousKey.emplace(input.key);
    if(p.observer&&p.observation)p.observer->capture(p.observation,immutable->original11.Get(),
        p.selectedRaw->original11.Get(),target);
    if(p.observation&&!ticket) {
        p.observation->finished.store(true,std::memory_order_release);
        SetEvent(p.observation->observed);
    }
    g.targetLease=target;g.signal_graphics();
    p.collect();
    p.tickWitness->counters=p.pipeline->counters();
    require(p.records.size()<=9 && p.pipeline->retired_generations()<=2,"native bounded three slots and two retired generations");
    return selected;
}
std::optional<TickWitness> NativeOutput::tick_witness() const
{
    auto witness=impl_->tickWitness;
    if(witness)witness->counters=counters();
    return witness;
}
pipeline::Counters NativeOutput::counters() const
{
    return impl_->pipeline?impl_->pipeline->counters():pipeline::Counters{};
}
uint32_t NativeOutput::latch_qa_failure() {
    auto& p=*impl_;
    if(!p.pipeline)return 0;
    const auto job=current_job();
    const auto code=job?job->qaFailure.load(std::memory_order_acquire):0;
    if(code)require(p.pipeline->reset(ResetCause::EvaluationFailure),"QA failure joint history reset");
    return code; // Filter latch prevents a second call after failure.
}
bool NativeOutput::on_graphics_owner() const {
    const auto& p=*impl_;
    return p.pipeline && GetCurrentThreadId()==p.graphicsOwner;
}
bool NativeOutput::release_original_marker(const JobKey& key) {
    auto& p=*impl_;
    if(!p.pipeline || GetCurrentThreadId()!=p.graphicsOwner)return false;
    const auto record=std::find_if(p.records.begin(),p.records.end(),[&](const auto& r){return r.ticket.key==key;});
    const auto owner=std::find_if(p.generations.begin(),p.generations.end(),[&](const auto& g){return g && g->id==key.resourceGeneration;});
    const auto state=p.pipeline->state(key);
    if(record==p.records.end() || owner==p.generations.end() || !(*owner)->model ||
        !state || !state->dispatchReturned || !(*owner)->model->request_original_marker(record->job,key))return false;
    p.pipeline->service(); // Exact owner consumed by existing model worker, not graphics.
    return true;
}
void NativeOutput::request_stop() {
    impl_->lifecycle.reset();impl_->observation=nullptr;
    impl_->stopping=true;
}
bool NativeOutput::try_close()
{
    auto& p=*impl_;if(p.closed) return true;
    auto& proof=p.closeProof;proof.instance=p.instance;++proof.sequence;
    proof.owner=p.graphicsOwner;proof.caller=GetCurrentThreadId();proof.stop=p.stopping;
    proof.workerExited=p.exited.valid() && p.exited.wait_for(std::chrono::seconds(0))==std::future_status::ready;
    proof.block=lifecycle::CloseBlock::None;
    proof.refusingGeneration=0;proof.refusingSerial=0;proof.refusingSlot=UINT32_MAX;
    if(!p.stopping){proof.block=lifecycle::CloseBlock::NotStopping;return false;}
    if(!p.pipeline) { p.closed=true;proof.closed=true;proof.pipeline=lifecycle::CloseResult::Complete;return true; }
    if(const auto block=lifecycle::close_entry_steps(proof,false,0,0,[&] {p.observe();});
        block!=lifecycle::CloseBlock::None){proof.block=block;return false;}
    if(!p.stopSent) {
        for(const auto& r:p.records) {
            const auto state=p.pipeline->state(r.ticket.key);
            const auto owner=std::find_if(p.generations.begin(),p.generations.end(),[&](const auto& g) {
                return g && g->id==r.ticket.key.resourceGeneration;
            });
            require(owner!=p.generations.end(),"native diagnostic exact generation owner");
            auto& g=proof.generations[static_cast<size_t>(owner-p.generations.begin())];
            g.id=r.ticket.key.resourceGeneration;
            auto& s=g.slots[r.ticket.slot];const auto physical=r.bridge->ownership(r.ticket.slot);
            s={r.ticket.key.submissionSerial,physical.producer,physical.producerDone,physical.worker,physical.workerDone,
                physical.consumer,physical.consumerDone,true,r.job->ready.load(std::memory_order_acquire),
                state && state->dispatchReturned,r.pin.has_value()};
            if(!s.ready || !s.returned) {
                proof.refusingGeneration=g.id;proof.refusingSerial=s.serial;proof.refusingSlot=static_cast<uint32_t>(r.ticket.slot);
                proof.block=lifecycle::close_slot(s);return false;
            }
        }
        p.end_pins();
        p.observe();
        for(const auto& r:p.records) {
            const auto owner=std::find_if(p.generations.begin(),p.generations.end(),[&](const auto& g) {
                return g && g->id==r.ticket.key.resourceGeneration;
            });
            require(owner!=p.generations.end(),"native diagnostic exact pin owner");
            auto& s=proof.generations[static_cast<size_t>(owner-p.generations.begin())].slots[r.ticket.slot];
            const auto physical=r.bridge->ownership(r.ticket.slot);
            s.pin=r.pin.has_value();s.consumer=physical.consumer;s.consumerDone=physical.consumerDone;
            s.producerDone=physical.producerDone;s.workerDone=physical.workerDone;
            if(const auto block=lifecycle::close_slot(s);block!=lifecycle::CloseBlock::None) {
                proof.refusingGeneration=r.ticket.key.resourceGeneration;proof.refusingSerial=s.serial;proof.refusingSlot=static_cast<uint32_t>(r.ticket.slot);
                proof.block=block;return false;
            }
            if(!interop::reusable(physical)){proof.block=lifecycle::CloseBlock::Consumer;return false;}
        }
        // Match the accepted caller: release all physically completed input
        // allocations before worker_stopping destroys the model/OFA sessions.
        if(p.lastInput) {
            const auto& old=*p.lastInput;const auto epochs=p.pipeline->epochs();
            const auto tick=obs_get_video_frame_time()/obs_get_frame_interval_ns()+1;
            proof.tick=tick;proof.lastTick=old.canonicalTick;
            if(const auto block=lifecycle::close_entry_steps(proof,true,tick,old.canonicalTick,[] {});
                block!=lifecycle::CloseBlock::None){proof.block=block;return false;}
            for(const auto& r:p.records)if(p.pipeline->state(r.ticket.key))
                require(p.pipeline->cancel(r.ticket.key),"native shutdown cancellation");
            const IngressSample disabled{{p.instance,epochs.contentEpoch,old.key.ingressSequence+1},tick,tick-1,
                old.sourceIdentity,old.deviceIdentity,epochs.resourceGeneration,old.validity,old.descriptor,
                {old.settings.revision,false,old.settings.runtimePath,old.settings.strength,old.settings.tuning}};
            (void)p.pipeline->begin(disabled,tick+1);
            require(p.pipeline->finish({}),"native disabled shutdown cutoff");
            p.pipeline->collect();
        }
        // The sink stays owned: a completion callback publishes ready before its last
        // Observation write and is drained only by the worker's settle before exit.
        p.clear_views();p.records.clear();p.lastInput.reset();
        p.collect();
        for(const auto& g:p.generations)if(g && !g->closeRequested.load(std::memory_order_acquire))return false;
        p.exited=p.pipeline->worker_exit(); // Subscribe before worker stop.
        p.pipeline->request_stop();p.stopSent=true;
    }
    p.collect();
    for(const auto& g:p.generations)if(g)return false;
    struct Actions {
        Impl& owner;
        bool try_close(){return owner.pipeline->try_close();}
        void release() {
            owner.records.clear();owner.previousRaw.reset();owner.previousKey.reset();
            owner.selectedJob.reset();owner.selectedRaw.reset();owner.pipeline.reset();
            owner.maintenance.reset();owner.observer.reset(); // After worker exit: no callback remains.
            owner.closed=true;
        }
    } actions{p};
    return lifecycle::close_pipeline_steps(proof,actions);
}
bool NativeOutput::closed() const { return impl_->closed; }
std::shared_future<void> NativeOutput::worker_exit() const { return impl_->pipeline?impl_->pipeline->worker_exit():impl_->exited; }
std::shared_ptr<model::QaSignal> NativeOutput::worker_exit_signal() {
    if(!impl_->pipeline&&!impl_->exited.valid())return {};
    if(!impl_->exitSignal) {
        impl_->exitSignal=model::make_qa_signal();
        impl_->exitSignal->bind(worker_exit());
    }
    return impl_->exitSignal;
}
std::array<std::shared_ptr<model::QaSignal>,3> NativeOutput::dispatch_signals() {
    for(auto& signal:impl_->dispatchSignals)if(!signal)signal=model::make_qa_signal();
    return impl_->dispatchSignals;
}
std::shared_ptr<model::Job> NativeOutput::current_job() const
{
    return impl_->records.empty()?nullptr:impl_->records.back().job;
}
std::shared_ptr<model::Job> NativeOutput::selected_job() const { return impl_->selectedJob; }
uint64_t NativeOutput::held_frames() const { return impl_->heldFrames; }
std::string NativeOutput::diag() const {
    const auto& p=*impl_;const auto c=p.pipeline?p.pipeline->counters():pipeline::Counters{};
    std::string s=" diag adm="+std::to_string(p.diagAdmit)+" refused="+std::to_string(p.diagRefused)+" noadm="+std::to_string(p.diagNoAdmit)+" disp="+std::to_string(c.dispatches)+" sel="+std::to_string(c.selections);
    for(size_t i=0;i<32;++i)if(p.diagReason[i])s+=" r"+std::to_string(i)+"="+std::to_string(p.diagReason[i]);
    if(p.active && p.active->model)s+=p.active->model->diag();
    return s;
}
std::shared_ptr<const Capture> NativeOutput::selected_raw() const { return impl_->selectedRaw; }
std::shared_ptr<const Capture> NativeOutput::current_raw() const { return impl_->previousRaw; }
std::shared_ptr<interop::Context> NativeOutput::context() const { return impl_->active?impl_->active->context:nullptr; }
std::shared_ptr<interop::Bridge> NativeOutput::transport() const { return impl_->active?impl_->active->bridge:nullptr; }
Epochs NativeOutput::epochs() const
{
    return impl_->pipeline?impl_->pipeline->epochs():Epochs{impl_->instance,1,1,1};
}
size_t NativeOutput::retired_generations() const { return impl_->pipeline?impl_->pipeline->retired_generations():0; }
void NativeOutput::observe_with(std::shared_ptr<model::ObservationSink> observer)
{
    if(!observer)impl_->observation=nullptr;
    impl_->observer=std::move(observer);
}
model::Observation* NativeOutput::observation() const { return impl_->observation; }
Diagnostics NativeOutput::diagnostics() const {
    Diagnostics result;
    if(!impl_->config.qaPreparation)return result;
    if(!impl_->records.empty())result.latest.emplace(impl_->witness(impl_->records.back()));
    if(impl_->firstCompleted)result.firstCompleted.emplace(*impl_->firstCompleted);
    if(impl_->firstOrdinary)result.firstOrdinary.emplace(*impl_->firstOrdinary);
    if(impl_->active && impl_->active->model)result.preparation=impl_->active->model->guide_preparation();
    return result;
}
lifecycle::CloseSnapshot NativeOutput::close_diagnostics(bool detached) const
{
    auto proof=impl_->closeProof;proof.instance=impl_->instance;proof.closed=impl_->closed;
    proof.sourceDetached=detached;
    return proof;
}
}
