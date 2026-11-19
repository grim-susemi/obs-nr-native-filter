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
    const Extent native{d.active.width,d.active.height};
    const auto work=work_extent(d.active,job.input.settings.workScale);
    const bool reduced=job.input.settings.workScale<100;
    const auto compact=[](const CoreTexture& x,Extent size) {
        return x.valid() && x.allocation==size && x.active==Rect{0,0,size.width,size.height};
    };
    // Canonical source/keep/output stay native; only model-side images use the work extent.
    // Guide subrects remain independent, exactly as R2 SetModelRegions.
    if(input_problem(job.input,in.epochs)!=Reason::None || !job.key.valid() ||
        job.key.frame!=job.input.key || job.key.frame.instanceId!=instanceId_ ||
        job.key.historyEpoch!=in.epochs.historyEpoch || job.key.resourceGeneration!=in.epochs.resourceGeneration ||
        !job.deadlineTick || job.deadlineTick<=job.input.canonicalTick ||
        !in.commands || !r.codecHeap || !compact(r.raw,native) || !compact(r.proxy,native) || !compact(r.keep,native) ||
        !compact(r.model,work) || !compact(r.output,native) || !r.depth.valid() || !r.motion.valid() ||
        !in.passes || in.passes>2 || in.passes>job.input.settings.passes ||
        (reduced && !compact(in.workProxy,work)) ||
        (in.passes>1 && (!compact(in.modelB,work) || !compact(in.modelC,work))) ||
        !in.history.epoch || in.history.epoch!=job.key.historyEpoch || (!in.history.valid && !in.history.reset) ||
        !std::isfinite(in.whitePoint) || in.whitePoint<=0 ||
        !std::isfinite(in.mvScaleX) || !std::isfinite(in.mvScaleY) ||
        (in.codecColor!=ColorSpace::Rec709Srgb && in.codecColor!=ColorSpace::ScRgbLinear) ||
        ((d.color==ColorSpace::Rec709Srgb)!=(in.codecColor==ColorSpace::Rec709Srgb)) ||
        (in.codecAlpha!=AlphaMode::Straight && in.codecAlpha!=AlphaMode::Opaque))
        return {CoreState::InvalidInput,false,false,0};
    std::array<const CoreTexture*,8> textures{&r.raw,&r.proxy,&r.keep,&r.model,&r.output};
    size_t count=5;
    if(reduced) textures[count++]=&in.workProxy;
    if(in.passes>1) { textures[count++]=&in.modelB; textures[count++]=&in.modelC; }
    for(size_t i=0;i<count;++i) for(size_t j=0;j<i;++j)
        if(textures[i]->native==textures[j]->native) return {CoreState::InvalidInput,false,false,0};
    if(!job.input.settings.enabled) return {CoreState::Disabled,false,false,0};
    CodecConstants c;
    c.width=d.active.width; c.height=d.active.height; c.whitePoint=in.whitePoint;
    c.passthrough=in.codecColor==ColorSpace::Rec709Srgb ? 1u : 0u;
    c.strength=job.input.settings.strength.value;
    if(!codec_->encode(in,c)) return {CoreState::EncodeError,false,false,0};
    const auto& source=reduced?in.workProxy:r.proxy;
    if(reduced && !codec_->downsample(in,r.proxy,source)) return {CoreState::EncodeError,false,false,0};
    const auto& p2=job.input.settings.pass2Tuning?*job.input.settings.pass2Tuning:t;
    const ModelTuning later{p2.preset,p2.style,p2.intensity,p2.localStructure,
        job.input.settings.pass2Tuning?p2.localTone:0.0f,p2.skinStructure,p2.autoMask};
    uint32_t completed=0,code=0,passError=0,failedPass=0;
    for(uint32_t pass=0;pass<in.passes;++pass) {
        const auto& answer=pass%2?in.modelB:r.model;
        const ModelInput model{in,pass?in.modelC:source,r.depth,r.motion,answer,pass?later:t,pass};
        const auto result=evaluator_->evaluate(model);
        code=result.code;
        switch(result.state) {
        case EvaluationState::Error:
            return {CoreState::EvaluationError,completed!=0,false,code,completed,0,pass+1};
        case EvaluationState::NotReady:
            return {CoreState::NotReady,completed!=0,false,code,completed,0,pass+1};
        case EvaluationState::Evaluated: break;
        }
        ++completed;
        if(completed<in.passes && !codec_->clamp(in,answer,in.modelC,pass)) {
            // No clamp commands recorded: A/B retains the last answer, so one final resolve is safe.
            passError=core_clamp_refused;failedPass=pass+2;break;
        }
    }
    c.mode=1;
    const auto& answer=completed%2?r.model:in.modelB;
    if(!codec_->resolve(in,c,source,answer)) return {CoreState::ResolveError,true,false,0,completed,passError,failedPass};
    return {CoreState::Recorded,true,true,code,completed,passError,failedPass};
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
bool D3D12Codec::dispatch(const CoreInput& in,const CodecConstants& constants,
    const CoreTexture& source,const CoreTexture& model,const CoreTexture& original,
    const CoreTexture& target,const CoreTexture& keep,uint32_t offset) {
    auto* cmd=static_cast<ID3D12GraphicsCommandList*>(in.commands);
    auto* heap=in.resources.codecHeap;
    if(!cmd || !heap || !constants.width || !constants.height) return false;
    const auto hd=heap->GetDesc();
    if(hd.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || hd.NumDescriptors<25 ||
        !(hd.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return false;
    const CoreTexture* inputs[5]={&source,&model,&original,&target,&keep};
    ID3D12Resource* textures[5]{};
    for(unsigned i=0;i<5;++i) {
        const auto& input=*inputs[i];
        if(!input.valid()) return false;
        auto* texture=static_cast<ID3D12Resource*>(input.native);
        textures[i]=texture;
        const auto d=texture->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=input.allocation.width ||
            d.Height!=input.allocation.height || d.DepthOrArraySize!=1 || d.MipLevels!=1 ||
            d.SampleDesc.Count!=1 || (i>=3 && !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> owner;
        if(FAILED(texture->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get()!=device_.Get()) return false;
    }
    if(target.active!=Rect{0,0,constants.width,constants.height}) return false;
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr+=static_cast<SIZE_T>(offset)*stride_;
    for(UINT i=0;i<5;++i) {
        const auto d=textures[i]->GetDesc();
        if(i<3) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=d.Format;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels=1;
            device_->CreateShaderResourceView(textures[i],&view,cpu);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=d.Format; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(textures[i],nullptr,&view,cpu);
        }
        cpu.ptr+=stride_;
    }
    if(constants.mode==1) transition(cmd,textures[1],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(constants.mode==3) transition(cmd,textures[0],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto gpu=heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr+=static_cast<UINT64>(offset)*stride_;
    cmd->SetDescriptorHeaps(1,&heap); cmd->SetComputeRootSignature(root_.Get()); cmd->SetPipelineState(pipeline_.Get());
    cmd->SetComputeRoot32BitConstants(0,sizeof(CodecConstants)/4,&constants,0);
    cmd->SetComputeRootDescriptorTable(1,gpu); cmd->Dispatch((constants.width+7)/8,(constants.height+7)/8,1);
    if(constants.mode==0) {
        transition(cmd,textures[3],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cmd,textures[4],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if(constants.mode==2) transition(cmd,textures[3],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(constants.mode==3) transition(cmd,textures[0],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}
bool D3D12Codec::encode(const CoreInput& in,const CodecConstants& c) {
    const auto& r=in.resources;
    const Rect full{0,0,c.width,c.height};
    if(r.raw.active!=full || r.proxy.active!=full || r.keep.active!=full) return false;
    return dispatch(in,c,r.raw,r.model,r.keep,r.proxy,r.keep,0);
}
bool D3D12Codec::resolve(const CoreInput& in,const CodecConstants& c,const CoreTexture& source,const CoreTexture& answer) {
    const auto& r=in.resources;
    if(source.allocation!=answer.allocation || source.active!=answer.active ||
        source.active!=Rect{0,0,source.allocation.width,source.allocation.height} ||
        r.keep.active!=Rect{0,0,c.width,c.height}) return false;
    return dispatch(in,c,source,answer,r.keep,r.output,r.keep,5);
}
bool D3D12Codec::downsample(const CoreInput& in,const CoreTexture& from,const CoreTexture& to) {
    if(from.active!=Rect{0,0,from.allocation.width,from.allocation.height} ||
        to.active!=Rect{0,0,to.allocation.width,to.allocation.height} ||
        from.allocation.width<to.allocation.width || from.allocation.height<to.allocation.height ||
        from.native==to.native) return false;
    CodecConstants c;c.mode=2;c.width=to.active.width;c.height=to.active.height;
    return dispatch(in,c,from,in.resources.model,in.resources.keep,to,in.resources.keep,10);
}
bool D3D12Codec::clamp(const CoreInput& in,const CoreTexture& from,const CoreTexture& to,uint32_t slot) {
    if(slot>1 || from.allocation!=to.allocation || from.active!=to.active ||
        from.active!=Rect{0,0,from.allocation.width,from.allocation.height} || from.native==to.native) return false;
    CodecConstants c;c.mode=3;c.width=to.active.width;c.height=to.active.height;
    return dispatch(in,c,from,in.resources.model,in.resources.keep,to,in.resources.keep,15+5*slot);
}
}
