// SPDX-License-Identifier: GPL-3.0-only
#include "nr_bootstrap_scenario.h"
#include "nr_bootstrap_images.h"
#include "nr_recording_encode.h"
#include "obs_native_output.h"
#include <obs.h>
#include <graphics/vec4.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
namespace {
using namespace nr;
using namespace nr::qa;
namespace image=nr::qa::bootstrap;
using Microsoft::WRL::ComPtr;
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
void checked(HRESULT hr,const char* message) { require(SUCCEEDED(hr),message); }
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Event() { require(value!=nullptr,"bootstrap exact event"); }
    ~Event() { CloseHandle(value); }
    void wait(uint32_t bound=30000) { require(WaitForSingleObject(value,bound)==WAIT_OBJECT_0,"bootstrap exact event timeout"); }
};
struct Fixture {
    const bool hdr,unrelated;
    gs_texture_t* texture=nullptr;
    uint64_t clock=0;
    uint32_t content=0,renders=0;
    Fixture(bool h,bool u):hdr(h),unrelated(u) {}
};
const char* source_name(void*) { return "Task12 bounded original source landmarks"; }
void source_state(void*,calldata_t*);
void* source_create(obs_data_t* settings,obs_source_t* source) {
    auto* f=new Fixture(obs_data_get_bool(settings,"hdr"),obs_data_get_bool(settings,"unrelated"));
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_fixture_state(out int renders, out int content)",source_state,f);
    return f;
}
void source_destroy(void* opaque) {
    auto* f=static_cast<Fixture*>(opaque);
    obs_enter_graphics();gs_texture_destroy(f->texture);obs_leave_graphics();delete f;
}
uint32_t source_width(void* opaque) { return static_cast<Fixture*>(opaque)->unrelated?128:image::width; }
uint32_t source_height(void* opaque) { return static_cast<Fixture*>(opaque)->unrelated?128:image::height; }
gs_color_space source_color(void* opaque,size_t,const gs_color_space*) {
    return static_cast<Fixture*>(opaque)->hdr?GS_CS_709_SCRGB:GS_CS_SRGB;
}
void source_state(void* opaque,calldata_t* query) {
    auto& f=*static_cast<Fixture*>(opaque);
    calldata_set_int(query,"renders",f.renders);calldata_set_int(query,"content",f.content);
}
void source_render(void* opaque,gs_effect_t*) {
    auto& f=*static_cast<Fixture*>(opaque);++f.renders;
    const auto clock=obs_get_video_frame_time();
    if(!f.texture)f.texture=gs_texture_create(source_width(&f),source_height(&f),f.hdr?GS_RGBA16F:GS_RGBA_UNORM,1,nullptr,GS_DYNAMIC);
    if(f.clock!=clock) {
        f.clock=clock;
        if(!f.unrelated) {
            f.content=std::min(f.content+1,16u);
            const auto bytes=image::fixture_bytes(f.content,f.hdr);
            gs_texture_set_image(f.texture,bytes.data(),image::width*(f.hdr?8:4),false);
        } else {
            std::vector<uint8_t> bytes(128*128*(f.hdr?8:4));
            for(size_t i=0;i<128*128;++i)for(size_t c=0;c<4;++c) {
                const float value=std::array<float,4>{.25f,.5f,.75f,1}[c];
                if(f.hdr) { const auto half=DirectX::PackedVector::XMConvertFloatToHalf(value);memcpy(bytes.data()+i*8+c*2,&half,2); }
                else bytes[i*4+c]=static_cast<uint8_t>(std::lround(value*255));
            }
            gs_texture_set_image(f.texture,bytes.data(),128*(f.hdr?8:4),false);
        }
    }
    require(f.texture!=nullptr,"actual bounded source texture");
    const bool srgb=gs_framebuffer_srgb_enabled();gs_enable_framebuffer_srgb(false);
    auto* effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),f.texture);
    while(gs_effect_loop(effect,"DrawAlphaDivide"))gs_draw_sprite(f.texture,0,source_width(&f),source_height(&f));
    gs_enable_framebuffer_srgb(srgb);
}
void register_fixture() {
    if(obs_source_get_display_name("nr_task12_fixture"))return;
    obs_source_info info{};info.id="nr_task12_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
    info.get_name=source_name;info.create=source_create;info.destroy=source_destroy;
    info.get_width=source_width;info.get_height=source_height;info.video_render=source_render;info.video_get_color_space=source_color;
    obs_register_source(&info);
}
std::vector<uint8_t> read_texture(ID3D11Texture2D* texture,bool hdr) {
    require(texture!=nullptr,"actual full-size texture");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    require(desc.Width==image::width && desc.Height==image::height,"actual full-size capture extent");
    require(hdr?(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS):
        (desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS ||
         desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS),"actual readback numeric format");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> graphics;
    texture->GetDevice(&device);device->GetImmediateContext(&graphics);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;checked(device->CreateTexture2D(&desc,nullptr,&stage),"bounded QA stage");
    graphics->CopyResource(stage.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};checked(graphics->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"bounded QA map");
    const uint32_t row=image::width*(hdr?8:4);
    std::vector<uint8_t> bytes(static_cast<size_t>(row)*image::height);
    for(uint32_t y=0;y<image::height;++y)memcpy(bytes.data()+static_cast<size_t>(y)*row,
        static_cast<const uint8_t*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,row);
    graphics->Unmap(stage.Get(),0);return bytes;
}
std::vector<uint8_t> read_texture(gs_texture_t* texture,bool hdr) {
    return read_texture(static_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture)),hdr);
}
struct Row {
    uint64_t tick,clock,interval,instance,ingress,selected,epoch;
    uint32_t sourceId,filteredId,programId,kind,renders,programFormat;
    uint64_t rawHash,filteredHash,programHash,unrelatedHash;
    double delta,maximumNits;
    uint64_t alphaErrors=0;
    bool genuine=false;
    uint64_t serial=0,history=0,generation=0;
    uintptr_t module=0;
    std::string modulePath,status;
};
enum class Action { None,Bind,Stop,Close,Release };
struct Harness {
    const std::filesystem::path root,directory;
    const bool hdr,deterministic;
    Event acted,warm,consumerDone;
    std::atomic<Action> action{Action::None};
    std::atomic<bool> observe{true};
    std::atomic<bool> recordingArmed{false};
    bool failed=false,bound=false,closed=false;
    std::string failure;
    obs_source_t* source=nullptr,*filter=nullptr,*unrelated=nullptr;
    obs_scene_t* scene=nullptr;
    std::vector<Row> rows;
    std::mutex rawMutex;
    std::vector<DecodedFrame> preencode;
    bool rawFailed=false;
    std::shared_ptr<model::Job> stoppingJob;
    std::shared_future<void> exited;
    ComPtr<ID3D11Fence> stoppingFence;
    std::optional<image::Image> rawPicture,filterPicture,programPicture;
    uint32_t pictureId=0;
    explicit Harness(const std::filesystem::path& r,const std::filesystem::path& d,bool h,bool copy):
        root(r),directory(d),hdr(h),deterministic(copy) { rows.reserve(16);preencode.reserve(16); }
    ~Harness() {
        if(scene)obs_scene_release(scene);
        if(source && filter)obs_source_filter_remove(source,filter);
        obs_source_release(filter);obs_source_release(source);obs_source_release(unrelated);
    }
    void execute(Action next) { ResetEvent(acted.value);action.store(next,std::memory_order_release);acted.wait(); }
    void create() {
        register_fixture();
        obs_data_t* settings=obs_data_create();obs_data_set_bool(settings,"hdr",hdr);
        source=obs_source_create_private("nr_task12_fixture","Task12 original",settings);
        obs_data_set_bool(settings,"unrelated",true);
        unrelated=obs_source_create_private("nr_task12_fixture","Task12 unrelated scene source",settings);
        obs_data_release(settings);
        settings=obs_data_create();
        obs_data_set_string(settings,"nr_shader_root",(root/"bin/64bit").generic_string().c_str());
        obs_data_set_string(settings,"nr_data_root",(root/"profile/ngx-data").generic_string().c_str());
        obs_data_set_bool(settings,"nr_qa_deterministic",deterministic);
        filter=obs_source_create_private("susemi_nr_filter","Task12 actual native filter",settings);
        obs_data_release(settings);
        require(source && filter && unrelated,"real source/filter graph creation");
        obs_source_filter_add(source,filter);
        scene=obs_scene_create_private("Task12 actual program scene");
        require(scene && obs_scene_add(scene,source),"real scene filtered source");
        auto* item=obs_scene_add(scene,unrelated);require(item!=nullptr,"real unrelated scene source");
        vec2 position{1792,952};obs_sceneitem_set_pos(item,&position);
    }
    void stop() {
        calldata_t query;calldata_init(&query);
        std::shared_ptr<interop::Bridge> bridge;
        calldata_set_ptr(&query,"retain_job",&stoppingJob);calldata_set_ptr(&query,"retain_bridge",&bridge);
        calldata_set_ptr(&query,"retain_exit",&exited);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&query),"native stop subscription snapshot");
        calldata_free(&query);
        if(stoppingJob && stoppingJob->ready.load(std::memory_order_acquire))stoppingJob.reset();
        if(bridge && !stoppingFence) {
            stoppingFence=bridge->consumer_fence();
            uint64_t value=0;for(size_t i=0;i<3;++i)value=std::max(value,bridge->ownership(i).consumer);
            checked(stoppingFence->SetEventOnCompletion(value,consumerDone.value),"pre-stop actual last consumer subscription");
        } else if(!bridge)SetEvent(consumerDone.value);
        calldata_init(&query);
        require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_stop",&query),"native cooperative stop");
        closed=calldata_bool(&query,"closed");calldata_free(&query);
    }
    void release() {
        require(closed,"graph release requires actual native close");
        obs_set_output_source(0,nullptr);bound=false;
        obs_source_filter_remove(source,filter);
        obs_scene_release(scene);scene=nullptr;
        obs_source_release(source);source=nullptr;obs_source_release(filter);filter=nullptr;
        obs_source_release(unrelated);unrelated=nullptr;stoppingJob.reset();stoppingFence.Reset();
    }
    void sample() {
        if(!observe.load(std::memory_order_acquire) || !bound || failed)return;
        try {
            require(rows.size()<16,"bounded graph samples exceeded");
            calldata_t data;calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_sampler_snapshot",&data),"actual canonical filter snapshot");
            Row row{};row.tick=calldata_int(&data,"tick");row.clock=calldata_int(&data,"clock");
            row.interval=calldata_int(&data,"interval");row.instance=calldata_int(&data,"instance");
            row.ingress=calldata_int(&data,"ingress");row.selected=calldata_int(&data,"selected");row.epoch=calldata_int(&data,"epoch");
            calldata_free(&data);
            std::shared_ptr<model::Job> selectedJob;
            std::shared_ptr<const obs_adapter::Capture> raw;
            std::shared_ptr<interop::Context> context;
            calldata_init(&data);calldata_set_ptr(&data,"retain_selected_job",&selectedJob);
            calldata_set_ptr(&data,"retain_raw",&raw);calldata_set_ptr(&data,"retain_context",&context);
            require(proc_handler_call(obs_source_get_proc_handler(filter),"nr_native_snapshot",&data),"actual native image snapshot");
            require(!calldata_bool(&data,"failed") && raw && context,"native raw fallback cannot pass");
            auto* output=static_cast<gs_texture_t*>(calldata_ptr(&data,"output_texture"));
            row.kind=static_cast<uint32_t>(calldata_int(&data,"kind"));row.status=calldata_string(&data,"status");
            calldata_free(&data);
            const auto rawBytes=read_texture(raw->original11.Get(),hdr);
            const auto outputBytes=read_texture(output,hdr);
            auto* programTexture=gs_get_render_target();
            const auto programFormat=gs_texture_get_color_format(programTexture);
            row.programFormat=static_cast<uint32_t>(programFormat);
            const bool programBgra=programFormat==GS_BGRA || programFormat==GS_BGRA_UNORM;
            require(hdr?programFormat==GS_RGBA16F:
                (programBgra || programFormat==GS_RGBA || programFormat==GS_RGBA_UNORM),"actual program RGB format");
            const auto programBytes=read_texture(programTexture,hdr);
            auto original=image::rgb_image(rawBytes,hdr,false,"immutable source");
            auto filtered=image::rgb_image(outputBytes,hdr,false,"selected filter");
            auto program=image::rgb_image(programBytes,hdr,programBgra,"program");
            row.sourceId=image::content_id(original);row.filteredId=image::content_id(filtered);row.programId=image::content_id(program);
            require(row.sourceId==row.selected && row.filteredId==row.sourceId && row.programId==row.sourceId,"source/filter/program independent image lineage");
            row.rawHash=image::hash(rawBytes);row.filteredHash=image::hash(outputBytes);row.programHash=image::hash(programBytes);
            row.unrelatedHash=14695981039346656037ull;
            const uint32_t pixelBytes=hdr?8u:4u,stride=image::width*pixelBytes;
            for(uint32_t y=968;y<1064;++y)for(uint32_t x=1808u*pixelBytes;x<1904u*pixelBytes;++x)
                row.unrelatedHash=(row.unrelatedHash^programBytes[static_cast<size_t>(y)*stride+x])*1099511628211ull;
            double squared=0;
            for(size_t i=0;i<original.pixels.size();++i) {
                if(original.pixels[i][3]!=filtered.pixels[i][3])++row.alphaErrors;
                for(size_t c=0;c<3;++c) {
                    const double d=filtered.pixels[i][c]-original.pixels[i][c];squared+=d*d;
                    row.maximumNits=std::max(row.maximumNits,static_cast<double>(filtered.pixels[i][c])*(hdr?80:1));
                }
            }
            row.delta=std::sqrt(squared/(image::width*image::height*3));
            require(row.alphaErrors==0,"actual source alpha changed");
            if(deterministic)require(row.rawHash==row.filteredHash,"deterministic transport altered original");
            if(row.kind==static_cast<unsigned>(OutputKind::DelayedNr)) {
                require(selectedJob && selectedJob->ready.load(std::memory_order_acquire) && selectedJob->published.load() &&
                    selectedJob->contract.key.frame.ingressSequence==row.selected,"exact same-key physical publication");
                row.serial=selectedJob->contract.key.submissionSerial;row.history=selectedJob->contract.key.historyEpoch;
                row.generation=selectedJob->contract.key.resourceGeneration;
                row.genuine=!deterministic && selectedJob->eligible && selectedJob->evaluated &&
                    selectedJob->composed && selectedJob->submitted && !selectedJob->invalid;
                row.module=selectedJob->runtimeModule;row.modulePath=std::filesystem::path(selectedJob->modulePath).generic_string();
                if(!deterministic)require(row.genuine && row.module && row.delta>0,"model no-op or metadata-only success");
            }
            require(row.tick==row.clock/row.interval+1 && row.ingress>0,"actual OBS canonical tick");
            if(!rows.empty()) {
                require(row.tick==rows.back().tick+1 && row.ingress==rows.back().ingress+1,"missing/reordered canonical sample");
                require(row.unrelatedHash==rows.front().unrelatedHash,"unrelated source/chrome-boundary pixels changed");
            }
            // Two extra real source renders use the same production filter latch.
            calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&data),"fixture source-render observation");
            row.renders=static_cast<uint32_t>(calldata_int(&data,"renders"));calldata_free(&data);
            for(unsigned view=0;view<2;++view) {
                auto* target=gs_texrender_create(hdr?GS_RGBA16F:GS_RGBA,GS_ZS_NONE);
                require(target && gs_texrender_begin_with_color_space(target,image::width,image::height,hdr?GS_CS_709_SCRGB:GS_CS_SRGB),"actual preview/projector target");
                gs_blend_state_push();gs_blend_function_separate(GS_BLEND_SRCALPHA,GS_BLEND_INVSRCALPHA,GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
                vec4 clear{};gs_clear(GS_CLEAR_COLOR,&clear,0,0);
                gs_ortho(0,static_cast<float>(image::width),0,static_cast<float>(image::height),-100,100);
                obs_source_video_render(source);gs_texrender_end(target);gs_blend_state_pop();
                const auto viewed=read_texture(gs_texrender_get_texture(target),hdr);gs_texrender_destroy(target);
                require(image::content_id(image::rgb_image(viewed,hdr,false,"secondary view"))==row.filteredId,"view-only or between-view selection changed");
            }
            calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(source),"nr_fixture_state",&data),"source count after views");
            require(static_cast<uint32_t>(calldata_int(&data,"renders"))==row.renders,"duplicate-view recapture");
            calldata_free(&data);
            if(recordingArmed.load(std::memory_order_acquire) && !rawPicture && row.sourceId>=3 && (deterministic || row.genuine)) {
                pictureId=row.sourceId;rawPicture.emplace(std::move(original));
                filterPicture.emplace(std::move(filtered));programPicture.emplace(std::move(program));
            }
            rows.push_back(std::move(row));
            const auto applied=std::count_if(rows.begin(),rows.end(),[](const Row& r){return r.kind==3;});
            if(rows.size()>=3 && applied>0)SetEvent(warm.value);
        }catch(const std::exception& e) { failed=true;failure=e.what();SetEvent(warm.value); }
    }
};
void action(void* opaque,uint32_t,uint32_t) {
    auto& h=*static_cast<Harness*>(opaque);const auto next=h.action.exchange(Action::None);
    if(next==Action::None)return;
    try {
        if(next==Action::Bind) { obs_set_output_source(0,obs_scene_get_source(h.scene));h.bound=true; }
        else if(next==Action::Release)h.release();
        else h.stop();
    }catch(const std::exception& e) { h.failed=true;h.failure=e.what(); }
    SetEvent(h.acted.value);
}
void rendered(void* opaque) { static_cast<Harness*>(opaque)->sample(); }
void preencode(void* opaque,video_data* data) {
    auto& h=*static_cast<Harness*>(opaque);std::lock_guard lock(h.rawMutex);
    if(h.preencode.size()>=16) { h.rawFailed=true;return; }
    DecodedFrame frame{};frame.width=image::width;frame.height=image::height;frame.timestamp=data->timestamp;
    frame.format=h.hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;frame.transfer=h.hdr?VIDEO_TRC_PQ:VIDEO_TRC_DEFAULT;
    frame.fullRange=false;frame.rowBytes={image::width*(h.hdr?2:1),image::width*(h.hdr?2:1),0};
    frame.rows={image::height,image::height/2,0};
    for(size_t p=0;p<2;++p) {
        if(!data->data[p] || data->linesize[p]<frame.rowBytes[p]) { h.rawFailed=true;return; }
        frame.planes[p].resize(static_cast<size_t>(frame.rowBytes[p])*frame.rows[p]);
        for(uint32_t y=0;y<frame.rows[p];++y)memcpy(frame.planes[p].data()+static_cast<size_t>(y)*frame.rowBytes[p],
            data->data[p]+static_cast<size_t>(y)*data->linesize[p],frame.rowBytes[p]);
    }
    h.preencode.push_back(std::move(frame));
}
void emit(const std::string& prefix,obs_data_t* data) {
    std::cout<<prefix<<obs_data_get_json(data)<<std::endl;obs_data_release(data);
}
void emit_rows(const Harness& h,unsigned lane) {
    for(const auto& r:h.rows) {
        auto* data=obs_data_create();
        for(const auto& pair:std::initializer_list<std::pair<const char*,uint64_t>>{
            {"lane",lane},{"tick",r.tick},{"clock",r.clock},{"interval",r.interval},{"instance",r.instance},
            {"ingress",r.ingress},{"selected",r.selected},{"content_epoch",r.epoch},{"kind",r.kind},
            {"source_id",r.sourceId},{"filter_id",r.filteredId},{"program_id",r.programId},{"width",image::width},
            {"height",image::height},{"source_renders",r.renders},{"alpha_errors",r.alphaErrors},{"program_gs_format",r.programFormat},
            {"submission_serial",r.serial},{"history_epoch",r.history},{"resource_generation",r.generation},{"module_base",r.module}})
            obs_data_set_int(data,pair.first,static_cast<int64_t>(pair.second));
        obs_data_set_bool(data,"hdr",h.hdr);obs_data_set_bool(data,"deterministic",h.deterministic);
        obs_data_set_bool(data,"genuine_nr",r.genuine);obs_data_set_bool(data,"same_latch_views",true);
        obs_data_set_double(data,"same_key_nr_rms",r.delta);obs_data_set_double(data,"maximum_linear_nits",r.maximumNits);
        obs_data_set_string(data,"raw_hash",std::to_string(r.rawHash).c_str());
        obs_data_set_string(data,"filter_hash",std::to_string(r.filteredHash).c_str());
        obs_data_set_string(data,"program_hash",std::to_string(r.programHash).c_str());
        obs_data_set_string(data,"unrelated_hash",std::to_string(r.unrelatedHash).c_str());
        obs_data_set_string(data,"module_path",r.modulePath.c_str());obs_data_set_string(data,"status",r.status.c_str());
        emit("NR_BOOTSTRAP_FRAME ",data);
    }
}
void stop_graph(Harness& h) {
    h.observe.store(false,std::memory_order_release);h.execute(Action::Stop);
    if(h.stoppingJob && !h.stoppingJob->ready.load(std::memory_order_acquire)) {
        require(WaitForSingleObject(h.stoppingJob->observed,30000)==WAIT_OBJECT_0,"normal pending model completion");
        h.stoppingJob.reset();
        h.execute(Action::Stop);
    }
    h.consumerDone.wait();
    h.execute(Action::Close); // Actual completed consumers permit input-lease collection before worker stop.
    if(h.exited.valid())require(h.exited.wait_for(std::chrono::seconds(30))==std::future_status::ready,"subscribed worker normal exit");
    h.execute(Action::Close);require(h.closed,"normal producer/worker/consumer/feature retirement");
    h.execute(Action::Release);
}
}
bool nr_run_bootstrap_scenario(const std::filesystem::path& root,const std::filesystem::path& directory,bool& retained) {
    const auto work=root.parent_path().parent_path().parent_path();
    const bool verifier=root.parent_path().filename()=="task-12-verifier";
    const auto base=work/".omo/evidence/obs-nr-native-filter-20260930/task-12";
    const auto expected=(verifier?base/"verifier":base)/"bootstrap-artifacts";
    if(directory.parent_path()!=expected || directory.filename().string().find("attempt-")!=0 || !std::filesystem::is_directory(directory)) {
        std::cout<<"NR_BOOTSTRAP_FAILURE native evidence path refused"<<std::endl;return false;
    }
    std::array<std::vector<DecodedFrame>,2> baselines;
    for(unsigned lane=0;lane<4;++lane) {
        const bool hdr=lane>=2,deterministic=(lane%2)==0;
        const auto laneDirectory=directory/("lane-"+std::to_string(lane));std::filesystem::create_directory(laneDirectory);
        auto h=std::make_unique<Harness>(root,laneDirectory,hdr,deterministic);
        bool callbacks=false,rawConnected=false;
        try {
            obs_video_info video{};require(obs_get_video_info(&video),"owned video info");
            video.fps_num=1;video.fps_den=1;video.base_width=video.output_width=image::width;
            video.base_height=video.output_height=image::height;video.gpu_conversion=true;
            video.output_format=hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;
            video.colorspace=hdr?VIDEO_CS_2100_PQ:VIDEO_CS_709;video.range=VIDEO_RANGE_PARTIAL;
            video.scale_type=OBS_SCALE_DISABLE;
            require(obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"full-size SDR/HDR owned video reset");
            obs_set_video_levels(300,1000);
            require(obs_get_video_sdr_white_level()==300 && obs_get_video_hdr_nominal_peak_level()==1000,
                "explicit owned OBS SDR-white/HDR-peak levels");
            h->create();
            obs_add_main_render_callback(action,h.get());obs_add_main_rendered_callback(rendered,h.get());callbacks=true;
            h->execute(Action::Bind);h->warm.wait();require(!h->failed,h->failure.c_str());
            {
                Recording recording(laneDirectory/"program.hevc",8,hdr);
                obs_add_raw_video_callback(nullptr,preencode,h.get());rawConnected=true;
                recording.start();
                recording.first_packet();
                h->recordingArmed.store(true,std::memory_order_release);
                const auto packets=recording.finish();
                obs_remove_raw_video_callback(preencode,h.get());rawConnected=false;
                h->observe.store(false,std::memory_order_release);
                stop_graph(*h);
                obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get());callbacks=false;
                require(!h->failed && !h->rawFailed,h->failure.c_str());
                require(h->rawPicture && h->filterPicture && h->programPicture,"actual source/filter/program image artifacts missing");
                image::save_bmp(laneDirectory/"source.bmp",*h->rawPicture);
                image::save_bmp(laneDirectory/"filter.bmp",*h->filterPicture);
                image::save_bmp(laneDirectory/"program.bmp",*h->programPicture);
                emit_rows(*h,lane); // Written partial image proof survives a later decoder failure.
                const auto decoded=decode_recording(laneDirectory/"program.hevc",hdr,static_cast<uint32_t>(packets.size()));
                require(decoded.size()==packets.size() && decoded.size()>=8,"actual encoded/decoded frame count");
                uint32_t previousId=0;
                std::vector<uint32_t> decodedIds;
                unsigned recordedPositive=0;
                for(const auto& frame:decoded) {
                    float matrix[16]{},minimum[3]{},maximum[3]{};
                    require(video_format_get_parameters_for_format(hdr?VIDEO_CS_2100_PQ:VIDEO_CS_709,
                        VIDEO_RANGE_PARTIAL,frame.format,matrix,minimum,maximum),"decoder explicit color matrix");
                    for(size_t i=0;i<16;++i)require(std::abs(matrix[i]-frame.matrix[i])<=1e-6,"actual decoder primaries matrix changed");
                    auto decodedPicture=image::decoded_image(frame,hdr);
                    const auto id=image::content_id(decodedPicture);
                    require(image::content_id(frame)==id,"decoded luma/RGB landmark disagreement");
                    require(!previousId || id==previousId+1,"decoded recording freeze/drop/reorder");
                    previousId=id;decodedIds.push_back(id);
                    const auto selected=std::find_if(h->rows.begin(),h->rows.end(),[&](const Row& r){return r.programId==id;});
                    require(selected!=h->rows.end() && selected->kind==3 && (deterministic || selected->genuine),
                        "preview-only or recording bypass cannot pass");
                    const auto before=std::find_if(h->preencode.begin(),h->preencode.end(),[&](const DecodedFrame& p){
                        return image::content_id(p)==id;
                    });
                    require(before!=h->preencode.end(),"independent decoded image lacks same-ID preencode");
                    uint64_t errors=0,checkedCodes=0;
                    for(uint32_t y=0;y<image::height;++y)for(uint32_t x=0;x<image::width;++x) {
                        if(image::code(frame,0,x,y)!=image::code(*before,0,x,y))++errors;++checkedCodes;
                    }
                    const bool planar=frame.format==VIDEO_FORMAT_I420 || frame.format==VIDEO_FORMAT_I010;
                    for(uint32_t y=0;y<image::height/2;++y)for(uint32_t x=0;x<image::width/2;++x)for(size_t c=0;c<2;++c) {
                        if(image::code(frame,planar?c+1:1,planar?x:x*2+static_cast<uint32_t>(c),y)!=
                            image::code(*before,1,x*2+static_cast<uint32_t>(c),y))++errors;
                        ++checkedCodes;
                    }
                    require(errors==0,"real lossless recording differs from actual preencode plane codes");
                    double decodedDelta=0;
                    if(!deterministic) {
                        const auto& baseline=baselines[hdr?1:0];
                        const auto raw=std::find_if(baseline.begin(),baseline.end(),[&](const DecodedFrame& p){return image::content_id(p)==id;});
                        if(raw!=baseline.end()) {
                            for(uint32_t y=0;y<image::height;++y)for(uint32_t x=0;x<image::width;++x) {
                                const double delta=static_cast<double>(image::code(frame,0,x,y))-image::code(*raw,0,x,y);
                                decodedDelta+=delta*delta;
                            }
                            decodedDelta=std::sqrt(decodedDelta/(image::width*image::height));
                            require(decodedDelta>0,"actual independently decoded same-ID NR no-op");++recordedPositive;
                        }
                    }
                    double peak=0;for(const auto& p:decodedPicture.pixels)for(size_t c=0;c<3;++c)peak=std::max(peak,static_cast<double>(p[c])*(hdr?80:1));
                    if(hdr)require(peak>200,"HDR numeric highlight coerced to SDR");
                    if(id==h->pictureId)image::save_bmp(laneDirectory/"decoded.bmp",decodedPicture);
                    auto* row=obs_data_create();obs_data_set_int(row,"lane",lane);obs_data_set_int(row,"source_id",id);
                    obs_data_set_int(row,"width",frame.width);obs_data_set_int(row,"height",frame.height);
                    obs_data_set_int(row,"bit_depth",hdr?10:8);obs_data_set_int(row,"format",frame.format);
                    obs_data_set_int(row,"transfer",frame.transfer);obs_data_set_bool(row,"full_range",frame.fullRange);
                    obs_data_set_bool(row,"primaries_matrix_preserved",true);obs_data_set_int(row,"codes_compared",static_cast<int64_t>(checkedCodes));
                    obs_data_set_int(row,"code_errors",static_cast<int64_t>(errors));obs_data_set_double(row,"decoded_peak_linear_nits",peak);
                    obs_data_set_double(row,"same_id_raw_decoded_nr_rms_codes",decodedDelta);
                    emit("NR_BOOTSTRAP_DECODED ",row);
                }
                if(deterministic)baselines[hdr?1:0]=decoded;
                else require(recordedPositive>=3,"missing independently decoded same-ID raw/NR positive set");
                auto* summary=obs_data_create();obs_data_set_int(summary,"lane",lane);
                obs_data_set_bool(summary,"hdr",hdr);obs_data_set_bool(summary,"deterministic",deterministic);
                obs_data_set_int(summary,"packets",static_cast<int64_t>(packets.size()));
                obs_data_set_int(summary,"decoded_frames",static_cast<int64_t>(decoded.size()));
                obs_data_set_int(summary,"picture_content_id",h->pictureId);
                obs_data_set_string(summary,"artifacts",laneDirectory.generic_string().c_str());
                obs_data_set_bool(summary,"display_tonemapped_only",hdr);
                obs_data_set_int(summary,"decoder_observation_fps",60);
                obs_data_set_int(summary,"decoder_elementary_stream_fps",1);
                emit("NR_BOOTSTRAP_LANE ",summary);
            }
            auto* close=obs_data_create();obs_data_set_int(close,"lane",lane);obs_data_set_bool(close,"closed",true);
            obs_data_set_bool(close,"worker_exit",true);obs_data_set_bool(close,"retained",false);
            emit("NR_BOOTSTRAP_CLEANUP ",close);
        }catch(const std::exception& e) {
            std::cout<<"NR_BOOTSTRAP_FAILURE lane="<<lane<<" "<<e.what()<<std::endl;
            if(rawConnected)obs_remove_raw_video_callback(preencode,h.get());
            try { if(callbacks && h->source)stop_graph(*h); }
            catch(const std::exception& cleanup) {
                std::cout<<"NR_BOOTSTRAP_RETAINED lane="<<lane<<" "<<cleanup.what()<<std::endl;retained=true;
            }
            if(!retained && callbacks) { obs_remove_main_rendered_callback(rendered,h.get());obs_remove_main_render_callback(action,h.get()); }
            if(retained)(void)h.release();
            return false;
        }
    }
    std::cout<<"NR_BOOTSTRAP_SCOPE real_filter=true Feature18=true SDR=true HDR=true recording=true independently_decoded=true quality=false performance=false cleanup=complete"<<std::endl;
    return true;
}
