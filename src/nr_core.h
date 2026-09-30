// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d12.h>
#include <memory>
#include <span>
#include <wrl/client.h>
namespace nr {
// Source-lock R1/R2 ordering and defaults; resource lifetime remains with the job owner.
inline ModelTuning core_default_tuning() { return {0,0,1,1,1,-1,true}; }
struct CodecConstants {
    uint32_t mode=0; float whitePoint=1; uint32_t width=0,height=0;
    float transferStrength=1,colourStrength=1,maxRatio=2; uint32_t passthrough=0;
    float strength=1; float padding[3]{};
};
static_assert(sizeof(CodecConstants)==48);
struct CoreTexture {
    void* const native;
    const Extent allocation;
    const Rect active;
    bool valid() const {
        return native && allocation.width && allocation.height && active.width && active.height &&
            active.x<=allocation.width && active.y<=allocation.height &&
            active.width<=allocation.width-active.x && active.height<=allocation.height-active.y;
    }
};
struct CoreResources {
    const CoreTexture raw,proxy,keep,model,output,depth,motion;
    // Shader-visible CBV/SRV/UAV heap: ten private descriptors per in-flight job.
    ID3D12DescriptorHeap* const codecHeap;
};
struct CoreHistory { const uint64_t epoch; const bool valid,reset; };
struct CoreInput {
    const JobContract job;
    const Epochs epochs;
    const CoreResources resources;
    const CoreHistory history;
    void* const commands;
    const float whitePoint; // Task9 supplies source-appropriate linear709 divisor, default 1.
    const float mvScaleX,mvScaleY; // working-image pixels; no second resolution scaling.
    const ColorSpace codecColor; // Task9 canonical linear709/SDR; original job descriptor stays intact.
    const AlphaMode codecAlpha; // Task9 unpremultiplies before codec, restores afterward.
};
struct ModelInput {
    const CoreInput& input;
    const CoreTexture& color;
    const CoreTexture& depth;
    const CoreTexture& motion;
    const CoreTexture& output;
    const ModelTuning& tuning;
};
enum class EvaluationState { NotReady, Evaluated, Error };
struct EvaluationResult { EvaluationState state; uint32_t code; };
class ModelEvaluator {
public:
    virtual ~ModelEvaluator()=default;
    // Task4 adapter owns Feature18 and all per-instance history/creation completion.
    virtual EvaluationResult evaluate(const ModelInput&)=0;
};
class Codec {
public:
    virtual ~Codec()=default;
    virtual bool encode(const CoreInput&,const CodecConstants&)=0;
    virtual bool resolve(const CoreInput&,const CodecConstants&)=0;
};
enum class CoreState { InvalidInput, Disabled, NotReady, EncodeError, EvaluationError, ResolveError, Recorded };
struct CoreResult {
    CoreState state;
    bool evaluated,composed;
    uint32_t error;
    // Recorded is NOT GPU-completed, publication-ready, or an inference-quality claim.
};
class Core {
    const uint64_t instanceId_;
    std::unique_ptr<Codec> codec_;
    std::unique_ptr<ModelEvaluator> evaluator_;
public:
    Core(uint64_t,std::unique_ptr<Codec>,std::unique_ptr<ModelEvaluator>);
    Core(const Core&)=delete;
    Core& operator=(const Core&)=delete;
    CoreResult record(const CoreInput&);
};
// Production D3D12 encode/resolve, using actual compiled nr_codec.hlsl bytes.
// Raw arrives NON_PIXEL_SHADER_RESOURCE, proxy/keep/model/output UAV.
// Encode leaves proxy/keep readable. Resolve leaves model readable and output UAV.
// The worker owns lists, barriers to external COMMON and fence retirement, never OBS context.
class D3D12Codec final : public Codec {
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    UINT stride_;
    bool dispatch(const CoreInput&,const CodecConstants&,bool);
public:
    D3D12Codec(ID3D12Device*,std::span<const std::byte> shader);
    bool encode(const CoreInput&,const CodecConstants&) override;
    bool resolve(const CoreInput&,const CodecConstants&) override;
};
}
