// SPDX-License-Identifier: GPL-3.0-only
#include "nr_core_gpu_fixture.h"
namespace core_gpu {
struct FakeModel final : ModelEvaluator {
    Gpu& gpu;
    const float gain;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    explicit FakeModel(Gpu& g,std::span<const std::byte> shader,float edit=0):gpu(g),gain(edit),descriptors(g.heap(6)) {
        D3D12_DESCRIPTOR_RANGE range[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,3};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,range};
        D3D12_ROOT_SIGNATURE_DESC desc{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
        check(g.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.CS={shader.data(),shader.size()};
        check(g.device->CreateComputePipelineState(&p,IID_PPV_ARGS(&pipeline)));
    }
    EvaluationResult evaluate(const ModelInput& m) override {
        const auto& color=m.color;const auto& output=m.output;
        auto* input=static_cast<ID3D12Resource*>(color.native);
        if(m.pass) gpu.barrier(input,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const UINT stride=gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(m.pass)*2*stride;
        gpu.device->CreateShaderResourceView(input,nullptr,cpu);
        cpu.ptr+=stride;gpu.device->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(output.native),nullptr,nullptr,cpu);
        const struct {uint32_t width,height;float gain;} c{output.active.width,output.active.height,gain};
        auto* list=gpu.list.Get();ID3D12DescriptorHeap* h=descriptors.Get();list->SetDescriptorHeaps(1,&h);
        list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        list->SetComputeRoot32BitConstants(0,3,&c,0);
        auto handle=h->GetGPUDescriptorHandleForHeapStart();handle.ptr+=UINT64(m.pass)*2*stride;
        list->SetComputeRootDescriptorTable(1,handle);
        list->Dispatch((c.width+7)/8,(c.height+7)/8,1);
        if(m.pass) gpu.barrier(input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.trace.push_back(100+int(m.pass));return {EvaluationState::Evaluated,1};
    }
};
struct TracedCodec final : Codec {
    Gpu& gpu;D3D12Codec codec;
    TracedCodec(Gpu& g,std::span<const std::byte> shader):gpu(g),codec(g.device.Get(),shader){}
    bool encode(const CoreInput& in,const CodecConstants& c) override {
        const bool ok=codec.encode(in,c);if(ok) gpu.trace.push_back(0);return ok;
    }
    bool downsample(const CoreInput& in,const CoreTexture& a,const CoreTexture& b) override {
        const bool ok=codec.downsample(in,a,b);if(ok) gpu.trace.push_back(10);return ok;
    }
    bool clamp(const CoreInput& in,const CoreTexture& a,const CoreTexture& b,uint32_t slot) override {
        const bool ok=codec.clamp(in,a,b,slot);if(ok) gpu.trace.push_back(15+5*int(slot));return ok;
    }
    bool resolve(const CoreInput& in,const CodecConstants& c,const CoreTexture& a,const CoreTexture& b) override {
        const bool ok=codec.resolve(in,c,a,b);if(ok) gpu.trace.push_back(5);return ok;
    }
};
Image colored(Extent size) {
    Image result(size_t(size.width)*size.height);
    for(uint32_t y=0;y<size.height;++y) for(uint32_t x=0;x<size.width;++x)
        result[y*size.width+x]=(x+y)%2 ? Pixel{.125f,.75f,.25f,.375f}:Pixel{.875f,.25f,.5f,.625f};
    return result;
}
int probe(const char* codecPath,const char* modelPath,const char* outputPath) {
    const auto shader=bytes(codecPath),fake=bytes(modelPath);
    std::ofstream output(outputPath,std::ios::binary);require(output.good(),"probe evidence output");
    bool passed=true;
    for(uint32_t scale:{50u,55u,75u}) {
        // Given native chromatic detail that cannot survive a reduced reconstruction.
        const Extent native{40,20},work=work_extent({0,0,40,20},scale);
        const auto input=colored(native);
        Gpu gpu;auto descriptors=gpu.heap(25);
        std::array<ComPtr<ID3D12Resource>,7> textures;
        for(size_t i=0;i<textures.size();++i) textures[i]=gpu.texture(i>=5?work:native);
        gpu.upload(textures[0].Get(),input);
        const auto tex=[&](size_t i) {const auto s=i>=5?work:native;return CoreTexture{textures[i].Get(),s,{0,0,s.width,s.height}};};
        const auto j=job(native,{scale});
        const CoreResources r{tex(0),tex(1),tex(2),tex(6),tex(4),tex(0),tex(0),descriptors.Get()};
        const CoreInput in{j,{7,1,1,1},r,{1,true,false},gpu.list.Get(),1,1,1,ColorSpace::ScRgbLinear,AlphaMode::Straight};
        D3D12Codec codec(gpu.device.Get(),shader);FakeModel model(gpu,fake);
        CodecConstants c;c.width=40;c.height=20;
        // When the actual codec resolves a fake identity answer at the working size.
        require(codec.encode(in,c),"probe encode");require(codec.downsample(in,tex(1),tex(5)),"probe downsample");
        const auto color=tex(5),answer=tex(6);
        model.evaluate({in,color,tex(0),tex(0),answer,j.input.settings.tuning});c.mode=1;
        require(codec.resolve(in,c,tex(5),tex(6)),"probe resolve");
        const auto actual=gpu.finish(textures[4].Get());
        float worst=0;int ulps=0;
        for(size_t i=0;i<input.size();++i) for(size_t ch=0;ch<4;++ch) {
            worst=std::max(worst,std::abs(input[i][ch]-actual[i][ch]));
            ulps=std::max(ulps,std::abs(int(DirectX::PackedVector::XMConvertFloatToHalf(input[i][ch]))-
                int(DirectX::PackedVector::XMConvertFloatToHalf(actual[i][ch]))));
        }
        // Then identity preserves every native channel within one binary16 step.
        const bool ok=ulps<=1;
        std::printf("colored_identity scale=%u work=%ux%u max_abs=%.9g max_fp16_ulps=%d limit=1 result=%s\n",
            scale,work.width,work.height,worst,ulps,ok?"PASS":"FAIL");
        output.write(reinterpret_cast<const char*>(input.data()),static_cast<std::streamsize>(input.size()*sizeof(Pixel)));
        output.write(reinterpret_cast<const char*>(actual.data()),static_cast<std::streamsize>(actual.size()*sizeof(Pixel)));
        passed=passed&&ok;
    }
    require(output.good(),"probe binary write");return passed?0:1;
}
struct Artifacts {const std::vector<std::byte> codec,fake;std::ofstream& output;};
void scenario(const Artifacts& a,Case c) {
    // Given native high-frequency colour or a neutral frame with a known right-half edit.
    const Extent native{40,20},work=work_extent({0,0,40,20},c.scale);
    const auto input=c.gain==0 && c.scale<100?colored(native):Image(800,Pixel{.25f,.25f,.25f,.375f});
    Gpu gpu;auto heap=gpu.heap(25);
    std::array<ComPtr<ID3D12Resource>,8> textures;
    for(size_t i=0;i<textures.size();++i) textures[i]=gpu.texture(i>=3 && i!=4?work:native);
    gpu.upload(textures[0].Get(),input);
    const auto tex=[&](size_t i) {const auto s=i>=3 && i!=4?work:native;
        return CoreTexture{textures[i].Get(),s,{0,0,s.width,s.height}};};
    const auto j=job(native,c);
    const CoreResources r{tex(0),tex(1),tex(2),tex(3),tex(4),tex(0),tex(0),heap.Get()};
    const CoreInput in{j,{7,1,1,1},r,{1,true,false},gpu.list.Get(),1,1,1,
        c.hdr?ColorSpace::ScRgbLinear:ColorSpace::Rec709Srgb,AlphaMode::Straight,c.passes,tex(5),tex(6),tex(7)};
    Core core(7,std::make_unique<TracedCodec>(gpu,a.codec),std::make_unique<FakeModel>(gpu,a.fake,c.gain));
    // When the production core records every actual pass, followed by one production resolve/readback.
    const auto result=core.record(in);
    require(result.state==CoreState::Recorded && result.passes==c.passes && !result.passError,"GPU core record");
    const auto actual=gpu.finish(textures[4].Get());
    std::vector<int> expected{0};if(c.scale<100) expected.push_back(10);
    for(uint32_t p=0;p<c.passes;++p) {
        expected.push_back(100+int(p));if(p+1<c.passes) expected.push_back(15+5*int(p));
    }
    expected.push_back(5);require(gpu.trace==expected,"GPU descriptor/evaluation trace");
    int ulps=0;float worst=0;uint32_t first=40;bool aligned=true;
    for(uint32_t y=0;y<20;++y) for(uint32_t x=0;x<40;++x) {
        const size_t i=y*40+x;
        for(size_t ch=0;ch<4;++ch) {
            require(std::isfinite(actual[i][ch]),"nonfinite GPU output");
            if(c.gain!=0 && x>=19 && x<=20 && ch<3) continue; // The two bilinear boundary pixels.
            const float wanted=input[i][ch]+(c.gain!=0 && x>20 && ch<3?c.gain*float(c.passes)*c.strength:0);
            const int distance=std::abs(int(DirectX::PackedVector::XMConvertFloatToHalf(wanted))-
                int(DirectX::PackedVector::XMConvertFloatToHalf(actual[i][ch])));
            ulps=std::max(ulps,distance);worst=std::max(worst,std::abs(wanted-actual[i][ch]));
        }
        if(c.gain!=0 && actual[i][0]-input[i][0]>.002f) first=std::min(first,x);
        aligned=aligned && actual[i][3]==input[i][3];
    }
    // Then identity keeps native chromatic detail and a nonidentity edit has native alignment/amplitude.
    std::printf("core_gpu scale=%u passes=%u hdr=%d gain=%.5f strength=%.2f max_abs=%.9g max_fp16_ulps=%d first_effect=%u trace=",
        c.scale,c.passes,int(c.hdr),c.gain,c.strength,worst,ulps,first);
    for(int value:gpu.trace) std::printf("%d,",value);std::puts("");
    a.output.write(reinterpret_cast<const char*>(input.data()),static_cast<std::streamsize>(input.size()*sizeof(Pixel)));
    a.output.write(reinterpret_cast<const char*>(actual.data()),static_cast<std::streamsize>(actual.size()*sizeof(Pixel)));
    require(a.output.good(),"GPU scenario output write");
    require(ulps<=1 && aligned,"native detail or single-blend amplitude exceeds one binary16 ULP");
    if(c.gain!=0) require(first>=19 && first<=21,"right-half edit has wrong native coordinates");
}
int acceptance(const char* codecPath,const char* modelPath,const char* outputPath) {
    std::ofstream output(outputPath,std::ios::binary);require(output.good(),"GPU evidence output");
    const Artifacts a{bytes(codecPath),bytes(modelPath),output};
    scenario(a,{}); // Exact native trace: descriptor 0, evaluate(0), descriptor 5.
    for(uint32_t scale:{50u,55u,75u}) for(uint32_t passes:{1u,2u}) {
        scenario(a,{scale,passes}); // HDR proxy encoding, coloured native spatial detail.
        scenario(a,{scale,passes,0,1,false}); // SDR proxy encoding, same spatial detail.
        scenario(a,{scale,passes,.0625f,.5f,false}); // Known nonzero residual, one final strength blend.
    }
    std::puts("PASS 28 real D3D12 codec/core cases, fake evaluator only; binary16 ULP limit=1; no vendor features");
    return 0;
}
}
int main(int argc,char** argv) try {
    core_gpu::require(argc==4 || (argc==5 && std::string(argv[4])=="--probe"),
        "usage: codec.dxil fake_model.dxil output.bin [--probe]");
    return argc==5?core_gpu::probe(argv[1],argv[2],argv[3]):core_gpu::acceptance(argv[1],argv[2],argv[3]);
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
