// SPDX-License-Identifier: GPL-3.0-only
// allow: SIZE_OK - The frozen vector harness and its D3D12 command-list proof share fixtures.
#define NOMINMAX
#include "nr_core.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>
using namespace nr;
using Microsoft::WRL::ComPtr;
void expect(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Direct3D proof failed"); }
struct Probe { std::vector<int> order; bool encode=true,resolve=true; EvaluationState eval=EvaluationState::Evaluated; };
struct TestCodec final : Codec {
    Probe& p;
    explicit TestCodec(Probe& probe):p(probe){}
    bool encode(const CoreInput&,const CodecConstants& c) override {
        p.order.push_back(1); expect(c.mode==0 && c.width==16 && c.height==8,"full-size encode");
        expect(c.transferStrength==1 && c.colourStrength==1 && c.maxRatio==2,"source defaults"); return p.encode;
    }
    bool downsample(const CoreInput&,const CoreTexture&,const CoreTexture&) override { return false; }
    bool clamp(const CoreInput&,const CoreTexture&,const CoreTexture&,uint32_t) override { return false; }
    bool resolve(const CoreInput& in,const CodecConstants& c,const CoreTexture& source,const CoreTexture& answer) override {
        p.order.push_back(3);expect(c.mode==1 && source.native==in.resources.proxy.native &&
            answer.native==in.resources.model.native,"resolve bindings");return p.resolve;
    }
};
struct TestModel final : ModelEvaluator {
    Probe& p;
    explicit TestModel(Probe& probe):p(probe){}
    EvaluationResult evaluate(const ModelInput& m) override {
        p.order.push_back(2);
        expect(m.color.native==m.input.resources.proxy.native && m.output.native==m.input.resources.model.native,"model handles");
        expect(m.depth.active==Rect{2,1,16,8} && m.motion.active==Rect{1,2,16,8},"independent guide rectangles");
        expect(m.input.mvScaleX==1 && m.input.mvScaleY==-1,"motion scaled twice");
        const auto* depth=static_cast<const float*>(m.depth.native);
        const auto* flow=static_cast<const float*>(m.motion.native);
        expect(depth[1*20+2]==1.0f && flow[(2*20+1)*2]==1.0f,"neutral depth/nonzero current-to-previous guides changed");
        expect(m.tuning.skinStructure==-1 && m.tuning.autoMask,"model defaults");return {p.eval,123};
    }
};
int handles[9];
std::array<float,240> neutralDepth=[] {std::array<float,240> a{};a.fill(1);return a;}();
std::array<float,480> guideFlow=[] {std::array<float,480> a{};for(size_t i=0;i<a.size();++i)a[i]=i%2?-1.0f:1.0f;return a;}();
CoreInput fixture(int fault=0,uint64_t id=7) {
    auto texture=[&](int n) {return CoreTexture{&handles[n],{16,8},{0,0,16,8}};};
    const FrameDescriptor d{{16,8},{0,0,16,8},PixelFormat::Rgba16Float,ColorSpace::ScRgbLinear,AlphaMode::Straight,80};
    const float strength=fault==3?2.0f:fault==4?std::numeric_limits<float>::quiet_NaN():1.0f;
    const ModelTuning tuning{0,0,1,fault==14?std::numeric_limits<float>::quiet_NaN():1.0f,1,-1,true};
    const Settings s{{fault==5?0u:1u},fault!=6,"runtime-owned-by-task4",{strength},tuning};
    const FrameKey fk{id,1,10};
    const IngressSample sample{fk,10,9,1,1,1,SourceValidity::Valid,d,s};
    const JobContract job{{fk,fault==7?2u:1u,1,1},sample,11};
    const CoreTexture guideDepth{fault==8?nullptr:neutralDepth.data(),{20,12},{2,1,16,8}};
    const CoreTexture guideMotion{guideFlow.data(),{20,12},{1,2,fault==9?21u:16u,8}};
    const CoreResources r{texture(0),texture(1),texture(2),texture(3),
        fault==10?texture(0):texture(4),guideDepth,guideMotion,
        fault==11?nullptr:reinterpret_cast<ID3D12DescriptorHeap*>(&handles[7])};
    return {job,{id,1,1,1},r,{fault==1?0u:1u,fault!=2,false},
        fault==12?nullptr:&handles[8],fault==13?0.0f:1.0f,1,-1,
        fault==15?ColorSpace::Rec709Srgb:ColorSpace::ScRgbLinear,
        fault==16?AlphaMode::Premultiplied:AlphaMode::Straight};
}
void contracts() {
    const auto defaults=core_default_tuning();
    expect(defaults.preset==0 && defaults.style==0 && defaults.intensity==1 && defaults.localStructure==1 &&
        defaults.localTone==1 && defaults.skinStructure==-1 && defaults.autoMask,"source tuning defaults changed");
    expect(CodecConstants{}.whitePoint==1 && CodecConstants{}.maxRatio==2,"composition defaults changed");
    Probe p; Core core(7,std::make_unique<TestCodec>(p),std::make_unique<TestModel>(p));
    const auto good=core.record(fixture());
    expect(good.state==CoreState::Recorded && good.evaluated && good.composed && p.order==std::vector<int>{1,2,3},"encode/evaluate/resolve order");
    for(int fault=1;fault<=16;++fault) {
        p.order.clear(); const auto result=core.record(fixture(fault));
        expect(!result.evaluated && !result.composed && p.order.empty(),"invalid/disabled input reached evaluate");
    }
    p.eval=EvaluationState::NotReady;p.order.clear();
    expect(!core.record(fixture()).evaluated && p.order==std::vector<int>{1,2},"not-ready resolved");
    p.eval=EvaluationState::Error;p.order.clear();
    expect(core.record(fixture()).state==CoreState::EvaluationError && p.order==std::vector<int>{1,2},"failed evaluate resolved");
    p.eval=EvaluationState::Evaluated;p.encode=false;p.order.clear();
    expect(!core.record(fixture()).evaluated && p.order==std::vector<int>{1},"failed encode evaluated");
    p.encode=true;p.resolve=false;p.order.clear();const auto failed=core.record(fixture());
    expect(failed.evaluated && !failed.composed && failed.state==CoreState::ResolveError,"resolve failure publication");
    p.resolve=true;p.order.clear();expect(!core.record(fixture(0,8)).evaluated && p.order.empty(),"foreign instance accepted");
    Probe other;Core second(8,std::make_unique<TestCodec>(other),std::make_unique<TestModel>(other));
    expect(second.record(fixture(0,8)).composed && p.order.empty(),"instance shared state");
    bool rejected=false;try {Core invalid(0,nullptr,nullptr);}catch(const std::invalid_argument&){rejected=true;}
    expect(rejected,"null adapters accepted");
    std::puts("PASS nr_core contracts: defaults, immutable guide regions, order, readiness, errors, history, settings, identity, instances");
}
struct Pixel { float r,g,b,a; };
using Image=std::vector<Pixel>;
struct ShaderRunner {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;ComPtr<ID3D11SamplerState> sampler;
    explicit ShaderRunner(std::span<const std::byte> bytes,D3D_DRIVER_TYPE driver=D3D_DRIVER_TYPE_WARP) {
        check(D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        check(device->CreateComputeShader(bytes.data(),bytes.size(),nullptr,&shader));
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&s,&sampler));
    }
    std::pair<Image,Image> run(const void* constants,UINT size,const Image& src,const Image& model,const Image& raw,
        UINT width=16,UINT height=8) {
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> inputs[3];ComPtr<ID3D11ShaderResourceView> views[3];
        const Image* images[]={&src,&model,&raw};
        for(UINT i=0;i<3;++i) {
            D3D11_SUBRESOURCE_DATA data{images[i]->data(),width*sizeof(Pixel),0};
            check(device->CreateTexture2D(&d,&data,&inputs[i]));check(device->CreateShaderResourceView(inputs[i].Get(),nullptr,&views[i]));
        }
        d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> outputs[2],staging[2];ComPtr<ID3D11UnorderedAccessView> uavs[2];
        for(UINT i=0;i<2;++i) {check(device->CreateTexture2D(&d,nullptr,&outputs[i]));check(device->CreateUnorderedAccessView(outputs[i].Get(),nullptr,&uavs[i]));}
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        for(auto& stage:staging) check(device->CreateTexture2D(&d,nullptr,&stage));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=size;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{constants,0,0};ComPtr<ID3D11Buffer> buffer;check(device->CreateBuffer(&bd,&init,&buffer));
        ID3D11ShaderResourceView* srvs[]={views[0].Get(),views[1].Get(),views[2].Get()};
        ID3D11UnorderedAccessView* outs[]={uavs[0].Get(),uavs[1].Get()};
        context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,buffer.GetAddressOf());
        context->CSSetSamplers(0,1,sampler.GetAddressOf());context->CSSetShaderResources(0,3,srvs);
        context->CSSetUnorderedAccessViews(0,2,outs,nullptr);context->Dispatch((width+7)/8,(height+7)/8,1);context->ClearState();
        Image result[2]={Image(width*height),Image(width*height)};
        for(UINT i=0;i<2;++i) {
            context->CopyResource(staging[i].Get(),outputs[i].Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging[i].Get(),0,D3D11_MAP_READ,0,&mapped));
            for(UINT y=0;y<height;++y) memcpy(result[i].data()+y*width,static_cast<char*>(mapped.pData)+y*mapped.RowPitch,width*sizeof(Pixel));
            context->Unmap(staging[i].Get(),0);
        }
        return {result[0],result[1]};
    }
};
std::vector<std::byte> binary(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);expect(file.good(),"shader binary missing");
    const auto size=file.tellg();expect(size>0,"empty shader binary");std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);expect(file.good(),"shader binary read failed");return bytes;
}
void codec_list_proof(std::span<const std::byte> shader) {
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12Codec codec(device.Get(),shader);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    auto heap=[&](UINT count) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors=count;d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> h;check(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&h)));return h;
    };
    auto descriptors=heap(25),smallHeap=heap(24),clearHeap=heap(9);
    const UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_DEFAULT;
    std::array<ComPtr<ID3D12Resource>,9> textures;
    for(UINT i=0;i<textures.size();++i) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=i<4?6:3;d.Height=i<4?4:2;d.DepthOrArraySize=d.MipLevels=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&textures[i])));
        auto cpu=clearHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(i)*stride;
        device->CreateUnorderedAccessView(textures[i].Get(),nullptr,nullptr,cpu);
        auto gpu=clearHeap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(i)*stride;
        ID3D12DescriptorHeap* h=clearHeap.Get();list->SetDescriptorHeaps(1,&h);
        const float values[4]={i==0?.2f:i==4?.8f:i==5?.1f:0.0f,
            i==0?.3f:i==4?-.1f:i==5?.4f:0.0f,
            i==0?.4f:i==4?1.7f:i==5?.7f:0.0f,.375f};
        list->ClearUnorderedAccessViewFloat(gpu,cpu,textures[i].Get(),values,0,nullptr);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=textures[i].Get();
        list->ResourceBarrier(1,&b);
    }
    auto transition=[&](UINT i,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={textures[i].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&b);
    };
    transition(0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto tex=[&](UINT i) {
        const uint32_t w=i<4?6u:3u,h=i<4?4u:2u;
        return CoreTexture{textures[i].Get(),{w,h},{0,0,w,h}};
    };
    const auto base=fixture();
    auto makeInput=[&](ID3D12DescriptorHeap* h) {
        CoreResources r{tex(0),tex(1),tex(2),tex(4),tex(3),tex(0),tex(0),h};
        return CoreInput{base.job,base.epochs,r,base.history,list.Get(),1,1,1,
            ColorSpace::Rec709Srgb,AlphaMode::Straight};
    };
    const auto in=makeInput(descriptors.Get()),tooSmall=makeInput(smallHeap.Get());
    CodecConstants c;c.width=6;c.height=4;c.passthrough=1;
    expect(!codec.encode(tooSmall,c),"24 descriptor heap accepted");
    const CoreTexture wrong{textures[4].Get(),{2,2},{0,0,2,2}};
    c.mode=1;
    expect(!codec.resolve(in,c,tex(8),wrong),"mismatched resolve accepted");
    c.mode=0;
    expect(codec.encode(in,c),"encode recording");
    expect(codec.downsample(in,tex(1),tex(8)),"downsample recording");
    expect(codec.clamp(in,tex(4),tex(6),0),"first clamp recording");
    expect(codec.clamp(in,tex(5),tex(7),1),"second clamp recording");
    c.mode=1;
    expect(codec.resolve(in,c,tex(8),tex(4)),"resolve recording");
    const UINT indices[]={8,6,7,3};
    D3D12_HEAP_PROPERTIES readProps{};readProps.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=4096;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&readProps,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    for(UINT i=0;i<4;++i) {
        const UINT index=indices[i];
        transition(index,index==8?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=textures[index].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset=UINT64(i)*1024;
        dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,index==3?6u:3u,index==3?4u:2u,1,256};
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    check(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr,"fence event");
    check(queue->Signal(fence.Get(),1));check(fence->SetEventOnCompletion(1,event));
    const DWORD wait=WaitForSingleObject(event,30000);CloseHandle(event);expect(wait==WAIT_OBJECT_0,"GPU proof timeout");
    void* memory=nullptr;D3D12_RANGE range{0,4096};
    check(readback->Map(0,&range,&memory));
    const auto* mapped=static_cast<const std::byte*>(memory);
    const auto* down=reinterpret_cast<const Pixel*>(mapped);
    const auto* first=reinterpret_cast<const Pixel*>(mapped+1024);
    const auto* second=reinterpret_cast<const Pixel*>(mapped+2048);
    const auto* resolved=reinterpret_cast<const Pixel*>(mapped+3072);
    expect(std::abs(down->r-.2f)<1e-3f && std::abs(down->g-.3f)<1e-3f,"downsample descriptor overwritten");
    expect(std::abs(first->r-.8f)<1e-3f && first->g==0 && first->b==1,"first clamp descriptor overwritten");
    expect(std::abs(second->r-.1f)<1e-3f && std::abs(second->g-.4f)<1e-3f &&
        std::abs(second->b-.7f)<1e-3f,"second clamp descriptor overwritten");
    expect(std::isfinite(resolved->r) && std::abs(resolved->r-.2f)>1e-3f,"resolve descriptor overwritten");
    D3D12_RANGE written{0,0};readback->Unmap(0,&written);
    std::puts("PASS D3D12 codec one-list descriptor ranges and pre-record refusals");
}
float error(const Image& a,const Image& b) {
    float worst=0;expect(a.size()==b.size(),"image extent");
    for(size_t i=0;i<a.size();++i) {
        const float x[]={a[i].r,a[i].g,a[i].b,a[i].a},y[]={b[i].r,b[i].g,b[i].b,b[i].a};
        for(int c=0;c<4;++c) {expect(std::isfinite(x[c]),"nonfinite shader output");worst=std::max(worst,std::abs(x[c]-y[c]));}
    }
    return worst;
}
float srgb(float x) {x=std::clamp(x,0.0f,1.0f);return x<=.0031308f?12.92f*x:1.055f*std::pow(x,1.0f/2.4f)-.055f;}
float linear(float x) {x=std::clamp(x,0.0f,1.0f);return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
std::array<uint32_t,64> reference_constants(const CodecConstants& c) {
    std::array<uint32_t,64> r{};r[0]=c.mode;r[2]=c.width;r[3]=c.height;r[8]=c.passthrough;r[20]=1;
    memcpy(&r[1],&c.whitePoint,4);memcpy(&r[4],&c.transferStrength,4);memcpy(&r[5],&c.colourStrength,4);memcpy(&r[7],&c.maxRatio,4);
    return r;
}
std::string selected_reference(const char* path) {
    std::ifstream file(path,std::ios::binary);expect(file.good(),"frozen source missing");
    std::string s((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    s.erase(std::remove(s.begin(),s.end(),'\r'),s.end());
    const auto slice=[&](const char* begin,const char* end) {
        const auto a=s.find(begin),b=s.find(end,a);
        expect(a!=std::string::npos && b!=std::string::npos,"frozen reference span missing");return s.substr(a,b-a);
    };
    // Exact source math spans. Only default/native routing is selected, not reimplemented.
    std::string out=s.substr(0,s.find("// Hue-preserving gamut"));
    out+=slice("// Hue-preserving gamut","// Bindings are stated");
    out+=slice("// Bindings are stated","float2 ExposureAnchor");
    out+="float WhitePoint() { return max(gWhitePoint,1e-4); }\n";
    out+=slice("static const float3 kLuma","// The edit at an arbitrary");
    out+=slice("// Soft-knee proxy","// Unclipped, hue-preserving Neutwo");
    out+=R"([numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID) {
    if(id.x>=gWidth || id.y>=gHeight) return;
    float2 cmpUv=(float2(id.xy)+0.5)/float2(gWidth,gHeight);
)";
    out+=slice("    if (gMode == 0)","        // What the model is shown.");
    out+="        float3 display=SoftKnee(frame / WhitePoint());\n";
    out+="        gTarget[id.xy]=float4(LinearToSrgb(display),source.a); return;\n    }\n";
    out+=slice("    float4 proxySample =","    // All three pictures have to share a scale");
    // Native default has no comparisons, reduced raster, residual transfer or replacement.
    const auto pos=out.find("    float4 originalSample = gCompareMode");
    expect(pos!=std::string::npos,"original source load missing");
    out.erase(pos);out+="    float4 originalSample=gOriginal.Load(int3(id.xy,0));\n";
    out+=slice("    const float normScale =","    // Apply the model.");
    out+=slice("    // Rescale the model answer to the original luminance","    // Replace modes decode the model answer directly");
    out+="    result *= normScale;\n";
    out+=s.substr(s.find("    gTarget[id.xy] = float4(max(result, float3(0.0"));
    return out;
}
void vectors(const char* shaderPath,const char* referencePath,const char* outputPath,bool red) {
    const auto bytes=binary(shaderPath);ShaderRunner port(bytes);
    const auto source=selected_reference(referencePath);
    ComPtr<ID3DBlob> referenceBlob,messages;
    const auto hr=D3DCompile(source.data(),source.size(),referencePath,nullptr,nullptr,"CSMain","cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_IEEE_STRICTNESS,0,&referenceBlob,&messages);
    if(messages) std::fprintf(stderr,"%s",static_cast<const char*>(messages->GetBufferPointer()));
    check(hr);
    const std::string referenceOutput=std::string(outputPath)+".reference.cso";
    std::ofstream savedReference(referenceOutput,std::ios::binary);
    savedReference.write(static_cast<const char*>(referenceBlob->GetBufferPointer()),static_cast<std::streamsize>(referenceBlob->GetBufferSize()));
    expect(savedReference.good(),"compiled reference evidence write");
    ShaderRunner reference({static_cast<const std::byte*>(referenceBlob->GetBufferPointer()),referenceBlob->GetBufferSize()});
    {
        ShaderRunner hardware(bytes,D3D_DRIVER_TYPE_HARDWARE);
        Image input(24);
        for(size_t i=0;i<input.size();++i) input[i]={float(i%6),float(i/6),0,float(i)/24};
        CodecConstants c;c.mode=2;c.width=3;c.height=2;
        const auto half=hardware.run(&c,sizeof(c),input,input,input,6,4).first;
        expect(std::abs(half[0].r-.5f)<1e-3f && std::abs(half[0].g-.5f)<1e-3f &&
            half[0].a==input[7].a,"2:1 area or nearest-center alpha");
        c.width=4;c.height=4;
        const auto fractional=hardware.run(&c,sizeof(c),input,input,input,6,4).first;
        expect(std::abs(fractional[0].r-1.0f/3.0f)<1e-3f &&
            std::abs(fractional[1].r-5.0f/3.0f)<1e-3f && fractional[1].a==input[2].a,
            "3:2 area or nearest-center alpha");
        input[0]={std::numeric_limits<float>::quiet_NaN(),-1,1.7f,.375f};
        input[1]={std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),.25f,.5f};
        c.mode=3;c.width=6;c.height=4;
        const auto clamped=hardware.run(&c,sizeof(c),input,input,input,6,4).first;
        expect(clamped[0].r==.5f && clamped[0].g==0 && clamped[0].b==1 && clamped[0].a==.375f,
            "finite range clamp");
        expect(clamped[1].r==.5f && clamped[1].g==.5f && clamped[1].b==.25f &&
            clamped[1].a==.5f,"infinite range clamp");
        std::printf("PASS hardware shader box: 2:1=(%.9g,%.9g,a=%.9g), 3:2=(%.9g,%.9g,a=%.9g); clamp=(%.9g,%.9g,%.9g,a=%.9g), inf=(%.9g,%.9g)\n",
            half[0].r,half[0].g,half[0].a,fractional[0].r,fractional[1].r,fractional[1].a,
            clamped[0].r,clamped[0].g,clamped[0].b,clamped[0].a,clamped[1].r,clamped[1].g);
    }
    if(red) {
        CodecConstants c;c.width=16;c.height=8;const auto rc=reference_constants(c);
        const Image raw(128,Pixel{-.1f,.2f,2.0f,.375f});const auto kept=reference.run(rc.data(),256,raw,raw,raw).second;
        expect(error(kept,raw)==0,"RED frozen source raw-copy clamps negative HDR; native exact raw contract requires preservation");
        throw std::runtime_error("RED fixture failed to detect raw-copy regression");
    }
    const std::string path(shaderPath);
    const auto dxil=binary((path.substr(0,path.find_last_of('.'))+".dxil").c_str());
    codec_list_proof(dxil);
    Image recorded;float worstReference=0,worstAnalytic=0;unsigned cases=0;
    for(uint32_t hdr:{0u,1u}) for(float white:{.5f,1.0f,2.0f}) {
        CodecConstants c;c.width=16;c.height=8;c.passthrough=1-hdr;c.whitePoint=white;
        Image raw(128),expected(128);
        for(size_t i=0;i<raw.size();++i) {
            const float value=std::array<float,8>{0,.0001f,.0031308f,.05f,.5f,.75f,1.5f,4.0f}[i%8];
            raw[i]={value,value,value,float(i%7)/7};float proxy=value;
            if(hdr) {proxy=value/white;if(proxy>.75f) proxy=.75f+.25f*(1-std::exp(-(proxy-.75f)/.25f));proxy=srgb(proxy);}
            expected[i]={proxy,proxy,proxy,raw[i].a};
        }
        const auto encoded=port.run(&c,sizeof(c),raw,raw,raw);auto rc=reference_constants(c);
        const auto originalEncoded=reference.run(rc.data(),256,raw,raw,raw);
        worstAnalytic=std::max(worstAnalytic,error(encoded.first,expected));
        expect(error(encoded.first,expected)<2e-6f && error(encoded.second,raw)==0,"analytic encode or exact raw/alpha");
        expect(error(encoded.first,originalEncoded.first)<2e-6f,"encode differs from frozen source");
        for(float gain:{0.0f,.5f,1.0f,1.25f,4.0f}) {
            Image answer=encoded.first,analytic=raw;
            for(size_t i=0;i<answer.size();++i) {
                const float p=hdr?linear(encoded.first[i].r):encoded.first[i].r;
                const float m=hdr?linear(srgb(p*gain)):p*gain;
                const float original=raw[i].r/(hdr?white:1.0f);
                const float upgraded=m<=1e-5f?original:original<p?m*original/std::max(p,1e-6f):m+std::max(0.0f,original-p);
                const float ratio=(upgraded+1.0f/512)/(original+1.0f/512);
                const float wanted=std::max(0.0f,upgraded*std::clamp(ratio,.5f,2.0f)/std::max(ratio,1e-6f))*(hdr?white:1.0f);
                const float sample=hdr?srgb(p*gain):p*gain;
                answer[i]={sample,sample,sample,1};analytic[i]={wanted,wanted,wanted,raw[i].a};
            }
            c.mode=1;rc=reference_constants(c);
            const auto resolved=port.run(&c,sizeof(c),encoded.first,answer,raw).first;
            const auto originalResolved=reference.run(rc.data(),256,encoded.first,answer,raw).first;
            const float re=error(resolved,originalResolved),ae=error(resolved,analytic);
            worstReference=std::max(worstReference,re);worstAnalytic=std::max(worstAnalytic,ae);
            expect(re<3e-5f && ae<3e-5f,"analytic neutral resolve/source comparison");
            for(float strength:{0.0f,.25f,.5f,1.0f}) {
                c.strength=strength;Image blended=raw;
                for(size_t i=0;i<raw.size();++i) {blended[i].r+=strength*(resolved[i].r-raw[i].r);blended[i].g+=strength*(resolved[i].g-raw[i].g);blended[i].b+=strength*(resolved[i].b-raw[i].b);}
                const auto out=port.run(&c,sizeof(c),encoded.first,answer,raw).first;
                expect(error(out,blended)<2e-6f,"bounded strength or source alpha");recorded.insert(recorded.end(),out.begin(),out.end());++cases;
            }
            c.strength=1;
        }
    }
    CodecConstants c;c.width=16;c.height=8;Image colour(128,Pixel{.12f,.5f,2.0f,.37f});
    const auto encoded=port.run(&c,sizeof(c),colour,colour,colour);auto rc=reference_constants(c);
    expect(error(encoded.first,reference.run(rc.data(),256,colour,colour,colour).first)<2e-6f,"saturated HDR encode");
    Image answer(128,Pixel{.4f,.6f,.7f,1});c.mode=1;rc=reference_constants(c);
    const auto out=port.run(&c,sizeof(c),encoded.first,answer,colour).first;
    expect(error(out,reference.run(rc.data(),256,encoded.first,answer,colour).first)<3e-5f,"RenoDX chromatic resolve");
    Image negative(128,Pixel{-.1f,.2f,2.0f,.375f});c.mode=0;
    expect(error(port.run(&c,sizeof(c),negative,negative,negative).second,negative)==0,"negative HDR raw not retained");
    c.mode=1;c.strength=0;expect(error(port.run(&c,sizeof(c),encoded.first,answer,negative).first,negative)==0,"zero strength exact raw");
    // Exercise the actual production D3D12 pipeline initialization with the compiled bytes.
    ComPtr<ID3D12Device> d12;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)));
    D3D12Codec codec(d12.Get(),bytes);
    std::ofstream output(outputPath,std::ios::binary);output.write(reinterpret_cast<const char*>(recorded.data()),static_cast<std::streamsize>(recorded.size()*sizeof(Pixel)));expect(output.good(),"vector evidence write");
    std::printf("PASS shader vectors: %u bounded composition cases; analytic max %.9g; frozen reference max %.9g; WARP arithmetic only, no model inference\n",cases,worstAnalytic,worstReference);
}
int main(int argc,char** argv) try {
    contracts();
    if(argc==5 && (std::string(argv[1])=="--shader-vectors" || std::string(argv[1])=="--source-red"))
        vectors(argv[2],argv[3],argv[4],std::string(argv[1])=="--source-red");
    else if(argc!=1) throw std::runtime_error("usage: --shader-vectors|--source-red compiled_shader frozen_source output_binary");
    return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
