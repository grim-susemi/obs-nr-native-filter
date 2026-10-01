// SPDX-License-Identifier: GPL-3.0-only
#include "nr_recording_encode.h"
#include <windows.h>
#include <fstream>
#include <stdexcept>
#include <string>
namespace nr::qa {
namespace {
void require(bool value,const char* message)
{
    if(!value) throw std::runtime_error(message);
}
struct Sink {
    obs_output_t* output;
    const uint32_t target;
    const HANDLE complete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    const HANDLE first=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::ofstream file;
    std::vector<PacketReceipt> packets;
    std::string failure;
    bool headerWritten=false;
    Sink(obs_data_t* settings,obs_output_t* owned):output(owned),
        target(static_cast<uint32_t>(obs_data_get_int(settings,"packets"))),
        file(std::filesystem::path(obs_data_get_string(settings,"path")),std::ios::binary)
    {
        require(complete && first && target>0 && target<=32 && file.good(),"bounded recording sink admission");
        packets.reserve(target+8);
    }
    ~Sink() { CloseHandle(complete);CloseHandle(first); }
};
void state(void* opaque,calldata_t* data) { calldata_set_ptr(data,"state",opaque); }
const char* name(void*) { return "Task12 actual elementary HEVC recording"; }
void* create(obs_data_t* settings,obs_output_t* output)
{
    auto* sink=new Sink(settings,output);
    proc_handler_add(obs_output_get_proc_handler(output),"void nr_packet_sink(out ptr state)",state,sink);
    return sink;
}
void destroy(void* opaque) { delete static_cast<Sink*>(opaque); }
bool start(void* opaque)
{
    auto& sink=*static_cast<Sink*>(opaque);
    if(!obs_output_can_begin_data_capture(sink.output,0) || !obs_output_initialize_encoders(sink.output,0)) return false;
    return sink.file.good() && obs_output_begin_data_capture(sink.output,0);
}
void stop(void* opaque,uint64_t)
{
    auto& sink=*static_cast<Sink*>(opaque);
    obs_output_end_data_capture(sink.output);
}
void packet(void* opaque,encoder_packet* encoded)
{
    auto& sink=*static_cast<Sink*>(opaque);
    if(!encoded || encoded->type!=OBS_ENCODER_VIDEO || !encoded->data || !encoded->size) {
        sink.failure="recording encoder empty packet";SetEvent(sink.complete);return;
    }
    // Pinned NVENC creates SPS/VPS/PPS only while receiving its first bitstream.
    if(!sink.headerWritten) {
        uint8_t* header=nullptr;size_t bytes=0;
        if(!obs_encoder_get_extra_data(obs_output_get_video_encoder(sink.output),&header,&bytes) || !header || !bytes) {
            sink.failure="recording first packet has no actual HEVC header";SetEvent(sink.complete);return;
        }
        sink.file.write(reinterpret_cast<const char*>(header),static_cast<std::streamsize>(bytes));
        sink.headerWritten=true;
    }
    sink.file.write(reinterpret_cast<const char*>(encoded->data),static_cast<std::streamsize>(encoded->size));
    sink.packets.push_back({encoded->pts,encoded->dts,encoded->timebase_num,encoded->timebase_den,encoded->size,encoded->keyframe});
    if(!sink.file.good()) sink.failure="recording packet write failed";
    if(sink.packets.size()==1 && sink.failure.empty())SetEvent(sink.first);
    if(sink.packets.size()>=sink.target || !sink.failure.empty()) SetEvent(sink.complete);
}
void stopped(void* opaque,calldata_t*) { SetEvent(static_cast<HANDLE>(opaque)); }
}
struct Recording::Impl {
    obs_encoder_t* encoder=nullptr;
    obs_output_t* output=nullptr;
    Sink* sink=nullptr;
    const HANDLE ended=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool active=false;
    ~Impl()
    {
        if(output) {
            if(active) {
                obs_output_stop(output);
                if(WaitForSingleObject(ended,30000)!=WAIT_OBJECT_0) {
                    // Caller must retain the libobs session on this named failure.
                    OutputDebugStringA("Task12 encoder stop unresolved");
                    return;
                }
            }
            signal_handler_disconnect(obs_output_get_signal_handler(output),"stop",stopped,ended);
            obs_output_release(output);
        }
        if(encoder) obs_encoder_release(encoder);
        if(ended) CloseHandle(ended);
    }
};
Recording::Recording(const std::filesystem::path& path,uint32_t packets,bool hdr):impl_(std::make_unique<Impl>())
{
    require(impl_->ended!=nullptr,"recording stopped subscription event");
    if(!obs_output_get_display_name("nr_task12_hevc_sink")) {
        obs_output_info info{};
        info.id="nr_task12_hevc_sink";info.flags=OBS_OUTPUT_VIDEO|OBS_OUTPUT_ENCODED;
        info.get_name=name;info.create=create;info.destroy=destroy;info.start=::nr::qa::start;
        info.stop=stop;info.encoded_packet=packet;info.encoded_video_codecs="hevc";
        obs_register_output(&info);
    }
    obs_data_t* settings=obs_data_create();
    obs_data_set_string(settings,"rate_control","lossless");
    obs_data_set_string(settings,"profile",hdr?"main10":"main");
    obs_data_set_string(settings,"preset","p4");
    obs_data_set_string(settings,"tune","ll");
    obs_data_set_string(settings,"multipass","disabled");
    obs_data_set_int(settings,"bf",0);
    obs_data_set_bool(settings,"lookahead",false);
    obs_data_set_bool(settings,"adaptive_quantization",false);
    impl_->encoder=obs_video_encoder_create("obs_nvenc_hevc_tex","Task12 local HEVC",settings,nullptr);
    obs_data_release(settings);
    require(impl_->encoder!=nullptr,"pinned official NVENC HEVC encoder");
    obs_encoder_set_video(impl_->encoder,obs_get_video());
    settings=obs_data_create();
    obs_data_set_string(settings,"path",path.generic_string().c_str());obs_data_set_int(settings,"packets",packets);
    impl_->output=obs_output_create("nr_task12_hevc_sink","Task12 recorded program",settings,nullptr);
    obs_data_release(settings);
    require(impl_->output!=nullptr,"real libobs recording output");
    obs_output_set_video_encoder(impl_->output,impl_->encoder);
    calldata_t query;calldata_init(&query);
    const bool observed=proc_handler_call(obs_output_get_proc_handler(impl_->output),"nr_packet_sink",&query);
    impl_->sink=static_cast<Sink*>(calldata_ptr(&query,"state"));calldata_free(&query);
    require(observed && impl_->sink,"recording sink ownership");
    signal_handler_connect(obs_output_get_signal_handler(impl_->output),"stop",stopped,impl_->ended);
}
Recording::~Recording()=default;
void Recording::start()
{
    require(!impl_->active && obs_output_start(impl_->output),"actual encoder output start");
    impl_->active=true;
}
void Recording::first_packet()
{
    require(impl_->active,"first-packet subscription requires active recording");
    const HANDLE events[]{impl_->sink->first,impl_->sink->complete};
    require(WaitForMultipleObjects(2,events,FALSE,90000)==WAIT_OBJECT_0,"actual first encoded packet event failed");
}
std::vector<PacketReceipt> Recording::finish()
{
    require(impl_->active,"actual recording was started");
    require(WaitForSingleObject(impl_->sink->complete,90000)==WAIT_OBJECT_0,"recording exact packet-count event timeout");
    obs_output_stop(impl_->output); // stopped subscription predates start and stop.
    require(WaitForSingleObject(impl_->ended,30000)==WAIT_OBJECT_0,"recording normal encoder stop timeout");
    impl_->active=false;
    impl_->sink->file.flush();impl_->sink->file.close();
    require(impl_->sink->failure.empty(),impl_->sink->failure.c_str());
    require(impl_->sink->packets.size()>=impl_->sink->target,"recording missing encoded frames");
    auto packets=impl_->sink->packets;
    signal_handler_disconnect(obs_output_get_signal_handler(impl_->output),"stop",stopped,impl_->ended);
    obs_output_release(impl_->output);impl_->output=nullptr;impl_->sink=nullptr;
    obs_encoder_release(impl_->encoder);impl_->encoder=nullptr;
    return packets;
}
}
