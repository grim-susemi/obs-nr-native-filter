// SPDX-License-Identifier: GPL-3.0-only
#include "obs_native_output.h"
#include <obs.h>
#include <algorithm>
#include <list>
#include <stdexcept>
namespace nr::obs_adapter {
namespace {
using Microsoft::WRL::ComPtr;
void require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
void checked(HRESULT value,const char* why) { require(SUCCEEDED(value),why); }
struct Record {
    interop::Ticket ticket;
    std::optional<interop::Consumer> pin;
    std::shared_ptr<model::Job> job;
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
        job->eligible=true;job->ready.store(true,std::memory_order_release);SetEvent(job->observed);
        return {work.job,true,job}; // Processor result, NOT Feature18 evaluated/composed.
    }
    void worker_stopping() noexcept override { closed.store(true,std::memory_order_release); }
};
}
struct NativeOutput::Impl {
    const uint64_t instance;
    const model::Config config;
    const bool deterministicQa;
    const std::vector<std::byte> vertexBytes,pixelBytes,analysisBytes;
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
    std::unique_ptr<pipeline::FramePipeline> pipeline;
    std::list<Record> records;
    std::shared_ptr<Capture> previousRaw;
    std::optional<FrameKey> previousKey;
    std::optional<IngressSample> lastInput;
    std::shared_ptr<model::Job> selectedJob;
    std::shared_ptr<const Capture> selectedRaw;
    std::shared_future<void> exited;
    bool stopping=false,stopSent=false,closed=false;
    Impl(uint64_t id,model::Config c,std::vector<std::byte> v,std::vector<std::byte> p,std::vector<std::byte> a,bool qa)
        :instance(id),config(std::move(c)),deterministicQa(qa),vertexBytes(std::move(v)),pixelBytes(std::move(p)),analysisBytes(std::move(a))
    {}
    void init(ID3D11Texture2D* borrowed,const FrameDescriptor& descriptor)
    {
        borrowed->GetDevice(&renderer);
        ComPtr<ID3D11DeviceContext> immediate;renderer->GetImmediateContext(&immediate);
        checked(immediate.As(&graphics),"native graphics owner context");
        context=interop::Context::create(renderer.Get());
        bridge=std::make_shared<interop::Bridge>(context,1,descriptor,vertexBytes,pixelBytes);
        if(deterministicQa) { copy=std::make_shared<CopyProcessor>();processor=copy; }
        else { model=std::make_shared<model::Processor>(context,config,instance);processor=model; }
        pipeline=std::make_unique<pipeline::FramePipeline>(Epochs{instance,1,1,1},processor);
        require(pipeline->hold_generation(1,processor),"native feature generation hold");
        checked(renderer->CreateVertexShader(vertexBytes.data(),vertexBytes.size(),nullptr,&vertex),"native analysis vertex shader");
        checked(renderer->CreatePixelShader(analysisBytes.data(),analysisBytes.size(),nullptr,&analysis),"native analysis pixel shader");
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(renderer->CreateBuffer(&cb,nullptr,&constants),"native analysis constants");
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        checked(renderer->CreateRasterizerState(&rs,&raster),"native analysis raster");
        ComPtr<ID3D11Device1> device;checked(renderer.As(&device),"native analysis state device");
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL chosen{};
        checked(device->CreateDeviceContextState(0,levels,2,D3D11_SDK_VERSION,__uuidof(ID3D11Device),
            &chosen,&analysisState),"native analysis isolated graphics state");
    }
    std::shared_ptr<Capture> capture(ID3D11Texture2D* borrowed,const FrameDescriptor& frame)
    {
        auto owned=std::make_shared<Capture>();
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
        } restore{graphics.Get(),{}};
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
    void observe()
    {
        for(auto& r:records) {
            if(!r.job->ready.load(std::memory_order_acquire)) continue;
            const auto physical=bridge->ownership(r.ticket.slot);
            if(!physical.key || *physical.key!=r.ticket.key) continue;
            if(r.job->composed && r.job->submitted && !r.job->eligible) {
                require(pipeline->cancel(r.ticket.key) && bridge->cancel(r.ticket),"native invalid completion cancellation");
            }
            require(pipeline->observe(r.ticket,bridge->ownership(r.ticket.slot)),"native exact ownership observation");
        }
    }
    void end_pins()
    {
        // Records remain capture-ordered; a newer reservation never overtakes an
        // older open consumer. The current pin is reserved only after this loop.
        for(auto& r:records) if(r.pin) {
            require(bridge->end_consumer(*r.pin),"native capture pin end");
            r.pin.reset();
        }
    }
};
NativeOutput::NativeOutput(uint64_t id,model::Config c,std::vector<std::byte> v,std::vector<std::byte> p,std::vector<std::byte> a,bool qa)
    :impl_(std::make_unique<Impl>(id,std::move(c),std::move(v),std::move(p),std::move(a),qa)) {}
NativeOutput::~NativeOutput()
{
    if(!impl_->closed) (void)impl_.release(); // Unresolved physical owners are never freed.
}
Selection NativeOutput::tick(const IngressSample& input,ID3D11Texture2D* borrowed,ID3D11Texture2D* target)
{
    auto& p=*impl_;require(!p.stopping,"native output is stopping");
    if(!p.pipeline) p.init(borrowed,input.descriptor);
    p.lastInput.emplace(input);
    p.observe();
    const auto plan=p.pipeline->begin(input,input.canonicalTick+1);
    require(plan.action==TickAction::CaptureOnce,"native immutable capture once per canonical tick");
    const auto immutable=p.capture(borrowed,input.descriptor);
    p.selectedJob.reset();
    const auto ticket=plan.admission?
        p.bridge->capture_at(plan.admission->slot,plan.admission->job.key,borrowed):std::nullopt;
    p.end_pins();
    // The readiness cutoff is latched by begin, not by completion during capture.
    if(plan.selection.kind==OutputKind::DelayedNr) {
        const auto old=std::find_if(p.records.begin(),p.records.end(),[&](const Record& r) {
            return plan.selection.job && r.ticket.key==*plan.selection.job;
        });
        require(old!=p.records.end(),"native exact selected NR resource owner");
        const auto consumer=p.bridge->acquire_output(old->ticket);
        require(consumer && p.bridge->copy_to(*consumer,target),"native selected output consumer copy");
        require(p.bridge->end_consumer(*consumer),"native selected output consumer end");
        if(p.model) require(p.model->publish(old->job,plan.selection,p.bridge->ownership(old->ticket.slot)),"native exact applied publication");
        else old->job->published.store(true,std::memory_order_release);
        p.selectedJob=old->job;
    } else if(plan.selection.kind==OutputKind::DelayedRaw) {
        require(p.previousRaw && p.previousKey==plan.selection.frame,"native independent delayed raw owner");
        p.graphics->CopyResource(target,p.previousRaw->original11.Get());
    } else p.graphics->CopyResource(target,immutable->original11.Get());
    auto raw=std::make_shared<const pipeline::RawFrame>(pipeline::RawFrame{input,immutable});
    if(ticket) {
        const auto pin=p.bridge->acquire_raw(*ticket);require(pin.has_value(),"native new capture pin reservation");
        const auto physical=p.bridge->ownership(ticket->slot);
        auto frame=std::make_shared<model::Frame>(immutable->original12.Get(),immutable->analysis12.Get(),immutable,
            NV_OF_FENCE_POINT{p.bridge->producer_fence(),physical.producer});
        auto job=std::make_shared<model::Job>(frame,plan.admission->job,ColorExposure{1,1});
        const pipeline::WorkBinding binding{*ticket,physical,p.bridge,job,pin};
        require(p.pipeline->finish(raw,&binding),"native exact model dispatch");
        p.records.push_back({*ticket,pin,std::move(job)});
    } else require(p.pipeline->finish(raw),"native independent raw admission skip");
    const auto* published=p.pipeline->view(input.canonicalTick);
    p.selectedRaw=published->raw?std::static_pointer_cast<const Capture>(published->raw->image):immutable;
    p.previousRaw=immutable;p.previousKey.emplace(input.key);
    p.records.remove_if([&](const Record& r) {
        return !r.pin && !p.pipeline->state(r.ticket.key);
    });
    require(p.records.size()<=3,"native bounded three physical owners");
    return p.pipeline->view(input.canonicalTick)->selection;
}
void NativeOutput::request_stop() { impl_->stopping=true; }
bool NativeOutput::try_close()
{
    auto& p=*impl_;if(p.closed) return true;
    if(!p.pipeline) { p.closed=true;return true; }
    if(!p.stopping) return false;
    p.observe();
    if(!p.stopSent) {
        for(const auto& r:p.records) {
            const auto state=p.pipeline->state(r.ticket.key);
            if(!r.job->ready.load(std::memory_order_acquire) || (state && !state->dispatchReturned)) return false;
        }
        p.end_pins();
        p.observe();
        for(const auto& r:p.records)if(!interop::reusable(p.bridge->ownership(r.ticket.slot)))return false;
        // Match the accepted caller: release all physically completed input
        // allocations before worker_stopping destroys the model/OFA sessions.
        if(p.lastInput) {
            const auto& old=*p.lastInput;const auto epochs=p.pipeline->epochs();
            const auto tick=obs_get_video_frame_time()/obs_get_frame_interval_ns()+1;
            if(tick<=old.canonicalTick)return false;
            for(const auto& r:p.records)if(p.pipeline->state(r.ticket.key))
                require(p.pipeline->cancel(r.ticket.key),"native shutdown cancellation");
            const IngressSample disabled{{p.instance,epochs.contentEpoch,old.key.ingressSequence+1},tick,tick-1,
                old.sourceIdentity,old.deviceIdentity,epochs.resourceGeneration,old.validity,old.descriptor,
                {old.settings.revision,false,old.settings.runtimePath,old.settings.strength,old.settings.tuning}};
            (void)p.pipeline->begin(disabled,tick+1);
            require(p.pipeline->finish({}),"native disabled shutdown cutoff");
            p.pipeline->collect();
        }
        p.analysisState.Reset(); // Drops retained D3D11 RTV/SRV input bindings too.
        p.records.clear();p.previousRaw.reset();p.previousKey.reset();p.selectedJob.reset();p.selectedRaw.reset();p.lastInput.reset();
        p.exited=p.pipeline->worker_exit(); // Subscribe before worker stop.
        p.pipeline->request_stop();p.stopSent=true;
    }
    if(p.model?!p.model->closed():!p.copy->closed.load(std::memory_order_acquire)) return false;
    p.observe();
    (void)p.pipeline->complete_generation(1);
    p.pipeline->collect();
    if(!p.pipeline->try_close()) return false;
    p.records.clear();p.previousRaw.reset();p.previousKey.reset();p.selectedJob.reset();p.selectedRaw.reset();p.pipeline.reset();
    if(!p.bridge->close()) return false;
    p.bridge.reset();p.processor.reset();p.model.reset();p.copy.reset();p.context.reset();
    p.analysisState.Reset();p.raster.Reset();p.constants.Reset();p.analysis.Reset();p.vertex.Reset();
    p.graphics.Reset();p.renderer.Reset();p.closed=true;return true;
}
bool NativeOutput::closed() const { return impl_->closed; }
std::shared_future<void> NativeOutput::worker_exit() const { return impl_->pipeline?impl_->pipeline->worker_exit():impl_->exited; }
std::shared_ptr<model::Job> NativeOutput::current_job() const
{
    return impl_->records.empty()?nullptr:impl_->records.back().job;
}
std::shared_ptr<model::Job> NativeOutput::selected_job() const { return impl_->selectedJob; }
std::shared_ptr<const Capture> NativeOutput::selected_raw() const { return impl_->selectedRaw; }
std::shared_ptr<interop::Context> NativeOutput::context() const { return impl_->context; }
std::shared_ptr<interop::Bridge> NativeOutput::transport() const { return impl_->bridge; }
Epochs NativeOutput::epochs() const
{
    return impl_->pipeline?impl_->pipeline->epochs():Epochs{impl_->instance,1,1,1};
}
}
