// SPDX-License-Identifier: GPL-3.0-only
// UG03 P15: obs_filter destroy() must not free the Filter while a video_tick that
// libobs already dispatched is still in flight (paused before its graphics
// entry). The production translation unit is compiled here unchanged against a
// deterministic link-time fake of the libobs entry points it imports: no OBS
// runtime, graphics device, vendor module or GPU is involved. The fake keeps the
// libobs threading shape that matters here: video_tick and the main render
// callbacks share one graphics thread; destroy may run on another thread.
// Defensive control: real libobs is expected to hold a source reference across
// video_tick (tick_sources), which would already exclude this interleaving; that
// is not verifiable from the headers here, so the fake deliberately omits it.
#include "../../src/obs_filter.cpp"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace fake_obs {
std::recursive_mutex graphics;
std::mutex gateLock;
std::condition_variable gateChanged;
thread_local bool gatedThread=false;
bool gateArmed=true,gateReached=false,gateOpen=false; // Armed before the graphics thread starts.
std::vector<std::pair<void (*)(void*,uint32_t,uint32_t),void*>> renderCallbacks;
void run_render_callbacks() // One main render pass of the graphics thread.
{
    std::lock_guard hold(graphics);
    const auto callbacks=renderCallbacks;
    for(const auto& [draw,param]:callbacks)draw(param,0,0);
}
}

extern "C" {
void obs_enter_graphics(void)
{
    {
        std::unique_lock lock(fake_obs::gateLock);
        if(fake_obs::gateArmed && fake_obs::gatedThread) {
            fake_obs::gateArmed=false;fake_obs::gateReached=true;fake_obs::gateChanged.notify_all();
            fake_obs::gateChanged.wait(lock,[]{return fake_obs::gateOpen;});
        }
    }
    fake_obs::graphics.lock();
}
void obs_leave_graphics(void) {fake_obs::graphics.unlock();}
void obs_add_main_render_callback(void (*draw)(void*,uint32_t,uint32_t),void* param)
{
    std::lock_guard hold(fake_obs::graphics);fake_obs::renderCallbacks.emplace_back(draw,param);
}
void obs_remove_main_render_callback(void (*draw)(void*,uint32_t,uint32_t),void* param)
{
    std::lock_guard hold(fake_obs::graphics);
    std::erase(fake_obs::renderCallbacks,std::pair<void (*)(void*,uint32_t,uint32_t),void*>{draw,param});
}
bool calldata_get_data(const calldata_t*,const char*,void*,size_t) {return false;}
void calldata_set_data(calldata_t*,const char*,const void*,size_t) {}
void bfree(void* ptr) {free(ptr);}
void blog(int, const char*, ...) {}
obs_source_t* obs_weak_source_get_source(obs_weak_source_t*) {return nullptr;}
obs_weak_source_t* obs_source_get_weak_source(obs_source_t*) {return nullptr;}
bool obs_weak_source_references_source(obs_weak_source_t*,obs_source_t*) {return false;}
void obs_weak_source_release(obs_weak_source_t*) {}
void obs_source_release(obs_source_t*) {}
void signal_handler_connect(signal_handler_t*,const char*,signal_callback_t,void*) {}
void signal_handler_disconnect(signal_handler_t*,const char*,signal_callback_t,void*) {}
signal_handler_t* obs_source_get_signal_handler(const obs_source_t*) {return nullptr;}
proc_handler_t* obs_source_get_proc_handler(const obs_source_t*) {return nullptr;}
proc_handler_t* obs_get_proc_handler(void) {return nullptr;}
void proc_handler_add(proc_handler_t*,const char*,proc_handler_proc_t,void*) {}
bool proc_handler_call(proc_handler_t*,const char*,calldata_t*) {return false;}
bool obs_source_enabled(const obs_source_t*) {return true;}
obs_source_t* obs_filter_get_target(const obs_source_t*) {return nullptr;}
obs_source_t* obs_filter_get_parent(const obs_source_t*) {return nullptr;}
void obs_source_skip_video_filter(obs_source_t*) {}
uint32_t obs_source_get_width(obs_source_t*) {return 0;}
uint32_t obs_source_get_height(obs_source_t*) {return 0;}
enum gs_color_space obs_source_get_color_space(obs_source_t*,size_t,const enum gs_color_space*) {return GS_CS_SRGB;}
uint64_t obs_get_video_frame_time(void) {return 0;}
uint64_t obs_get_frame_interval_ns(void) {return 1;}
void obs_register_source_s(const struct obs_source_info*,size_t) {}
obs_module_t* obs_current_module(void) {return nullptr;}
const char* obs_module_text(const char* lookup) {return lookup;}
char* obs_find_module_file(obs_module_t*,const char*) {return nullptr;}
char* obs_module_get_config_path(obs_module_t*,const char*) {return nullptr;}
const char* obs_data_get_string(obs_data_t*,const char*) {return "";}
bool obs_data_get_bool(obs_data_t*,const char*) {return false;}
double obs_data_get_double(obs_data_t*,const char*) {return 0;}
void obs_data_set_default_bool(obs_data_t*,const char*,bool) {}
void obs_data_set_default_string(obs_data_t*,const char*,const char*) {}
void obs_data_set_default_double(obs_data_t*,const char*,double) {}
obs_properties_t* obs_properties_create(void) {return nullptr;}
obs_property_t* obs_properties_add_bool(obs_properties_t*,const char*,const char*) {return nullptr;}
obs_property_t* obs_properties_add_path(obs_properties_t*,const char*,const char*,enum obs_path_type,const char*,
    const char*) {return nullptr;}
obs_property_t* obs_properties_add_float_slider(obs_properties_t*,const char*,const char*,double,double,double)
{return nullptr;}
obs_property_t* obs_properties_add_text(obs_properties_t*,const char*,const char*,enum obs_text_type) {return nullptr;}
void obs_property_set_enabled(obs_property_t*,bool) {}
obs_property_t* obs_properties_get(obs_properties_t*,const char*) {return nullptr;}
int gs_get_device_type(void) {return 0;}
void* gs_get_device_obj(void) {return nullptr;}
void gs_texture_destroy(gs_texture_t*) {}
void gs_texrender_destroy(gs_texrender_t*) {}
gs_texrender_t* gs_texrender_create(enum gs_color_format,enum gs_zstencil_format) {return nullptr;}
void gs_texrender_reset(gs_texrender_t*) {}
bool gs_texrender_begin_with_color_space(gs_texrender_t*,uint32_t,uint32_t,enum gs_color_space) {return false;}
void gs_texrender_end(gs_texrender_t*) {}
gs_texture_t* gs_texrender_get_texture(const gs_texrender_t*) {return nullptr;}
void gs_blend_state_push(void) {}
void gs_blend_state_pop(void) {}
void gs_blend_function_separate(enum gs_blend_type,enum gs_blend_type,enum gs_blend_type,enum gs_blend_type) {}
bool gs_framebuffer_srgb_enabled(void) {return false;}
void gs_enable_framebuffer_srgb(bool) {}
void gs_clear(uint32_t,const struct vec4*,float,uint8_t) {}
void gs_ortho(float,float,float,float,float,float) {}
uint32_t gs_texture_get_width(const gs_texture_t*) {return 0;}
uint32_t gs_texture_get_height(const gs_texture_t*) {return 0;}
gs_texture_t* gs_texture_create(uint32_t,uint32_t,enum gs_color_format,uint32_t,const uint8_t**,uint32_t) {return nullptr;}
void* gs_texture_get_obj(gs_texture_t*) {return nullptr;}
gs_effect_t* obs_get_base_effect(enum obs_base_effect) {return nullptr;}
gs_eparam_t* gs_effect_get_param_by_name(const gs_effect_t*,const char*) {return nullptr;}
void gs_effect_set_texture(gs_eparam_t*,gs_texture_t*) {}
bool gs_effect_loop(gs_effect_t*,const char*) {return false;}
void gs_draw_sprite(gs_texture_t*,uint32_t,uint32_t,uint32_t) {}
}

namespace {
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::atomic<bool> tickInFlight{false};
std::atomic<void*> watchedFilter{nullptr};
std::atomic<unsigned> storageFreed{0},storageFreedDuringTick{0},ownersReleased{0};
void note_free(void* storage) noexcept
{
    if(!storage || storage!=watchedFilter.load())return;
    storageFreed.fetch_add(1);
    if(tickInFlight.load())storageFreedDuringTick.fetch_add(1);
}
void* allocate(std::size_t size)
{
    if(void* storage=std::malloc(size?size:1))return storage;
    throw std::bad_alloc();
}
void* make_filter(obs_source_t* source)
{
    auto* filter=new Filter(source);
    // An accepting native owner exactly as create() leaves it; never prepared,
    // so destroy's actual Close succeeds and nothing is retained.
    filter->native=std::make_unique<nr::obs_adapter::NativeOutput>(filter->instance,nr::model::Config{},
        std::vector<std::byte>{},std::vector<std::byte>{},std::vector<std::byte>{},true);
    // Owner witness: the configured settings are released with the filter's owners.
    std::shared_ptr<const nr::Settings> settings(new nr::Settings{{1},true,"C:/qa/not-loaded-runtime.dll",{1},
        {0,0,1,1,0,-1,true}},[](const nr::Settings* value) {ownersReleased.fetch_add(1);delete value;});
    filter->configuration.store(std::make_shared<const nr::properties::Configuration>(
        nr::properties::Configuration{std::move(settings),{}}),std::memory_order_release);
    watchedFilter.store(filter);
    return filter;
}
}
// Storage witness: the Filter object itself (not its owners) being returned to the heap.
void* operator new(std::size_t size) {return allocate(size);}
void operator delete(void* storage) noexcept {note_free(storage);std::free(storage);}
void operator delete(void* storage,std::size_t) noexcept {note_free(storage);std::free(storage);}

int main() try {
    int sourceStorage=0;
    auto* const source=reinterpret_cast<obs_source_t*>(&sourceStorage);
    void* const opaque=make_filter(source);
    // libobs graphics thread: a tick for this filter is dispatched and pauses
    // before it enters graphics.
    std::thread graphicsThread([opaque] {
        fake_obs::gatedThread=true;
        tickInFlight.store(true);
        video_tick(opaque,0);
        tickInFlight.store(false);
    });
    {
        std::unique_lock lock(fake_obs::gateLock);
        expect(fake_obs::gateChanged.wait_for(lock,std::chrono::seconds(10),[]{return fake_obs::gateReached;}),
            "dispatched video_tick reached its graphics entry");
    }
    // Another thread destroys the source while that tick is in flight.
    destroy(opaque);
    if(storageFreedDuringTick.load()) {
        std::fprintf(stderr,"FAIL filter freed by destroy while a dispatched video_tick was in flight\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(),1); // Resuming the tick would touch freed memory.
    }
    expect(storageFreed.load()==0,"destroy freed the filter before the graphics thread drained");
    expect(ownersReleased.load()==1,"destroy kept the filter's owners past its own return");
    {
        std::lock_guard lock(fake_obs::gateLock);fake_obs::gateOpen=true;
    }
    fake_obs::gateChanged.notify_all();
    graphicsThread.join();
    expect(!tickInFlight.load() && storageFreed.load()==0,"in-flight tick returned without freeing its filter");
    // The same graphics thread's next main render pass reclaims the filter.
    fake_obs::run_render_callbacks();
    expect(storageFreed.load()==1 && storageFreedDuringTick.load()==0,"filter reclaimed exactly once after the tick drained");
    fake_obs::run_render_callbacks();
    expect(storageFreed.load()==1 && fake_obs::renderCallbacks.empty(),"reclaim callback retired itself");
    // A later destroy schedules a fresh reclaim; no tick is in flight.
    storageFreed.store(0);ownersReleased.store(0);
    void* const second=make_filter(source);
    destroy(second);
    expect(ownersReleased.load()==1 && storageFreed.load()==0 && fake_obs::renderCallbacks.size()==1,
        "second destroy released owners and scheduled one reclaim");
    fake_obs::run_render_callbacks();
    expect(storageFreed.load()==1 && fake_obs::renderCallbacks.empty(),"second shell reclaimed by the next render pass");
    std::printf("PASS filter_destroy_drain checks=%u tick_drained_before_free=true owners_released_in_destroy=true "
        "fake_libobs=true CPU_only=true\n",checks);
    return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
