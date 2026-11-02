// SPDX-License-Identifier: GPL-3.0-only
#include "nr_ghost_scenario.h"
#include "nr_bootstrap_scenario.h"
#include "nr_bootstrap_images.h"
#include "nr_ghost_digest.h"
#include "nr_ghost_chunk.h"
#include "nr_ghost_storage.h"
#include "nr_ghost_receipt.h"
#include "nr_recording_encode.h"
#include "nr_recording_cleanup.h"
#include "nr_window_artifacts.h"
#include "obs_native_output.h"
#include "ghost_oracle.h"
#include "ghost_freeze.h"
#include "ghost_baseline.h"
#include <DirectXPackedVector.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <functional>
#include <semaphore>
#include <thread>
#include <syncstream>
#include <psapi.h>

namespace {
using namespace nr;
namespace ghost=nr::ghost;
namespace gb=nr::ghost::baseline;
namespace image=nr::qa::bootstrap;
namespace identity=nr::qa::ghost_digest;
using Microsoft::WRL::ComPtr;
constexpr Extent canvas{1920,1080};
void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
void checked(HRESULT value,const char* why) {require(SUCCEEDED(value),why);}

std::string freeze_file_hash(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    require(input.good()&&input.tellg()>0,"freeze source file absent");
    std::vector<uint8_t> bytes(static_cast<size_t>(input.tellg()));input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    require(input.good(),"freeze source file incomplete");
    identity::Sha256 hash;return identity::hex(hash.hash(bytes));
}
ghost::FreezePolicy load_ghost_freeze(const std::filesystem::path& work) {
    std::array<char,8192> text{};
    const auto length=GetEnvironmentVariableA("NR_GHOST_FREEZE_POLICY",text.data(),static_cast<DWORD>(text.size()));
    require(length>0&&length<text.size(),"public five-file freeze binding absent or oversized");
    auto* data=obs_data_create_from_json(text.data());require(data!=nullptr,"public freeze binding JSON");
    struct Owner {obs_data_t* value;~Owner(){obs_data_release(value);}} owner{data};
    const auto base=work/".omo/evidence/obs-nr-native-filter-20260930/task-26";
    const auto policyRoot=base/"foundation-20261014";
    require(std::filesystem::path(obs_data_get_string(data,"policy_root"))==policyRoot,
        "public policy root differs from source-bound Task26 freeze");
    ghost::FreezePolicy policy;
    for(size_t i=0;i<policy.hashes.size();++i) {
        policy.hashes[i]=obs_data_get_string(data,("hash"+std::to_string(i)).c_str());
        require(freeze_file_hash(policyRoot/ghost::freeze_files[i])==policy.hashes[i],"actual freeze input hash drift");
    }
    const auto manifest=base/"manifests/cpu-fixtures.json";
    require(std::filesystem::path(obs_data_get_string(data,"manifest_path"))==manifest,"public manifest path differs");
    policy.manifestHash=obs_data_get_string(data,"manifest_hash");
    require(freeze_file_hash(manifest)==policy.manifestHash,"actual retained manifest hash drift");
    const auto number=[&](const char* name){return static_cast<uint32_t>(obs_data_get_int(data,name));};
    policy.calibrationSeed=number("calibration_seed");policy.holdoutSeed=number("holdout_seed");policy.ticks=number("ticks");
    policy.rateNumerators={number("rate0_num"),number("rate1_num")};
    policy.rateDenominators={number("rate0_den"),number("rate1_den")};
    policy.width=number("width");policy.height=number("height");policy.visibleTop=number("visible_top");
    policy.physicalWidth=number("physical_width");policy.physicalHeight=number("physical_height");
    policy.staticEdit={number("static_x"),number("static_y"),number("static_width"),number("static_height")};
    policy.movingInset=number("moving_inset");policy.editRetention=obs_data_get_double(data,"edit_retention");
    policy.temporalFactor=obs_data_get_double(data,"temporal_factor");policy.detailFactor=obs_data_get_double(data,"detail_factor");
    policy.editFactor=obs_data_get_double(data,"edit_factor");ghost::validate_freeze(policy);return policy;
}

std::string read_accepted_text(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    require(input.good()&&input.tellg()>0,"accepted Task26 document absent");
    std::string text(static_cast<size_t>(input.tellg()),'\0');input.seekg(0);
    input.read(text.data(),static_cast<std::streamsize>(text.size()));
    require(input.good(),"accepted Task26 document incomplete");
    return text;
}
std::string lowered(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value;
}
// The late-bound final-hash argument: the accepted Task26 calibration/controls bytes are read-only inputs whose SHA-256
// must equal the values the entry passes in NR_GHOST_ACCEPTED_CALIBRATION. Nothing here opens a file for writing.
gb::AcceptedCalibration load_accepted_calibration(const std::filesystem::path& work,const ghost::FreezePolicy& policy) {
    std::array<char,8192> text{};
    const auto length=GetEnvironmentVariableA("NR_GHOST_ACCEPTED_CALIBRATION",text.data(),static_cast<DWORD>(text.size()));
    require(length>0&&length<text.size(),"late-bound accepted Task26 calibration binding absent or oversized");
    auto* data=obs_data_create_from_json(text.data());require(data!=nullptr,"accepted Task26 calibration binding JSON");
    struct Owner {obs_data_t* value;~Owner(){obs_data_release(value);}} owner{data};
    const auto task26=lowered((work/".omo/evidence/obs-nr-native-filter-20260930/task-26").lexically_normal().generic_string())+"/";
    const auto document=[&](const char* name) {
        const std::filesystem::path path(obs_data_get_string(data,name));
        require(lowered(path.lexically_normal().generic_string()).rfind(task26,0)==0,
            "accepted Task26 document outside the Task26 evidence tree");
        return read_accepted_text(path);
    };
    const auto calibration=document("calibration_path"),controls=document("controls_path");
    return gb::accept_calibration(calibration,controls,obs_data_get_string(data,"calibration_sha256"),
        obs_data_get_string(data,"controls_sha256"),policy);
}

double linear(double v) {return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);}
ghost::Rgb to709(ghost::Rgb c,ghost::FixtureColor color) {
    if(color!=ghost::FixtureColor::Hdr2020)return c;
    return {1.6604910021*c.r-.5876411388*c.g-.0728498633*c.b,
        -.1245504745*c.r+1.1328998971*c.g-.0083494226*c.b,
        -.0181507634*c.r-.1005788980*c.g+1.1187296614*c.b};
}
ghost::Rgb from709(ghost::Rgb c,ghost::FixtureColor color) {
    if(color!=ghost::FixtureColor::Hdr2020)return c;
    return {.6274038959*c.r+.3292830384*c.g+.0433130657*c.b,
        .0690972894*c.r+.9195403951*c.g+.0113623155*c.b,
        .0163914389*c.r+.0880133079*c.g+.8955952532*c.b};
}
struct PackedPixels {
    std::span<const uint8_t> bytes;
    bool half,bgra,gamma;
    ghost::FixtureColor color;
    double factor;
    void validate() const {
        require(bytes.size()==static_cast<size_t>(canvas.width)*canvas.height*(half?8u:4u),
            "full-size detached packed RGB bounds");
        if(half)require(image::finite_half(bytes),"nonfinite detached packed RGB channel");
    }
    ghost::Rgb rgb(unsigned x,unsigned y) const {
        auto c=image::packed_rgb(bytes,static_cast<size_t>(y)*canvas.width+x,half,gamma,bgra);
        c=from709(c,color);return {c.r*factor,c.g*factor,c.b*factor};
    }
    static ghost::Rgb read(const void* value,unsigned x,unsigned y) {
        return static_cast<const PackedPixels*>(value)->rgb(x,y);
    }
};
bool packed_scene_identity(const ghost::Fixture& f,unsigned tick,const PackedPixels& pixels,obs_data_t* witness=nullptr) {
    unsigned id=0;
    const auto point=[&](const char* name,unsigned x,unsigned y) {
        if(!witness)return;
        uint64_t word=0;const size_t stride=pixels.half?8u:4u;
        memcpy(&word,pixels.bytes.data()+(static_cast<size_t>(y)*canvas.width+x)*stride,stride);
        obs_data_set_string(witness,name,std::to_string(word).c_str());
    };
    if(witness)obs_data_set_int(witness,"expected_id",f.schedule.at(tick).contentId);
    for(unsigned bit=0;bit<16;++bit) {
        const auto a=pixels.rgb(bit*4+1,0),b=pixels.rgb(bit*4+1,2);
        if(!std::isfinite(a.r)||!std::isfinite(b.r)||std::abs(a.r+b.r-1)>.1||std::abs(a.r-b.r)<.5) {
            if(witness) {
                obs_data_set_string(witness,"failure","header_signal");obs_data_set_int(witness,"bit",bit);
                obs_data_set_double(witness,"row0_r",a.r);obs_data_set_double(witness,"row2_r",b.r);
                obs_data_set_double(witness,"complement_error",std::abs(a.r+b.r-1));
                obs_data_set_double(witness,"signed_contrast",a.r-b.r);
                point("row0_actual_packed",bit*4+1,0);point("row2_actual_packed",bit*4+1,2);
            }
            return false;
        }
        if(a.r>b.r)id|=1u<<bit;
    }
    if(witness)obs_data_set_int(witness,"decoded_id",id);
    if(id!=f.schedule.at(tick).contentId) {
        if(witness)obs_data_set_string(witness,"failure","decoded_id");return false;
    }
    const double bound=f.color==ghost::FixtureColor::DarkSdr?.125*.0625:.125;
    for(unsigned y=20;y<68;y+=4)for(unsigned x=2;x<192;x+=4) {
        const auto a=pixels.rgb(x,y),b=ghost::clean_pixel(f,f.schedule[tick],x,y);
        if(std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),std::abs(a.b-b.b)})>bound) {
            if(witness) {
                obs_data_set_string(witness,"failure","geometry");obs_data_set_int(witness,"x",x);obs_data_set_int(witness,"y",y);
                obs_data_set_double(witness,"bound",bound);
                obs_data_set_double(witness,"actual_r",a.r);obs_data_set_double(witness,"actual_g",a.g);obs_data_set_double(witness,"actual_b",a.b);
                obs_data_set_double(witness,"expected_r",b.r);obs_data_set_double(witness,"expected_g",b.g);obs_data_set_double(witness,"expected_b",b.b);
                point("actual_packed",x,y);
            }
            return false;
        }
    }
    return true;
}
double alpha(uint32_t x,uint32_t y) {return x<128&&y>=960&&y<1040?0:1;}
ghost::Image analytic(const ghost::Fixture& f,unsigned tick) {
    auto result=ghost::clean_image(f,tick,canvas);
    for(uint32_t y=960;y<1040;++y)for(uint32_t x=0;x<128;++x)result[y*canvas.width+x]={0,0,0};
    return result;
}
ghost::Image canonical(const image::Image& pixels,const ghost::Fixture& f,bool sourceLinear,
    double whiteNits=80,bool encodedProxy=false,bool integerGamma=false) {
    require(pixels.pixels.size()==static_cast<size_t>(canvas.width)*canvas.height,"actual full-size RGB observation");
    ghost::Image result(pixels.pixels.size());
    for(size_t p=0;p<result.size();++p) {
        const auto& value=pixels.pixels[p];
        ghost::Rgb c{value[0],value[1],value[2]};
        if(integerGamma) {
            static const auto values=[] {
                std::array<double,256> table{};
                for(size_t code=0;code<table.size();++code)table[code]=linear(static_cast<double>(code)/255);
                return table;
            }();
            c={values[static_cast<size_t>(std::lround(c.r*255))],
                values[static_cast<size_t>(std::lround(c.g*255))],
                values[static_cast<size_t>(std::lround(c.b*255))]};
        }else if(!sourceLinear||encodedProxy)c={linear(c.r),linear(c.g),linear(c.b)};
        if(encodedProxy) {
            // The actual encoded model input is a lossy soft-knee proxy, NOT source RGB.
            // Its own fixed-domain image/geometry is checked separately; no inverse brightness guess.
            result[p]=c;continue;
        }
        c=from709(c,f.color);
        const double factor=whiteNits/f.referenceWhiteNits;
        result[p]={c.r*factor,c.g*factor,c.b*factor};
    }
    return result;
}
struct BorrowedCanonicalPixels {
    const image::Image& pixels;
    const ghost::Fixture& fixture;
    bool sourceLinear;
    double whiteNits;
    ghost::Rgb read(size_t index) const {
        const auto& value=pixels.pixels.at(index);
        ghost::Rgb c{value[0],value[1],value[2]};
        if(!sourceLinear)c={linear(c.r),linear(c.g),linear(c.b)};
        c=from709(c,fixture.color);
        const double factor=whiteNits/fixture.referenceWhiteNits;
        return {c.r*factor,c.g*factor,c.b*factor};
    }
    bool identity(unsigned tick) const {
        require(pixels.pixels.size()==static_cast<size_t>(canvas.width)*canvas.height,
            "complete borrowed actual decoded RGB canvas");
        // Preserve the complete canonical conversion/finite consumer, without
        // retaining a second owning image. Every read uses actual decoded RGB.
        for(size_t p=0;p<pixels.pixels.size();++p) {
            const auto c=read(p);
            require(std::isfinite(c.r)&&std::isfinite(c.g)&&std::isfinite(c.b),"complete actual decoded canonical finiteness");
        }
        unsigned content=0;
        for(unsigned bit=0;bit<16;++bit) {
            const auto a=read(bit*4+1),b=read(2*canvas.width+bit*4+1);
            require(std::abs(a.r+b.r-1)<=.1&&std::abs(a.r-b.r)>=.5,"independent decoded canonical landmark");
            if(a.r>b.r)content|=1u<<bit;
        }
        if(content!=fixture.schedule.at(tick).contentId)return false;
        const double bound=fixture.color==ghost::FixtureColor::DarkSdr?.125*.0625:.125;
        for(unsigned y=20;y<68;y+=4)for(unsigned x=2;x<192;x+=4) {
            const auto a=read(static_cast<size_t>(y)*canvas.width+x);
            const auto b=ghost::clean_pixel(fixture,fixture.schedule.at(tick),x,y);
            if(std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),std::abs(a.b-b.b)})>bound)return false;
        }
        return true;
    }
};
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Event(){require(value!=nullptr,"ghost exact event");}
    ~Event(){CloseHandle(value);}
    void wait(uint32_t bound=30000){require(WaitForSingleObject(value,bound)==WAIT_OBJECT_0,"ghost exact event bound");}
};
// One bounded QA readback owner; all real queue work and resources survive an unresolved wait.
struct Readback {
    const std::shared_ptr<interop::Context> context;
    ComPtr<ID3D12Resource> input,buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    Event event;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows=0;
    UINT64 rowBytes=0,bytes=0;
    bool pending=false;
    Readback(std::shared_ptr<interop::Context> c,ID3D12Resource* resource):context(std::move(c)),input(resource) {
        require(context&&input,"actual QA resource/context");
        auto* device=context->device();const auto desc=input->GetDesc();
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&bytes);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC allocation{};allocation.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        allocation.Width=bytes;allocation.Height=allocation.DepthOrArraySize=allocation.MipLevels=1;
        allocation.SampleDesc.Count=1;allocation.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&allocation,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)),"bounded QA readback allocation");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"QA observation allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"QA observation list");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"QA observation fence");
    }
    std::vector<uint8_t> read() {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={input.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION source{},target{};source.pResource=input.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.pResource=buffer.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=footprint;
        list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        checked(list->Close(),"QA observation close");
        checked(fence->SetEventOnCompletion(1,event.value),"pre-trigger QA observation fence subscription");
        pending=true;ID3D12CommandList* commands[]{list.Get()};context->queue()->ExecuteCommandLists(1,commands);
        checked(context->queue()->Signal(fence.Get(),1),"QA observation signal");
        event.wait();require(fence->GetCompletedValue()==1,"actual QA observation completion");
        pending=false;
        void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
        checked(buffer->Map(0,&range,&mapped),"bounded QA observation map");
        std::vector<uint8_t> result(static_cast<size_t>(rowBytes)*rows);
        for(UINT y=0;y<rows;++y)memcpy(result.data()+static_cast<size_t>(y)*rowBytes,
            static_cast<const uint8_t*>(mapped)+footprint.Offset+static_cast<size_t>(y)*footprint.Footprint.RowPitch,
            static_cast<size_t>(rowBytes));
        const D3D12_RANGE empty{0,0};buffer->Unmap(0,&empty);return result;
    }
};
std::vector<uint8_t> read_resource(const std::shared_ptr<interop::Context>& context,ID3D12Resource* resource,bool& retained) {
    auto owner=std::make_unique<Readback>(context,resource);
    try{return owner->read();}
    catch(...){if(owner->pending){retained=true;(void)owner.release();}throw;}
}
void emit(const char* prefix,obs_data_t* row) {
    std::osyncstream(std::cout)<<prefix<<obs_data_get_json(row)<<std::endl;obs_data_release(row);
}
obs_data_t* frame_key(const FrameKey& key) {
    auto* data=obs_data_create();
    obs_data_set_int(data,"instance",static_cast<int64_t>(key.instanceId));
    obs_data_set_int(data,"content_epoch",static_cast<int64_t>(key.contentEpoch));
    obs_data_set_int(data,"ingress",static_cast<int64_t>(key.ingressSequence));return data;
}
void set_job_key(obs_data_t* data,const char* name,const std::optional<JobKey>& key) {
    if(!key)return;
    auto* value=frame_key(key->frame);
    obs_data_set_int(value,"history",static_cast<int64_t>(key->historyEpoch));
    obs_data_set_int(value,"generation",static_cast<int64_t>(key->resourceGeneration));
    obs_data_set_int(value,"submission",static_cast<int64_t>(key->submissionSerial));
    obs_data_set_obj(data,name,value);obs_data_release(value);
}
void digest_frame_key(qa::GhostDigestRecord& record,const char* name,const FrameKey& key) {
    record.key(name);record.object();
    record.number("instance",key.instanceId);record.number("content_epoch",key.contentEpoch);
    record.number("ingress",key.ingressSequence);record.end_object();
}
void digest_job_key(qa::GhostDigestRecord& record,const char* name,const std::optional<JobKey>& key) {
    if(!key)return;
    record.key(name);record.object();
    record.number("instance",key->frame.instanceId);record.number("content_epoch",key->frame.contentEpoch);
    record.number("ingress",key->frame.ingressSequence);record.number("history",key->historyEpoch);
    record.number("generation",key->resourceGeneration);record.number("submission",key->submissionSerial);
    record.end_object();
}
struct GhostDigestLane {
    const std::shared_ptr<interop::Context> context;
    identity::ChunkPipeline pipeline;
    GhostDigestLane(std::shared_ptr<interop::Context> owner,std::span<const std::byte> shader):
        context(std::move(owner)),pipeline(context->device(),shader) {}
};
class GhostCodecDigest final {
    std::shared_ptr<GhostDigestLane> lane_;
    std::shared_ptr<qa::GhostCredits> credits_;
    ComPtr<ID3D12Resource> upload_,leaves_,readback_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence_;
    Event event_;
    uint64_t value_=0;
public:
    bool pending=false;
    std::array<uint64_t,3> capacities{};
    explicit GhostCodecDigest(std::shared_ptr<GhostDigestLane> lane,bool hdr,
        std::shared_ptr<qa::GhostCredits> credits):lane_(std::move(lane)),credits_(std::move(credits)) {
        require(lane_!=nullptr,"retired native private codec lane");
        require(credits_!=nullptr,"same retired capture credit ledger");
        auto* device=lane_->context->device();const uint64_t bytes=hdr?6220800ull:3110400ull;
        const uint64_t leafBytes=((bytes+identity::chunkBytes-1)/identity::chunkBytes)*32+4;
        const std::array<ComPtr<ID3D12Resource>*,3> outputs{&upload_,&leaves_,&readback_};
        const std::array<D3D12_HEAP_TYPE,3> heaps{D3D12_HEAP_TYPE_UPLOAD,D3D12_HEAP_TYPE_DEFAULT,D3D12_HEAP_TYPE_READBACK};
        for(size_t i=0;i<outputs.size();++i) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width=i==0?bytes:leafBytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
            d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(i==1)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=heaps[i];
            checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
                i==0?D3D12_RESOURCE_STATE_GENERIC_READ:i==1?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(outputs[i]->ReleaseAndGetAddressOf())),"one actual codec digest frame");
            capacities[i]=device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;
        }
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_)),"codec digest allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,
            IID_PPV_ARGS(&commands_)),"codec digest commands");checked(commands_->Close(),"codec prepared command close");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)),"codec digest actual fence");
    }
    identity::Digest hash(std::span<const std::span<const uint8_t>> planes,identity::Sha256& root,identity::Timing& timing) {
        const auto start=std::chrono::steady_clock::now();uint64_t bytes=0;
        for(const auto plane:planes)bytes+=plane.size();
        require(!pending&&bytes>0&&bytes<=upload_->GetDesc().Width&&bytes<=UINT32_MAX,"one complete actual codec upload");
        void* mapped=nullptr;const D3D12_RANGE none{0,0};
        checked(upload_->Map(0,&none,&mapped),"codec complete logical plane upload map");
        size_t at=0;for(const auto plane:planes) {
            if(!plane.empty())memcpy(static_cast<uint8_t*>(mapped)+at,plane.data(),plane.size());
            at+=plane.size();
        }
        upload_->Unmap(0,nullptr);
        checked(allocator_->Reset(),"codec allocator actual completion reuse");
        checked(commands_->Reset(allocator_.Get(),nullptr),"codec list actual completion reuse");
        lane_->pipeline.record(commands_.Get(),upload_.Get(),leaves_.Get(),static_cast<uint32_t>(bytes),
            static_cast<uint32_t>(bytes),static_cast<uint32_t>(bytes),0,false);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={leaves_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands_->ResourceBarrier(1,&barrier);
        const auto leafBytes=((bytes+identity::chunkBytes-1)/identity::chunkBytes)*32;
        commands_->CopyBufferRegion(readback_.Get(),0,leaves_.Get(),0,leafBytes+4);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands_->ResourceBarrier(1,&barrier);
        checked(commands_->Close(),"codec actual command close");require(value_<UINT64_MAX-1,"codec actual fence exhaustion");
        ++value_;ResetEvent(event_.value);
        checked(fence_->SetEventOnCompletion(value_,event_.value),"codec event subscribed before submission");
        ID3D12CommandList* lists[]{commands_.Get()};pending=true;lane_->context->queue()->ExecuteCommandLists(1,lists);
        checked(lane_->context->queue()->Signal(fence_.Get(),value_),"codec actual fence signal");
        require(WaitForSingleObject(event_.value,30000)==WAIT_OBJECT_0&&fence_->GetCompletedValue()!=UINT64_MAX&&
            fence_->GetCompletedValue()>=value_,"codec exact chunk completion");pending=false;
        const D3D12_RANGE range{0,static_cast<SIZE_T>(leafBytes+4)};
        checked(readback_->Map(0,&range,&mapped),"codec completed actual leaf map");
        identity::Digest digest{};
        try {
            uint32_t invalid=0;memcpy(&invalid,static_cast<const uint8_t*>(mapped)+leafBytes,4);
            require(invalid==0,"codec integer planes not half-float storage");
            digest=identity::chunk_root(root,bytes,{static_cast<const uint8_t*>(mapped),static_cast<size_t>(leafBytes)});
        }catch(...){readback_->Unmap(0,&none);throw;}
        readback_->Unmap(0,&none);
        timing.bytes=bytes;timing.hashNs=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count();return digest;
    }
    std::array<uint64_t,2> completion_values() const {return {value_,fence_->GetCompletedValue()};}
};
void worker_timing(const std::shared_ptr<model::Job>& job) {
    if(!job||!job->ready.load(std::memory_order_acquire))return;
    LARGE_INTEGER frequency{};require(QueryPerformanceFrequency(&frequency)!=FALSE,"actual QPC frequency");
    auto* row=obs_data_create();
    obs_data_set_int(row,"instance",static_cast<int64_t>(job->contract.key.frame.instanceId));
    obs_data_set_int(row,"ingress",static_cast<int64_t>(job->contract.key.frame.ingressSequence));
    obs_data_set_int(row,"history",static_cast<int64_t>(job->contract.key.historyEpoch));
    obs_data_set_int(row,"serial",static_cast<int64_t>(job->contract.key.submissionSerial));
    obs_data_set_int(row,"qpc_frequency",frequency.QuadPart);
    for(size_t i=0;i<job->qaTiming.size();++i)
        obs_data_set_int(row,("stage_"+std::to_string(i)).c_str(),job->qaTiming[i]);
    for(size_t i=0;i<job->chronology.size();++i)
        obs_data_set_int(row,("chronology_"+std::to_string(i)).c_str(),job->chronology[i]);
    obs_data_set_bool(row,"ready",true);emit("NR_GHOST_WORKER_TIMING ",row);
}
}

std::vector<uint8_t> nr_ghost_fixture_bytes(uint32_t seed,uint32_t color,uint32_t tick,bool linearSource) {
    const auto started=std::chrono::steady_clock::now();
    require(color<=3&&tick<256,"frozen physical fixture selector");
    const auto f=ghost::fixture(seed,static_cast<ghost::FixtureColor>(color));
    require(linearSource||color<2,"HDR fixture requires actual half-float transport");
    auto bytes=image::ghost_fixture_bytes(f,tick,linearSource);
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
    std::osyncstream(std::cout)<<"NR_GHOST_SOURCE_TIMING seed="<<seed<<" color="<<color<<" source_tick="<<tick
        <<" bytes="<<bytes.size()<<" cpu_ns="<<elapsed<<std::endl;
    return bytes;
}

namespace {
struct Metrics {
    double formatBound=0,rawMaximum=0,guideMismatch=0,absoluteVector=0,relativeVector=0;
    double staticRms=0,movingRms=0,staticRetention=0,movingRetention=0;
    uint64_t rawSamples=0,nativeGuides=0,guidePoints=0,invalidSupport=0,modelSamples=0;
    uint64_t alphaErrors=0,codecFrames=0,codecErrors=0;
};
double declared_bound(const ghost::Fixture& f,bool sourceLinear) {
    if(!sourceLinear)return linear(1)-linear(1-.5/255);
    const auto maximum=to709({4,2,1},f.color);
    const double peak=(f.color==ghost::FixtureColor::Sdr||f.color==ghost::FixtureColor::DarkSdr)?
        1:std::max({maximum.r,maximum.g,maximum.b})*f.referenceWhiteNits/80;
    return (std::ldexp(1.0,std::ilogb(peak)-11)+peak*std::numeric_limits<float>::epsilon())*80/f.referenceWhiteNits;
}
std::vector<size_t> roi(const ghost::Fixture& f,unsigned tick,bool moving,const ghost::FreezePolicy* policy=nullptr) {
    std::vector<size_t> points;
    if(moving) {
        const auto& b=f.schedule[tick].bar;
        const int inset=policy?static_cast<int>(policy->movingInset):8;
        for(int y=b.y+inset;y<b.y+b.height-inset;++y)for(int x=b.x+inset;x<b.x+b.width-inset;++x)
            points.push_back(static_cast<size_t>(y)*canvas.width+static_cast<size_t>(x));
    }else {
        const auto box=policy?policy->staticEdit:std::array<uint32_t,4>{256,128,32,32};
        for(unsigned y=box[1];y<box[1]+box[3];++y)for(unsigned x=box[0];x<box[0]+box[2];++x)points.push_back(y*canvas.width+x);
    }
    return points;
}
std::vector<ghost::Edge> prescribed_edges(const ghost::Image& clean) {
    std::vector<ghost::Edge> edges;
    for(const auto& region:std::array<Rect,3>{{{0,8,192,88},{256,128,32,32},{0,79,1920,3}}})
        for(unsigned y=region.y;y<region.y+region.height;++y)for(unsigned x=region.x;x<region.x+region.width;++x) {
            const size_t p=y*canvas.width+x;
            if(x+1<region.x+region.width)edges.push_back({p,p+1});
            if(y+1<region.y+region.height)edges.push_back({p,p+canvas.width});
        }
    require(clean.size()==static_cast<size_t>(canvas.width)*canvas.height,"full-size detail canvas");
    return edges;
}
// Signed 1px channel contrast loss on the prescribed regions, for the raw (n-1) and published pixels in one pass.
uint64_t baseline_edges(const ghost::Fixture& f,unsigned tick,ghost::PixelView raw,ghost::PixelView selected,
    double& rawLoss,double& selectedLoss) {
    uint64_t checks=0;
    rawLoss=selectedLoss=0;
    for(const auto& region:std::array<Rect,3>{{{0,8,192,88},{256,128,32,32},{0,79,1920,3}}})
        for(unsigned y=region.y;y<region.y+region.height;++y)for(unsigned x=region.x;x<region.x+region.width;++x)
            for(const auto next:std::array<std::pair<unsigned,unsigned>,2>{{{x+1,y},{x,y+1}}}) {
                if(next.first>=region.x+region.width||next.second>=region.y+region.height)continue;
                const auto a=ghost::clean_pixel(f,f.schedule[tick],x,y);
                const auto b=ghost::clean_pixel(f,f.schedule[tick],next.first,next.second);
                const auto r0=raw.read(raw.pixels,x,y),r1=raw.read(raw.pixels,next.first,next.second);
                const auto s0=selected.read(selected.pixels,x,y),s1=selected.read(selected.pixels,next.first,next.second);
                const std::array<double,3> before{a.r-b.r,a.g-b.g,a.b-b.b},afterRaw{r0.r-r1.r,r0.g-r1.g,r0.b-r1.b},
                    afterSelected{s0.r-s1.r,s0.g-s1.g,s0.b-s1.b};
                for(size_t channel=0;channel<3;++channel) {
                    if(before[channel]==0)continue;
                    ++checks;
                    const double sign=before[channel]>0?1.0:-1.0;
                    rawLoss=std::max(rawLoss,std::abs(before[channel])-sign*afterRaw[channel]);
                    selectedLoss=std::max(selectedLoss,std::abs(before[channel])-sign*afterSelected[channel]);
                }
            }
    return checks;
}
enum class Action { None,Bind,Prepare,Arm,Off,CloseObservation,Stop,Release };
struct FrameRow {
    uint64_t tick=0,clock=0,interval=0,ingress=0,selected=0;
    unsigned sourceTick=0,content=0,kind=0;
    identity::Digest rawHash{},selectedHash{},programHash{};
    std::optional<JobKey> job;
    std::optional<FrameKey> selectedFrame;
    uint64_t settingsRevision=0,generation=0;
};
class SnapshotPool final:public model::ObservationSink {
public:
    enum Graphic:size_t { CurrentRaw,SelectedRaw,Selected,Program,Preview,Projector,GraphicsCount };
    static constexpr size_t modelCount=model::Observation::Count,roleCount=modelCount+GraphicsCount;
    enum class Refusal:uint32_t { None,Saturation,Shape,Signal,Validation,Unknown };
    enum Cpu:size_t { Validity,RawAdmission,Alpha,Identity,GraphicsHash,SelectedEdit,Views,
        Model,GuideHash,GuideArithmetic,Pictures,Score,CpuCount };
    struct Envelope final:model::Observation {
        FrameRow row;
        std::optional<qa::GhostCredits::Token> token;
        std::optional<Selection> selection;
        std::optional<JobKey> fixtureKey,fixtureReference;
        std::array<ComPtr<ID3D11Texture2D>,GraphicsCount> graphics,sources;
        std::array<ComPtr<ID3D12Resource>,GraphicsCount> opened;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,roleCount> footprints{};
        std::array<UINT,roleCount> rows{};
        std::array<UINT64,roleCount> rowBytes{};
        std::array<std::vector<uint8_t>,roleCount> compact;
        std::array<std::span<const uint8_t>,roleCount> packed;
        std::array<qa::GhostImageRows,roleCount> imageRows;
        std::array<uint64_t,roleCount> leafOffsets{};
        std::array<std::optional<identity::Digest>,roleCount> digests;
        identity::Timing digestTiming;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> readback,leafReadback;
        ComPtr<ID3D12Fence> producer,batch;
        ComPtr<ID3D11Fence> producer11;
        Event modelEvent,producerEvent,batchEvent;
        uint64_t settingsRevision=0,producerValue=0,batchValue=0,graphicsMask=0,readbackBytes=0;
        std::array<int64_t,2> drainTiming{};
        std::array<int64_t,3> readbackParts{};
        uint64_t copiedBytes=0,mappedImageBytes=0;
        bool mapOpen=false,leafMapOpen=false;
        uint64_t leafReadbackBytes=0;
        std::array<int64_t,CpuCount> cpuNs{};
        image::RawScanCounts rawCounts;
        int64_t consumerTotalNs=0;
        bool queued=false,uncertain=false,sameViews=false,preparing=false;
        bool offConfigurationEnabled=true,offFilterEnabled=true;
        bool copySubmitted=false;
        std::chrono::steady_clock::time_point creditStarted{},copyStarted{};
        Refusal refusal=Refusal::None;
        Envelope(){observed=modelEvent.value;}
    };
    struct Bytes { uint64_t gpu=0,readback=0,packed=0,leafReadback=0; };
private:
    struct State {
        std::shared_ptr<interop::Context> context;
        ComPtr<ID3D11Device5> renderer;
        ComPtr<ID3D11DeviceContext4> immediate;
        std::array<Envelope,3> envelopes;
        std::shared_ptr<qa::GhostCredits> credits=std::make_shared<qa::GhostCredits>();
        std::array<Envelope*,3> pending{};
        std::atomic<uint64_t> written{0},read{0};
        std::counting_semaphore<4> available{0};
        std::atomic<bool> closed{false},exited{false};
        std::atomic<Refusal> failure{Refusal::None};
        Event exit;
        std::thread drain;
        std::function<void(Envelope&)> consumer;
        std::function<void(uint64_t,int64_t,int64_t,int64_t)> measurementRetired;
        identity::Sha256* digest=nullptr; // Borrow existing Harness provider; drain joins before codec.
        std::shared_ptr<GhostDigestLane> digestLane;
        ComPtr<ID3D12Resource> leafScratch;
        bool sourceLinear=false,hdrOutput=false;
        uint32_t oracleColor=0;
        double oracleWhite=80;
        Bytes bytes;
        DWORD graphicsThread=GetCurrentThreadId();
        uint64_t baseIngress=0;
        bool episode=false; // Graphics-owned; each queued envelope snapshots this phase.
        // Drain-owned, read only after the existing join. Bounded scalar
        // counters include the final SERVICE_CYCLE serialization/emission.
        uint64_t measuredIterations=0;
        int64_t firstIterationNs=0,subsequentMinimumNs=INT64_MAX,subsequentMaximumNs=0;
    };
    State* state=nullptr;
    static D3D12_RESOURCE_DESC texture(DXGI_FORMAT format,Extent size=canvas) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=size.width;d.Height=size.height;d.DepthOrArraySize=d.MipLevels=1;
        d.Format=format;d.SampleDesc.Count=1;return d;
    }
    static D3D12_RESOURCE_DESC buffer(uint64_t width,bool unordered=false) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=(width+3)&~3ull;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(unordered)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return d;
    }
    static DXGI_FORMAT family(DXGI_FORMAT format) {
        switch(format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default:return format;
        }
    }
    static void refuse(State& s,Envelope& e,Refusal reason) noexcept {
        if(e.refusal==Refusal::None)e.refusal=reason;
        e.refused.store(true,std::memory_order_release);s.failure.store(reason,std::memory_order_release);
    }
    static bool present(const Envelope& e,size_t role) {
        return role<modelCount?(e.stages&(1ull<<role))!=0:(e.graphicsMask&(1ull<<(role-modelCount)))!=0;
    }
    static bool read_present(const Envelope& e,size_t role) {
        return present(e,role); // Every present actual payload, including preparation and views.
    }
    static void digest_stages(State& s,Envelope& e,std::span<const uint8_t> leaves) {
        const auto started=std::chrono::steady_clock::now();
        require(s.digest&&leaves.size()==e.leafReadbackBytes,"actual root owner and leaf extent");
        for(size_t role=0;role<roleCount;++role)if(present(e,role)) {
            const auto length=e.imageRows[role].logical_bytes(),count=(length+identity::chunkBytes-1)/identity::chunkBytes;
            const auto leafBytes=count*32,offset=e.leafOffsets[role];
            require(offset<=leaves.size()&&leafBytes+4<=leaves.size()-offset,"actual ordered leaf receipt bounds");
            uint32_t invalid=0;memcpy(&invalid,leaves.data()+offset+leafBytes,4);
            const auto format=e.footprints[role].Footprint.Format;
            const bool half=format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R16G16_FLOAT;
            // The existing complete CPU half/alpha scan remains authoritative;
            // the GPU flag is an additional exact scan, not its replacement.
            if(half)require(image::finite_half(e.packed[role])&&invalid==0,"actual complete CPU/GPU half finiteness");
            else require(invalid==0,"integer role cannot report half nonfiniteness");
            const auto begin=std::chrono::steady_clock::now();
            e.digests[role]=identity::chunk_root(*s.digest,length,leaves.subspan(offset,leafBytes));
            e.digestTiming.hashNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-begin).count();
            e.digestTiming.bytes+=length;
        }
        serialize_stages(s,e,"NR_GHOST_STAGE_DIGEST ");
        e.cpuNs[GraphicsHash]=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count();
    }
    static size_t serialize_stages(State& s,Envelope& e,const char* prefix) {
        qa::GhostDigestRecord record;record.object();
        record.text("identity_digest_algorithm",identity::chunkAlgorithm);
        record.text("digest_mechanism","gpu_exact_chunks_cpu_ordered_root");record.number("schema",identity::chunkSchema);
        record.number("serial",e.serial);record.number("tick",e.row.tick);record.number("clock",e.row.clock);
        record.number("interval",e.row.interval);record.boolean("preparation",e.preparing);record.boolean("reset",e.reset);
        record.boolean("source_linear",s.sourceLinear);record.boolean("hdr_output",s.hdrOutput);
        record.boolean("model_evaluated",e.evaluated&&e.composed&&e.submitted);
        record.boolean("isolated_fixture",e.fixtureKey.has_value());
        digest_job_key(record,"fixture_key",e.fixtureKey);
        digest_job_key(record,"fixture_reference_key",e.fixtureReference);
        record.number("model_mask",e.stages);record.number("graphics_mask",e.graphicsMask);
        record.number("settings_revision",e.settingsRevision);record.number("resource_generation",e.token->generation);
        digest_frame_key(record,"current_key",e.token->frame);digest_job_key(record,"model_key",e.key);
        digest_job_key(record,"selected_key",e.row.job);digest_job_key(record,"reference_key",e.reference);
        if(e.row.selectedFrame)digest_frame_key(record,"selected_frame",*e.row.selectedFrame);
        record.array("stages");
        for(size_t role=0;role<roleCount;++role) {
            record.array_object();const bool exists=present(e,role);
            record.text("role",identity::roles[role]);record.boolean("present",exists);
            record.number("bytes",exists?e.imageRows[role].logical_bytes():0);
            const auto& footprint=e.footprints[role].Footprint;
            record.key("domain");record.object();
            record.number("width",footprint.Width);record.number("height",footprint.Height);
            record.number("row_bytes",e.rowBytes[role]);record.number("rows",e.rows[role]);
            record.number("storage_dxgi",footprint.Format);
            const char* transfer=role==1||role==3?"model_proxy":role==5||role==6?"srgb_analysis":
                role==7?"working_pixel_motion":role==8?"signed_1_32_native_flow":role==9?"native_cost":
                role==modelCount+Program?(s.hdrOutput?"scrgb_linear":"srgb"):(s.sourceLinear?"scrgb_linear":"srgb");
            record.text("transfer",transfer);record.text("primaries","rec709");
            record.text("alpha",role==7||role==8||role==9?"none":role==5||role==6?"opaque":
                role==1||role==3?"model_proxy":role==modelCount+Program?"composited":"source_preserved");
            record.number("reference_white_nits",role==modelCount+Program&&s.hdrOutput?300:80);
            record.number("oracle_reference_white_nits",s.oracleWhite);record.number("oracle_color",s.oracleColor);
            record.number("exposure",1);record.end_object();
            const FrameKey* bound=&e.token->frame;std::optional<JobKey> job;
            if(role<modelCount&&e.key) {
                job.emplace(*e.key);
                bound=role==model::Observation::ReferenceAnalysis&&e.reference?&e.reference->frame:&job->frame;
            }else if(role>=modelCount&&role!=modelCount+CurrentRaw&&e.selection&&e.selection->frame) {
                bound=&*e.selection->frame;if(e.row.job)job.emplace(*e.row.job);
            }
            digest_frame_key(record,"frame_key",*bound);digest_job_key(record,"job_key",job);
            if(exists)record.digest("digest",*e.digests[role]);
            record.end_object();
        }
        record.end_array();record.number("digest_bytes",e.digestTiming.bytes);
        record.number("digest_setup_ns",e.digestTiming.setupNs);record.number("digest_hash_ns",e.digestTiming.hashNs);
        record.number("digest_finish_ns",e.digestTiming.finishNs);record.end_object();
        qa::emit_ghost_digest(std::cout,prefix,record);
        return record.view().size();
    }
    static bool fence_ready(ID3D12Fence* fence,uint64_t value) {
        if(!fence||!value||value==UINT64_MAX)return false;
        const auto done=fence->GetCompletedValue();return done!=UINT64_MAX&&done>=value;
    }
    static void copy_graphic(State& s,Envelope& e,size_t role,ID3D11Texture2D* source) noexcept {
        if(!source){refuse(s,e,Refusal::Shape);return;}
        D3D11_TEXTURE2D_DESC a{},b{};source->GetDesc(&a);e.graphics[role]->GetDesc(&b);
        if(a.Width!=b.Width||a.Height!=b.Height||family(a.Format)!=family(b.Format)||
            a.MipLevels!=1||a.ArraySize!=1||a.SampleDesc.Count!=1) {refuse(s,e,Refusal::Shape);return;}
        e.sources[role]=source;s.immediate->CopyResource(e.graphics[role].Get(),source);e.graphicsMask|=1ull<<role;
    }
    static void enqueue_readback(State& s,Envelope& e) {
        const auto started=std::chrono::steady_clock::now();
        require(!e.copySubmitted&&!e.uncertain,"QA exact once readback submission");
        for(auto& source:e.sources)source.Reset();
        checked(e.allocator->Reset(),"QA batch allocator reuse");
        checked(e.commands->Reset(e.allocator.Get(),nullptr),"QA batch list reuse");
        for(size_t role=0;role<roleCount;++role)if(read_present(e,role)) {
            // The canonical role's immutable CPU image and leaves are copied
            // first. Reuse only that same credit-owned buffer for graphics.
            auto* resource=role<modelCount?e.images[role].Get():e.images[model::Observation::Canonical].Get();
            if(role>=modelCount) {
                qa::GhostTextureBytes local{e.footprints[role],e.rows[role],e.rowBytes[role]};
                local.footprint.Offset=0;
                require(qa::ghost_copy_texture_bytes(e.commands.Get(),e.opened[role-modelCount].Get(),resource,local),
                    "actual graphics texture-to-byte diagnostic copy");
            }
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
            e.commands->ResourceBarrier(1,&barrier);
            const auto length=e.rowBytes[role]*e.rows[role];
            const auto physical=static_cast<uint64_t>(e.rows[role]-1)*e.footprints[role].Footprint.RowPitch+e.rowBytes[role];
            e.commands->CopyBufferRegion(e.readback.Get(),e.footprints[role].Offset,resource,0,physical);
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            e.commands->ResourceBarrier(1,&barrier);
            const auto format=e.footprints[role].Footprint.Format;
            require(length<=UINT32_MAX&&e.rowBytes[role]<=UINT32_MAX,"actual diagnostic chunk uint32 addressing");
            s.digestLane->pipeline.record(e.commands.Get(),resource,s.leafScratch.Get(),static_cast<uint32_t>(length),
                static_cast<uint32_t>(e.rowBytes[role]),e.footprints[role].Footprint.RowPitch,0,
                format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R16G16_FLOAT);
            D3D12_RESOURCE_BARRIER scratch{};scratch.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            scratch.Transition={s.leafScratch.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
            e.commands->ResourceBarrier(1,&scratch);
            e.commands->CopyBufferRegion(e.leafReadback.Get(),e.leafOffsets[role],s.leafScratch.Get(),0,
                ((length+identity::chunkBytes-1)/identity::chunkBytes)*32+4);
            std::swap(scratch.Transition.StateBefore,scratch.Transition.StateAfter);e.commands->ResourceBarrier(1,&scratch);
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;e.commands->ResourceBarrier(1,&barrier);
        }
        checked(e.commands->Close(),"QA batch close");require(e.batchValue<UINT64_MAX-1,"QA real batch fence exhaustion");
        ++e.batchValue;ResetEvent(e.batchEvent.value);
        checked(e.batch->SetEventOnCompletion(e.batchValue,e.batchEvent.value),"pre-trigger QA batch event");
        ID3D12CommandList* commands[]{e.commands.Get()};e.uncertain=true;
        e.copyStarted=std::chrono::steady_clock::now();
        s.context->queue()->ExecuteCommandLists(1,commands);
        checked(s.context->queue()->Signal(e.batch.Get(),e.batchValue),"QA real batch signal");
        e.copySubmitted=true;
        e.readbackParts[0]=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count();
    }
    static void enqueue_ready(State& s) {
        const auto end=s.written.load(std::memory_order_acquire);
        for(auto index=s.read.load(std::memory_order_relaxed);index<end;++index) {
            auto& next=*s.pending[index%3];
            if(next.copySubmitted||next.uncertain||!next.finished.load(std::memory_order_acquire))continue;
            if(next.key&&(!next.returned||!next.returned->ready()))continue;
            if(next.completionFence&&!fence_ready(next.completionFence.Get(),next.completionValue))continue;
            if(!fence_ready(next.producer.Get(),next.producerValue))continue;
            try{enqueue_readback(s,next);}
            catch(...){next.uncertain=true;refuse(s,next,Refusal::Unknown);}
        }
    }
    static bool consume(State& s,Envelope& e) {
        const auto started=std::chrono::steady_clock::now();
        if(!e.copySubmitted) {
            if(WaitForSingleObject(e.modelEvent.value,30000)!=WAIT_OBJECT_0||
                !e.finished.load(std::memory_order_acquire))return false;
            if(e.key&&(!e.returned||!e.returned->wait_for(30000)))return false;
            if(e.completionFence&&!fence_ready(e.completionFence.Get(),e.completionValue))return false;
            if(e.uncertain||WaitForSingleObject(e.producerEvent.value,30000)!=WAIT_OBJECT_0||
                !fence_ready(e.producer.Get(),e.producerValue))return false;
            e.drainTiming[0]=std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-started).count();
            enqueue_readback(s,e);
        }
        enqueue_ready(s);
        const auto waitBegin=std::chrono::steady_clock::now();
        const auto lead=std::chrono::duration_cast<std::chrono::nanoseconds>(waitBegin-e.copyStarted).count();
        if(WaitForSingleObject(e.batchEvent.value,30000)!=WAIT_OBJECT_0||!fence_ready(e.batch.Get(),e.batchValue))return false;
        const auto mapBegin=std::chrono::steady_clock::now();
        e.readbackParts[1]=std::chrono::duration_cast<std::chrono::nanoseconds>(mapBegin-waitBegin).count();
        e.uncertain=false;void* mapped=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(e.readbackBytes)};
        checked(e.readback->Map(0,&range,&mapped),"QA detached batch map");
        e.mapOpen=true;
        for(size_t role=0;role<roleCount;++role)if(read_present(e,role)) {
            const auto bytes=static_cast<size_t>(e.rowBytes[role])*e.rows[role];e.mappedImageBytes+=bytes;
            const auto* source=static_cast<const uint8_t*>(mapped)+e.footprints[role].Offset;
            const auto pitch=e.footprints[role].Footprint.RowPitch;
            e.imageRows[role]={{source,static_cast<size_t>(e.rows[role]-1)*pitch+static_cast<size_t>(e.rowBytes[role])},
                static_cast<size_t>(e.rowBytes[role]),pitch,e.rows[role]};
            e.imageRows[role].validate();
            if(role==model::Observation::Cost)continue; // Borrow actual pitch, never allocate compact cost.
            if(pitch==e.rowBytes[role])e.packed[role]=e.imageRows[role].tight();
            else {
                e.copiedBytes+=bytes;
                for(UINT y=0;y<e.rows[role];++y)memcpy(e.compact[role].data()+static_cast<size_t>(y)*e.rowBytes[role],
                    source+static_cast<size_t>(y)*e.footprints[role].Footprint.RowPitch,static_cast<size_t>(e.rowBytes[role]));
                e.packed[role]=e.compact[role];
            }
        }
        e.readbackParts[2]=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-mapBegin).count();
        e.drainTiming[1]=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count();
        // Ready later envelopes own different prepared targets/readbacks.
        // Their real GPU copies overlap this CPU scan; CPU consumption stays FIFO.
        enqueue_ready(s);
        const auto cpuStarted=std::chrono::steady_clock::now();
        void* leafMap=nullptr;const D3D12_RANGE leafRange{0,static_cast<SIZE_T>(e.leafReadbackBytes)};
        checked(e.leafReadback->Map(0,&leafRange,&leafMap),"actual completed chunk leaf map");e.leafMapOpen=true;
        try{digest_stages(s,e,{static_cast<const uint8_t*>(leafMap),static_cast<size_t>(e.leafReadbackBytes)});}
        catch(const std::exception& error) {
            refuse(s,e,Refusal::Validation);
            std::osyncstream(std::cout)<<"NR_GHOST_DIGEST_FAILURE serial="<<e.serial<<" "<<error.what()<<std::endl;
        }
        const D3D12_RANGE noLeafWrite{0,0};e.leafReadback->Unmap(0,&noLeafWrite);e.leafMapOpen=false;
        try{s.consumer(e);}catch(...){refuse(s,e,Refusal::Validation);}
        e.consumerTotalNs=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-cpuStarted).count();
        // CPU readers finish before any mapped view or its evidence credit retires.
        for(auto& view:e.packed)view={};
        for(auto& view:e.imageRows)view={};
        const auto unmapBegin=std::chrono::steady_clock::now();
        const D3D12_RANGE empty{0,0};e.readback->Unmap(0,&empty);e.mapOpen=false;
        const auto unmapNs=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-unmapBegin).count();
        auto* service=obs_data_create();
        obs_data_set_int(service,"serial",static_cast<int64_t>(e.serial));
        obs_data_set_int(service,"ingress",static_cast<int64_t>(e.row.ingress));
        obs_data_set_int(service,"selected",static_cast<int64_t>(e.row.selected));
        obs_data_set_int(service,"kind",e.row.kind);
        obs_data_set_bool(service,"preparation",e.preparing);
        obs_data_set_bool(service,"has_selected_job",e.row.job.has_value());
        if(e.row.job) {
            obs_data_set_int(service,"selected_job_ingress",static_cast<int64_t>(e.row.job->frame.ingressSequence));
            obs_data_set_int(service,"selected_job_serial",static_cast<int64_t>(e.row.job->submissionSerial));
        }
        obs_data_set_bool(service,"model_evaluated",e.evaluated);obs_data_set_bool(service,"model_reset",e.reset);
        obs_data_set_bool(service,"restore_reference_valid",e.referenceValid);
        obs_data_set_int(service,"model_stage_mask",static_cast<int64_t>(e.stages));
        obs_data_set_int(service,"dependency_ns",e.drainTiming[0]);
        obs_data_set_int(service,"readback_exclusive_ns",e.drainTiming[1]-e.drainTiming[0]);
        obs_data_set_int(service,"readback_commands_submit_ns",e.readbackParts[0]);
        obs_data_set_int(service,"qa_fence_wait_ns",e.readbackParts[1]);
        obs_data_set_int(service,"readback_enqueue_lead_ns",lead);
        obs_data_set_int(service,"map_view_compact_copy_ns",e.readbackParts[2]);
        obs_data_set_int(service,"unmap_after_consumer_ns",unmapNs);
        obs_data_set_int(service,"actual_copied_bytes",static_cast<int64_t>(e.copiedBytes));
        obs_data_set_int(service,"actual_mapped_image_bytes",static_cast<int64_t>(e.mappedImageBytes));
        obs_data_set_bool(service,"maps_closed_before_release",!e.mapOpen&&!e.leafMapOpen);
        obs_data_set_int(service,"consumer_total_ns",e.consumerTotalNs);
        int64_t classified=0;
        constexpr std::array<const char*,CpuCount> names{"validity_ns","raw_admission_ns","alpha_ns",
            "identity_ns","all_stage_digest_ns","selected_edit_ns","views_ns","model_ns","guide_digest_receipt_ns",
            "guide_arithmetic_receipt_ns","pictures_ns","score_ns"};
        for(size_t at=0;at<CpuCount;++at){obs_data_set_int(service,names[at],e.cpuNs[at]);classified+=e.cpuNs[at];}
        obs_data_set_int(service,"consumer_other_ns",e.consumerTotalNs-classified);
        obs_data_set_string(service,"identity_digest_algorithm",identity::chunkAlgorithm.data());
        obs_data_set_int(service,"digest_bytes",static_cast<int64_t>(e.digestTiming.bytes));
        obs_data_set_int(service,"digest_setup_ns",e.digestTiming.setupNs);
        obs_data_set_int(service,"digest_hash_ns",e.digestTiming.hashNs);
        obs_data_set_int(service,"digest_finish_ns",e.digestTiming.finishNs);
        obs_data_set_int(service,"background_matches",static_cast<int64_t>(e.rawCounts.matches));
        obs_data_set_int(service,"background_mismatches",static_cast<int64_t>(e.rawCounts.mismatches));
        obs_data_set_int(service,"special_scalar_pixels",static_cast<int64_t>(e.rawCounts.special));
        const auto rawNs=e.cpuNs[RawAdmission];
        const auto creditStarted=e.creditStarted;
        const auto completedSerial=e.serial;
        const bool released=e.token&&s.credits->release(*e.token,{e.producerValue,e.producer->GetCompletedValue(),
            e.batchValue,e.batch->GetCompletedValue(),true,true,!e.mapOpen&&!e.leafMapOpen,true});
        // No envelope field is read after release: the graphics owner can reuse it immediately.
        const auto retiredAt=std::chrono::steady_clock::now();
        const auto creditAge=std::chrono::duration_cast<std::chrono::nanoseconds>(retiredAt-creditStarted).count();
        const auto total=std::chrono::duration_cast<std::chrono::nanoseconds>(
            retiredAt-started).count();
        obs_data_set_int(service,"entry_to_credit_release_ns",total);
        obs_data_set_int(service,"reserve_to_credit_release_ns",creditAge);
        obs_data_set_int(service,"remainder_without_admission_ns",total-rawNs);
        obs_data_set_bool(service,"credit_released",released);
        obs_data_set_int(service,"occupied_after_release",s.credits->occupied());
        emit("NR_GHOST_SERVICE ",service);
        if(released&&s.measurementRetired) {
            const auto callbackStarted=std::chrono::steady_clock::now();
            s.measurementRetired(completedSerial,creditAge,total,
                std::chrono::duration_cast<std::chrono::nanoseconds>(callbackStarted-started).count());
            const auto callbackEnded=std::chrono::steady_clock::now();
            auto* cycle=obs_data_create();
            obs_data_set_int(cycle,"serial",static_cast<int64_t>(completedSerial));
            obs_data_set_int(cycle,"postrelease_serialization_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(callbackStarted-retiredAt).count());
            obs_data_set_int(cycle,"callback_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(callbackEnded-callbackStarted).count());
            obs_data_set_int(cycle,"complete_drain_cycle_through_callback_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(callbackEnded-started).count());
            obs_data_set_int(cycle,"reserve_through_callback_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(callbackEnded-creditStarted).count());
            obs_data_set_bool(cycle,"credit_released",true);
            emit("NR_GHOST_SERVICE_CYCLE ",cycle);
        }
        return released;
    }
    static void drain(State* s) noexcept {
        for(;;) {
            s->available.acquire();
            const auto iterationStarted=std::chrono::steady_clock::now();
            const auto index=s->read.load(std::memory_order_relaxed);
            bool measured=false;
            if(index!=s->written.load(std::memory_order_acquire)) {
                auto& e=*s->pending[index%3];s->read.store(index+1,std::memory_order_release);
                try {if(!consume(*s,e)){e.uncertain=true;refuse(*s,e,Refusal::Unknown);}}
                catch(...){e.uncertain=true;refuse(*s,e,Refusal::Unknown);}
                measured=true;
            }
            const bool finished=s->closed.load(std::memory_order_acquire)&&s->read.load()==s->written.load(std::memory_order_acquire);
            if(measured&&s->measurementRetired) {
                const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-iterationStarted).count();
                if(s->measuredIterations++==0)s->firstIterationNs=elapsed;
                else {
                    s->subsequentMinimumNs=std::min(s->subsequentMinimumNs,elapsed);
                    s->subsequentMaximumNs=std::max(s->subsequentMaximumNs,elapsed);
                }
            }
            if(finished)break;
        }
        s->exited.store(true,std::memory_order_release);SetEvent(s->exit.value);
    }
public:
    SnapshotPool(std::shared_ptr<GhostDigestLane> lane,ID3D11Device* renderer,DXGI_FORMAT sourceFormat,
        DXGI_FORMAT programFormat,const D3D12_RESOURCE_DESC& vectors,const D3D12_RESOURCE_DESC& cost,
        uint64_t baseIngress,const std::array<std::shared_ptr<model::QaSignal>,3>& dispatchSignals,
        std::function<void(Envelope&)> consumer,bool sourceLinear,bool hdrOutput,uint32_t oracleColor,
        double oracleWhite,identity::Sha256& root,
        std::function<void(uint64_t,int64_t,int64_t,int64_t)> measurementRetired={}):state(nullptr) {
        auto s=std::make_unique<State>();s->digestLane=std::move(lane);
        require(s->digestLane!=nullptr,"prepared existing digest lane");
        s->context=s->digestLane->context;s->digest=&root;s->consumer=std::move(consumer);
        s->measurementRetired=std::move(measurementRetired);
        s->sourceLinear=sourceLinear;s->hdrOutput=hdrOutput;s->oracleColor=oracleColor;s->oracleWhite=oracleWhite;
        s->baseIngress=baseIngress;require(s->context&&renderer&&s->consumer,"prepared QA pool inputs");
        require(s->graphicsThread==s->context->graphics_thread(),"QA control graphics owner");
        checked(renderer->QueryInterface(IID_PPV_ARGS(&s->renderer)),"QA renderer fence interface");
        ComPtr<ID3D11DeviceContext> immediate;renderer->GetImmediateContext(&immediate);
        checked(immediate.As(&s->immediate),"QA producer graphics interface");
        std::array<D3D12_RESOURCE_DESC,roleCount> desc{};
        for(size_t role=0;role<5;++role)desc[role]=texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
        desc[5]=desc[6]=texture(DXGI_FORMAT_B8G8R8A8_UNORM);desc[7]=texture(DXGI_FORMAT_R16G16_FLOAT);
        desc[8]=vectors;desc[9]=cost;desc[10]=texture(sourceFormat);
        for(size_t role=modelCount;role<roleCount;++role)desc[role]=texture(role==modelCount+Program?programFormat:sourceFormat);
        auto* device=s->context->device();
        D3D12_HEAP_PROPERTIES scratchHeap{};scratchHeap.Type=D3D12_HEAP_TYPE_DEFAULT;
        const auto scratch=buffer(((16588800ull+identity::chunkBytes-1)/identity::chunkBytes)*32+4,true);
        checked(device->CreateCommittedResource(&scratchHeap,D3D12_HEAP_FLAG_NONE,&scratch,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s->leafScratch)),"one ordered chunk scratch");
        s->bytes.gpu+=device->GetResourceAllocationInfo(0,1,&scratch).SizeInBytes;
        for(size_t index=0;index<s->envelopes.size();++index) {
            auto& e=s->envelopes[index];e.returned=dispatchSignals[index];
            require(e.returned!=nullptr,"pre-created module-owned QA dispatch lease");
            uint64_t end=0,leafEnd=0;
            for(size_t role=0;role<roleCount;++role) {
                auto d=desc[role];d.Flags=D3D12_RESOURCE_FLAG_NONE;
                const auto local=qa::ghost_texture_bytes(device,d);
                UINT64 bytes=0;device->GetCopyableFootprints(&d,0,1,(end+511)&~511ull,&e.footprints[role],
                    &e.rows[role],&e.rowBytes[role],&bytes);
                require(e.rows[role]&&e.rowBytes[role],"QA full-stage footprint");
                end=e.footprints[role].Offset+local.physical_bytes();
                e.leafOffsets[role]=leafEnd;
                leafEnd+=((local.logical_bytes()+identity::chunkBytes-1)/identity::chunkBytes)*32+4;
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
                if(role<modelCount) {
                    e.imageFootprints[role]=local.footprint;d=buffer(local.physical_bytes());
                    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
                        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&e.images[role])),"prepared model byte evidence target");
                }
                else {
                    const auto g=role-modelCount;D3D11_TEXTURE2D_DESC allocation{};
                    allocation.Width=static_cast<UINT>(d.Width);allocation.Height=d.Height;
                    allocation.MipLevels=allocation.ArraySize=allocation.SampleDesc.Count=1;allocation.Format=d.Format;
                    allocation.Usage=D3D11_USAGE_DEFAULT;allocation.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                    allocation.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                    checked(renderer->CreateTexture2D(&allocation,nullptr,&e.graphics[g]),"prepared graphic evidence target");
                    ComPtr<IDXGIResource1> shared;checked(e.graphics[g].As(&shared),"QA target NT share");
                    HANDLE handle=nullptr;checked(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|
                        DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"QA target NT handle");
                    const auto hr=interop::open_shared_resource(device,handle,&e.opened[g]);CloseHandle(handle);
                    checked(hr,"QA target private open");
                    d=e.opened[g]->GetDesc(); // Actual shared flags, never count the alias twice.
                }
                const auto allocation=device->GetResourceAllocationInfo(0,1,&d);
                require(allocation.SizeInBytes!=UINT64_MAX,"actual QA target allocation size");s->bytes.gpu+=allocation.SizeInBytes;
                if(role!=model::Observation::Cost&&e.footprints[role].Footprint.RowPitch!=e.rowBytes[role]) {
                    e.compact[role].resize(static_cast<size_t>(e.rowBytes[role])*e.rows[role]);
                    s->bytes.packed+=e.compact[role].size();
                }
            }
            e.readbackBytes=end;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=end;buffer.Height=1;buffer.DepthOrArraySize=1;
            buffer.MipLevels=1;buffer.SampleDesc.Count=1;
            buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&e.readback)),"prepared full-stage readback");
            s->bytes.readback+=device->GetResourceAllocationInfo(0,1,&buffer).SizeInBytes;
            e.leafReadbackBytes=leafEnd;const auto leafBuffer=SnapshotPool::buffer(leafEnd);
            checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&leafBuffer,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&e.leafReadback)),"prepared exact ordered leaf readback");
            s->bytes.leafReadback+=device->GetResourceAllocationInfo(0,1,&leafBuffer).SizeInBytes;
            checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&e.allocator)),"QA batch allocator");
            checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,e.allocator.Get(),nullptr,
                IID_PPV_ARGS(&e.commands)),"QA batch list");checked(e.commands->Close(),"QA prepared list close");
            checked(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&e.producer)),"real QA producer fence");
            checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&e.batch)),"real QA batch fence");
            HANDLE handle=nullptr;checked(device->CreateSharedHandle(e.producer.Get(),nullptr,GENERIC_ALL,nullptr,&handle),
                "QA producer share");const auto hr=s->renderer->OpenSharedFence(handle,IID_PPV_ARGS(&e.producer11));
            CloseHandle(handle);checked(hr,"QA producer renderer open");
        }
        s->drain=std::thread(drain,s.get());state=s.release();
    }
    ~SnapshotPool(){if(stop())delete state;} // Unresolved entire state/context/thread remains retained.
    model::Observation* reserve(const IngressSample& input,const pipeline::TickPlan& plan) noexcept override {
        auto& s=*state;if(s.closed.load(std::memory_order_acquire)||s.failure.load(std::memory_order_acquire)!=Refusal::None)return nullptr;
        auto token=s.credits->reserve(input.key,input.resourceGeneration);
        if(!token){s.failure.store(Refusal::Saturation);return nullptr;}
        auto& e=s.envelopes[token->index];e.token.emplace(*token);e.selection.emplace(plan.selection);
        e.creditStarted=std::chrono::steady_clock::now();e.copySubmitted=false;
        std::destroy_at(&e.row);std::construct_at(&e.row);e.row.tick=input.canonicalTick;
        e.row.ingress=input.key.ingressSequence;e.row.selected=plan.selection.frame?plan.selection.frame->ingressSequence:0;
        e.row.kind=static_cast<unsigned>(plan.selection.kind);
        if(plan.selection.job)e.row.job.emplace(*plan.selection.job);
        if(plan.selection.frame)e.row.selectedFrame.emplace(*plan.selection.frame);
        e.row.settingsRevision=input.settings.revision.value;e.row.generation=input.resourceGeneration;
        e.row.sourceTick=static_cast<unsigned>(input.key.ingressSequence-s.baseIngress-1);
        e.key.reset();e.reference.reset();if(plan.admission)e.key.emplace(plan.admission->job.key);
        e.fixtureKey.reset();e.fixtureReference.reset();
        e.completionFence.Reset();e.completionValue=0;e.serial=token->serial;e.stages=0;e.grid=e.code=e.invalid=0;e.module=0;
        e.created=e.evaluated=e.composed=e.submitted=e.reset=e.referenceValid=false;e.refused.store(false);e.finished.store(false);
        e.chronology.fill(0);e.timing.fill(0);e.graphicsMask=0;e.settingsRevision=input.settings.revision.value;
        e.queued=e.uncertain=e.sameViews=false;e.preparing=!s.episode;
        e.refusal=Refusal::None;e.producerValue=token->serial;
        e.cpuNs.fill(0);e.rawCounts={};e.consumerTotalNs=0;
        for(auto& digest:e.digests)digest.reset();e.digestTiming={};
        e.drainTiming.fill(0);
        e.readbackParts.fill(0);e.copiedBytes=e.mappedImageBytes=0;e.mapOpen=e.leafMapOpen=false;
        for(auto& view:e.packed)view={};
        for(auto& view:e.imageRows)view={};
        ResetEvent(e.modelEvent.value);ResetEvent(e.producerEvent.value);
        if(FAILED(e.producer->SetEventOnCompletion(e.producerValue,e.producerEvent.value))) {
            e.uncertain=true;refuse(s,e,Refusal::Signal);
        }
        return &e;
    }
    void capture(model::Observation* observation,ID3D11Texture2D* current,ID3D11Texture2D* raw,
        ID3D11Texture2D* selected) noexcept override {
        if(!observation)return;auto& e=*static_cast<Envelope*>(observation);
        if(e.preparing)return; // Only real worker-owned D/header stages seed the predecessor witness.
        copy_graphic(*state,e,CurrentRaw,current);copy_graphic(*state,e,SelectedRaw,raw);copy_graphic(*state,e,Selected,selected);
    }
    void submit(model::Observation* observation,ID3D11Texture2D* program,ID3D11Texture2D* preview,
        ID3D11Texture2D* projector,uint64_t clock,uint64_t interval,bool sameViews) noexcept {
        if(!observation)return;auto& s=*state;auto& e=*static_cast<Envelope*>(observation);
        if(e.queued)return;e.row.clock=clock;e.row.interval=interval;e.sameViews=sameViews;
        if(!e.preparing) {
            copy_graphic(s,e,Program,program);copy_graphic(s,e,Preview,preview);copy_graphic(s,e,Projector,projector);
        }
        if(FAILED(s.immediate->Signal(e.producer11.Get(),e.producerValue))) {
            e.uncertain=true;refuse(s,e,Refusal::Signal);
        }
        s.immediate->Flush();e.queued=true;const auto index=s.written.load(std::memory_order_relaxed);
        s.pending[index%3]=&e;s.written.store(index+1,std::memory_order_release);s.available.release();
    }
    void close_admission() noexcept {
        if(!state->closed.exchange(true,std::memory_order_acq_rel))state->available.release();
    }
    bool current_capture(const IngressSample& input,ID3D11Texture2D* current,ID3D11Texture2D* output,
        ID3D11Texture2D* program,uint64_t clock,uint64_t interval,bool configurationEnabled,bool filterEnabled) {
        // OFF has a current FrameKey and no model JobKey. This envelope observes the real disabled OBS path;
        // the TickPlan supplies credit metadata only and does not manufacture a model/scheduler result.
        const pipeline::TickPlan plan{TickAction::CaptureOnce,
            Selection{input.canonicalTick,OutputKind::CurrentSource,Status::Disabled,Reason::None,input.key,{},false},
            Reason::Cancelled,{}};
        auto* observation=reserve(input,plan);
        if(!observation)return false;
        auto& e=*static_cast<Envelope*>(observation);
        e.offConfigurationEnabled=configurationEnabled;e.offFilterEnabled=filterEnabled;
        capture(&e,current,current,output);
        e.finished.store(true,std::memory_order_release);SetEvent(e.modelEvent.value);
        submit(&e,program,current,output,clock,interval,true);
        return true;
    }
    bool measurement_capture(const IngressSample& input,const pipeline::TickPlan& plan,
        const JobKey& fixtureKey,const std::optional<JobKey>& fixtureReference,
        const std::array<ID3D12Resource*,modelCount>& fixtureSources,
        ID3D11Texture2D* current,ID3D11Texture2D* selected,
        ID3D11Texture2D* program,uint64_t clock,uint64_t interval,HANDLE completionEvent) {
        auto* observation=reserve(input,plan);
        if(!observation)return false;auto& e=*static_cast<Envelope*>(observation);auto& s=*state;
        e.fixtureKey.emplace(fixtureKey);if(fixtureReference)e.fixtureReference.emplace(*fixtureReference);e.grid=1;
        capture(&e,current,selected,selected);
        checked(e.allocator->Reset(),"fixture completed allocator reuse");
        checked(e.commands->Reset(e.allocator.Get(),nullptr),"fixture completed command reuse");
        model::Observation fixtureCopy;
        fixtureCopy.key.emplace(fixtureKey);fixtureCopy.images=e.images;fixtureCopy.imageFootprints=e.imageFootprints;
        require(model::copy_observation(e.commands.Get(),fixtureCopy,fixtureKey,fixtureSources),"actual fixture model-role copy");
        e.stages=fixtureCopy.stages;
        checked(e.commands->Close(),"fixture copy command close");require(e.batchValue<UINT64_MAX-1,"fixture copy fence exhaustion");
        ++e.batchValue;e.completionFence=e.batch;e.completionValue=e.batchValue;
        require(ResetEvent(completionEvent)!=FALSE,"fixture completion event reset before subscription");
        checked(e.batch->SetEventOnCompletion(e.batchValue,completionEvent),"fixture copy event before trigger");
        ID3D12CommandList* lists[]{e.commands.Get()};e.uncertain=true;s.context->queue()->ExecuteCommandLists(1,lists);
        checked(s.context->queue()->Signal(e.batch.Get(),e.batchValue),"fixture copy real signal");
        require(WaitForSingleObject(completionEvent,30000)==WAIT_OBJECT_0&&fence_ready(e.batch.Get(),e.batchValue),
            "fixture copy actual completion");e.uncertain=false;
        // This is actual fixture-copy completion, not NR evaluation or g1.
        e.finished.store(true,std::memory_order_release);SetEvent(e.modelEvent.value);
        submit(&e,program,selected,selected,clock,interval,true);return true;
    }
    bool stop() noexcept {
        if(!state->closed.load(std::memory_order_acquire)||WaitForSingleObject(state->exit.value,30000)!=WAIT_OBJECT_0)return false;
        if(state->drain.joinable())state->drain.join();return state->credits->occupied()==0;
    }
    Bytes bytes() const{return state->bytes;}
    int64_t complete_iteration_maximum() const {
        return std::max(state->firstIterationNs,state->subsequentMaximumNs);
    }
    void report_complete_iterations(obs_data_t* result) const {
        obs_data_set_int(result,"whole_drain_iterations",static_cast<int64_t>(state->measuredIterations));
        obs_data_set_int(result,"first_whole_drain_iteration_through_final_emission_ns",state->firstIterationNs);
        obs_data_set_int(result,"subsequent_whole_drain_iteration_min_ns",
            state->measuredIterations>1?state->subsequentMinimumNs:0);
        obs_data_set_int(result,"subsequent_whole_drain_iteration_max_ns",state->subsequentMaximumNs);
        obs_data_set_bool(result,"service_cycle_final_emission_included",true);
        obs_data_set_string(result,"whole_drain_timing_scope",
            "After semaphore acquire through final SERVICE_CYCLE emission and close decision; excludes scalar counter bookkeeping and this post-join lane summary");
    }
    static constexpr size_t added_metadata_capacity() {
        // Count all added fixed owners and bounded serialization, with no
        // credit for removed batch state or the previous dynamic serializer.
        return qa::ghost_digest_serializer_capacity+sizeof(GhostDigestLane)+sizeof(GhostCodecDigest)+
            2*sizeof(std::shared_ptr<GhostDigestLane>)+sizeof(ComPtr<ID3D12Resource>)+sizeof(identity::Sha256*)+
            sizeof(State::measurementRetired)+2*sizeof(std::shared_ptr<qa::GhostCredits>)+64+
            sizeof(State::measuredIterations)+sizeof(State::firstIterationNs)+
            sizeof(State::subsequentMinimumNs)+sizeof(State::subsequentMaximumNs)+
            3*(sizeof(Envelope::imageFootprints)+sizeof(Envelope::imageRows)+sizeof(Envelope::leafOffsets)+
                sizeof(Envelope::leafReadback)+sizeof(Envelope::leafReadbackBytes)+sizeof(Envelope::leafMapOpen)+
                sizeof(Envelope::fixtureKey)+sizeof(Envelope::fixtureReference)+64)+
            sizeof(identity::Digest)+sizeof(identity::Timing)+sizeof(std::array<uint8_t,25>)+
            sizeof(std::array<uint8_t,21>)+sizeof(std::array<std::span<const uint8_t>,3>)+
            sizeof(std::function<void(const char*)>)+1024;
    }
    static void metadata_proof() {
        State fixture;auto& e=fixture.envelopes.front();
        constexpr uint64_t maximum=UINT64_MAX;
        const FrameKey frame{maximum,maximum,maximum};
        const JobKey job{frame,maximum,maximum,maximum};
        e.token.emplace(qa::GhostCredits::Token{0,maximum,frame,maximum});
        e.key.emplace(job);e.reference.emplace(job);e.row.job.emplace(job);e.row.selectedFrame.emplace(frame);
        e.fixtureKey.emplace(job);e.fixtureReference.emplace(job);
        e.selection.emplace(Selection{maximum,OutputKind::DelayedNr,Status::Applied,Reason::None,frame,job,false});
        e.serial=e.settingsRevision=e.row.tick=e.row.clock=e.row.interval=maximum;
        e.stages=(1ull<<modelCount)-1;e.graphicsMask=(1ull<<GraphicsCount)-1;
        fixture.sourceLinear=fixture.hdrOutput=true;fixture.oracleWhite=10000;fixture.oracleColor=3;
        e.digestTiming.bytes=maximum;
        for(size_t role=0;role<roleCount;++role) {
            auto& f=e.footprints[role].Footprint;f.Width=f.Height=UINT32_MAX;f.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            e.rows[role]=UINT32_MAX;e.rowBytes[role]=UINT32_MAX;
            e.imageRows[role].rowBytes=UINT32_MAX;e.imageRows[role].rows=UINT32_MAX;
            identity::Digest digest{};digest.fill(0xff);e.digests[role]=digest;
        }
        // Metadata-only worst-width fixture, not an image or shader receipt.
        const auto jsonBytes=serialize_stages(fixture,e,"NR_GHOST_METADATA_SERIALIZER_FIXTURE ");
        static_assert(added_metadata_capacity()<=109568,"registered HDR root/serializer metadata headroom");
        std::printf("PRODUCTION_STORAGE_METADATA serializer_fixed_capacity=%zu total_added_metadata_upper=%zu envelope_size=%zu state_size=%zu codec_owner_size=%zu actual_worst_width_record_bytes=%zu root_provider_count_added=0 compact_cost_capacity=0 GPU_calls=0 physical_acceptance=false\n",
            qa::ghost_digest_serializer_capacity,added_metadata_capacity(),sizeof(Envelope),sizeof(State),
            sizeof(GhostCodecDigest),jsonBytes);
    }
    int64_t digest_preparation_ns() const {return state->digest->preparation_ns();}
    void episode_base(uint64_t ingress) noexcept {state->baseIngress=ingress;state->episode=true;}
    Refusal failure() const{return state->failure.load(std::memory_order_acquire);}
    unsigned occupied() const{return state->credits->occupied();}
    std::shared_ptr<qa::GhostCredits> credit_ledger() const{return state->credits;}
};
struct Harness {
    const std::filesystem::path root,directory;
    const ghost::Fixture corpus;
    const bool sourceLinear,hdrOutput,deterministic,calibrating,baseline;
    const nr::qa::StreamRate rate;
    const Metrics* frozen;
    Metrics measured;
    Event acted,complete,recordStart,consumerDone,preparable,armReady,offDone;
    std::atomic<bool> offRequested{false};
    ghost::PhysicalLane offProof;
    std::atomic<Action> action{Action::None};
    std::atomic<bool> observing{true},pictureArmed{false};
    unsigned warmDiagnostics=0;
    bool bound=false,closed=false,retained=false;
    std::atomic<bool> failed{false};
    std::atomic<bool> rawFailed{false};
    std::atomic<uint32_t> producerFailure{0};
    std::string failure;
    obs_source_t *source=nullptr,*filter=nullptr,*unrelated=nullptr;
    obs_scene_t* scene=nullptr;
    std::vector<FrameRow> rows;
    std::vector<nr::qa::DecodedFrame> preencode;
    identity::Sha256 codecDigest;
    std::shared_ptr<GhostDigestLane> digestLane;
    std::unique_ptr<GhostCodecDigest> codec;
    std::shared_ptr<qa::GhostCredits> codecCredits;
    bool serviceMeasurement=false; // Private workload driver; never an NR evaluation/quality claim.
    bool fixturePoolRetired=false;
    std::function<void(const char*)> serviceSample;
    std::mutex rawMutex;
    std::shared_ptr<model::Job> stoppingJob;
    std::shared_ptr<model::QaSignal> exited;
    ComPtr<ID3D11Fence> stoppingFence;
    std::array<unsigned,512> streak{},longest{};
    std::optional<ghost::Image> previousRaw;
    std::optional<unsigned> previousRawTick;
    std::optional<image::Image> sourcePicture,selectedPicture,programPicture;
    unsigned pictureContent=0;
    std::ofstream scores;
    std::shared_ptr<SnapshotPool> snapshots;
    std::array<gs_texrender_t*,2> views{};
    std::atomic<bool> armed{false};
    uint64_t baseIngress=0,lastPreparationTick=0;
    unsigned preparedConsecutive=0;
    std::optional<JobKey> priorEditKey;
    std::array<ghost::Rgb,1536> priorDelta{};
    static constexpr std::array<model::Observation::Stage,6> headerStages{
        model::Observation::Canonical,model::Observation::Encoded,model::Observation::Keep,
        model::Observation::Foreign,model::Observation::Resolved,model::Observation::Completed};
    std::array<std::array<uint64_t,32>,headerStages.size()> priorHeader{};
    bool priorReferenceValid=false;
    uint64_t selectedEditSamples=0,staticPositiveSamples=0,movingPositiveSamples=0;
    const ghost::FreezePolicy* policy=nullptr;
    std::optional<gb::Tally> tally; // Exactly one independently measured baseline rung per Harness.
    bool rawOutput=false;
    std::optional<IngressSample> offInput;
    uint64_t offRevision=0;
    std::array<ghost::PhysicalControl,9> physicalControls{};
    uint64_t physicalAnchors=0;
    Harness(const std::filesystem::path& r,const std::filesystem::path& d,uint32_t seed,ghost::FixtureColor color,
        bool linearSource,bool copy,bool calibration,const Metrics* thresholds,nr::qa::StreamRate streamRate,const ghost::FreezePolicy* freezePolicy=nullptr,
        bool baselineLane=false):
        root(r),directory(d),corpus(ghost::fixture(seed,color)),sourceLinear(linearSource),
        hdrOutput(color==ghost::FixtureColor::Hdr709||color==ghost::FixtureColor::Hdr2020),
        deterministic(copy),calibrating(calibration),baseline(baselineLane),rate(streamRate),frozen(thresholds),policy(freezePolicy) {
        require(rate.valid()&&rate.numerator!=1,"actual ghost production rational cadence");
        rows.reserve(258);preencode.reserve(40);
        measured.formatBound=declared_bound(corpus,sourceLinear);
        scores.open(directory/"templates.tsv",std::ios::binary);
        require(scores.good(),"bounded scalar score artifact");
        scores<<"tick\tprior\tregion\tpixels\tdenominator\tbound\tincremental\traw\ttotal\tzero\tpass\tconsecutive\tlongest\n";
    }
    ~Harness() {
        for(auto* view:views)gs_texrender_destroy(view);
        if(scene)obs_scene_release(scene);
        if(source&&filter)obs_source_filter_remove(source,filter);
        obs_source_release(filter);obs_source_release(source);obs_source_release(unrelated);
    }
    void execute(Action next) {
        ResetEvent(acted.value);action.store(next,std::memory_order_release);acted.wait();
        if(next==Action::Bind)require(!failed,failure.c_str());
    }
    void create() {
        nr_register_bootstrap_fixture();
        auto* data=obs_data_create();obs_data_set_bool(data,"hdr",sourceLinear);
        obs_data_set_bool(data,"ghost_prepare",true);
        obs_data_set_int(data,"ghost_seed",corpus.seed);obs_data_set_int(data,"ghost_color",static_cast<int64_t>(corpus.color));
        source=obs_source_create_private("nr_task12_fixture","Task26 frozen original",data);
        obs_data_set_bool(data,"unrelated",true);obs_data_set_bool(data,"ghost_prepare",false);obs_data_set_bool(data,"hdr",hdrOutput);
        unrelated=obs_source_create_private("nr_task12_fixture","Task26 unrelated source",data);
        obs_data_release(data);
        data=obs_data_create();obs_data_set_string(data,"nr_shader_root",(root/"bin/64bit").generic_string().c_str());
        obs_data_set_string(data,"nr_data_root",(root/"profile/ngx-data").generic_string().c_str());
        obs_data_set_bool(data,"nr_qa_deterministic",deterministic);obs_data_set_bool(data,"nr_qa_observe",false);
        obs_data_set_bool(data,"nr_qa_prepare",true);
        filter=obs_source_create_private("susemi_nr_filter","Task26 production native filter",data);obs_data_release(data);
        require(source&&filter&&unrelated,"actual frozen source/native graph");
        obs_source_filter_add(source,filter);
        scene=obs_scene_create_private("Task26 actual program");require(scene&&obs_scene_add(scene,source),"actual filtered scene");
        auto* item=obs_scene_add(scene,unrelated);require(item!=nullptr,"actual unrelated source");
        vec2 position{1792,952};obs_sceneitem_set_pos(item,&position);
    }
    void stop() {
        calldata_t query;calldata_init(&query);
        std::shared_ptr<interop::Bridge> bridge;
        calldata_set_ptr(&query,"retain_job",&stoppingJob);calldata_set_ptr(&query,"retain_bridge",&bridge);
        if(!exited)calldata_set_ptr(&query,"retain_exit_signal",&exited);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"pre-stop actual subscriptions");
        calldata_free(&query);
        if(stoppingJob&&stoppingJob->ready.load(std::memory_order_acquire)) {
            worker_timing(stoppingJob);
            stoppingJob.reset();
        }
        if(bridge&&!stoppingFence) {
            stoppingFence=bridge->consumer_fence();uint64_t value=0;
            for(size_t i=0;i<3;++i)value=std::max(value,bridge->ownership(i).consumer);
            checked(stoppingFence->SetEventOnCompletion(value,consumerDone.value),"pre-stop consumer event");
        }else if(!bridge)SetEvent(consumerDone.value);
        if(bridge)for(size_t slot=0;slot<3;++slot) {
            const auto physical=bridge->ownership(slot);auto* row=obs_data_create();
            obs_data_set_int(row,"slot",static_cast<int64_t>(slot));
            obs_data_set_int(row,"producer",static_cast<int64_t>(physical.producer));
            obs_data_set_int(row,"producer_done",static_cast<int64_t>(physical.producerDone));
            obs_data_set_int(row,"worker",static_cast<int64_t>(physical.worker));
            obs_data_set_int(row,"worker_done",static_cast<int64_t>(physical.workerDone));
            obs_data_set_int(row,"consumer",static_cast<int64_t>(physical.consumer));
            obs_data_set_int(row,"consumer_done",static_cast<int64_t>(physical.consumerDone));
            obs_data_set_bool(row,"consumer_open",physical.consumerOpen);
            obs_data_set_bool(row,"reusable",interop::reusable(physical));emit("NR_GHOST_PHYSICAL_STOP ",row);
        }
        // The local Bridge lease ends before the cooperative stop entry point: a lease held
        // across it keeps the very owner under test alive. The retained fence holds its own
        // reference, so the pre-stop subscriptions and diagnostic rows above survive it.
        nr::qa::enter_stop_released(bridge,[&] {
            calldata_init(&query);require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_stop",&query),"cooperative native stop");
            closed=calldata_bool(&query,"closed");calldata_free(&query);
        });
    }
    void release() {
        require(closed,"observations/inputs must retire before graph release");
        for(auto& view:views){gs_texrender_destroy(view);view=nullptr;}
        obs_set_output_source(0,nullptr);bound=false;obs_source_filter_remove(source,filter);
        obs_scene_release(scene);scene=nullptr;obs_source_release(filter);filter=nullptr;
        obs_source_release(source);source=nullptr;obs_source_release(unrelated);unrelated=nullptr;
        stoppingJob.reset();stoppingFence.Reset();previousRaw.reset();exited.reset();
    }
    void score(unsigned tick,ghost::PixelView raw,ghost::PixelView selected) {
        if(!frozen&&!serviceMeasurement)return; // Physical candidate still requires frozen RAW/SDK calibration.
        const auto format=frozen?frozen->formatBound:measured.formatBound;
        const auto rawMaximum=frozen?frozen->rawMaximum:measured.rawMaximum;
        const ghost::Calibration calibration{format,rawMaximum,std::max(format,rawMaximum)};
        const auto report=ghost::temporal(corpus,tick,raw,selected,calibration);
        for(const auto& s:report.templates) {
            const auto key=s.priorTick*2+s.region;
            streak[key]=s.pass?0:streak[key]+1;longest[key]=std::max(longest[key],streak[key]);
            scores<<std::setprecision(17)<<tick<<'\t'<<s.priorTick<<'\t'<<s.region<<'\t'<<s.pixels<<'\t'
                <<s.denominator<<'\t'<<s.bound<<'\t'<<s.incremental<<'\t'<<s.raw<<'\t'<<s.total<<'\t'
                <<s.zeroContrast<<'\t'<<s.pass<<'\t'<<streak[key]<<'\t'<<longest[key]<<'\n';
        }
        require(scores.good(),"all template/region score receipt");
        if(deterministic&&!baseline)require(report.pass,"physical deterministic transport added temporal trail");
        if(deterministic&&!baseline) {
            for(const auto& region:std::array<Rect,3>{{{0,8,192,88},{256,128,32,32},{0,79,1920,3}}})
                for(unsigned y=region.y;y<region.y+region.height;++y)for(unsigned x=region.x;x<region.x+region.width;++x)
                    for(const auto next:std::array<std::pair<unsigned,unsigned>,2>{{{x+1,y},{x,y+1}}}) {
                        if(next.first>=region.x+region.width||next.second>=region.y+region.height)continue;
                        const auto a=ghost::clean_pixel(corpus,corpus.schedule[tick],x,y);
                        const auto b=ghost::clean_pixel(corpus,corpus.schedule[tick],next.first,next.second);
                        const auto c=selected.read(selected.pixels,x,y),d=selected.read(selected.pixels,next.first,next.second);
                        const std::array<double,3> before{a.r-b.r,a.g-b.g,a.b-b.b},after{c.r-d.r,c.g-d.g,c.b-d.b};
                        for(size_t channel=0;channel<3;++channel)
                            require(before[channel]==0||std::abs(before[channel])-
                                (before[channel]>0?after[channel]:-after[channel])<=2*calibration.epsilon,
                                "physical raw transport lost prescribed signed1pixel contrast");
                    }
        }
        if(baseline&&tally) {
            // Tallied, never thrown: the binary rung is judged after the lane, unprotected NR may legitimately fail.
            tally->temporal(tick,report);
            double rawLoss=0,selectedLoss=0;
            const auto edgeChecks=baseline_edges(corpus,tick,raw,selected,rawLoss,selectedLoss);
            tally->edge(tick,selectedLoss,edgeChecks);
        }
        auto* row=obs_data_create();obs_data_set_int(row,"tick",tick);
        obs_data_set_double(row,"maximum_incremental",report.maximum);obs_data_set_double(row,"maximum_raw",report.maximumRaw);
        obs_data_set_double(row,"maximum_total",report.maximumTotal);obs_data_set_bool(row,"pass",report.pass);
        obs_data_set_bool(row,"diagnostic_unprotected_nr",!deterministic);
        emit(serviceMeasurement?"NR_GHOST_MEASUREMENT_ORACLE ":"NR_GHOST_ORACLE ",row);
    }
    void guide(const std::shared_ptr<model::Job>& job,const std::shared_ptr<interop::Context>& context);
    void guide_bytes(const JobKey&,const std::optional<JobKey>&,uint32_t grid,bool reset,
        std::span<const uint8_t> current,std::span<const uint8_t> previous,std::span<const uint8_t> motion,
        std::span<const uint8_t> hardware,qa::GhostImageRows cost,
        const D3D12_RESOURCE_DESC& vectors,const D3D12_RESOURCE_DESC& costs,int64_t* hashNs=nullptr,
        const std::array<identity::Digest,5>* observedHashes=nullptr);
    void sample();
    void current_sample();
    void off_sample();
    void legacy_sample();
    void prepare();
    void arm();
    void consume(SnapshotPool::Envelope&);
    void remember_edit(SnapshotPool::Envelope&,unsigned);
    void close_observation() {
        observing.store(false,std::memory_order_release);
        std::shared_ptr<model::ObservationSink> empty;
        calldata_t query;calldata_init(&query);calldata_set_ptr(&query,"observer",&empty);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_observe",&query),"QA producer detach");
        calldata_free(&query);if(snapshots)snapshots->close_admission();
    }
};

void Harness::off_sample() {
    require((calibrating||baseline)&&!deterministic&&source&&filter,"OFF episode requires existing owned native calibration graph");
    const auto tick=obs_get_video_frame_time()/obs_get_frame_interval_ns()+1;
    std::shared_ptr<const obs_adapter::Capture> current;
    calldata_t query;calldata_init(&query);calldata_set_ptr(&query,"retain_current_raw",&current);
    require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"OFF immutable current capture query");
    const bool nativeFailed=calldata_bool(&query,"failed");calldata_free(&query);
    require(current&&!nativeFailed&&current->input.canonicalTick==tick&&current->input.key.valid(),
        "OFF current capture must belong to this actual canonical tick");
    calldata_init(&query);require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&query),"OFF source ingress before render");
    const auto sourceIngress=static_cast<uint64_t>(calldata_int(&query,"ingress"));
    const auto content=static_cast<unsigned>(calldata_int(&query,"content"));
    const bool missing=calldata_bool(&query,"missing");calldata_free(&query);
    require(!missing&&sourceIngress>0,"OFF independent current source provider");
    const FrameKey outputKey{current->input.key.instanceId,current->input.key.contentEpoch,baseIngress+sourceIngress};
    const auto sourceTick=static_cast<unsigned>(std::min<uint64_t>(sourceIngress-1,255));
    require(content==corpus.schedule[sourceTick].contentId,"OFF actual current provider content identity");
    // OBS skips disabled filters. Render that actual source-output path; never
    // reuse output_texture, selected_raw or a deterministic delayed-copy view.
    auto* settings=obs_source_get_settings(filter);obs_data_set_bool(settings,"nr_enabled",false);
    obs_source_update(filter,settings);obs_data_release(settings);obs_source_set_enabled(filter,false);
    const bool filterEnabled=obs_source_enabled(filter);
    settings=obs_source_get_settings(filter);const bool configuredEnabled=obs_data_get_bool(settings,"nr_enabled");obs_data_release(settings);
    calldata_init(&query);require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"OFF applied configuration witness");
    const auto revision=static_cast<uint64_t>(calldata_int(&query,"settings_revision"));calldata_free(&query);
    auto* target=gs_texrender_create(sourceLinear?GS_RGBA16F:GS_RGBA,GS_ZS_NONE);
    require(target!=nullptr,"OFF independent current-output surface");
    struct TargetOwner {gs_texrender_t* value;~TargetOwner(){gs_texrender_destroy(value);}} owner{target};
    require(gs_texrender_begin_with_color_space(target,canvas.width,canvas.height,sourceLinear?GS_CS_709_SCRGB:GS_CS_SRGB),"OFF actual output render begin");
    gs_blend_state_push();const bool srgb=gs_framebuffer_srgb_enabled();gs_enable_framebuffer_srgb(false);
    gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);gs_ortho(0,static_cast<float>(canvas.width),0,static_cast<float>(canvas.height),-100,100);
    obs_source_video_render(source);gs_enable_framebuffer_srgb(srgb);gs_blend_state_pop();gs_texrender_end(target);
    require(!obs_source_enabled(filter),"NR filter re-enabled during OFF output");
    settings=obs_source_get_settings(filter);const bool afterConfigured=obs_data_get_bool(settings,"nr_enabled");obs_data_release(settings);
    require(!afterConfigured,"NR configuration re-enabled during OFF output");
    calldata_init(&query);require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&query),"OFF source ingress after render");
    const bool sameSource=static_cast<uint64_t>(calldata_int(&query,"ingress"))==sourceIngress&&
        static_cast<unsigned>(calldata_int(&query,"content"))==content&&!calldata_bool(&query,"missing");calldata_free(&query);
    const auto outputTick=obs_get_video_frame_time()/obs_get_frame_interval_ns()+1;
    require(sameSource&&outputTick==tick,"OFF source changed across actual disabled output render");
    const auto currentBytes=nr_bootstrap_read_texture(current->original11.Get(),sourceLinear);
    const auto outputBytes=nr_bootstrap_read_texture(static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(target))),sourceLinear);
    const auto factor=80/corpus.referenceWhiteNits;
    const PackedPixels a{currentBytes,sourceLinear,false,!sourceLinear,corpus.color,factor};
    const PackedPixels b{outputBytes,sourceLinear,false,!sourceLinear,corpus.color,factor};a.validate();b.validate();
    ghost::collect_off(offProof,{corpus,sourceTick,current->input.key,outputKey,current->input.canonicalTick,outputTick,tick,revision,
        configuredEnabled,filterEnabled,OutputKind::CurrentSource,{&a,canvas,PackedPixels::read},{&b,canvas,PackedPixels::read},
        currentBytes,outputBytes,sourceLinear?8u:4u,std::max(measured.formatBound,measured.rawMaximum)});
    identity::Sha256 hash;const auto currentHash=identity::hex(hash.hash(currentBytes)),outputHash=identity::hex(hash.hash(outputBytes));
    auto* row=obs_data_create();obs_data_set_int(row,"schema",2);obs_data_set_string(row,"evidence_class","physical_disabled_filter");
    obs_data_set_string(row,"identity_digest_algorithm","sha256");obs_data_set_string(row,"kind","current-source");
    obs_data_set_bool(row,"configuration_enabled",configuredEnabled);obs_data_set_bool(row,"filter_enabled",filterEnabled);
    obs_data_set_int(row,"mode",static_cast<int64_t>(corpus.color));obs_data_set_int(row,"rate_num",rate.numerator);obs_data_set_int(row,"rate_den",rate.denominator);
    obs_data_set_int(row,"source_tick",sourceTick);obs_data_set_int(row,"source_ingress",static_cast<int64_t>(sourceIngress));
    obs_data_set_int(row,"current_tick",static_cast<int64_t>(current->input.canonicalTick));obs_data_set_int(row,"output_tick",static_cast<int64_t>(outputTick));
    obs_data_set_int(row,"settings_revision",static_cast<int64_t>(revision));obs_data_set_int(row,"width",canvas.width);obs_data_set_int(row,"height",canvas.height);
    const auto frame=[&](const char* name,const FrameKey& key) {
        auto* data=obs_data_create();obs_data_set_int(data,"instance",static_cast<int64_t>(key.instanceId));
        obs_data_set_int(data,"content_epoch",static_cast<int64_t>(key.contentEpoch));obs_data_set_int(data,"ingress",static_cast<int64_t>(key.ingressSequence));
        obs_data_set_obj(row,name,data);obs_data_release(data);
    };
    frame("current_key",current->input.key);frame("output_key",outputKey);
    obs_data_set_string(row,"current_hash",currentHash.c_str());obs_data_set_string(row,"output_hash",outputHash.c_str());
    obs_data_set_bool(row,"current_source_pixels_proved",true);emit("NR_GHOST_OFF ",row);
}

void Harness::prepare() {
    std::optional<obs_adapter::Diagnostics> diagnostics;
    std::shared_ptr<const obs_adapter::Capture> current;
    std::shared_ptr<interop::Context> context;
    std::array<std::shared_ptr<model::QaSignal>,3> dispatchSignals;
    calldata_t query;calldata_init(&query);calldata_set_ptr(&query,"diagnostics",&diagnostics);
    calldata_set_ptr(&query,"retain_current_raw",&current);calldata_set_ptr(&query,"retain_context",&context);
    calldata_set_ptr(&query,"retain_dispatch_signals",&dispatchSignals);
    require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"QA preparation actual native context");
    calldata_free(&query);require(current&&context&&diagnostics,"QA prepare actual immutable input/context");
    D3D12_RESOURCE_DESC vectors{},cost{};
    if(!deterministic) {
        require(diagnostics->preparation!=nullptr,"QA actual queried owned guide descriptors");
        const auto& prepared=*diagnostics->preparation;
        vectors=prepared.descriptors[0];cost=prepared.descriptors[1];
        require(prepared.grid>0 && vectors.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            vectors.Width==(canvas.width+prepared.grid-1)/prepared.grid &&
            vectors.Height==(canvas.height+prepared.grid-1)/prepared.grid &&
            vectors.Format==DXGI_FORMAT_R16G16_SINT && cost.Width==vectors.Width &&
            cost.Height==vectors.Height && flow_cost_format_valid(cost.Format),"QA queried owned descriptor admission");
    }else {
        vectors.Dimension=cost.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        vectors.Width=cost.Width=vectors.Height=cost.Height=1;
        vectors.DepthOrArraySize=cost.DepthOrArraySize=vectors.MipLevels=cost.MipLevels=1;
        vectors.SampleDesc.Count=cost.SampleDesc.Count=1;
        vectors.Format=DXGI_FORMAT_R16G16_SINT;cost.Format=DXGI_FORMAT_R8_UINT; // Unused: deterministic has no SDK stages.
    }
    ComPtr<ID3D11Device> renderer;current->original11->GetDevice(&renderer);
    baseIngress=current->input.key.ingressSequence;
    std::ifstream shaderFile(root/"bin/64bit/nr_ghost_chunk.dxil",std::ios::binary|std::ios::ate);
    require(shaderFile.good()&&shaderFile.tellg()>0,"selected actual chunk shader bytes");
    std::vector<std::byte> shader(static_cast<size_t>(shaderFile.tellg()));
    shaderFile.seekg(0);shaderFile.read(reinterpret_cast<char*>(shader.data()),static_cast<std::streamsize>(shader.size()));
    require(shaderFile.good(),"complete selected chunk shader read");
    digestLane=std::make_shared<GhostDigestLane>(context,shader);
    std::vector<std::byte>{}.swap(shader);
    snapshots=std::make_shared<SnapshotPool>(digestLane,renderer.Get(),sourceLinear?DXGI_FORMAT_R16G16B16A16_FLOAT:
        DXGI_FORMAT_R8G8B8A8_UNORM,hdrOutput?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM,
        vectors,cost,baseIngress,dispatchSignals,[this](SnapshotPool::Envelope& packet){consume(packet);},
        sourceLinear,hdrOutput,static_cast<uint32_t>(corpus.color),corpus.referenceWhiteNits,codecDigest);
    // Capture the lawful warm predecessor before its ready/publication cutoff.
    // These packets use the same three credits and ordered worker/drain.
    std::shared_ptr<model::ObservationSink> observer=snapshots;
    calldata_init(&query);calldata_set_ptr(&query,"observer",&observer);
    require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_observe",&query),"prepared predecessor observer attach");
    calldata_free(&query);
    for(auto& view:views)view=gs_texrender_create(sourceLinear?GS_RGBA16F:GS_RGBA,GS_ZS_NONE);
    require(views[0]&&views[1],"prepared full-size real view witnesses");
    const auto bytes=snapshots->bytes();auto* row=obs_data_create();
    obs_data_set_int(row,"credits",3);obs_data_set_int(row,"gpu_bytes",static_cast<int64_t>(bytes.gpu));
    obs_data_set_int(row,"readback_bytes",static_cast<int64_t>(bytes.readback));
    obs_data_set_int(row,"packed_bytes",static_cast<int64_t>(bytes.packed));
    obs_data_set_int(row,"leaf_readback_bytes",static_cast<int64_t>(bytes.leafReadback));
    obs_data_set_string(row,"identity_digest_algorithm",identity::chunkAlgorithm.data());
    obs_data_set_int(row,"digest_provider_preparation_ns",snapshots->digest_preparation_ns());
    emit("NR_GHOST_QA_PREPARED ",row);
}
void Harness::arm() {
    require(!failed&&snapshots&&snapshots->failure()==SnapshotPool::Refusal::None,
        "bounded preparation observation refusal");
    calldata_t query;calldata_init(&query);
    require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&query),"actual episode baseline");
    baseIngress=static_cast<uint64_t>(calldata_int(&query,"ingress"));calldata_free(&query);
    if(rawOutput) {
        std::shared_ptr<const obs_adapter::Capture> current;
        calldata_init(&query);calldata_set_ptr(&query,"retain_current_raw",&current);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"OFF episode input identity");
        calldata_free(&query);require(current&&current->input.key.ingressSequence==baseIngress,"OFF warm source binding");
        offInput.emplace(current->input);
        // Detach only the native observer. The same bounded pool remains open for actual OFF graphics.
        std::shared_ptr<model::ObservationSink> empty;
        calldata_init(&query);calldata_set_ptr(&query,"observer",&empty);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_observe",&query),"OFF native observer detach");
        calldata_free(&query);
        auto* settings=obs_source_get_settings(filter);obs_data_set_bool(settings,"nr_enabled",false);
        obs_source_update(filter,settings);obs_data_release(settings);obs_source_set_enabled(filter,false);
        calldata_init(&query);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"OFF configured revision");
        offRevision=static_cast<uint64_t>(calldata_int(&query,"settings_revision"));calldata_free(&query);
        require(offRevision>0,"OFF settings revision absent");
    }
    require(snapshots!=nullptr,"prepared QA credits before episode");
    snapshots->episode_base(baseIngress);
    // Pool was prepared on the prior cutoff; no ingress/key rewrite occurs.
    calldata_init(&query);require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_arm",&query),"exact fixture episode arm");
    calldata_free(&query);armed.store(true,std::memory_order_release);
}
bool scene_identity(const ghost::Fixture&,unsigned,const ghost::Image&);
void Harness::remember_edit(SnapshotPool::Envelope& packet,unsigned tick) {
    const auto& completedKey=serviceMeasurement?packet.fixtureKey:packet.key;
    require(completedKey&&packet.token&&completedKey->frame==packet.token->frame&&(packet.module||serviceMeasurement)&&packet.invalid==0,
        "actual detached completed model/source identity");
    constexpr uint64_t colorStages=(1ull<<model::Observation::Completed)|((1ull<<5)-1);
    require((packet.stages&colorStages)==colorStages,"actual keyed D/header stages missing");
    const double sourceFactor=80/corpus.referenceWhiteNits;
    const PackedPixels modelRaw{packet.packed[model::Observation::Canonical],true,false,!sourceLinear,corpus.color,sourceFactor};
    const PackedPixels modelResolved{packet.packed[model::Observation::Resolved],true,false,!sourceLinear,corpus.color,sourceFactor};
    modelRaw.validate();modelResolved.validate();
    require(packet.packed[model::Observation::Encoded].size()!=packet.packed[model::Observation::Foreign].size()||
        std::memcmp(packet.packed[model::Observation::Encoded].data(),packet.packed[model::Observation::Foreign].data(),
            packet.packed[model::Observation::Encoded].size())!=0,
        "actual model encoded-input/foreign-output no-op");
    size_t at=0;
    const auto unselectedEdit=[&](bool moving) {
        const auto points=roi(corpus,tick,moving,policy);
        double energy=0;
        for(auto p:points) {
            const auto x=modelRaw.rgb(static_cast<unsigned>(p%canvas.width),static_cast<unsigned>(p/canvas.width));
            const auto y=modelResolved.rgb(static_cast<unsigned>(p%canvas.width),static_cast<unsigned>(p/canvas.width));
            const ghost::Rgb delta{y.r-x.r,y.g-x.g,y.b-x.b};
            priorDelta.at(at++)=delta;
            energy+=delta.r*delta.r+delta.g*delta.g+delta.b*delta.b;
        }
        return std::sqrt(energy/static_cast<double>(points.size()));
    };
    const double staticEdit=unselectedEdit(false),movingEdit=unselectedEdit(true);
    require(at==priorDelta.size(),"bounded current-job D-before-g ROI coverage");
    for(size_t stage=0;stage<headerStages.size();++stage) {
        const size_t stride=headerStages[stage]!=model::Observation::Completed||sourceLinear?8u:4u;
        const auto bytes=packet.packed[headerStages[stage]];
        require(bytes.size()==static_cast<size_t>(canvas.width)*canvas.height*stride,"actual keyed model-stage header bounds");
        for(unsigned point=0;point<32;++point) {
            const unsigned x=(point/2)*4+1,y=point%2==0?0:2;
            priorHeader[stage][point]=0;
            memcpy(&priorHeader[stage][point],bytes.data()+(static_cast<size_t>(y)*canvas.width+x)*stride,stride);
        }
    }
    priorReferenceValid=packet.referenceValid;priorEditKey.emplace(*completedKey);
    if(!packet.preparing&&!serviceMeasurement)++measured.modelSamples;
    auto* record=obs_data_create();obs_data_set_int(record,"ingress",static_cast<int64_t>(completedKey->frame.ingressSequence));
    obs_data_set_int(record,"instance",static_cast<int64_t>(completedKey->frame.instanceId));
    obs_data_set_int(record,"content_epoch",static_cast<int64_t>(completedKey->frame.contentEpoch));
    obs_data_set_int(record,"history",static_cast<int64_t>(completedKey->historyEpoch));
    obs_data_set_int(record,"generation",static_cast<int64_t>(completedKey->resourceGeneration));
    obs_data_set_int(record,"submission",static_cast<int64_t>(completedKey->submissionSerial));
    obs_data_set_int(record,"envelope_serial",static_cast<int64_t>(packet.serial));
    LARGE_INTEGER frequency{};require(QueryPerformanceFrequency(&frequency)!=FALSE,"actual D witness QPC frequency");
    obs_data_set_int(record,"qpc_frequency",frequency.QuadPart);
    for(size_t index=0;index<packet.timing.size();++index)
        obs_data_set_int(record,("process_qpc_"+std::to_string(index)).c_str(),packet.timing[index]);
    for(size_t index=0;index<packet.chronology.size();++index)
        obs_data_set_int(record,("chronology_qpc_"+std::to_string(index)).c_str(),packet.chronology[index]);
    obs_data_set_int(record,"stage_mask",static_cast<int64_t>(packet.stages));
    obs_data_set_bool(record,"preparation",packet.preparing);
    obs_data_set_bool(record,"selected",packet.row.job&&*packet.row.job==*completedKey);
    obs_data_set_bool(record,"D_before_g",true);
    obs_data_set_double(record,"unselected_D_static_rms",staticEdit);obs_data_set_double(record,"unselected_D_moving_rms",movingEdit);
    obs_data_set_bool(record,"linear_reference_rgb_D",true);obs_data_set_bool(record,"canonical_resolved_gamma_half",!sourceLinear);
    obs_data_set_bool(record,"restore_reference_valid",packet.referenceValid);
    obs_data_set_int(record,"binary_g",packet.referenceValid?1:0);obs_data_set_bool(record,"selected_edit_retention_proof",false);
    emit(serviceMeasurement?"NR_GHOST_MEASUREMENT_D ":
        packet.preparing?"NR_GHOST_PREPARED_D ":"NR_GHOST_DETACHED_MODEL ",record);
}
void Harness::consume(SnapshotPool::Envelope& packet) {
    const auto started=std::chrono::steady_clock::now();
    if(serviceSample)serviceSample("consumer_entry");
    std::array<int64_t,5> timing{};
    const auto mark=[&](size_t index) {
        timing[index]=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count();
    };
    const auto elapsed=[](const auto& begin) {return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-begin).count();};
    try {
        if(packet.preparing) {
            require(!packet.refused.load(std::memory_order_acquire),"prepared model D copy refused");
            if((packet.evaluated&&packet.composed&&packet.submitted)||serviceMeasurement) {
                const auto begin=std::chrono::steady_clock::now();
                remember_edit(packet,0); // The production fixture is still in its frozen preparation phase.
                packet.cpuNs[SnapshotPool::Model]=elapsed(begin);
            }
            packet.consumerTotalNs=elapsed(started);return;
        }
        require(!packet.refused.load(std::memory_order_acquire)&&packet.graphicsMask==63&&packet.sameViews,
            "required detached graphic stages/view latch missing");
        auto& row=packet.row;
        require(row.tick==row.clock/row.interval+1,"actual QA canonical clock");
        require(row.kind<=static_cast<unsigned>(OutputKind::PrimingRaw)?row.selected==row.ingress:
            row.selected+1==row.ingress,"unchanged exact current/one-tick selection");
        if(!rows.empty())require(row.tick==rows.back().tick+1&&row.ingress==rows.back().ingress+1,
            "actual copied-stage missing/reordered emission");
        const auto currentTick=static_cast<unsigned>(row.ingress-baseIngress-1);
        const auto selectedTick=row.selected<=baseIngress?0u:static_cast<unsigned>(row.selected-baseIngress-1);
        require(currentTick<256&&selectedTick<256,"declared finite source episode");
        const auto role=[](SnapshotPool::Graphic stage){return SnapshotPool::modelCount+static_cast<size_t>(stage);};
        const auto& sourceBytes=packet.packed[role(SnapshotPool::CurrentRaw)];
        const auto& rawBytes=packet.packed[role(SnapshotPool::SelectedRaw)];
        const auto& selectedBytes=packet.packed[role(SnapshotPool::Selected)];
        const auto& programBytes=packet.packed[role(SnapshotPool::Program)];
        const double sourceFactor=80/corpus.referenceWhiteNits;
        const PackedPixels rawRgb{rawBytes,sourceLinear,false,!sourceLinear,corpus.color,sourceFactor};
        const PackedPixels selectedRgb{selectedBytes,sourceLinear,false,!sourceLinear,corpus.color,sourceFactor};
        const PackedPixels programRgb{programBytes,hdrOutput,!hdrOutput,!hdrOutput,corpus.color,
            (hdrOutput?300.0:80.0)/corpus.referenceWhiteNits};
        rawRgb.validate();selectedRgb.validate();programRgb.validate();
        mark(0);
        packet.cpuNs[SnapshotPool::Validity]=elapsed(started);
        auto begin=std::chrono::steady_clock::now();
        const double tickRawMaximum=image::ghost_raw_maximum(corpus,currentTick,sourceLinear,sourceBytes,measured.formatBound,&packet.rawCounts);
        measured.rawMaximum=std::max(measured.rawMaximum,tickRawMaximum);
        packet.cpuNs[SnapshotPool::RawAdmission]=elapsed(begin);
        begin=std::chrono::steady_clock::now();
        const auto* raw=rawBytes.data();const auto* selected=selectedBytes.data();
        for(size_t p=0;p<static_cast<size_t>(canvas.width)*canvas.height;++p) {
            if(sourceLinear) {
                uint16_t a=0,b=0;memcpy(&a,raw+p*8+6,2);memcpy(&b,selected+p*8+6,2);
                if((a&0x7fff)==0)a=0;if((b&0x7fff)==0)b=0;
                if(a!=b)++measured.alphaErrors;
            }else if(raw[p*4+3]!=selected[p*4+3])++measured.alphaErrors;
        }
        require(measured.alphaErrors==0,"actual detached source/filter alpha");
        ++measured.rawSamples;
        mark(1);
        packet.cpuNs[SnapshotPool::Alpha]=elapsed(begin);
        begin=std::chrono::steady_clock::now();
        const bool rawIdentity=packed_scene_identity(corpus,selectedTick,rawRgb);
        const bool selectedIdentity=packed_scene_identity(corpus,selectedTick,selectedRgb);
        if(!rawIdentity||!selectedIdentity) {
            auto* detail=obs_data_create();
            obs_data_set_int(detail,"serial",static_cast<int64_t>(packet.serial));
            obs_data_set_int(detail,"ingress",static_cast<int64_t>(row.ingress));
            obs_data_set_int(detail,"selected",static_cast<int64_t>(row.selected));
            obs_data_set_int(detail,"kind",row.kind);obs_data_set_int(detail,"source_tick",selectedTick);
            obs_data_set_int(detail,"seed",corpus.seed);obs_data_set_int(detail,"color",static_cast<int64_t>(corpus.color));
            obs_data_set_bool(detail,"source_half",sourceLinear);obs_data_set_double(detail,"source_white",80);
            obs_data_set_double(detail,"reference_white",corpus.referenceWhiteNits);
            obs_data_set_bool(detail,"raw_identity",rawIdentity);obs_data_set_bool(detail,"selected_identity",selectedIdentity);
            if(row.job) {
                obs_data_set_int(detail,"selected_instance",static_cast<int64_t>(row.job->frame.instanceId));
                obs_data_set_int(detail,"selected_content_epoch",static_cast<int64_t>(row.job->frame.contentEpoch));
                obs_data_set_int(detail,"selected_history",static_cast<int64_t>(row.job->historyEpoch));
                obs_data_set_int(detail,"selected_generation",static_cast<int64_t>(row.job->resourceGeneration));
                obs_data_set_int(detail,"selected_submission",static_cast<int64_t>(row.job->submissionSerial));
            }
            auto* rawDetail=obs_data_create();auto* selectedDetail=obs_data_create();
            (void)packed_scene_identity(corpus,selectedTick,rawRgb,rawDetail);
            (void)packed_scene_identity(corpus,selectedTick,selectedRgb,selectedDetail);
            obs_data_set_obj(detail,"raw",rawDetail);obs_data_set_obj(detail,"selected_image",selectedDetail);
            obs_data_release(rawDetail);obs_data_release(selectedDetail);
            const bool sameHeader=row.job&&priorEditKey&&*row.job==*priorEditKey;
            obs_data_set_bool(detail,"same_selected_JobKey_stage_header",sameHeader);
            if(sameHeader)obs_data_set_bool(detail,"selected_restore_reference_valid",priorReferenceValid);
            if(sameHeader) {
                constexpr std::array<const char*,6> names{"canonical","encoded","keep","foreign","resolved","completed"};
                auto* headers=obs_data_array_create();
                for(size_t stage=0;stage<headerStages.size();++stage)for(unsigned point=0;point<32;++point) {
                    const bool half=headerStages[stage]!=model::Observation::Completed||sourceLinear;
                    const bool proxy=headerStages[stage]==model::Observation::Encoded||headerStages[stage]==model::Observation::Foreign;
                    const bool gamma=proxy||!sourceLinear;
                    const auto bytes=std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&priorHeader[stage][point]),half?8u:4u};
                    const auto native=image::packed_rgb(bytes,0,half,false);
                    const auto fixed=image::packed_rgb(bytes,0,half,gamma);
                    auto* header=obs_data_create();obs_data_set_string(header,"stage",names[stage]);
                    obs_data_set_int(header,"bit",point/2);obs_data_set_int(header,"row",point%2==0?0:2);
                    obs_data_set_string(header,"actual_packed",std::to_string(priorHeader[stage][point]).c_str());
                    obs_data_set_bool(header,"half",half);obs_data_set_bool(header,"gamma",gamma);
                    obs_data_set_double(header,"native_r",native.r);obs_data_set_double(header,"fixed_domain_linear_r",fixed.r);
                    obs_data_set_bool(header,"source_reference_domain",!sourceLinear||!proxy);
                    if(!sourceLinear||!proxy) {
                        const auto normalized=from709(fixed,corpus.color);
                        obs_data_set_double(header,"reference_linear_r",normalized.r*sourceFactor);
                    }
                    obs_data_array_push_back(headers,header);obs_data_release(header);
                }
                obs_data_set_array(detail,"selected_job_stage_header",headers);obs_data_array_release(headers);
            }
            emit("NR_GHOST_IDENTITY_FAILURE ",detail);
        }
        require(rawIdentity&&selectedIdentity,"actual detached independent source IDs/geometry");
        if(baseline&&rawOutput) {
            require(tally&&packet.token&&row.selectedFrame&&!row.job&&selectedTick==currentTick,
                "raw OFF current key/phase cannot come from a delayed NR selection");
            const PackedPixels currentRaw{sourceBytes,sourceLinear,false,!sourceLinear,corpus.color,sourceFactor};
            currentRaw.validate();
            const double outputError=image::ghost_raw_maximum(corpus,currentTick,sourceLinear,selectedBytes,measured.formatBound);
            tally->off_frame({corpus,currentTick,packet.token->frame,*row.selectedFrame,row.tick,row.tick,row.tick,
                row.settingsRevision,packet.offConfigurationEnabled,packet.offFilterEnabled,static_cast<OutputKind>(row.kind),
                {&currentRaw,canvas,PackedPixels::read},{&selectedRgb,canvas,PackedPixels::read},sourceBytes,selectedBytes,
                sourceLinear?8u:4u,tally->epsilon},outputError);
        }
        packet.cpuNs[SnapshotPool::Identity]=elapsed(begin);
        // One bounded crop cohort from actual immutable physical observations.
        // Controls mutate these CPU copies, not GPU submissions or the producer.
        if(policy&&(calibrating||baseline)&&!physicalAnchors&&currentTick>=32&&currentTick<64&&row.job&&priorEditKey&&
            packet.key&&packet.reference&&!packet.reset&&row.selectedFrame&&
            !packet.packed[model::Observation::CurrentAnalysis].empty()&&
            !packet.packed[model::Observation::ReferenceAnalysis].empty()) {
            const PackedPixels currentRgb{sourceBytes,sourceLinear,false,!sourceLinear,corpus.color,sourceFactor};
            const PackedPixels currentGuide{packet.packed[model::Observation::CurrentAnalysis],false,true,false,corpus.color,1};
            const PackedPixels previousGuide{packet.packed[model::Observation::ReferenceAnalysis],false,true,false,corpus.color,1};
            currentRgb.validate();currentGuide.validate();previousGuide.validate();
            const auto crop=[](const PackedPixels& pixels) {
                ghost::Image result;result.reserve(ghost::Fixture::width*ghost::Fixture::height);
                for(unsigned y=0;y<ghost::Fixture::height;++y)for(unsigned x=0;x<ghost::Fixture::width;++x)
                    result.push_back(pixels.rgb(x,y));
                return result;
            };
            const auto current=crop(currentRgb),previous=crop(rawRgb),guideCurrent=crop(currentGuide),guidePrevious=crop(previousGuide);
            const double controlFormat=baseline&&frozen?frozen->formatBound:measured.formatBound;
            const double controlRaw=baseline&&frozen?frozen->rawMaximum:measured.rawMaximum;
            const auto epsilon=std::max(controlFormat,controlRaw);
            physicalControls=ghost::calibration_controls({corpus,currentTick,current,previous,guideCurrent,guidePrevious,
                {controlFormat,controlRaw,epsilon},packet.key->frame,*row.selectedFrame,*row.job,*priorEditKey,
                *packet.key,*packet.reference,packet.graphicsMask,true});
            ++physicalAnchors;
            const std::array<size_t,4> stages{role(SnapshotPool::CurrentRaw),role(SnapshotPool::SelectedRaw),
                model::Observation::CurrentAnalysis,model::Observation::ReferenceAnalysis};
            constexpr std::array<const char*,4> names{"current_hash","prior_hash","current_guide_hash","prior_guide_hash"};
            for(size_t i=0;i<physicalControls.size();++i) {
                auto* result=obs_data_create();obs_data_set_int(result,"schema",2);
                obs_data_set_string(result,"evidence_class","physical_source_pixel_mutation");
                obs_data_set_string(result,"name",ghost::physical_control_names[i]);
                obs_data_set_int(result,"serial",static_cast<int64_t>(packet.serial));
                obs_data_set_int(result,"source_tick",currentTick);obs_data_set_int(result,"seed",corpus.seed);
                obs_data_set_int(result,"mode",static_cast<int64_t>(corpus.color));
                obs_data_set_int(result,"rate_num",rate.numerator);obs_data_set_int(result,"rate_den",rate.denominator);
                set_job_key(result,"current_key",packet.key);set_job_key(result,"reference_key",packet.reference);
                set_job_key(result,"completed_key",row.job);
                obs_data_set_bool(result,"rejected",*physicalControls[i].rejected);
                obs_data_set_bool(result,"actual_pixels",true);obs_data_set_bool(result,"gpu_submission",false);
                for(size_t stage=0;stage<stages.size();++stage) {
                    require(packet.digests[stages[stage]].has_value(),"actual physical control stage digest absent");
                    obs_data_set_string(result,names[stage],identity::hex(*packet.digests[stages[stage]]).c_str());
                }
                emit("NR_GHOST_PHYSICAL_CONTROL ",result);
            }
        }

        require(packet.digests[role(SnapshotPool::SelectedRaw)]&&packet.digests[role(SnapshotPool::Selected)]&&
            packet.digests[role(SnapshotPool::Program)],"actual complete graphic SHA256 missing");
        row.rawHash=*packet.digests[role(SnapshotPool::SelectedRaw)];
        row.selectedHash=*packet.digests[role(SnapshotPool::Selected)];
        row.programHash=*packet.digests[role(SnapshotPool::Program)];
        if(deterministic)require(row.rawHash==row.selectedHash,"same production deterministic pixels");
        begin=std::chrono::steady_clock::now();
        require(packed_scene_identity(corpus,selectedTick,programRgb),"actual detached program image identity");
        packet.cpuNs[SnapshotPool::Identity]+=elapsed(begin);
        mark(2);
        begin=std::chrono::steady_clock::now();
        if(!deterministic&&(row.kind==static_cast<unsigned>(OutputKind::DelayedNr)||serviceMeasurement)) {
            require(row.job&&priorEditKey&&*row.job==*priorEditKey,"selected output lacks matching JobKey D-before-g");
            const double epsilon=frozen?std::max(frozen->formatBound,frozen->rawMaximum):measured.formatBound;
            size_t at=0;
            auto selectedEdit=[&](bool moving) {
                const auto points=roi(corpus,selectedTick,moving,policy);
                double actual=0,intended=0;
                for(auto p:points) {
                    const auto a=selectedRgb.rgb(static_cast<unsigned>(p%canvas.width),static_cast<unsigned>(p/canvas.width));
                    const auto b=rawRgb.rgb(static_cast<unsigned>(p%canvas.width),static_cast<unsigned>(p/canvas.width));
                    const auto d=priorDelta.at(at++);
                    const ghost::Rgb change{a.r-b.r,a.g-b.g,a.b-b.b};
                    actual+=change.r*change.r+change.g*change.g+change.b*change.b;
                    intended+=d.r*d.r+d.g*d.g+d.b*d.b;
                }
                return ghost::EditScore{std::sqrt(actual/static_cast<double>(points.size())),
                    intended?actual/intended:0,intended>0&&actual>0};
            };
            const auto staticEdit=selectedEdit(false),movingEdit=selectedEdit(true);
            require(at==priorDelta.size(),"prescribed bounded selected edit ROI coverage");
            if(baseline&&tally) {
                tally->edit(selectedTick,false,priorReferenceValid,staticEdit.rms,staticEdit.retained);
                tally->edit(selectedTick,true,priorReferenceValid,movingEdit.rms,movingEdit.retained);
            }
            measured.staticRms=std::max(measured.staticRms,staticEdit.rms);
            measured.movingRms=std::max(measured.movingRms,movingEdit.rms);
            measured.staticRetention=std::max(measured.staticRetention,staticEdit.retained);
            measured.movingRetention=std::max(measured.movingRetention,movingEdit.retained);
            ++selectedEditSamples;
            const bool staticPositive=priorReferenceValid&&staticEdit.rms>(policy?policy->editFactor:8)*epsilon&&staticEdit.retained>=(policy?policy->editRetention:.8);
            const bool movingPositive=priorReferenceValid&&movingEdit.rms>(policy?policy->editFactor:8)*epsilon&&movingEdit.retained>=(policy?policy->editRetention:.8);
            if(staticPositive)++staticPositiveSamples;if(movingPositive)++movingPositiveSamples;
            auto* record=obs_data_create();obs_data_set_int(record,"selected_ingress",static_cast<int64_t>(row.selected));
            obs_data_set_int(record,"selected_serial",static_cast<int64_t>(row.job->submissionSerial));
            obs_data_set_int(record,"instance",static_cast<int64_t>(row.job->frame.instanceId));
            obs_data_set_int(record,"content_epoch",static_cast<int64_t>(row.job->frame.contentEpoch));
            obs_data_set_int(record,"history",static_cast<int64_t>(row.job->historyEpoch));
            obs_data_set_int(record,"generation",static_cast<int64_t>(row.job->resourceGeneration));
            obs_data_set_bool(record,"same_JobKey_D_before_g",true);
            set_job_key(record,"D_key",priorEditKey);
            obs_data_set_bool(record,"restore_reference_valid",priorReferenceValid);
            obs_data_set_int(record,"binary_g",priorReferenceValid?1:0);
            obs_data_set_double(record,"epsilon",epsilon);obs_data_set_double(record,"static_rms",staticEdit.rms);
            obs_data_set_double(record,"moving_rms",movingEdit.rms);
            obs_data_set_double(record,"static_retention",staticEdit.retained);
            obs_data_set_double(record,"moving_retention",movingEdit.retained);
            obs_data_set_bool(record,"static_positive",staticPositive);
            obs_data_set_bool(record,"moving_positive",movingPositive);
            emit(serviceMeasurement?"NR_GHOST_MEASUREMENT_ROI ":"NR_GHOST_SELECTED_EDIT ",record);
        }
        packet.cpuNs[SnapshotPool::SelectedEdit]=elapsed(begin);
        begin=std::chrono::steady_clock::now();
        for(auto view:{SnapshotPool::Preview,SnapshotPool::Projector})
            require(packet.packed[role(view)].size()==selectedBytes.size()&&
                std::memcmp(packet.packed[role(view)].data(),selectedBytes.data(),selectedBytes.size())==0,
                "real full-size view pixels changed within canonical latch");
        packet.cpuNs[SnapshotPool::Views]=elapsed(begin);
        row.sourceTick=selectedTick;row.content=corpus.schedule[selectedTick].contentId;
        if((packet.evaluated&&packet.composed&&packet.submitted)||serviceMeasurement) {
            const auto modelBegin=std::chrono::steady_clock::now();
            const uint64_t required=((1ull<<model::Observation::Count)-1)&
                ~(packet.reset?((1ull<<model::Observation::Vectors)|(1ull<<model::Observation::Cost)):0ull);
            require((packet.stages&required)==required,"required actual model/guide stage pixels missing");
            remember_edit(packet,currentTick);
            packet.cpuNs[SnapshotPool::Model]=elapsed(modelBegin);
            const auto guideBegin=std::chrono::steady_clock::now();
            std::array<identity::Digest,5> guideHashes{};
            for(size_t index=0;index<guideHashes.size();++index) {
                const auto& digest=packet.digests[model::Observation::CurrentAnalysis+index];
                require(digest.has_value()||(packet.reset&&index>=3),"actual complete guide SHA256 missing");
                if(digest)guideHashes[index]=*digest;
            }
            guide_bytes(*(serviceMeasurement?packet.fixtureKey:packet.key),
                serviceMeasurement?packet.fixtureReference:packet.reference,packet.grid,packet.reset&&!serviceMeasurement,
                packet.packed[model::Observation::CurrentAnalysis],packet.packed[model::Observation::ReferenceAnalysis],
                packet.packed[model::Observation::Motion],
                packet.reset&&!serviceMeasurement?std::span<const uint8_t>{}:std::span<const uint8_t>{packet.packed[model::Observation::Vectors]},
                packet.reset&&!serviceMeasurement?qa::GhostImageRows{}:packet.imageRows[model::Observation::Cost],
                [&]{D3D12_RESOURCE_DESC d{};const auto& f=packet.footprints[model::Observation::Vectors].Footprint;
                    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=f.Width;d.Height=f.Height;d.Format=f.Format;return d;}(),
                [&]{D3D12_RESOURCE_DESC d{};const auto& f=packet.footprints[model::Observation::Cost].Footprint;
                    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=f.Width;d.Height=f.Height;d.Format=f.Format;return d;}(),
                &packet.cpuNs[SnapshotPool::GuideHash],&guideHashes);
            packet.cpuNs[SnapshotPool::GuideArithmetic]=elapsed(guideBegin)-packet.cpuNs[SnapshotPool::GuideHash];
        }
        mark(3);
        begin=std::chrono::steady_clock::now();
        if(pictureArmed.load(std::memory_order_acquire)&&!sourcePicture) {
            pictureContent=row.content;sourcePicture.emplace(image::rgb_image(rawBytes,sourceLinear,false,"detached selected raw"));
            selectedPicture.emplace(image::rgb_image(selectedBytes,sourceLinear,false,"detached selected filter"));
            programPicture.emplace(image::rgb_image(programBytes,hdrOutput,!hdrOutput,"detached actual program"));
        }
        packet.cpuNs[SnapshotPool::Pictures]=elapsed(begin);
        if(serviceSample)serviceSample("pictures_and_consumers");
        begin=std::chrono::steady_clock::now();
        score(selectedTick,ghost::PixelView{&rawRgb,canvas,PackedPixels::read},
            ghost::PixelView{&selectedRgb,canvas,PackedPixels::read});rows.push_back(row);
        if(baseline&&tally&&!rawOutput) {
            const uint64_t instanceId=row.selectedFrame?row.selectedFrame->instanceId:0;
            tally->frame(row.kind,row.ingress,row.selected,instanceId);
        }
        auto* frame=obs_data_create();
        obs_data_set_string(frame,"identity_digest_algorithm",identity::chunkAlgorithm.data());
        obs_data_set_int(frame,"schema",identity::chunkSchema);
        obs_data_set_int(frame,"tick",static_cast<int64_t>(row.tick));
        obs_data_set_int(frame,"clock",static_cast<int64_t>(row.clock));
        obs_data_set_int(frame,"interval",static_cast<int64_t>(row.interval));
        obs_data_set_int(frame,"ingress",static_cast<int64_t>(row.ingress));
        obs_data_set_int(frame,"selected",static_cast<int64_t>(row.selected));
        obs_data_set_int(frame,"serial",static_cast<int64_t>(packet.serial));
        obs_data_set_int(frame,"kind",row.kind);obs_data_set_int(frame,"source_tick",row.sourceTick);
        obs_data_set_int(frame,"content_id",row.content);
        obs_data_set_string(frame,"raw_hash",identity::hex(row.rawHash).c_str());
        obs_data_set_string(frame,"selected_hash",identity::hex(row.selectedHash).c_str());
        obs_data_set_string(frame,"program_hash",identity::hex(row.programHash).c_str());
        set_job_key(frame,"selected_key",row.job);
        if(row.selectedFrame) {
            auto* key=frame_key(*row.selectedFrame);obs_data_set_obj(frame,"selected_frame",key);obs_data_release(key);
        }
        obs_data_set_int(frame,"settings_revision",static_cast<int64_t>(row.settingsRevision));
        obs_data_set_int(frame,"resource_generation",static_cast<int64_t>(row.generation));
        emit(serviceMeasurement?"NR_GHOST_MEASUREMENT_FRAME ":"NR_GHOST_FRAME ",frame);
        mark(4);
        packet.cpuNs[SnapshotPool::Score]=elapsed(begin);
        auto* receipt=obs_data_create();obs_data_set_int(receipt,"tick",static_cast<int64_t>(row.tick));
        obs_data_set_int(receipt,"ingress",static_cast<int64_t>(row.ingress));obs_data_set_int(receipt,"selected",static_cast<int64_t>(row.selected));
        obs_data_set_int(receipt,"envelope_serial",static_cast<int64_t>(packet.serial));
        obs_data_set_int(receipt,"producer_required",static_cast<int64_t>(packet.producerValue));
        obs_data_set_int(receipt,"producer_done",static_cast<int64_t>(packet.producer->GetCompletedValue()));
        obs_data_set_int(receipt,"batch_required",static_cast<int64_t>(packet.batchValue));
        obs_data_set_int(receipt,"batch_done",static_cast<int64_t>(packet.batch->GetCompletedValue()));
        obs_data_set_int(receipt,"graphic_mask",static_cast<int64_t>(packet.graphicsMask));
        obs_data_set_bool(receipt,"source_detached",true);obs_data_set_bool(receipt,"dispatch_returned",true);
        obs_data_set_int(receipt,"drain_dependencies_ns",packet.drainTiming[0]);
        obs_data_set_int(receipt,"drain_readback_ns",packet.drainTiming[1]);
        for(size_t index=0;index<timing.size();++index)
            obs_data_set_int(receipt,("consumer_elapsed_"+std::to_string(index)+"_ns").c_str(),timing[index]);
        obs_data_set_bool(receipt,"same_views",packet.sameViews);emit("NR_GHOST_COPY_RECEIPT ",receipt);
        if(currentTick>=48)SetEvent(recordStart.value);
        if(currentTick==255){observing.store(false,std::memory_order_release);SetEvent(complete.value);}
        packet.consumerTotalNs=elapsed(started);
    }catch(const std::exception& error) {
        packet.consumerTotalNs=elapsed(started);
        std::osyncstream(std::cout)<<"NR_GHOST_DETACHED_FAILURE serial="<<packet.serial
            <<" ingress="<<packet.row.ingress<<" selected="<<packet.row.selected
            <<" kind="<<packet.row.kind<<" "<<error.what()<<std::endl;
        failure=error.what();failed.store(true,std::memory_order_release);SetEvent(recordStart.value);SetEvent(complete.value);
        throw;
    }
}
void Harness::current_sample() {
    require(offInput&&snapshots,"prepared OFF episode observations");
    calldata_t query;calldata_init(&query);
    require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&query),"OFF finite source ingress");
    const auto ingress=static_cast<uint64_t>(calldata_int(&query,"ingress"));
    const auto content=static_cast<unsigned>(calldata_int(&query,"content"));
    const bool missing=calldata_bool(&query,"missing");calldata_free(&query);
    require(!missing&&ingress>0,"OFF finite source provider absent");
    if(ingress>ghost::Fixture::ticks)return; // The ordered drain, not an extra render, closes the finite cohort.
    const auto sourceTick=static_cast<unsigned>(ingress-1);
    require(content==corpus.schedule[sourceTick].contentId,"OFF current source content schedule");
    const auto clock=obs_get_video_frame_time(),interval=obs_get_frame_interval_ns();
    const auto tick=clock/interval+1;
    auto* settings=obs_source_get_settings(filter);
    const bool configurationEnabled=obs_data_get_bool(settings,"nr_enabled");obs_data_release(settings);
    const bool filterEnabled=obs_source_enabled(filter);
    calldata_init(&query);
    require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"OFF current settings witness");
    const auto revision=static_cast<uint64_t>(calldata_int(&query,"settings_revision"));
    const bool nativeFailed=calldata_bool(&query,"failed");calldata_free(&query);
    require(!nativeFailed&&revision==offRevision&&!configurationEnabled&&!filterEnabled,"OFF configuration changed in episode");
    // Independent current-source renders and the actual program surface; no native selected_raw/output is used.
    for(auto* view:views) {
        gs_texrender_reset(view);
        require(gs_texrender_begin_with_color_space(view,canvas.width,canvas.height,
            sourceLinear?GS_CS_709_SCRGB:GS_CS_SRGB),"OFF current output view");
        gs_blend_state_push();const bool srgb=gs_framebuffer_srgb_enabled();gs_enable_framebuffer_srgb(false);
        gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
        vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);
        gs_ortho(0,static_cast<float>(canvas.width),0,static_cast<float>(canvas.height),-100,100);
        obs_source_video_render(source);gs_enable_framebuffer_srgb(srgb);gs_blend_state_pop();gs_texrender_end(view);
    }
    calldata_init(&query);
    require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&query),"OFF current source after views");
    const bool same=static_cast<uint64_t>(calldata_int(&query,"ingress"))==ingress&&
        static_cast<unsigned>(calldata_int(&query,"content"))==content&&!calldata_bool(&query,"missing");
    calldata_free(&query);
    require(same&&clock==obs_get_video_frame_time()&&!obs_source_enabled(filter),"OFF source/key changed across views");
    const auto& prior=*offInput;
    const FrameKey key{prior.key.instanceId,prior.key.contentEpoch,baseIngress+ingress};
    const IngressSample input{key,tick,tick-1,prior.sourceIdentity,prior.deviceIdentity,prior.resourceGeneration,
        prior.validity,prior.descriptor,Settings{{revision},false,prior.settings.runtimePath,prior.settings.strength,prior.settings.tuning}};
    require(snapshots->current_capture(input,
        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(views[0]))),
        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(views[1]))),
        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_get_render_target())),clock,interval,
        configurationEnabled,filterEnabled),"bounded OFF current-output capture refused");
}
void Harness::sample() {
    if(offRequested.exchange(false,std::memory_order_acq_rel)) {
        try{off_sample();}catch(const std::exception& error){failed=true;failure=error.what();}
        SetEvent(offDone.value);return;
    }
    if(!observing.load(std::memory_order_acquire)||!bound)return;
    if(rawOutput&&armed.load(std::memory_order_acquire)) {
        try{current_sample();}
        catch(const std::exception& error) {
            failure=error.what();failed.store(true,std::memory_order_release);
            observing.store(false,std::memory_order_release);SetEvent(recordStart.value);SetEvent(complete.value);
        }
        return;
    }
    if(!armed.load(std::memory_order_acquire)) {
        calldata_t query;calldata_init(&query);
        std::optional<obs_adapter::Diagnostics> diagnostics;
        std::shared_ptr<const obs_adapter::Capture> current;
        calldata_set_ptr(&query,"diagnostics",&diagnostics);calldata_set_ptr(&query,"retain_current_raw",&current);
        const bool returned=proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query);
        const bool nativeFailed=calldata_bool(&query,"failed");calldata_free(&query);
        const unsigned seen=1u|(diagnostics&&diagnostics->firstCompleted?2u:0u)|
            (diagnostics&&diagnostics->firstOrdinary?4u:0u)|(diagnostics&&diagnostics->preparation?8u:0u);
        if((seen&~warmDiagnostics)!=0) {
            warmDiagnostics|=seen;
            auto* row=obs_data_create();obs_data_set_bool(row,"procedure_returned",returned);
            obs_data_set_bool(row,"native_failed",nativeFailed);obs_data_set_bool(row,"current_capture",current!=nullptr);
            auto witness=[&](const char* name,const std::optional<obs_adapter::JobWitness>& value) {
                if(!value)return;
                auto* job=obs_data_create();obs_data_set_int(job,"ingress",static_cast<int64_t>(value->key.frame.ingressSequence));
                obs_data_set_int(job,"history",static_cast<int64_t>(value->key.historyEpoch));
                obs_data_set_int(job,"serial",static_cast<int64_t>(value->key.submissionSerial));
                obs_data_set_bool(job,"ready",value->ready);obs_data_set_bool(job,"dispatch_returned",value->dispatchReturned);
                obs_data_set_int(job,"publication",value->publication);obs_data_set_int(job,"code",value->code);
                obs_data_set_bool(job,"flow",value->flow);obs_data_set_bool(job,"reset",value->reset);
                obs_data_set_bool(job,"evaluated",value->evaluated);obs_data_set_bool(job,"submitted",value->submitted);
                obs_data_set_int(job,"producer",static_cast<int64_t>(value->physical.producer));
                obs_data_set_int(job,"worker",static_cast<int64_t>(value->physical.worker));
                obs_data_set_int(job,"consumer",static_cast<int64_t>(value->physical.consumer));
                obs_data_set_bool(job,"consumer_open",value->physical.consumerOpen);
                obs_data_set_string(job,"failure",value->failure.data());
                obs_data_set_int(job,"failure_bytes",value->failureBytes);
                obs_data_set_bool(job,"failure_truncated",value->failureTruncated);
                for(size_t i=0;i<value->chronology.size();++i)
                    obs_data_set_int(job,("chronology_"+std::to_string(i)).c_str(),value->chronology[i]);
                obs_data_set_obj(row,name,job);obs_data_release(job);
            };
            if(diagnostics) {
                witness("latest",diagnostics->latest);witness("first_completed",diagnostics->firstCompleted);
                witness("first_ordinary",diagnostics->firstOrdinary);
                if(diagnostics->preparation) {
                    const auto& prepared=*diagnostics->preparation;
                    obs_data_set_int(row,"prepared_ingress",static_cast<int64_t>(prepared.key.frame.ingressSequence));
                    obs_data_set_int(row,"queried_grid",prepared.grid);
                    obs_data_set_int(row,"owned_vector_width",static_cast<int64_t>(prepared.descriptors[0].Width));
                    obs_data_set_int(row,"owned_vector_height",prepared.descriptors[0].Height);
                    obs_data_set_int(row,"owned_cost_format",prepared.descriptors[1].Format);
                }
            }
            emit("NR_GHOST_PREPARATION ",row);
        }
        if(returned&&current&&diagnostics&&(deterministic||diagnostics->preparation))SetEvent(preparable.value);
        if(snapshots&&returned&&current&&!nativeFailed) {
            calldata_init(&query);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&query),"actual prepared episode phase");
            const auto tick=static_cast<uint64_t>(calldata_int(&query,"tick"));
            const auto ingress=static_cast<uint64_t>(calldata_int(&query,"ingress"));
            const auto selected=static_cast<uint64_t>(calldata_int(&query,"selected"));
            const auto kind=static_cast<OutputKind>(calldata_int(&query,"kind"));
            const auto clock=static_cast<uint64_t>(calldata_int(&query,"clock"));
            const auto interval=static_cast<uint64_t>(calldata_int(&query,"interval"));
            calldata_free(&query);
            calldata_init(&query);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),
                "prepared actual model envelope");
            auto* packet=static_cast<model::Observation*>(calldata_ptr(&query,"observation"));
            calldata_free(&query);
            if(packet)snapshots->submit(packet,nullptr,nullptr,nullptr,clock,interval,false);
            if(failed.load(std::memory_order_acquire)||snapshots->failure()!=SnapshotPool::Refusal::None) {
                SetEvent(armReady.value);return;
            }
            const bool known=packet&&current->input.canonicalTick==tick&&current->input.key.ingressSequence==ingress&&selected&&
                ((kind==OutputKind::PrimingRaw&&selected==ingress)||
                 ((kind==OutputKind::DelayedRaw||kind==OutputKind::DelayedNr)&&selected+1==ingress));
            preparedConsecutive=known?(tick==lastPreparationTick+1?preparedConsecutive+1:1):0;
            lastPreparationTick=tick;
            if(preparedConsecutive>=2)SetEvent(armReady.value);
        }
        return;
    }
    calldata_t query;calldata_init(&query);proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&query);
    const bool missing=calldata_bool(&query,"missing");calldata_free(&query);
    calldata_init(&query);proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query);
    auto* packet=static_cast<model::Observation*>(calldata_ptr(&query,"observation"));calldata_free(&query);
    if(missing||!packet||failed.load(std::memory_order_acquire)||snapshots->failure()!=SnapshotPool::Refusal::None) {
        producerFailure.store((missing?1u:0u)|(!packet?2u:0u)|
            (failed.load(std::memory_order_acquire)?4u:0u)|
            (static_cast<uint32_t>(snapshots->failure())<<8),std::memory_order_release);
        if(packet) {
            auto* program=gs_get_render_target();
            snapshots->submit(packet,static_cast<ID3D11Texture2D*>(gs_texture_get_obj(program)),nullptr,nullptr,
                obs_get_video_frame_time(),obs_get_frame_interval_ns(),false);
        }
        close_observation();failed.store(true,std::memory_order_release);SetEvent(recordStart.value);SetEvent(complete.value);return;
    }
    const auto tick=obs_get_video_frame_time()/obs_get_frame_interval_ns()+1;
    bool same=true;
    for(auto* view:views) {
        gs_texrender_reset(view);
        if(!gs_texrender_begin_with_color_space(view,canvas.width,canvas.height,sourceLinear?GS_CS_709_SCRGB:GS_CS_SRGB)) {
            same=false;break;
        }
        vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);
        gs_ortho(0,static_cast<float>(canvas.width),0,static_cast<float>(canvas.height),-100,100);
        obs_source_video_render(source);gs_texrender_end(view);
        calldata_init(&query);proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&query);
        same=same&&static_cast<uint64_t>(calldata_int(&query,"tick"))==tick;
        calldata_free(&query);
    }
    auto* program=gs_get_render_target();
    snapshots->submit(packet,static_cast<ID3D11Texture2D*>(gs_texture_get_obj(program)),
        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(views[0]))),
        static_cast<ID3D11Texture2D*>(gs_texture_get_obj(gs_texrender_get_texture(views[1]))),
        obs_get_video_frame_time(),obs_get_frame_interval_ns(),same);
}
void Harness::guide(const std::shared_ptr<model::Job>& job,const std::shared_ptr<interop::Context>& context) {
    require(job&&job->ready.load(std::memory_order_acquire)&&job->flow&&job->flow->key==job->contract.key,
        "actual completed guide/job binding");
    const auto resources=job->guide_observation_resources();
    require(resources[0]&&resources[1]&&job->guideGrid>0,"immutable guide snapshots missing");
    const auto current=read_resource(context,job->frame->analysis.Get(),retained);
    const auto previous=read_resource(context,resources[0],retained);
    const auto motion=read_resource(context,resources[1],retained);
    const auto hardware=resources[2]?read_resource(context,resources[2],retained):std::vector<uint8_t>{};
    const auto cost=resources[3]?read_resource(context,resources[3],retained):std::vector<uint8_t>{};
    guide_bytes(job->contract.key,job->guideReferenceKey,job->guideGrid,job->flow->reset,
        current,previous,motion,hardware,
        cost.empty()?qa::GhostImageRows{}:qa::GhostImageRows{cost,cost.size()/resources[3]->GetDesc().Height,
            cost.size()/resources[3]->GetDesc().Height,resources[3]->GetDesc().Height},
        resources[2]?resources[2]->GetDesc():D3D12_RESOURCE_DESC{},
        resources[3]?resources[3]->GetDesc():D3D12_RESOURCE_DESC{});
}
void Harness::guide_bytes(const JobKey& key,const std::optional<JobKey>& referenceKey,uint32_t grid,bool reset,
    std::span<const uint8_t> current,std::span<const uint8_t> previous,std::span<const uint8_t> motion,
    std::span<const uint8_t> hardware,qa::GhostImageRows cost,
    const D3D12_RESOURCE_DESC& desc,const D3D12_RESOURCE_DESC& costDesc,int64_t* hashNs,
    const std::array<identity::Digest,5>* observedHashes) {
    require(current.size()==1920ull*1080*4&&previous.size()==current.size()&&motion.size()==current.size(),"actual guide canvas");
    cost.validate();
    auto pixel=[](std::span<const uint8_t> bytes,unsigned x,unsigned y) {
        const size_t p=(static_cast<size_t>(y)*canvas.width+x)*4;
        return ghost::Rgb{bytes[p+2]/255.0,bytes[p+1]/255.0,bytes[p]/255.0};
    };
    auto* record=obs_data_create();
    obs_data_set_string(record,"identity_digest_algorithm",observedHashes?identity::chunkAlgorithm.data():identity::algorithm.data());
    if(observedHashes)obs_data_set_int(record,"schema",identity::chunkSchema);
    obs_data_set_bool(record,"actual_sdk",!serviceMeasurement);
    obs_data_set_int(record,"ingress",static_cast<int64_t>(key.frame.ingressSequence));
    obs_data_set_int(record,"instance",static_cast<int64_t>(key.frame.instanceId));
    obs_data_set_int(record,"content_epoch",static_cast<int64_t>(key.frame.contentEpoch));
    obs_data_set_int(record,"history",static_cast<int64_t>(key.historyEpoch));
    obs_data_set_int(record,"generation",static_cast<int64_t>(key.resourceGeneration));
    obs_data_set_int(record,"submission",static_cast<int64_t>(key.submissionSerial));
    obs_data_set_int(record,"grid",grid);obs_data_set_bool(record,"reset",reset);
    obs_data_set_bool(record,"reference_present",referenceKey.has_value());
    const auto firstHash=std::chrono::steady_clock::now();
    const auto hashes=observedHashes?
        std::array<identity::Digest,3>{(*observedHashes)[0],(*observedHashes)[1],(*observedHashes)[2]}:
        std::array<identity::Digest,3>{codecDigest.hash(current),codecDigest.hash(previous),codecDigest.hash(motion)};
    if(hashNs)*hashNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-firstHash).count();
    obs_data_set_string(record,"current_hash",identity::hex(hashes[0]).c_str());
    obs_data_set_string(record,"reference_hash",identity::hex(hashes[1]).c_str());
    obs_data_set_string(record,"motion_hash",identity::hex(hashes[2]).c_str());
    if(reset) {
        require(!referenceKey&&hardware.empty()&&cost.empty(),"reset cannot masquerade as native ordinary flow");
        emit(serviceMeasurement?"NR_GHOST_MEASUREMENT_GUIDE ":"NR_GHOST_GUIDE ",record);return;
    }
    require(referenceKey&&!hardware.empty()&&!cost.empty()&&grid>0,"ordinary native guide reference/vector/cost");
    const auto& reference=*referenceKey;
    require(reference.frame.instanceId==key.frame.instanceId&&reference.frame.ingressSequence+1==key.frame.ingressSequence&&
        reference.historyEpoch==key.historyEpoch&&reference.resourceGeneration==key.resourceGeneration,
        "actual immutable current/reference guide keys");
    const unsigned columns=(canvas.width+grid-1)/grid;
    const unsigned gridRows=(canvas.height+grid-1)/grid;
    require(desc.Width==columns&&desc.Height==gridRows&&desc.Format==DXGI_FORMAT_R16G16_SINT&&
        hardware.size()==static_cast<size_t>(columns)*gridRows*4,"actual signed10.5 native cells");
    require(flow_cost_format_valid(costDesc.Format),"queried native cost format");
    require(cost.rows==costDesc.Height&&cost.rowBytes==
        costDesc.Width*(costDesc.Format==DXGI_FORMAT_R8_UINT?1u:4u),"complete actual pitched cost extent");
    obs_data_set_int(record,"reference_ingress",static_cast<int64_t>(reference.frame.ingressSequence));
    obs_data_set_int(record,"cost_format",costDesc.Format);
    const auto secondHash=std::chrono::steady_clock::now();
    const auto hardwareHash=observedHashes?(*observedHashes)[3]:codecDigest.hash(hardware);
    const auto costHash=observedHashes?(*observedHashes)[4]:codecDigest.hash(cost.tight());
    if(hashNs)*hashNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-secondHash).count();
    obs_data_set_string(record,"hardware_hash",identity::hex(hardwareHash).c_str());
    obs_data_set_string(record,"cost_hash",identity::hex(costHash).c_str());
    require(key.frame.ingressSequence>baseIngress,"actual guide inside prescribed episode");
    const unsigned tick=static_cast<unsigned>(key.frame.ingressSequence-baseIngress-1);
    require(tick<256,"ordinary guide episode reference");
    const unsigned referenceTick=reference.frame.ingressSequence<=baseIngress?0u:
        static_cast<unsigned>(reference.frame.ingressSequence-baseIngress-1);
    require(referenceTick<256,"actual guide reference fixture mapping");
    const auto& now=corpus.schedule.at(tick);const auto& before=corpus.schedule.at(referenceTick);
    const double expectedX=now.pan-before.pan;
    for(unsigned y:{128u,192u,320u,512u,704u,896u})for(unsigned x:{256u,320u,512u,768u,1024u,1536u}) {
        uint16_t packedX=0,packedY=0;const size_t at=(static_cast<size_t>(y)*canvas.width+x)*4;
        memcpy(&packedX,motion.data()+at,2);memcpy(&packedY,motion.data()+at+2,2);
        const double vx=DirectX::PackedVector::XMConvertHalfToFloat(packedX);
        const double vy=DirectX::PackedVector::XMConvertHalfToFloat(packedY);
        require(std::isfinite(vx)&&std::isfinite(vy),"nonfinite actual native vectors");
        const double gx=std::clamp((x+.5)/grid-.5,0.0,static_cast<double>(columns-1));
        const double gy=std::clamp((y+.5)/grid-.5,0.0,static_cast<double>(gridRows-1));
        const unsigned ax=static_cast<unsigned>(std::floor(gx)),ay=static_cast<unsigned>(std::floor(gy));
        const unsigned bx=std::min(ax+1,columns-1),by=std::min(ay+1,gridRows-1);
        auto cell=[&](unsigned cx,unsigned cy,unsigned component) {
            int16_t value=0;memcpy(&value,hardware.data()+(static_cast<size_t>(cy)*columns+cx)*4+component*2,2);
            return static_cast<double>(value)/32;
        };
        auto interpolate=[&](unsigned component) {
            const double top=cell(ax,ay,component)*(1-(gx-ax))+cell(bx,ay,component)*(gx-ax);
            const double bottom=cell(ax,by,component)*(1-(gx-ax))+cell(bx,by,component)*(gx-ax);
            return top*(1-(gy-ay))+bottom*(gy-ay);
        };
        require(packedX==DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(interpolate(0)))&&
            packedY==DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(interpolate(1))),
            "SDK sign/1-32 decode/single working-rectangle scale hard assertion");
        std::array<bool,4> support{};size_t supportIndex=0;
        for(auto cy:{ay,by})for(auto cx:{ax,bx}) {
            const double px=(cx+.5)*grid-.5,py=(cy+.5)*grid-.5;
            support[supportIndex++]=px>=0&&py>=0&&px+expectedX>=0&&px+expectedX<canvas.width&&
                py<canvas.height&&alpha(static_cast<unsigned>(px),static_cast<unsigned>(py))>0;
        }
        if(!ghost::valid_support(support)){++measured.invalidSupport;continue;}
        const double wx=x+vx,wy=y+vy;
        if(wx<0||wy<0||wx>=canvas.width-1||wy>=canvas.height-1){++measured.invalidSupport;continue;}
        const unsigned left=static_cast<unsigned>(std::floor(wx)),top=static_cast<unsigned>(std::floor(wy));
        const double fx=wx-left,fy=wy-top;
        const auto a=pixel(previous,left,top),b=pixel(previous,left+1,top),c=pixel(previous,left,top+1),d=pixel(previous,left+1,top+1);
        const ghost::Rgb warped{(a.r*(1-fx)+b.r*fx)*(1-fy)+(c.r*(1-fx)+d.r*fx)*fy,
            (a.g*(1-fx)+b.g*fx)*(1-fy)+(c.g*(1-fx)+d.g*fx)*fy,
            (a.b*(1-fx)+b.b*fx)*(1-fy)+(c.b*(1-fx)+d.b*fx)*fy};
        const auto present=pixel(current,x,y);
        const ghost::GuidePoint point{present,warped,vx,vy,expectedX,0,true};
        const std::array<ghost::GuidePoint,1> cohort{point};const auto value=ghost::calibrate_guides(cohort);
        measured.guideMismatch=std::max(measured.guideMismatch,value.mismatch);
        measured.absoluteVector=std::max(measured.absoluteVector,value.absoluteError);
        measured.relativeVector=std::max(measured.relativeVector,value.relativeError);++measured.guidePoints;
    }
    if(!serviceMeasurement)++measured.nativeGuides;
    obs_data_set_double(record,"maximum_visible_mismatch",measured.guideMismatch);
    obs_data_set_double(record,"maximum_absolute_vector_error",measured.absoluteVector);
    obs_data_set_double(record,"maximum_relative_vector_error",measured.relativeVector);
    obs_data_set_int(record,"labeled_points",static_cast<int64_t>(measured.guidePoints));
    obs_data_set_int(record,"invalid_support",static_cast<int64_t>(measured.invalidSupport));
    obs_data_set_bool(record,"sign_units_rectangle_asserted",true);
    emit(serviceMeasurement?"NR_GHOST_MEASUREMENT_GUIDE ":"NR_GHOST_GUIDE ",record);
}
bool scene_identity(const ghost::Fixture& f,unsigned tick,const ghost::Image& pixels) {
    if(ghost::decode_content(pixels,canvas)!=f.schedule.at(tick).contentId)return false;
    const auto clean=ghost::clean_image(f,tick);
    const double bound=f.color==ghost::FixtureColor::DarkSdr?.125*.0625:.125;
    for(unsigned y=20;y<68;y+=4)for(unsigned x=2;x<192;x+=4) {
        const auto& a=pixels[y*canvas.width+x];const auto& b=clean[y*192+x];
        if(std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),std::abs(a.b-b.b)})>bound)return false;
    }
    return true;
}
void Harness::legacy_sample() {
    if(!observing.load(std::memory_order_acquire)||!bound||failed)return;
    const auto started=std::chrono::steady_clock::now();
    std::array<int64_t,11> timing{};
    auto mark=[&](size_t stage) {
        timing[stage]=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
    };
    FrameRow row{};
    std::shared_ptr<model::Job> currentJob;
    try {
        require(rows.size()<258,"bounded physical episode plus publication tail");
        calldata_t query;calldata_init(&query);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&query),"actual canonical sampler");
        row.tick=static_cast<uint64_t>(calldata_int(&query,"tick"));
        row.clock=static_cast<uint64_t>(calldata_int(&query,"clock"));row.interval=static_cast<uint64_t>(calldata_int(&query,"interval"));
        row.ingress=static_cast<uint64_t>(calldata_int(&query,"ingress"));row.selected=static_cast<uint64_t>(calldata_int(&query,"selected"));
        calldata_free(&query);
        std::shared_ptr<const obs_adapter::Capture> raw,current;
        std::shared_ptr<model::Job> selectedJob;
        std::shared_ptr<interop::Context> context;
        calldata_init(&query);calldata_set_ptr(&query,"retain_raw",&raw);calldata_set_ptr(&query,"retain_current_raw",&current);
        calldata_set_ptr(&query,"retain_selected_job",&selectedJob);calldata_set_ptr(&query,"retain_context",&context);
        calldata_set_ptr(&query,"retain_job",&currentJob);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"actual immutable native observations");
        require(!calldata_bool(&query,"failed")&&raw&&current&&context,"native fallback/error cannot establish physical model proof");
        auto* selectedTexture=static_cast<gs_texture_t*>(calldata_ptr(&query,"output_texture"));
        row.kind=static_cast<unsigned>(calldata_int(&query,"kind"));calldata_free(&query);
        mark(0);
        if(row.tick!=row.clock/row.interval+1||row.ingress!=current->input.key.ingressSequence||
            raw->input.key.ingressSequence!=row.selected)
            throw std::runtime_error("canonical/raw binding tick="+std::to_string(row.tick)+
                " clock="+std::to_string(row.clock)+" interval="+std::to_string(row.interval)+
                " ingress="+std::to_string(row.ingress)+" current="+std::to_string(current->input.key.ingressSequence)+
                " selected="+std::to_string(row.selected)+" raw="+std::to_string(raw->input.key.ingressSequence)+
                " kind="+std::to_string(row.kind));
        require(row.kind==static_cast<unsigned>(OutputKind::PrimingRaw)?
            row.selected==row.ingress:row.selected+1==row.ingress,"OFF/priming current versus exact n-1 selection");
        if(!rows.empty())require(row.tick==rows.back().tick+1&&row.ingress==rows.back().ingress+1,
            "physical missing/reordered canonical emission");
        const auto sourceBytes=nr_bootstrap_read_texture(current->original11.Get(),sourceLinear);
        mark(1);
        const auto rawBytes=nr_bootstrap_read_texture(raw->original11.Get(),sourceLinear);
        mark(2);
        const auto selectedBytes=nr_bootstrap_read_texture(
            static_cast<ID3D11Texture2D*>(gs_texture_get_obj(selectedTexture)),sourceLinear);
        mark(3);
        auto sourceImage=image::rgb_image(sourceBytes,sourceLinear,false,"current immutable ingress");
        auto rawImage=image::rgb_image(rawBytes,sourceLinear,false,"selected immutable raw");
        auto selectedImage=image::rgb_image(selectedBytes,sourceLinear,false,"selected native output");
        auto sourceRgb=canonical(sourceImage,corpus,sourceLinear,80,false,!sourceLinear);
        auto rawRgb=canonical(rawImage,corpus,sourceLinear,80,false,!sourceLinear);
        auto selectedRgb=canonical(selectedImage,corpus,sourceLinear,80,false,!sourceLinear);
        mark(4);
        row.sourceTick=static_cast<unsigned>(row.selected-1);row.content=corpus.schedule.at(std::min(row.sourceTick,255u)).contentId;
        require(scene_identity(corpus,std::min(row.sourceTick,255u),rawRgb)&&
            scene_identity(corpus,std::min(row.sourceTick,255u),selectedRgb),"independent raw/selected source IDs and scene geometry");
        row.rawHash=codecDigest.hash(rawBytes);row.selectedHash=codecDigest.hash(selectedBytes);
        if(deterministic)require(row.rawHash==row.selectedHash,"production deterministic transport changed source pixels");
        if(row.ingress<=256) {
            const auto expected=analytic(corpus,static_cast<unsigned>(row.ingress-1));
            require(ghost::transport(expected,sourceRgb,measured.formatBound),"actual raw/color format admission failed");
            for(size_t p=0;p<expected.size();++p) {
                const auto a=sourceRgb[p],b=expected[p];
                measured.rawMaximum=std::max(measured.rawMaximum,std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),std::abs(a.b-b.b)}));
            }
            ++measured.rawSamples;
        }
        for(size_t p=0;p<rawImage.pixels.size();++p)if(rawImage.pixels[p][3]!=selectedImage.pixels[p][3])++measured.alphaErrors;
        require(measured.alphaErrors==0,"actual source/filter alpha differs");
        mark(5);
        if(!deterministic&&row.kind==static_cast<unsigned>(OutputKind::DelayedNr)) {
            require(selectedJob&&selectedJob->ready.load(std::memory_order_acquire)&&selectedJob->published.load()&&
                selectedJob->eligible&&selectedJob->evaluated&&selectedJob->composed&&selectedJob->submitted&&
                !selectedJob->invalid&&selectedJob->runtimeModule&&selectedJob->contract.key.frame==raw->input.key,
                "actual same-input completed Feature18 publication");
            row.job.emplace(selectedJob->contract.key);
            const auto resources=selectedJob->color_resources();
            std::array<identity::Digest,5> hashes{};
            std::array<image::Image,2> bounded;
            for(size_t role=0;role<resources.size();++role) {
                require(resources[role]!=nullptr,"actual model-stage resource");
                const auto bytes=read_resource(context,resources[role],retained);hashes[role]=codecDigest.hash(bytes);
                if(role==0||role==4)bounded[role==0?0:1]=image::rgb_image(bytes,true,false,"actual canonical/bounded resolve");
            }
            require(hashes[1]!=hashes[3],"actual encoded model input/output no-op");
            const auto canonicalRaw=canonical(bounded[0],corpus,true),resolved=canonical(bounded[1],corpus,true);
            ghost::Image delta(canonicalRaw.size());
            for(size_t p=0;p<delta.size();++p)delta[p]={resolved[p].r-canonicalRaw[p].r,
                resolved[p].g-canonicalRaw[p].g,resolved[p].b-canonicalRaw[p].b};
            const double epsilon=frozen?std::max(frozen->formatBound,frozen->rawMaximum):0;
            const auto staticEdit=ghost::edit(rawRgb,selectedRgb,delta,roi(corpus,row.sourceTick,false,policy),epsilon);
            const auto movingEdit=ghost::edit(rawRgb,selectedRgb,delta,roi(corpus,row.sourceTick,true,policy),epsilon);
            if(staticEdit.rms>measured.staticRms){measured.staticRms=staticEdit.rms;measured.staticRetention=staticEdit.retained;}
            if(movingEdit.rms>measured.movingRms){measured.movingRms=movingEdit.rms;measured.movingRetention=movingEdit.retained;}
            ++measured.modelSamples;guide(selectedJob,context);
            auto* stages=obs_data_create();
            obs_data_set_string(stages,"identity_digest_algorithm",identity::algorithm.data());
            obs_data_set_int(stages,"source_tick",row.sourceTick);
            for(size_t role=0;role<hashes.size();++role)
                obs_data_set_string(stages,("color_"+std::to_string(role)+"_hash").c_str(),identity::hex(hashes[role]).c_str());
            obs_data_set_bool(stages,"D_before_g",true);obs_data_set_int(stages,"settings_revision",raw->input.settings.revision.value);
            obs_data_set_double(stages,"static_edit_rms",staticEdit.rms);obs_data_set_double(stages,"moving_edit_rms",movingEdit.rms);
            obs_data_set_double(stages,"static_retention",staticEdit.retained);obs_data_set_double(stages,"moving_retention",movingEdit.retained);
            emit("NR_GHOST_MODEL_STAGES ",stages);
        }
        mark(6);
        auto* program=gs_get_render_target();const auto format=gs_texture_get_color_format(program);
        const bool bgra=format==GS_BGRA||format==GS_BGRA_UNORM;
        const auto programBytes=nr_bootstrap_read_texture(static_cast<ID3D11Texture2D*>(gs_texture_get_obj(program)),hdrOutput);
        mark(7);
        auto programImage=image::rgb_image(programBytes,hdrOutput,bgra,"actual program");
        const auto programRgb=canonical(programImage,corpus,hdrOutput,
            (hdrOutput||sourceLinear)?300:80,false,!hdrOutput);
        require(scene_identity(corpus,std::min(row.sourceTick,255u),programRgb),"actual program independent content/geometry");
        row.programHash=codecDigest.hash(programBytes);
        mark(8);
        if(pictureArmed.load(std::memory_order_acquire)&&!sourcePicture) {
            pictureContent=row.content;sourcePicture.emplace(std::move(rawImage));
            selectedPicture.emplace(std::move(selectedImage));programPicture.emplace(std::move(programImage));
        }
        if(row.sourceTick<256) {
            const auto read=[](const void* value,unsigned x,unsigned y) {
                return (*static_cast<const ghost::Image*>(value))[static_cast<size_t>(y)*canvas.width+x];
            };
            score(row.sourceTick,ghost::PixelView{&rawRgb,canvas,read},ghost::PixelView{&selectedRgb,canvas,read});
        }
        mark(9);
        rows.push_back(row);
        auto* record=obs_data_create();
        for(const auto& field:std::initializer_list<std::pair<const char*,uint64_t>>{
            {"tick",row.tick},{"clock",row.clock},{"interval",row.interval},{"ingress",row.ingress},{"selected",row.selected},
            {"source_tick",row.sourceTick},{"content_id",row.content},{"kind",row.kind},{"width",1920},{"height",1080},
            {"instance",raw->input.key.instanceId},{"content_epoch",raw->input.key.contentEpoch},
            {"generation",raw->input.resourceGeneration},{"alpha_errors",measured.alphaErrors}})
            obs_data_set_int(record,field.first,static_cast<int64_t>(field.second));
        obs_data_set_bool(record,"source_linear",sourceLinear);obs_data_set_bool(record,"hdr_output",hdrOutput);
        obs_data_set_bool(record,"deterministic",deterministic);obs_data_set_double(record,"source_white",80);
        obs_data_set_double(record,"fixture_white",corpus.referenceWhiteNits);obs_data_set_double(record,"exposure",1);
        obs_data_set_string(record,"identity_digest_algorithm",identity::algorithm.data());
        obs_data_set_string(record,"raw_hash",identity::hex(row.rawHash).c_str());
        obs_data_set_string(record,"selected_hash",identity::hex(row.selectedHash).c_str());
        obs_data_set_string(record,"program_hash",identity::hex(row.programHash).c_str());
        emit("NR_GHOST_FRAME ",record);
        if(row.sourceTick==255)observing.store(false,std::memory_order_release);
    }catch(const std::exception& error) {
        failed=true;failure=error.what();observing.store(false,std::memory_order_release);
    }
    mark(10);
    auto* record=obs_data_create();
    obs_data_set_int(record,"tick",static_cast<int64_t>(row.tick));
    obs_data_set_int(record,"ingress",static_cast<int64_t>(row.ingress));
    obs_data_set_int(record,"selected",static_cast<int64_t>(row.selected));
    obs_data_set_int(record,"interval_ns",static_cast<int64_t>(row.interval));
    obs_data_set_bool(record,"failed",failed);
    for(size_t i=0;i<timing.size();++i)
        obs_data_set_int(record,("elapsed_stage_"+std::to_string(i)+"_ns").c_str(),timing[i]);
    emit("NR_GHOST_CALLBACK_TIMING ",record);
    worker_timing(currentJob);
    currentJob.reset(); // No diagnostic caller binding survives a stop trigger.
    if(failed||row.ingress>=48)SetEvent(recordStart.value);
    if(failed||row.sourceTick==255)SetEvent(complete.value);
}
void action(void* opaque,uint32_t,uint32_t) {
    auto& h=*static_cast<Harness*>(opaque);const auto next=h.action.exchange(Action::None);
    if(next==Action::None)return;
    try {
        if(next==Action::Bind){obs_set_output_source(0,obs_scene_get_source(h.scene));h.bound=true;}
        else if(next==Action::Prepare)h.prepare();
        else if(next==Action::Arm)h.arm();
        else if(next==Action::Off){h.close_observation();h.offRequested.store(true,std::memory_order_release);}
        else if(next==Action::CloseObservation)h.close_observation();
        else if(next==Action::Release)h.release();else h.stop();
    }catch(const std::exception& error){h.failed=true;h.failure=error.what();}
    SetEvent(h.acted.value);
}
void rendered(void* opaque){static_cast<Harness*>(opaque)->sample();}
void preencode(void* opaque,video_data* data) {
    auto& h=*static_cast<Harness*>(opaque);std::lock_guard lock(h.rawMutex);
    if(h.preencode.size()>=40){h.rawFailed.store(true,std::memory_order_release);return;}
    nr::qa::DecodedFrame frame{};frame.width=canvas.width;frame.height=canvas.height;frame.timestamp=data->timestamp;
    frame.format=h.hdrOutput?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;frame.transfer=h.hdrOutput?VIDEO_TRC_PQ:VIDEO_TRC_DEFAULT;
    frame.fullRange=false;frame.rowBytes={canvas.width*(h.hdrOutput?2u:1u),canvas.width*(h.hdrOutput?2u:1u),0};
    frame.rows={canvas.height,canvas.height/2,0};
    for(size_t p=0;p<2;++p) {
        if(!data->data[p]||data->linesize[p]<frame.rowBytes[p]){h.rawFailed.store(true,std::memory_order_release);return;}
        frame.planes[p].resize(static_cast<size_t>(frame.rowBytes[p])*frame.rows[p]);
        for(uint32_t y=0;y<frame.rows[p];++y)memcpy(frame.planes[p].data()+static_cast<size_t>(y)*frame.rowBytes[p],
            data->data[p]+static_cast<size_t>(y)*data->linesize[p],frame.rowBytes[p]);
    }
    h.preencode.push_back(std::move(frame));
}
void stop_graph(Harness& h) {
    h.observing.store(false,std::memory_order_release);h.execute(Action::CloseObservation);
    if(h.snapshots) {
        require(h.snapshots->stop(),"actual QA producer/copy/drain retirement unresolved");
        h.codecCredits=h.snapshots->credit_ledger();
        h.snapshots.reset(); // Detached evidence/source/analysis/context leases release before model/OFA stop.
    }
    h.execute(Action::Stop);
    if(h.stoppingJob&&!h.stoppingJob->ready.load(std::memory_order_acquire)) {
        require(WaitForSingleObject(h.stoppingJob->observed,30000)==WAIT_OBJECT_0,"real pending model completion");
        h.stoppingJob.reset();h.execute(Action::Stop);
    }
    h.consumerDone.wait();h.execute(Action::Stop);
    if(h.exited)require(h.exited->wait_for(30000),"pre-subscribed actual module-owned worker exit");
    h.execute(Action::Stop);require(h.closed,"real input-before-session/native retirement");
    auto* receipt=obs_data_create();obs_data_set_bool(receipt,"closed",h.closed);
    obs_data_set_bool(receipt,"worker_exit",!h.exited||h.exited->ready());
    obs_data_set_bool(receipt,"consumer_event",WaitForSingleObject(h.consumerDone.value,0)==WAIT_OBJECT_0);
    obs_data_set_bool(receipt,"retained",h.retained);emit("NR_GHOST_CLEANUP ",receipt);
    nr::qa::release_after_final_close(h.exited,h.closed); // The exit-signal hold ends with the final Close confirmation.
    h.execute(Action::Release);
}
// A lane whose exact encoder stop stayed unresolved keeps its whole graph: the raw
// registration, the scene/source owners and the Harness itself outlive this call.
std::vector<std::unique_ptr<Harness>> retainedGhostGraphs;
uint64_t plane_errors(const nr::qa::DecodedFrame& decoded,const nr::qa::DecodedFrame& before) {
    uint64_t errors=0;
    const bool planar=decoded.format==VIDEO_FORMAT_I420||decoded.format==VIDEO_FORMAT_I010;
    for(unsigned y=0;y<canvas.height;++y)for(unsigned x=0;x<canvas.width;++x)
        if(image::code(decoded,0,x,y)!=image::code(before,0,x,y))++errors;
    for(unsigned y=0;y<canvas.height/2;++y)for(unsigned x=0;x<canvas.width/2;++x)for(size_t c=0;c<2;++c)
        if(image::code(decoded,planar?c+1:1,planar?x:x*2+static_cast<unsigned>(c),y)!=
            image::code(before,1,x*2+static_cast<unsigned>(c),y))++errors;
    return errors;
}
void codec_digest(Harness& h,const nr::qa::DecodedFrame& pixels,const FrameRow& row,const char* role,size_t index) {
    require((h.closed||(h.serviceMeasurement&&h.fixturePoolRetired))&&!h.snapshots&&h.digestLane,
        "codec has only retired native or fixture lane/code, not live pool/input owners");
    std::array<std::span<const uint8_t>,3> parts{};uint64_t bytes=0;
    for(size_t plane=0;plane<parts.size();++plane) {
        require(pixels.planes[plane].size()==static_cast<size_t>(pixels.rowBytes[plane])*pixels.rows[plane],
            "complete actual codec logical plane extent");
        parts[plane]=pixels.planes[plane];bytes+=parts[plane].size();
    }
    require(bytes>0&&row.selectedFrame.has_value(),"actual codec payload/full source key");
    require(h.codecCredits!=nullptr,"capture ledger saved before pool retirement");
    const auto token=h.codecCredits->reserve(*row.selectedFrame,row.generation);
    require(token.has_value(),"codec uses the same three capture credits");
    const auto creditStarted=std::chrono::steady_clock::now();
    if(!h.codec)h.codec=std::make_unique<GhostCodecDigest>(h.digestLane,h.hdrOutput,h.codecCredits);
    identity::Timing timing;identity::Digest digest{};
    try{digest=h.codec->hash(parts,h.codecDigest,timing);}
    catch(...){if(h.codec->pending){h.retained=true;(void)h.codec.release();}throw;}
    qa::GhostDigestRecord record;record.object();
    record.text("identity_digest_algorithm",identity::chunkAlgorithm);record.number("schema",identity::chunkSchema);
    record.text("digest_mechanism","gpu_exact_chunks_cpu_ordered_root");record.text("role",role);record.number("index",index);
    record.boolean("isolated_fixture",h.serviceMeasurement);
    record.boolean("independent_recording_decode",!h.serviceMeasurement);
    record.digest("digest",digest);record.number("bytes",bytes);record.number("timestamp",pixels.timestamp);
    record.number("source_clock",row.clock);record.number("tick",row.tick);record.number("width",pixels.width);
    record.number("height",pixels.height);record.number("format",pixels.format);record.number("transfer",pixels.transfer);
    const char* format=pixels.format==VIDEO_FORMAT_NV12?"nv12":pixels.format==VIDEO_FORMAT_P010?"p010":
        pixels.format==VIDEO_FORMAT_I420?"i420":pixels.format==VIDEO_FORMAT_I010?"i010":nullptr;
    require(format!=nullptr,"actual codec digest format");record.text("format_tag",format);
    record.text("transfer_tag",pixels.transfer==VIDEO_TRC_PQ?"pq":"sdr");record.boolean("full_range",pixels.fullRange);
    record.text("primaries",h.hdrOutput?"rec2020":"rec709");record.number("settings_revision",row.settingsRevision);
    record.number("resource_generation",row.generation);digest_frame_key(record,"frame_key",*row.selectedFrame);
    digest_job_key(record,"job_key",row.job);record.array("planes");
    for(size_t plane=0;plane<parts.size();++plane) {
        record.array_object();record.number("index",plane);record.number("row_bytes",pixels.rowBytes[plane]);
        record.number("rows",pixels.rows[plane]);record.number("bytes",parts[plane].size());record.end_object();
    }
    record.end_array();record.number("hash_ns",timing.hashNs);record.number("finish_ns",timing.finishNs);
    record.number("upload_capacity",h.codec->capacities[0]);record.number("leaf_gpu_capacity",h.codec->capacities[1]);
    record.number("leaf_readback_capacity",h.codec->capacities[2]);record.boolean("conversion_temporaries_live",false);
    record.boolean("maps_closed",true);record.boolean("fence_complete",true);record.end_object();
    qa::emit_ghost_digest(std::cout,"NR_GHOST_CODEC_DIGEST ",record);
    const auto done=h.codec->completion_values();
    require(h.codecCredits->release(*token,{done[0],done[1],done[0],done[1],true,true,true,true}),
        "codec actual completion maps consumers and serialization before same credit release");
    if(h.serviceMeasurement) {
        auto* service=obs_data_create();obs_data_set_string(service,"role",role);
        obs_data_set_int(service,"serial",static_cast<int64_t>(token->serial));
        obs_data_set_int(service,"reserve_to_release_including_serialization_ns",
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-creditStarted).count());
        obs_data_set_int(service,"occupied_after_release",h.codecCredits->occupied());
        obs_data_set_bool(service,"credit_released",true);emit("NR_GHOST_CODEC_SERVICE ",service);
    }
}
void decoded(Harness& h,const std::vector<nr::qa::PacketReceipt>& packets) {
    require(!h.rawFailed.load(std::memory_order_acquire)&&packets.size()==32,"actual bounded encoder/preencode packet window");
    const auto frames=nr::qa::decode_recording(h.directory/"program.hevc",h.hdrOutput,static_cast<uint32_t>(packets.size()),h.rate);
    require(frames.size()==packets.size(),"actual independent decode count");
    size_t first=h.preencode.size(),anchors=0;
    for(size_t i=0;i<h.preencode.size();++i)if(plane_errors(frames.front(),h.preencode[i])==0){first=i;++anchors;}
    require(anchors==1&&first+frames.size()<=h.preencode.size(),"first actual packet missing/ambiguous preencode anchor");
    unsigned holdCount=0;
    for(size_t i=0;i<frames.size();++i) {
        const auto& frame=frames[i];const auto& before=h.preencode[first+i];
        require(packets[i].timebaseNumerator>0&&packets[i].timebaseDenominator>0&&
            static_cast<uint64_t>(packets[i].timebaseNumerator)*h.rate.numerator==
                static_cast<uint64_t>(packets[i].timebaseDenominator)*h.rate.denominator,
            "actual encoder packet timebase differs from declared rational stream");
        require(packets[i].pts==packets.front().pts+static_cast<int64_t>(i)&&
            packets[i].dts==packets.front().dts+static_cast<int64_t>(i),"actual packet missing/reordered rational timestamp");
        require(before.timestamp>=h.preencode[first].timestamp&&frame.timestamp>=frames.front().timestamp&&
            h.rate.timestamp_delta(before.timestamp-h.preencode[first].timestamp,static_cast<uint32_t>(i))&&
            h.rate.timestamp_delta(frame.timestamp-frames.front().timestamp,static_cast<uint32_t>(i)),
            "actual preencode/decoder timestamps differ from declared rational cadence");
        const auto errors=plane_errors(frame,before);
        h.measured.codecErrors+=errors;++h.measured.codecFrames;
        require(errors==0,"actual lossless decoder/transport code mismatch");
        const auto row=std::find_if(h.rows.begin(),h.rows.end(),[&](const FrameRow& value){return value.clock==before.timestamp;});
        require(row!=h.rows.end(),"actual preencode timestamp lacks canonical production selection");
        {
            const auto rgb=image::decoded_image(frame,h.hdrOutput);
            const BorrowedCanonicalPixels linearRgb{rgb,h.corpus,h.hdrOutput,
                (h.sourceLinear&&!h.hdrOutput)?300.0:80.0};
            require(linearRgb.identity(row->sourceTick),"independently decoded identity/geometry disagrees with exact selection");
            if(row->sourceTick>=64&&row->sourceTick<80)++holdCount;
            if(row->content==h.pictureContent)image::save_bmp(h.directory/"decoded.bmp",rgb);
            require(rgb.pixels.capacity()*sizeof(rgb.pixels.front())==33177600,"actual decoded RGB capacity phase recipe");
        } // Complete image/identity/BMP consumers retire before any codec upload.
        codec_digest(h,before,*row,"preencode",i);codec_digest(h,frame,*row,"decoded",i);
        h.codec.reset(); // No upload/leaf/readback owner overlaps the next conversion or BMP.
        auto* record=obs_data_create();obs_data_set_int(record,"index",static_cast<int64_t>(i));
        obs_data_set_int(record,"content_id",row->content);obs_data_set_int(record,"selected",static_cast<int64_t>(row->selected));
        obs_data_set_int(record,"tick",static_cast<int64_t>(row->tick));
        obs_data_set_int(record,"preencode_timestamp",static_cast<int64_t>(before.timestamp));
        obs_data_set_int(record,"packet_pts",packets[i].pts);obs_data_set_int(record,"packet_dts",packets[i].dts);
        obs_data_set_int(record,"timebase_num",packets[i].timebaseNumerator);
        obs_data_set_int(record,"timebase_den",packets[i].timebaseDenominator);
        obs_data_set_int(record,"stream_rate_num",h.rate.numerator);obs_data_set_int(record,"stream_rate_den",h.rate.denominator);
        obs_data_set_int(record,"decoder_timestamp",static_cast<int64_t>(frame.timestamp));
        obs_data_set_int(record,"decoder_observation_fps",h.rate.decoder_observation_fps());
        obs_data_set_bool(record,"packet_preencode_decoder_rational_order",true);
        obs_data_set_int(record,"format",frame.format);obs_data_set_int(record,"transfer",frame.transfer);
        obs_data_set_int(record,"bit_depth",h.hdrOutput?10:8);obs_data_set_bool(record,"full_range",frame.fullRange);
        obs_data_set_int(record,"codes_compared",3110400);obs_data_set_int(record,"code_errors",0);
        emit("NR_GHOST_DECODED ",record);
    }
    require(holdCount==16,"recorded sixteen-tick identical hold not covered");
    h.codec.reset();h.digestLane.reset();
    std::osyncstream(std::cout)<<"NR_GHOST_CODEC_CLOSED uploads=0 leaf_buffers=0 readbacks=0 maps=0 queues=0 contexts=0 code=0 model_input_owners=0 normal_release=true"<<std::endl;
}
}

bool nr_ghost_complete_service(const std::filesystem::path& shaderPath,const std::filesystem::path& directory) {
    // This entry precedes libobs startup/module loading. It drives the existing
    // drain and CPU consumers, not a model, SDK, filter graph or native stream.
    try {
        require(shaderPath.filename()=="nr_ghost_chunk.dxil"&&
            directory.filename().string().starts_with("isolated-service-attempt-")&&
            std::filesystem::is_directory(directory)&&std::filesystem::is_empty(directory),
            "private complete service evidence boundary");
        const LUID expected{398289484,1};
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter3> adapter;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"service private factory");
        checked(factory->EnumAdapterByLuid(expected,IID_PPV_ARGS(&adapter)),"service admitted adapter");
        ComPtr<ID3D11Device> renderer;ComPtr<ID3D11DeviceContext> immediate;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};D3D_FEATURE_LEVEL level{};
        checked(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,1,D3D11_SDK_VERSION,
            &renderer,&level,&immediate),"service private renderer");
        std::array<uint64_t,3> high{};
        std::mutex sampleMutex;
        const auto sample=[&](const char* phase) {
            std::lock_guard lock(sampleMutex);
            DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};PROCESS_MEMORY_COUNTERS_EX cpu{};cpu.cb=sizeof(cpu);
            checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local),"service local sample");
            checked(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal),"service nonlocal sample");
            require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&cpu),sizeof(cpu))!=FALSE,
                "service same process private sample");
            high[0]=std::max(high[0],local.CurrentUsage);high[1]=std::max(high[1],nonlocal.CurrentUsage);
            high[2]=std::max(high[2],static_cast<uint64_t>(cpu.PrivateUsage));
            auto* row=obs_data_create();obs_data_set_string(row,"phase",phase);
            obs_data_set_int(row,"pid",GetCurrentProcessId());
            obs_data_set_int(row,"local_sample",static_cast<int64_t>(local.CurrentUsage));
            obs_data_set_int(row,"nonlocal_sample",static_cast<int64_t>(nonlocal.CurrentUsage));
            obs_data_set_int(row,"private_sample",static_cast<int64_t>(cpu.PrivateUsage));
            obs_data_set_bool(row,"sample_not_capacity_or_unique_union",true);
            obs_data_set_string(row,"vendor_model_terms","excluded_not_loaded_not_established_zero");
            emit("NR_GHOST_MEASUREMENT_MEMORY ",row);
        };
        sample("renderer_baseline");
        bool allViable=true;uint64_t lanes=0,captured=0,codecFrames=0;
        constexpr std::array<qa::StreamRate,2> rates{{{60,1},{60000,1001}}};
        for(const auto rate:rates)for(const auto color:{ghost::FixtureColor::Sdr,ghost::FixtureColor::Hdr709}) {
            const bool hdr=color==ghost::FixtureColor::Hdr709;
            const auto laneDirectory=directory/(std::string(hdr?"hdr-":"sdr-")+std::to_string(rate.numerator));
            std::filesystem::create_directory(laneDirectory);
            auto h=std::make_unique<Harness>(shaderPath.parent_path(),laneDirectory,0xA17,color,hdr,false,false,nullptr,rate);
            h->serviceMeasurement=true;h->baseIngress=0;h->serviceSample=sample;
            auto context=interop::Context::create(renderer.Get());
            require(interop::same_luid(context->luid(),expected),"service single same-adapter lane");
            std::ifstream file(shaderPath,std::ios::binary|std::ios::ate);
            require(file.good()&&file.tellg()>0,"service fresh bound shader");
            std::vector<std::byte> shader(static_cast<size_t>(file.tellg()));file.seekg(0);
            file.read(reinterpret_cast<char*>(shader.data()),static_cast<std::streamsize>(shader.size()));
            require(file.good(),"service complete shader bytes");
            h->digestLane=std::make_shared<GhostDigestLane>(context,shader);
            std::vector<std::byte>{}.swap(shader);auto* device=context->device();
            const auto texture=[](DXGI_FORMAT format) {
                D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width=canvas.width;d.Height=canvas.height;d.DepthOrArraySize=d.MipLevels=1;
                d.Format=format;d.SampleDesc.Count=1;return d;
            };
            const auto vectors=texture(DXGI_FORMAT_R16G16_SINT),cost=texture(DXGI_FORMAT_R8_UINT);
            std::array<std::shared_ptr<model::QaSignal>,3> signals;
            for(auto& signal:signals)signal=model::make_qa_signal();
            std::atomic<uint64_t> retired{0};
            std::atomic<int64_t> maximumCredit{0},maximumCycle{0};
            const auto maximum=[](std::atomic<int64_t>& value,int64_t next) {
                auto prior=value.load();
                while(prior<next&&!value.compare_exchange_weak(prior,next)) {}
            };
            h->snapshots=std::make_shared<SnapshotPool>(h->digestLane,renderer.Get(),
                hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM,
                hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM,vectors,cost,0,signals,
                [&](SnapshotPool::Envelope& packet){h->consume(packet);},hdr,hdr,
                static_cast<uint32_t>(color),h->corpus.referenceWhiteNits,h->codecDigest,
                [&](uint64_t,int64_t credit,int64_t,int64_t cycle) {
                    const auto begin=std::chrono::steady_clock::now();sample("postrelease_callback");
                    maximum(maximumCredit,credit);
                    maximum(maximumCycle,cycle+std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()-begin).count());retired.fetch_add(1);
                });
            h->codecCredits=h->snapshots->credit_ledger();
            h->snapshots->episode_base(0);
            const auto capacities=h->snapshots->bytes();
            auto* capacity=obs_data_create();
            obs_data_set_bool(capacity,"hdr",hdr);obs_data_set_int(capacity,"rate_num",rate.numerator);
            obs_data_set_int(capacity,"rate_den",rate.denominator);obs_data_set_int(capacity,"credits",3);
            obs_data_set_int(capacity,"gpu_unique_capacity",static_cast<int64_t>(capacities.gpu));
            obs_data_set_int(capacity,"image_readback_unique_capacity",static_cast<int64_t>(capacities.readback));
            obs_data_set_int(capacity,"leaf_readback_unique_capacity",static_cast<int64_t>(capacities.leafReadback));
            obs_data_set_int(capacity,"compact_cpu_capacity",static_cast<int64_t>(capacities.packed));
            obs_data_set_int(capacity,"added_metadata_upper",SnapshotPool::added_metadata_capacity());
            obs_data_set_bool(capacity,"shared_graphics_aliases_counted_once",true);
            obs_data_set_bool(capacity,"vendor_and_driver_opaque_terms_included",false);
            emit("NR_GHOST_MEASUREMENT_CAPACITY ",capacity);sample("pool_prepared");
            {
                // Frozen hold ticks 71..76 have identical pixels but distinct
                // current/reference/selection keys for every arriving envelope.
                auto raw=image::ghost_fixture_bytes(h->corpus,72,hdr);
                auto half=image::ghost_fixture_bytes(h->corpus,72,true);
                if(!hdr)for(size_t p=0;p<raw.size()/4;++p)for(size_t c=0;c<4;++c) {
                    const auto value=DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(raw[p*4+c])/255);
                    memcpy(half.data()+p*8+c*2,&value,2);
                }
                auto foreign=half;foreign[foreign.size()-8]^=1;
                auto analysis=image::ghost_fixture_bytes(
                    hdr?ghost::fixture(0xA17,ghost::FixtureColor::Sdr):h->corpus,72,false);
                // SDR analysis remains a gamma BGRA8 view, even for the HDR fixture.
                if(hdr) {
                    const auto sdr=ghost::fixture(0xA17,ghost::FixtureColor::Sdr);
                    analysis=image::ghost_fixture_bytes(sdr,72,false);
                }
                for(size_t p=0;p<analysis.size();p+=4)std::swap(analysis[p],analysis[p+2]);
                std::vector<uint8_t> zeroMotion(1920ull*1080*4),zeroCost(1920ull*1080);
                std::array<std::span<const uint8_t>,SnapshotPool::modelCount> logical{
                    half,half,half,foreign,half,analysis,analysis,zeroMotion,zeroMotion,zeroCost,raw};
                std::array<ComPtr<ID3D12Resource>,SnapshotPool::modelCount> fixtures;
                std::array<ID3D12Resource*,SnapshotPool::modelCount> sources{};
                ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
                ComPtr<ID3D12Fence> fence;Event uploaded;uint64_t fixtureGpu=0;
                checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"fixture preparation allocator");
                checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),
                    "fixture preparation commands");
                checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fixture preparation fence");
                std::array<ComPtr<ID3D12Resource>,SnapshotPool::modelCount> uploads;
                for(size_t role=0;role<fixtures.size();++role) {
                    if(role>0&&role<5&&role!=model::Observation::Foreign) {
                        fixtures[role]=fixtures[0];sources[role]=fixtures[role].Get();continue;
                    }
                    if(role==model::Observation::ReferenceAnalysis) {
                        fixtures[role]=fixtures[model::Observation::CurrentAnalysis];sources[role]=fixtures[role].Get();continue;
                    }
                    auto d=texture(role<5?DXGI_FORMAT_R16G16B16A16_FLOAT:role<7?DXGI_FORMAT_B8G8R8A8_UNORM:
                        role==7?DXGI_FORMAT_R16G16_FLOAT:role==8?DXGI_FORMAT_R16G16_SINT:
                        role==9?DXGI_FORMAT_R8_UINT:hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM);
                    const auto layout=qa::ghost_texture_bytes(device,d);
                    require(layout.logical_bytes()==logical[role].size(),"fixture full role logical extent");
                    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
                    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr,IID_PPV_ARGS(&fixtures[role])),"fixture actual source texture");
                    fixtureGpu+=device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;sources[role]=fixtures[role].Get();
                    D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=layout.physical_bytes();
                    b.Height=1;b.DepthOrArraySize=b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    heap.Type=D3D12_HEAP_TYPE_UPLOAD;
                    checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,
                        nullptr,IID_PPV_ARGS(&uploads[role])),"fixture bounded preparation upload");
                    void* mapped=nullptr;const D3D12_RANGE none{0,0};
                    checked(uploads[role]->Map(0,&none,&mapped),"fixture preparation map");
                    for(UINT y=0;y<layout.rows;++y)memcpy(static_cast<uint8_t*>(mapped)+
                        static_cast<size_t>(y)*layout.footprint.Footprint.RowPitch,
                        logical[role].data()+static_cast<size_t>(y)*layout.rowBytes,static_cast<size_t>(layout.rowBytes));
                    uploads[role]->Unmap(0,nullptr);
                    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[role].Get();
                    from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout.footprint;
                    to.pResource=fixtures[role].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition={fixtures[role].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON};commands->ResourceBarrier(1,&barrier);
                }
                checked(commands->Close(),"fixture prepared copy close");
                checked(fence->SetEventOnCompletion(1,uploaded.value),"fixture upload event before trigger");
                ID3D12CommandList* lists[]{commands.Get()};context->queue()->ExecuteCommandLists(1,lists);
                checked(context->queue()->Signal(fence.Get(),1),"fixture upload real signal");uploaded.wait();
                require(fence->GetCompletedValue()==1,"fixture preparation actual complete");
                for(auto& upload:uploads)upload.Reset();
                commands.Reset();allocator.Reset();fence.Reset();
                D3D11_TEXTURE2D_DESC graphic{};graphic.Width=canvas.width;graphic.Height=canvas.height;
                graphic.MipLevels=graphic.ArraySize=graphic.SampleDesc.Count=1;
                graphic.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
                graphic.Usage=D3D11_USAGE_DEFAULT;graphic.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> current,program;
                const D3D11_SUBRESOURCE_DATA input{raw.data(),canvas.width*(hdr?8u:4u),0};
                checked(renderer->CreateTexture2D(&graphic,&input,&current),"fixture current selected source");
                auto programBytes=raw;
                if(hdr)for(size_t p=0;p<programBytes.size();p+=8)for(size_t c=0;c<3;++c) {
                    uint16_t bits=0;memcpy(&bits,programBytes.data()+p+c*2,2);
                    const auto value=DirectX::PackedVector::XMConvertFloatToHalf(
                        DirectX::PackedVector::XMConvertHalfToFloat(bits)*80.0f/300.0f);
                    memcpy(programBytes.data()+p+c*2,&value,2);
                }else for(size_t p=0;p<programBytes.size();p+=4)std::swap(programBytes[p],programBytes[p+2]);
                graphic.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM;
                const D3D11_SUBRESOURCE_DATA output{programBytes.data(),canvas.width*(hdr?8u:4u),0};
                checked(renderer->CreateTexture2D(&graphic,&output,&program),"fixture actual program source");
                SnapshotPool::Envelope predecessor;
                const JobKey prior{{1,1,72},1,1,72};
                predecessor.fixtureKey.emplace(prior);predecessor.token.emplace(qa::GhostCredits::Token{0,0,prior.frame,1});
                predecessor.stages=(1ull<<SnapshotPool::modelCount)-1;
                for(size_t role=0;role<logical.size();++role)predecessor.packed[role]=logical[role];
                h->remember_edit(predecessor,71);h->pictureArmed.store(true);
                auto* fixture=obs_data_create();obs_data_set_int(fixture,"unique_model_source_gpu_capacity",static_cast<int64_t>(fixtureGpu));
                obs_data_set_bool(fixture,"supplemental_fixture_owners_not_production_admission",true);
                obs_data_set_int(fixture,"source_tick_first",72);obs_data_set_int(fixture,"source_tick_last",75);
                obs_data_set_bool(fixture,"held_pixels_distinct_keys",true);emit("NR_GHOST_MEASUREMENT_FIXTURE ",fixture);
                sample("fixture_prepared_uploads_retired");
                Event cadence;HANDLE timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
                require(timer!=nullptr,"service rational deadline timer");
                struct TimerOwner {HANDLE handle;~TimerOwner(){CloseHandle(handle);}} timerOwner{timer};
                const auto started=std::chrono::steady_clock::now();
                const auto elapsed=[&] {return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-started).count();};
                const uint64_t interval=static_cast<uint64_t>(rate.denominator)*1000000000ull/rate.numerator;
                unsigned admitted=0,refused=0;
                for(unsigned arrival=0;arrival<4;++arrival) {
                    const auto deadline=static_cast<int64_t>(static_cast<uint64_t>(arrival)*rate.denominator*1000000000ull/rate.numerator);
                    const auto remaining=deadline-elapsed();
                    if(remaining>0) {
                        LARGE_INTEGER due{};due.QuadPart=-((remaining+99)/100);
                        require(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)!=FALSE,"exact rational deadline arm");
                        require(WaitForSingleObject(timer,30000)==WAIT_OBJECT_0,"exact rational deadline event");
                    }
                    const auto captureStart=elapsed();const uint64_t ingress=73+arrival,tick=arrival+1;
                    const JobKey key{{1,1,ingress},1,1,ingress},reference{{1,1,ingress-1},1,1,ingress-1};
                    const Settings settings{{1},true,"isolated-fixture-not-loaded",{1},{0,0,0,0,0,-1,false}};
                    const FrameDescriptor descriptor{canvas,{0,0,1920,1080},hdr?PixelFormat::Rgba16Float:PixelFormat::Rgba8,
                        hdr?ColorSpace::ScRgbLinear:ColorSpace::Rec709Srgb,AlphaMode::Premultiplied,80};
                    const IngressSample inputSample{key.frame,tick,tick-1,1,1,1,SourceValidity::Valid,descriptor,settings};
                    const Selection selection{tick,OutputKind::DelayedRaw,Status::Bypassed,Reason::None,reference.frame,reference,false};
                    const pipeline::TickPlan plan{TickAction::CaptureOnce,selection,Reason::None,std::nullopt};
                    const bool accepted=h->snapshots->measurement_capture(inputSample,plan,key,reference,sources,
                        current.Get(),current.Get(),program.Get(),arrival*interval,interval,cadence.value);
                    if(accepted)++admitted;else ++refused;
                    auto* arrivalRow=obs_data_create();obs_data_set_bool(arrivalRow,"hdr",hdr);
                    obs_data_set_int(arrivalRow,"rate_num",rate.numerator);obs_data_set_int(arrivalRow,"rate_den",rate.denominator);
                    obs_data_set_int(arrivalRow,"arrival",arrival);obs_data_set_int(arrivalRow,"deadline_ns",deadline);
                    obs_data_set_int(arrivalRow,"capture_start_ns",captureStart);obs_data_set_int(arrivalRow,"capture_end_ns",elapsed());
                    obs_data_set_int(arrivalRow,"lateness_ns",captureStart-deadline);obs_data_set_bool(arrivalRow,"accepted",accepted);
                    obs_data_set_int(arrivalRow,"occupied",h->snapshots->occupied());
                    obs_data_set_int(arrivalRow,"refusal",static_cast<uint32_t>(h->snapshots->failure()));
                    obs_data_set_bool(arrivalRow,"waited_for_credit",false);emit("NR_GHOST_MEASUREMENT_ARRIVAL ",arrivalRow);
                    sample("capture_submission");
                }
                h->snapshots->close_admission();
                require(h->snapshots->stop(),"service real producer copy drain join credit retirement");
                require(!h->failed,h->failure.c_str());
                require(retired.load()==admitted&&h->rows.size()==admitted,"service successful release callback coverage");
                const bool viable=refused==0&&maximumCredit.load()<=static_cast<int64_t>(interval)&&
                    h->snapshots->complete_iteration_maximum()<=static_cast<int64_t>(interval);
                allViable=allViable&&viable;captured+=admitted;++lanes;
                auto* result=obs_data_create();obs_data_set_bool(result,"hdr",hdr);
                obs_data_set_int(result,"rate_num",rate.numerator);obs_data_set_int(result,"rate_den",rate.denominator);
                obs_data_set_int(result,"interval_ns",static_cast<int64_t>(interval));
                obs_data_set_int(result,"admitted",admitted);obs_data_set_int(result,"refused",refused);
                obs_data_set_int(result,"retired",static_cast<int64_t>(retired.load()));
                obs_data_set_int(result,"maximum_reserve_to_release_ns",maximumCredit.load());
                obs_data_set_int(result,"maximum_complete_cycle_with_callback_ns",maximumCycle.load());
                h->snapshots->report_complete_iterations(result);
                obs_data_set_bool(result,"service_viable",viable);obs_data_set_int(result,"model_role_count",11);
                obs_data_set_int(result,"graphic_role_count",6);
                sample("drain_joined");h->snapshots.reset();
                require(h->sourcePicture&&h->selectedPicture&&h->programPicture,"complete picture consumers");
                const auto pictureWriteStarted=std::chrono::steady_clock::now();
                image::save_bmp(laneDirectory/"fixture-source.bmp",*h->sourcePicture);
                image::save_bmp(laneDirectory/"fixture-selected.bmp",*h->selectedPicture);
                image::save_bmp(laneDirectory/"fixture-program.bmp",*h->programPicture);
                obs_data_set_int(result,"fixture_three_bmp_writes_ns",
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()-pictureWriteStarted).count());
                obs_data_set_int(result,"fixture_bmp_count",3);
                obs_data_set_bool(result,"fixture_bmp_writes_outside_drain",true);
                emit("NR_GHOST_MEASUREMENT_LANE ",result);
                sample("picture_artifacts");
                h->sourcePicture.reset();h->selectedPicture.reset();h->programPicture.reset();
                h->fixturePoolRetired=true;
            } // Fixture textures, byte/upload/command/picture owners end before codec.
            signals={};sample("pool_fixture_and_pictures_retired");
            require(h->codecCredits->occupied()==0&&h->rows.size()>0,"same capture ledger empty before codec");
            const auto codecStarted=std::chrono::steady_clock::now();
            qa::DecodedFrame before{};before.width=1920;before.height=1080;before.fullRange=false;
            before.format=hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;before.transfer=hdr?VIDEO_TRC_PQ:VIDEO_TRC_DEFAULT;
            before.timestamp=h->rows.front().clock;const uint32_t b=hdr?2u:1u;
            before.rowBytes={1920*b,1920*b,0};before.rows={1080,540,0};
            for(size_t p=0;p<2;++p)before.planes[p].resize(static_cast<size_t>(before.rowBytes[p])*before.rows[p]);
            const auto encoded=[&](unsigned x,unsigned y) {
                auto c=ghost::clean_pixel(h->corpus,h->corpus.schedule[71],x,y);
                if(alpha(x,y)==0)c={0,0,0};
                const auto gamma=[](double v){return v<=.0031308?12.92*v:1.055*std::pow(std::max(v,0.0),1/2.4)-.055;};
                if(!hdr)return ghost::Rgb{gamma(c.r),gamma(c.g),gamma(c.b)};
                c={.6274038959*c.r+.3292830384*c.g+.0433130657*c.b,
                    .0690972894*c.r+.9195403951*c.g+.0113623155*c.b,
                    .0163914389*c.r+.0880133079*c.g+.8955952532*c.b};
                const auto pq=[&](double v) {
                    const double power=std::pow(std::max(v,0.0)*h->corpus.referenceWhiteNits/10000,.1593017578125);
                    return std::pow((.8359375+18.8515625*power)/(1+18.6875*power),78.84375);
                };
                return ghost::Rgb{pq(c.r),pq(c.g),pq(c.b)};
            };
            const auto store=[&](size_t plane,unsigned x,unsigned y,double value) {
                const auto code=static_cast<uint16_t>(std::lround(std::clamp(value,0.0,hdr?1023.0:255.0)));
                const size_t at=static_cast<size_t>(y)*before.rowBytes[plane]+x*b;
                if(hdr){const uint16_t word=static_cast<uint16_t>(code<<6);memcpy(before.planes[plane].data()+at,&word,2);}
                else before.planes[plane][at]=static_cast<uint8_t>(code);
            };
            const double scale=hdr?4.0:1.0,kr=hdr?.2627:.2126,kb=hdr?.0593:.0722;
            for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x) {
                const auto c=encoded(x,y);const auto luma=kr*c.r+(1-kr-kb)*c.g+kb*c.b;
                store(0,x,y,(16+219*luma)*scale);
                if((x&1)==0&&(y&1)==0) {
                    store(1,x,y/2,(128+224*(c.b-luma)/(2*(1-kb)))*scale);
                    store(1,x+1,y/2,(128+224*(c.r-luma)/(2*(1-kr)))*scale);
                }
            }
            qa::DecodedFrame repacked{};repacked.width=1920;repacked.height=1080;repacked.fullRange=false;
            repacked.timestamp=before.timestamp;repacked.format=hdr?VIDEO_FORMAT_I010:VIDEO_FORMAT_I420;
            repacked.transfer=before.transfer;repacked.rowBytes={1920*b,960*b,960*b};repacked.rows={1080,540,540};
            for(size_t p=0;p<3;++p)repacked.planes[p].resize(static_cast<size_t>(repacked.rowBytes[p])*repacked.rows[p]);
            for(size_t p=0;p<3;++p)for(unsigned y=0;y<repacked.rows[p];++y)for(unsigned x=0;x<repacked.rowBytes[p]/b;++x) {
                const auto code=image::code(before,p==0?0:1,p==0?x:x*2+static_cast<unsigned>(p-1),y);
                const auto at=static_cast<size_t>(y)*repacked.rowBytes[p]+x*b;
                if(hdr){const auto word=static_cast<uint16_t>(code);memcpy(repacked.planes[p].data()+at,&word,2);}
                else repacked.planes[p][at]=static_cast<uint8_t>(code);
            }
            require(plane_errors(repacked,before)==0,"full fixture planar repacking code comparison");
            sample("codec_planes_prepared");
            {
                const auto rgb=image::decoded_image(repacked,hdr);
                sample("codec_full_rgb_conversion");
                const BorrowedCanonicalPixels canonicalPixels{rgb,h->corpus,hdr,80};
                require(canonicalPixels.identity(71),"full independent YUV RGB borrowed canonical fixture identity");
                image::save_bmp(laneDirectory/"fixture-repacked-rgb.bmp",rgb);
                sample("codec_canonical_and_bmp");
            }
            sample("codec_conversion_retired");
            codec_digest(*h,before,h->rows.front(),"fixture_preencode",0);
            sample("codec_first_digest");
            codec_digest(*h,repacked,h->rows.front(),"fixture_repacked_not_independent_decode",0);
            sample("codec_second_digest");
            h->codec.reset();++codecFrames;
            auto* codecResult=obs_data_create();obs_data_set_bool(codecResult,"hdr",hdr);
            obs_data_set_int(codecResult,"complete_conversion_bmp_both_digests_cycle_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-codecStarted).count());
            obs_data_set_bool(codecResult,"independent_recording_decode",false);
            obs_data_set_bool(codecResult,"independent_rgb_conversion_consumer",true);
            obs_data_set_int(codecResult,"codes_compared",3110400);
            obs_data_set_int(codecResult,"same_ledger_occupied",h->codecCredits->occupied());
            emit("NR_GHOST_MEASUREMENT_CODEC ",codecResult);
            h->serviceSample={};h->digestLane.reset();context.reset();h.reset();
            sample("lane_all_owners_retired");
        }
        immediate.Reset();renderer.Reset();sample("renderer_retired");
        auto* result=obs_data_create();obs_data_set_int(result,"lanes",static_cast<int64_t>(lanes));
        obs_data_set_int(result,"captured",static_cast<int64_t>(captured));obs_data_set_int(result,"codec_frames",static_cast<int64_t>(codecFrames));
        obs_data_set_bool(result,"complete_consumers",true);obs_data_set_bool(result,"service_viable",allViable);
        obs_data_set_int(result,"sampled_local_high",static_cast<int64_t>(high[0]));
        obs_data_set_int(result,"sampled_nonlocal_high",static_cast<int64_t>(high[1]));
        obs_data_set_int(result,"sampled_private_high",static_cast<int64_t>(high[2]));
        obs_data_set_bool(result,"highs_are_whole_physical_totals",false);
        obs_data_set_bool(result,"actual_nr_evaluation",false);obs_data_set_bool(result,"actual_sdk",false);
        obs_data_set_bool(result,"native_modules_loaded",false);obs_data_set_bool(result,"independent_recording_decode",false);
        obs_data_set_bool(result,"physical_Task26_acceptance",false);
        obs_data_set_bool(result,"normal_release",true);emit("NR_GHOST_COMPLETE_SERVICE ",result);
        return true;
    }catch(const std::exception& error) {
        std::cerr<<"NR_GHOST_COMPLETE_SERVICE_FAILURE "<<error.what()<<std::endl;return false;
    }
}

bool nr_ghost_storage_metadata() {
    try {
        SnapshotPool::metadata_proof();uint64_t compared=0;
        for(uint32_t seed:{0xA17u,0xB29u})for(unsigned color=0;color<4;++color)for(bool planar:{false,true}) {
            const bool hdr=color>=2;const auto fixture=ghost::fixture(seed,static_cast<ghost::FixtureColor>(color));
            qa::DecodedFrame frame{};frame.width=canvas.width;frame.height=canvas.height;frame.fullRange=false;
            frame.format=hdr?(planar?VIDEO_FORMAT_I010:VIDEO_FORMAT_P010):(planar?VIDEO_FORMAT_I420:VIDEO_FORMAT_NV12);
            frame.transfer=hdr?VIDEO_TRC_PQ:VIDEO_TRC_DEFAULT;
            const uint32_t bytes=hdr?2u:1u;
            frame.rowBytes=planar?std::array<uint32_t,3>{1920*bytes,960*bytes,960*bytes}:
                std::array<uint32_t,3>{1920*bytes,1920*bytes,0};
            frame.rows=planar?std::array<uint32_t,3>{1080,540,540}:std::array<uint32_t,3>{1080,540,0};
            for(size_t p=0;p<frame.planes.size();++p) {
                auto& plane=frame.planes[p];plane.resize(static_cast<size_t>(frame.rowBytes[p])*frame.rows[p]);
                for(size_t at=0;at<plane.size();at+=bytes) {
                    const uint32_t code=(p==0?16u:112u)+static_cast<uint32_t>((at/bytes+seed)%17);
                    if(hdr){const uint16_t value=static_cast<uint16_t>(code*4*(planar?1u:64u));memcpy(plane.data()+at,&value,2);}
                    else plane[at]=static_cast<uint8_t>(code);
                }
            }
            const auto rgb=image::decoded_image(frame,hdr);const double white=hdr?80.0:300.0;
            const auto owning=canonical(rgb,fixture,hdr,white);
            const BorrowedCanonicalPixels borrowed{rgb,fixture,hdr,white};
            require(owning.size()==rgb.pixels.size(),"actual full decoded canonical parity extent");
            for(size_t p=0;p<owning.size();++p) {
                const auto actual=borrowed.read(p);
                require(memcmp(&actual,&owning[p],sizeof(actual))==0,"actual decoded borrowed canonical changed storage bits");
                ++compared;
            }
        }
        std::printf("PRODUCTION_BORROWED_CANONICAL_PARITY cases=16 compared_pixels=%llu full_double_bits_equal=true actual_decoded_RGB_consumer=true canonical_owning_capacity_after=0 GPU_calls=0 physical_acceptance=false\n",
            static_cast<unsigned long long>(compared));
        return true;
    }catch(const std::exception& error){std::cerr<<"FAIL production_storage_metadata "<<error.what()<<std::endl;return false;}
}

bool nr_run_ghost_scenario(const std::filesystem::path& root,const std::filesystem::path& directory,bool& retained) {
    const auto work=root.parent_path().parent_path().parent_path();
    if(!nr::qa::artifact_directory_allowed(root,directory,nr::qa::ArtifactScenario::GhostCalibration)) {
        std::cout<<"NR_GHOST_FAILURE evidence path refused"<<std::endl;return false;
    }
    ghost::FreezePolicy policy;
    std::string hostHash,sourceHash;
    try {
        policy=load_ghost_freeze(work);
        std::array<wchar_t,32768> executable{};
        const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        require(length>0&&length<executable.size(),"actual host executable path");
        hostHash=freeze_file_hash(std::filesystem::path(executable.data()));
        // Receipt source identity is the digest of the bytes compiled into this image (sealed S_n row), never a runtime W lookup.
        sourceHash=ghost::compiled_host_source_sha256;
    }catch(const std::exception& error){std::cout<<"NR_GHOST_FAILURE "<<error.what()<<std::endl;return false;}
    const std::array<nr::qa::StreamRate,2> rates{{{policy.rateNumerators[0],policy.rateDenominators[0]},
        {policy.rateNumerators[1],policy.rateDenominators[1]}}};
    std::array<std::array<Metrics,4>,2> frozen{};
    std::array<ghost::PhysicalLane,8> physicalLanes{};
    std::array<ghost::PhysicalControl,9> physicalControls{};
    uint64_t physicalAnchors=0;
    const auto save=[&] {
        std::ofstream calibrationFile(directory/"calibration.json",std::ios::binary|std::ios::trunc);
        std::ofstream controlsFile(directory/"bad-controls.json",std::ios::binary|std::ios::trunc);
        ghost::write_calibration(calibrationFile,policy,physicalLanes,hostHash,sourceHash);
        ghost::write_controls(controlsFile,policy,physicalControls,physicalAnchors,hostHash,sourceHash);
    };
    try{save();}catch(const std::exception& error){std::cout<<"NR_GHOST_FAILURE "<<error.what()<<std::endl;return false;}
    for(unsigned pass=0;pass<3;++pass)for(size_t rateIndex=0;rateIndex<rates.size();++rateIndex)for(unsigned mode=0;mode<4;++mode) {
        const bool calibration=pass==0,copy=pass==1,sourceLinear=mode>=2;
        const auto color=static_cast<ghost::FixtureColor>(mode);
        const auto laneDirectory=directory/("pass-"+std::to_string(pass)+"-rate-"+std::to_string(rateIndex)+"-mode-"+std::to_string(mode));
        std::filesystem::create_directory(laneDirectory);
        auto h=std::make_unique<Harness>(root,laneDirectory,calibration?policy.calibrationSeed:policy.holdoutSeed,color,sourceLinear,
            copy,calibration,calibration?nullptr:&frozen[rateIndex][mode],rates[rateIndex],&policy);
        bool callbacks=false,rawConnected=false;
        std::unique_ptr<nr::qa::Recording> recording;
        try {
            if(!calibration){ghost::require_calibration(physicalLanes);ghost::require_controls(physicalControls,physicalAnchors);}
            obs_video_info video{};require(obs_get_video_info(&video),"owned calibration video info");
            video.fps_num=h->rate.numerator;video.fps_den=h->rate.denominator;video.base_width=video.output_width=1920;
            video.base_height=video.output_height=1080;video.scale_type=OBS_SCALE_DISABLE;video.gpu_conversion=true;
            video.output_format=h->hdrOutput?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;
            video.colorspace=h->hdrOutput?VIDEO_CS_2100_PQ:VIDEO_CS_709;video.range=VIDEO_RANGE_PARTIAL;
            require(obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"actual normal SDR8/HDR16 private video reset");
            obs_set_video_levels(300,1000);
            h->create();obs_add_main_render_callback(action,h.get());obs_add_main_rendered_callback(rendered,h.get());callbacks=true;
            h->execute(Action::Bind);h->preparable.wait(90000);h->execute(Action::Prepare);
            h->armReady.wait(90000); // Real post-preallocation phase, never a synthetic key or clock reset.
            h->execute(Action::Arm);h->recordStart.wait(90000);
            const auto refusal="bounded source/QA producer refusal bits="+
                std::to_string(h->producerFailure.load(std::memory_order_acquire))+
                " pool="+std::to_string(static_cast<uint32_t>(h->snapshots->failure()))+
                " occupied="+std::to_string(h->snapshots->occupied());
            require(!h->failed,h->failure.empty()?refusal.c_str():h->failure.c_str());
            recording=std::make_unique<nr::qa::Recording>(laneDirectory/"program.hevc",32,h->hdrOutput);
            obs_add_raw_video_callback(nullptr,preencode,h.get());rawConnected=true;
            recording->start();recording->first_packet();h->pictureArmed.store(true,std::memory_order_release);
            const auto packets=recording->finish();
            obs_remove_raw_video_callback(preencode,h.get());rawConnected=false;
            h->complete.wait(300000);require(!h->failed,h->failure.c_str());
            if(calibration) {
                h->execute(Action::Off);h->offDone.wait(30000);require(!h->failed,h->failure.c_str());
                require(h->offProof.offCurrentSamples>0,"genuine disabled OFF output absent");
            }
            stop_graph(*h);obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get());callbacks=false;
            require(h->sourcePicture&&h->selectedPicture&&h->programPicture,"actual physical surface artifacts");
            image::save_bmp(laneDirectory/"source.bmp",*h->sourcePicture);
            image::save_bmp(laneDirectory/"filter.bmp",*h->selectedPicture);
            image::save_bmp(laneDirectory/"program.bmp",*h->programPicture);
            decoded(*h,packets);
            require(h->measured.rawSamples==256&&h->measured.alphaErrors==0,"complete RAW calibration corpus/alpha");
            if(!copy)require(h->selectedEditSamples>0&&h->staticPositiveSamples>0&&h->movingPositiveSamples>0&&
                h->measured.nativeGuides>0,"genuine selected static/moving NR edits and ordinary guides absent");
            if(calibration) {
                require(h->measured.nativeGuides>0&&h->measured.guidePoints>0&&h->measured.modelSamples>0,
                    "actual SDK/Feature18 calibration positives absent");
                frozen[rateIndex][mode]=h->measured;
                const auto& m=h->measured;auto& lane=physicalLanes[rateIndex*4+mode];
                require(h->offProof.offInstance==h->rows.front().selectedFrame->instanceId,"OFF native lane instance binding");
                lane.offCurrentSamples=h->offProof.offCurrentSamples;lane.offInstance=h->offProof.offInstance;
                lane.offIngress=h->offProof.offIngress;lane.offTick=h->offProof.offTick;
                lane.rawSamples=m.rawSamples;lane.guideSamples=m.nativeGuides;lane.guidePoints=m.guidePoints;
                lane.modelSamples=m.modelSamples;lane.codecFrames=m.codecFrames;lane.codecErrors=m.codecErrors;lane.closed=h->closed;
                if(m.rawSamples){lane.formatBound=m.formatBound;lane.rawMaximum=m.rawMaximum;}
                if(m.guidePoints){lane.guideMismatch=m.guideMismatch;lane.absoluteVector=m.absoluteVector;lane.relativeVector=m.relativeVector;}
                physicalAnchors+=h->physicalAnchors;
                for(size_t i=0;i<physicalControls.size();++i) {
                    const auto& measuredControl=h->physicalControls[i];auto& total=physicalControls[i];
                    if(measuredControl.cases&&measuredControl.rejected) {
                        total.rejected=total.rejected.value_or(true)&&*measuredControl.rejected;
                        total.actualPixels=(total.cases?total.actualPixels:true)&&measuredControl.actualPixels;
                        total.cases+=measuredControl.cases;
                    }
                }
                save();
            }
            auto* row=obs_data_create();obs_data_set_int(row,"pass",pass);obs_data_set_int(row,"mode",mode);
            require(!h->rows.empty()&&h->rows.front().selectedFrame,"actual completed lane source instance");
            obs_data_set_int(row,"instance",static_cast<int64_t>(h->rows.front().selectedFrame->instanceId));
            obs_data_set_int(row,"rate_index",static_cast<int64_t>(rateIndex));
            obs_data_set_int(row,"rate_num",h->rate.numerator);obs_data_set_int(row,"rate_den",h->rate.denominator);
            obs_data_set_bool(row,"source_linear",sourceLinear);obs_data_set_bool(row,"hdr_output",h->hdrOutput);
            obs_data_set_bool(row,"deterministic",copy);obs_data_set_bool(row,"calibration",calibration);
            obs_data_set_int(row,"seed",calibration?policy.calibrationSeed:policy.holdoutSeed);obs_data_set_int(row,"raw_samples",256);
            obs_data_set_int(row,"model_samples",static_cast<int64_t>(h->measured.modelSamples));
            obs_data_set_int(row,"selected_edit_samples",static_cast<int64_t>(h->selectedEditSamples));
            obs_data_set_int(row,"static_positive_samples",static_cast<int64_t>(h->staticPositiveSamples));
            obs_data_set_int(row,"moving_positive_samples",static_cast<int64_t>(h->movingPositiveSamples));
            obs_data_set_double(row,"declared_format_bound",frozen[rateIndex][mode].formatBound);
            obs_data_set_double(row,"raw_maximum",frozen[rateIndex][mode].rawMaximum);
            obs_data_set_double(row,"epsilon",std::max(frozen[rateIndex][mode].formatBound,frozen[rateIndex][mode].rawMaximum));
            obs_data_set_double(row,"guide_mismatch",frozen[rateIndex][mode].guideMismatch);
            obs_data_set_double(row,"absolute_vector_error",frozen[rateIndex][mode].absoluteVector);
            obs_data_set_double(row,"relative_vector_error",frozen[rateIndex][mode].relativeVector);
            obs_data_set_double(row,"static_edit_rms",h->measured.staticRms);
            obs_data_set_double(row,"moving_edit_rms",h->measured.movingRms);
            obs_data_set_double(row,"static_retention",h->measured.staticRetention);
            obs_data_set_double(row,"moving_retention",h->measured.movingRetention);
            obs_data_set_bool(row,"closed",h->closed);obs_data_set_bool(row,"worker_exit",true);
            obs_data_set_bool(row,"retained",false);emit("NR_GHOST_LANE ",row);
            if(calibration&&rateIndex==rates.size()-1&&mode==3)
                std::cout<<"NR_GHOST_FREEZE raw_only=true actual_sdk=true both_rational_rates=true holdout_not_started=true"<<std::endl;
        }catch(const std::exception& error) {
            std::cout<<"NR_GHOST_FAILURE pass="<<pass<<" rate="<<rateIndex<<" mode="<<mode<<" "<<error.what()<<std::endl;
            // Encoder stop first: an unresolved exact stop keeps every raw registration,
            // graph and session owner, and is reported through the registry main.cpp folds
            // into `retained` instead of stopping or destroying anything.
            const auto retirement=nr::qa::recording_retirement(nr::qa::retain_recording_failure(recording),
                rawConnected,callbacks&&h->source!=nullptr);
            if(retirement.retained) {
                retained=true;
                std::cout<<"NR_GHOST_RETAINED pass="<<pass<<" exact encoder stop unresolved; raw registration/graph/session owners preserved"<<std::endl;
                retainedGhostGraphs.push_back(std::move(h));
                return false;
            }
            if(retirement.removeRawCallback)obs_remove_raw_video_callback(preencode,h.get());
            if(h->retained)retained=true;
            try{if(retirement.stopGraph&&!retained)stop_graph(*h);}
            catch(const std::exception& stop){retained=true;std::cout<<"NR_GHOST_RETAINED "<<stop.what()<<std::endl;}
            if(!retained&&retirement.destroyGraph&&callbacks){obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get());}
            if(retained)(void)h.release();
            return false;
        }
    }
    return true;
}

bool nr_run_ghost_baseline(const std::filesystem::path& root,const std::filesystem::path& directory,bool& retained) {
    const auto work=root.parent_path().parent_path().parent_path();
    const bool verifier=root.parent_path().filename()=="task-27-verifier";
    const auto base=work/".omo/evidence/obs-nr-native-filter-20260930/task-27";
    const auto expected=(verifier?base/"verifier":base)/"ghost-artifacts";
    if(directory.parent_path()!=expected||directory.filename().string().find("attempt-")!=0||
        !std::filesystem::is_directory(directory)) {
        std::cout<<"NR_GHOST_FAILURE evidence path refused"<<std::endl;return false;
    }
    ghost::FreezePolicy policy;
    gb::Summary summary;
    try {
        // Holdout admission: the compiled five-file freeze, the actual freeze files and the accepted Task26 calibration
        // must all agree before the first holdout frame. Every document is opened read-only.
        policy=load_ghost_freeze(work);
        summary.policy=policy;
        summary.accepted=load_accepted_calibration(work,policy);
        std::array<wchar_t,32768> executable{};
        const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        require(length>0&&length<executable.size(),"actual host executable path");
        summary.hostSha256=freeze_file_hash(std::filesystem::path(executable.data()));
        summary.hostSourceSha256=ghost::compiled_host_source_sha256;
    }catch(const std::exception& error){std::cout<<"NR_GHOST_FAILURE "<<error.what()<<std::endl;return false;}
    std::cout<<"NR_GHOST_BASELINE_ADMITTED holdout_seed="<<policy.holdoutSeed<<" accepted_calibration_sha256="
        <<summary.accepted.calibrationSha256<<" accepted_controls_sha256="<<summary.accepted.controlsSha256
        <<" freeze_read_only=true"<<std::endl;
    const std::array<nr::qa::StreamRate,2> rates{{{policy.rateNumerators[0],policy.rateDenominators[0]},
        {policy.rateNumerators[1],policy.rateDenominators[1]}}};
    std::array<ghost::PhysicalControl,9> controls{};
    uint64_t anchors=0;
    const auto save=[&] {
        const auto path=directory/"ghost-baseline.json";
        {auto out=gb::open_output(path);gb::write_summary(out,summary);}
        return freeze_file_hash(path);
    };
    try{save();}catch(const std::exception& error){std::cout<<"NR_GHOST_FAILURE "<<error.what()<<std::endl;return false;}
    bool halt=false;
    for(size_t rateIndex=0;rateIndex<rates.size()&&!halt;++rateIndex)for(unsigned mode=0;mode<4&&!halt;++mode) {
        const auto& threshold=summary.accepted.lanes[rateIndex*4+mode];
        Metrics frozenMetrics;
        frozenMetrics.formatBound=threshold.formatBound;frozenMetrics.rawMaximum=threshold.rawMaximum;
        frozenMetrics.guideMismatch=threshold.guideMismatch;frozenMetrics.absoluteVector=threshold.absoluteVector;
        frozenMetrics.relativeVector=threshold.relativeVector;
        const bool sourceLinear=mode>=2;
        const auto color=static_cast<ghost::FixtureColor>(mode);
        // Independently keyed/output/codec-observed rungs. Only binary/NR use selected n-1 pixels.
        for(const auto rung:{gb::Rung::Raw,gb::Rung::Binary,gb::Rung::UnprotectedNr}) {
            if(halt)break;
            const bool raw=rung==gb::Rung::Raw,binary=rung==gb::Rung::Binary;
            const auto laneDirectory=directory/(std::string("baseline-")+(raw?"raw":binary?"binary":"nr")+"-rate-"+
                std::to_string(rateIndex)+"-mode-"+std::to_string(mode));
            std::filesystem::create_directory(laneDirectory);
            auto h=std::make_unique<Harness>(root,laneDirectory,policy.holdoutSeed,color,sourceLinear,raw||binary,false,
                &frozenMetrics,rates[rateIndex],&policy,true);
            h->tally.emplace(rung,static_cast<unsigned>(rateIndex),mode,threshold.epsilon());
            h->rawOutput=raw;
            bool callbacks=false,rawConnected=false;
            std::unique_ptr<nr::qa::Recording> recording;
            try {
                obs_video_info video{};require(obs_get_video_info(&video),"owned baseline video info");
                video.fps_num=h->rate.numerator;video.fps_den=h->rate.denominator;video.base_width=video.output_width=1920;
                video.base_height=video.output_height=1080;video.scale_type=OBS_SCALE_DISABLE;video.gpu_conversion=true;
                video.output_format=h->hdrOutput?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;
                video.colorspace=h->hdrOutput?VIDEO_CS_2100_PQ:VIDEO_CS_709;video.range=VIDEO_RANGE_PARTIAL;
                require(obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"actual normal SDR8/HDR16 private video reset");
                obs_set_video_levels(300,1000);
                h->create();obs_add_main_render_callback(action,h.get());obs_add_main_rendered_callback(rendered,h.get());callbacks=true;
                h->execute(Action::Bind);h->preparable.wait(90000);h->execute(Action::Prepare);
                h->armReady.wait(90000);
                h->execute(Action::Arm);h->recordStart.wait(90000);
                const auto refusal="bounded source/QA producer refusal bits="+
                    std::to_string(h->producerFailure.load(std::memory_order_acquire))+
                    " pool="+std::to_string(static_cast<uint32_t>(h->snapshots->failure()))+
                    " occupied="+std::to_string(h->snapshots->occupied());
                require(!h->failed,h->failure.empty()?refusal.c_str():h->failure.c_str());
                recording=std::make_unique<nr::qa::Recording>(laneDirectory/"program.hevc",32,h->hdrOutput);
                obs_add_raw_video_callback(nullptr,preencode,h.get());rawConnected=true;
                recording->start();recording->first_packet();h->pictureArmed.store(true,std::memory_order_release);
                const auto packets=recording->finish();
                obs_remove_raw_video_callback(preencode,h.get());rawConnected=false;
                h->complete.wait(300000);require(!h->failed,h->failure.c_str());
                stop_graph(*h);obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get());callbacks=false;
                require(h->sourcePicture&&h->selectedPicture&&h->programPicture,"actual physical surface artifacts");
                image::save_bmp(laneDirectory/"source.bmp",*h->sourcePicture);
                image::save_bmp(laneDirectory/"filter.bmp",*h->selectedPicture);
                image::save_bmp(laneDirectory/"program.bmp",*h->programPicture);
                decoded(*h,packets);
                require(h->measured.rawSamples==256&&h->measured.alphaErrors==0,"complete RAW holdout corpus/alpha");
                require(std::abs(h->measured.formatBound-threshold.formatBound)<=1e-12*std::max(1.0,threshold.formatBound),
                    "declared format bound differs from the accepted Task26 calibration");
                if(!raw&&!binary)require(h->selectedEditSamples>0&&h->measured.nativeGuides>0,
                    "genuine selected NR edits and ordinary guides absent (fault, not a quality result)");
                require(!h->rows.empty()&&h->rows.front().selectedFrame,"actual completed lane source instance");
                auto& tally=*h->tally;
                tally.codecFrames=h->measured.codecFrames;tally.codecErrors=h->measured.codecErrors;
                tally.nativeGuides=h->measured.nativeGuides;tally.guidePoints=h->measured.guidePoints;
                tally.modelSamples=h->measured.modelSamples;tally.guideMismatch=h->measured.guideMismatch;
                tally.absoluteVector=h->measured.absoluteVector;tally.relativeVector=h->measured.relativeVector;
                tally.guideBound=threshold;
                if(!raw&&!binary) {
                    anchors+=h->physicalAnchors;
                    for(size_t i=0;i<controls.size();++i) {
                        const auto& measuredControl=h->physicalControls[i];auto& total=controls[i];
                        if(measuredControl.cases&&measuredControl.rejected) {
                            total.rejected=total.rejected.value_or(true)&&*measuredControl.rejected;
                            total.actualPixels=(total.cases?total.actualPixels:true)&&measuredControl.actualPixels;
                            total.cases+=measuredControl.cases;
                        }
                    }
                }
                const auto emitLane=[&](const gb::Tally& lane) {
                    auto* row=obs_data_create();
                    obs_data_set_string(row,"rung",gb::rung_name(lane.rung));
                    obs_data_set_int(row,"rate_index",static_cast<int64_t>(rateIndex));obs_data_set_int(row,"mode",mode);
                    obs_data_set_int(row,"instance",static_cast<int64_t>(lane.instance));
                    obs_data_set_int(row,"rate_num",h->rate.numerator);obs_data_set_int(row,"rate_den",h->rate.denominator);
                    obs_data_set_int(row,"seed",policy.holdoutSeed);obs_data_set_int(row,"ticks",static_cast<int64_t>(lane.ticks));
                    obs_data_set_bool(row,"deterministic",binary);obs_data_set_bool(row,"calibration",false);
                    obs_data_set_bool(row,"pass",lane.pass());
                    obs_data_set_int(row,"template_failures",static_cast<int64_t>(lane.templateFailures));
                    obs_data_set_double(row,"epsilon",lane.epsilon);
                    obs_data_set_bool(row,"closed",h->closed);obs_data_set_bool(row,"worker_exit",true);
                    obs_data_set_bool(row,"retained",false);emit("NR_GHOST_BASELINE_LANE ",row);
                };
                emitLane(tally);
                summary.tallies.push_back(tally);
                if(!tally.pass()) {
                    // Raw and binary are controls and must pass; a failing NR lane is a measurement, not a stop.
                    if(raw||binary)halt=true;
                }
                summary.controls=controls;summary.anchors=anchors;
                save();
            }catch(const std::exception& error) {
                std::cout<<"NR_GHOST_FAILURE baseline rung="<<gb::rung_name(rung)<<" rate="<<rateIndex<<" mode="<<mode<<" "
                    <<error.what()<<std::endl;
                summary.faults.push_back(std::string(gb::rung_name(rung))+" rate "+std::to_string(rateIndex)+" mode "+
                    std::to_string(mode)+": "+error.what());
                // Encoder stop first: an unresolved exact stop keeps every raw registration,
                // graph and session owner, and is reported through the registry main.cpp folds
                // into `retained` instead of stopping or destroying anything.
                const auto retirement=nr::qa::recording_retirement(nr::qa::retain_recording_failure(recording),
                    rawConnected,callbacks&&h->source!=nullptr);
                if(retirement.retained) {
                    retained=true;
                    std::cout<<"NR_GHOST_RETAINED baseline rung="<<gb::rung_name(rung)<<" exact encoder stop unresolved; raw registration/graph/session owners preserved"<<std::endl;
                    retainedGhostGraphs.push_back(std::move(h));
                    try{summary.controls=controls;summary.anchors=anchors;save();}catch(const std::exception&){}
                    return false;
                }
                if(retirement.removeRawCallback)obs_remove_raw_video_callback(preencode,h.get());
                if(h->retained)retained=true;
                try{if(retirement.stopGraph&&!retained)stop_graph(*h);}
                catch(const std::exception& stop){retained=true;std::cout<<"NR_GHOST_RETAINED "<<stop.what()<<std::endl;}
                if(!retained&&retirement.destroyGraph&&callbacks){obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get());}
                if(retained)(void)h.release();
                try{summary.controls=controls;summary.anchors=anchors;save();}catch(const std::exception&){}
                return false;
            }
        }
    }
    try {
        summary.controls=controls;summary.anchors=anchors;
        for(unsigned mode=0;mode<4;++mode) {
            const auto& threshold=summary.accepted.lanes[mode];
            const ghost::Calibration calibration{threshold.formatBound,threshold.rawMaximum,threshold.epsilon()};
            summary.inherited.push_back(gb::attribute_inherited(ghost::fixture(policy.holdoutSeed,static_cast<ghost::FixtureColor>(mode)),calibration));
        }
        const auto digest=save();
        const auto defects=gb::collect_defects(summary);
        std::cout<<"NR_GHOST_BASELINE_RESULT result="<<gb::result_label(summary)<<" baseline_valid="<<gb::baseline_valid(summary)
            <<" raw_pass="<<gb::rung_pass(summary,gb::Rung::Raw)<<" binary_pass="<<gb::rung_pass(summary,gb::Rung::Binary)
            <<" controls_pass="<<gb::controls_valid(summary)<<" unprotected_nr_pass="<<gb::nr_pass(summary)
            <<" defects="<<defects.size()<<" summary_sha256="<<digest<<" freeze_files_written=0"<<std::endl;
        std::cout<<"NR_GHOST_BASELINE holdout_started=true freeze_written=false rungs=raw,binary,unprotected_nr"<<std::endl;
        return gb::baseline_valid(summary);
    }catch(const std::exception& error){std::cout<<"NR_GHOST_FAILURE "<<error.what()<<std::endl;return false;}
}
