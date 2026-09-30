// SPDX-License-Identifier: GPL-3.0-only
#include "nr_core.h"
#include <stdexcept>
#include <utility>
namespace nr {
Core::Core(uint64_t id,std::unique_ptr<Codec> codec,std::unique_ptr<ModelEvaluator> evaluator)
    : instanceId_(id),codec_(std::move(codec)),evaluator_(std::move(evaluator)) {
    if(!id || !codec_ || !evaluator_) throw std::invalid_argument("core requires instance-owned adapters");
}
CoreResult Core::record(const CoreInput& in) {
    const auto& job=in.job;
    const auto& r=in.resources;
    const auto& d=job.input.descriptor;
    const auto& t=job.input.settings.tuning;
    const auto compact=[&](const CoreTexture& x) {
        return x.valid() && x.allocation==Extent{d.active.width,d.active.height} &&
            x.active==Rect{0,0,d.active.width,d.active.height};
    };
    // Worker supplies compact full-size linear709/SDR images after Task9 conversion.
    // Guide subrects remain independent, exactly as R2 SetModelRegions.
    if(input_problem(job.input,in.epochs)!=Reason::None || !job.key.valid() ||
        job.key.frame!=job.input.key || job.key.frame.instanceId!=instanceId_ ||
        job.key.historyEpoch!=in.epochs.historyEpoch || job.key.resourceGeneration!=in.epochs.resourceGeneration ||
        !job.deadlineTick || job.deadlineTick<=job.input.canonicalTick ||
        !in.commands || !r.codecHeap || !compact(r.raw) || !compact(r.proxy) || !compact(r.keep) ||
        !compact(r.model) || !compact(r.output) || !r.depth.valid() || !r.motion.valid() ||
        !in.history.epoch || in.history.epoch!=job.key.historyEpoch || (!in.history.valid && !in.history.reset) ||
        !std::isfinite(in.whitePoint) || in.whitePoint<=0 ||
        !std::isfinite(in.mvScaleX) || !std::isfinite(in.mvScaleY) ||
        (in.codecColor!=ColorSpace::Rec709Srgb && in.codecColor!=ColorSpace::ScRgbLinear) ||
        ((d.color==ColorSpace::Rec709Srgb)!=(in.codecColor==ColorSpace::Rec709Srgb)) ||
        (in.codecAlpha!=AlphaMode::Straight && in.codecAlpha!=AlphaMode::Opaque) ||
        r.proxy.native==r.raw.native || r.keep.native==r.raw.native || r.output.native==r.raw.native ||
        r.model.native==r.proxy.native || r.model.native==r.raw.native || r.model.native==r.keep.native ||
        r.output.native==r.proxy.native || r.output.native==r.keep.native ||
        r.output.native==r.model.native || r.keep.native==r.proxy.native)
        return {CoreState::InvalidInput,false,false,0};
    if(!job.input.settings.enabled) return {CoreState::Disabled,false,false,0};
    CodecConstants c;
    c.width=d.active.width; c.height=d.active.height; c.whitePoint=in.whitePoint;
    c.passthrough=in.codecColor==ColorSpace::Rec709Srgb ? 1u : 0u;
    c.strength=job.input.settings.strength.value;
    // R1 EncodeInput -> R2 Context::Run -> R1 resolve; one history, one pass, full size.
    if(!codec_->encode(in,c)) return {CoreState::EncodeError,false,false,0};
    const ModelInput model{in,r.proxy,r.depth,r.motion,r.model,t};
    const auto result=evaluator_->evaluate(model);
    if(result.state==EvaluationState::Error) return {CoreState::EvaluationError,false,false,result.code};
    if(result.state!=EvaluationState::Evaluated) return {CoreState::NotReady,false,false,result.code};
    c.mode=1;
    if(!codec_->resolve(in,c)) return {CoreState::ResolveError,true,false,0};
    return {CoreState::Recorded,true,true,result.code};
}
namespace {
void checked(HRESULT result) { if(FAILED(result)) throw std::runtime_error("D3D12 codec initialization failed"); }
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
}
D3D12Codec::D3D12Codec(ID3D12Device* device,std::span<const std::byte> shader) : device_(device) {
    if(!device || shader.empty()) throw std::invalid_argument("device and compiled shader required");
    stride_=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,0};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,3};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants={0,0,sizeof(CodecConstants)/4};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.MaxAnisotropy=1;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC desc{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
    checked(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    checked(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root_.Get();
    pso.CS={shader.data(),shader.size()};
    checked(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline_)));
}
bool D3D12Codec::dispatch(const CoreInput& in,const CodecConstants& constants,bool resolvePass) {
    auto* cmd=static_cast<ID3D12GraphicsCommandList*>(in.commands);
    auto* heap=in.resources.codecHeap;
    const auto hd=heap->GetDesc();
    if(hd.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || hd.NumDescriptors<10 ||
        !(hd.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return false;
    const auto& r=in.resources;
    ID3D12Resource* textures[5]={static_cast<ID3D12Resource*>(resolvePass?r.proxy.native:r.raw.native),
        static_cast<ID3D12Resource*>(r.model.native),static_cast<ID3D12Resource*>(r.keep.native),
        static_cast<ID3D12Resource*>(resolvePass?r.output.native:r.proxy.native),static_cast<ID3D12Resource*>(r.keep.native)};
    for(auto* texture:textures) {
        const auto d=texture->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=constants.width || d.Height!=constants.height ||
            d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if(FAILED(texture->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get()) return false;
    }
    const UINT offset=resolvePass?5u:0u;
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=static_cast<SIZE_T>(offset)*stride_;
    for(UINT i=0;i<5;++i) {
        const auto d=textures[i]->GetDesc();
        if(i<3) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=d.Format;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
            device_->CreateShaderResourceView(textures[i],&view,cpu);
        } else {
            if(!(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=d.Format; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(textures[i],nullptr,&view,cpu);
        }
        cpu.ptr+=stride_;
    }
    if(resolvePass) transition(cmd,textures[1],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=static_cast<UINT64>(offset)*stride_;
    cmd->SetDescriptorHeaps(1,&heap); cmd->SetComputeRootSignature(root_.Get()); cmd->SetPipelineState(pipeline_.Get());
    cmd->SetComputeRoot32BitConstants(0,sizeof(CodecConstants)/4,&constants,0);
    cmd->SetComputeRootDescriptorTable(1,gpu); cmd->Dispatch((constants.width+7)/8,(constants.height+7)/8,1);
    if(!resolvePass) {
        transition(cmd,textures[3],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cmd,textures[4],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    return true;
}
bool D3D12Codec::encode(const CoreInput& in,const CodecConstants& c) { return dispatch(in,c,false); }
bool D3D12Codec::resolve(const CoreInput& in,const CodecConstants& c) { return dispatch(in,c,true); }
}
