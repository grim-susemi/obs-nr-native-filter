// SPDX-License-Identifier: GPL-3.0-only
// UG03 P05: process() subscribes the job's completion wait (a threadpool
// registration holding its own Job lease and plugin callback code) before the
// pending owner is published by signal(). A failure between the two must not
// leave that registration outside settlement and the model close receipt.
// White-box: the production translation unit is compiled here unchanged so its
// actual Processor::Impl::subscribe/signal and worker close run on a CPU WARP
// device. No vendor module, NGX session or GPU is involved.
#include "../../src/nr_model.cpp"
#include <cstdio>
namespace {
struct ProcessorImplTag {};
// Explicit instantiation may name a private member; this only reaches the
// existing owner struct, it adds no product seam.
template<auto Member> struct ExposeProcessorImpl {
    friend auto expose(ProcessorImplTag) { return Member; }
};
template struct ExposeProcessorImpl<&nr::model::Processor::impl_>;
auto expose(ProcessorImplTag);
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void hr(HRESULT value,const char* why){expect(SUCCEEDED(value),why);}
Microsoft::WRL::ComPtr<ID3D12Resource> committed(ID3D12Device* device) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=256;desc.Height=1;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,
        IID_PPV_ARGS(&result)),"CPU WARP immutable frame allocation");
    return result;
}
}
int main() try {
    using namespace nr;
    Microsoft::WRL::ComPtr<ID3D11Device> renderer;Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&renderer,nullptr,&immediate),
        "CPU WARP renderer");
    const auto context=interop::Context::create(renderer.Get());
    model::Config config;config.codec=config.color=config.guides={std::byte{1}};
    model::Processor processor(context,config,7);
    auto& owner=*(processor.*expose(ProcessorImplTag{}));
    auto* device=context->device();
    const auto original=committed(device),analysis=committed(device);
    Microsoft::WRL::ComPtr<ID3D12Fence> producer;
    hr(device->CreateFence(1,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&producer)),"completed producer fence");
    const auto frame=std::make_shared<const model::Frame>(original.Get(),analysis.Get(),std::make_shared<int>(1),
        NV_OF_FENCE_POINT{producer.Get(),1});
    const Settings settings{{1},true,"C:/qa/not-loaded-runtime.dll",{1},{0,0,1,1,0,-1,true}};
    const IngressSample input{{7,1,1},1000,999,31,91,1,SourceValidity::Valid,
        {{64,64},{0,0,64,64},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Straight,80},settings};
    const JobContract contract{{{7,1,1},1,1,1},input,1001};
    auto job=std::make_shared<model::Job>(frame,contract,ColorExposure{1,1});
    job->resources=std::make_shared<model::Job::Resources>(nullptr);
    // process(): actual completion subscription, then a failure before signal()
    // publishes the pending owner (the catch path keeps the resources, no ready).
    owner.subscribe(job);
    expect(job->resources->wait!=nullptr,"actual completion wait registered");
    processor.worker_stopping(); // Existing worker-only close, on this worker.
    const bool closedWithRegistration=processor.closed();
    const auto refused=processor.close_diagnostics();
    // The late original completion arrives; the retained owner still settles.
    owner.signal(job);
    expect(WaitForSingleObject(job->observed,10000)==WAIT_OBJECT_0 && job->ready.load(),
        "registered completion callback delivered");
    if(closedWithRegistration) {
        job->resources->unsubscribe(); // Test cleanup only; production never reaches this.
        throw std::runtime_error("model close completed while a registered completion wait was outside pending");
    }
    expect(refused.settlement==lifecycle::CloseResult::Refused && refused.callbackRetained &&
        refused.pendingSerial==contract.key.submissionSerial && refused.completionRequired==1 && !refused.completionDone,
        "close receipt names the registered completion owner");
    processor.worker_stopping();
    const auto collected=processor.close_diagnostics();
    expect(processor.closed() && job->resources->wait==nullptr && !collected.callbackRetained &&
        collected.settlement==lifecycle::CloseResult::Complete,"settled registration unsubscribed before close");
    std::printf("PASS model_subscription_pending checks=%u registered_wait_in_settlement=true CPU_WARP=true vendor=false\n",checks);
    return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
