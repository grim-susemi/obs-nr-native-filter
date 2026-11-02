// SPDX-License-Identifier: GPL-3.0-only
#include "nr_recording_decode.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>
namespace nr::qa {
namespace {
void require(bool value,const char* message)
{
    if(!value) throw std::runtime_error(message);
}
struct Probe {
    const HANDLE complete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    const HANDLE framesComplete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    const uint32_t expected;
    std::vector<DecodedFrame> frames;
    std::string failure;
    explicit Probe(uint32_t count):expected(count) {
        require(complete && framesComplete && count>0 && count<=32,"recording decoder pre-trigger count/events");
        frames.reserve(count);
    }
    ~Probe() { CloseHandle(complete);CloseHandle(framesComplete); }
};
void state(void* opaque,calldata_t* data) { calldata_set_ptr(data,"state",opaque); }
const char* name(void*) { return "Task12 bounded recording decoder observer"; }
void* create(obs_data_t* settings,obs_source_t* source)
{
    auto* probe=new Probe(static_cast<uint32_t>(obs_data_get_int(settings,"expected_frames")));
    proc_handler_add(obs_source_get_proc_handler(source),"void nr_decode_observer(out ptr state)",state,probe);
    return probe;
}
void destroy(void* opaque) { delete static_cast<Probe*>(opaque); }
obs_source_frame* decoded(void* opaque,obs_source_frame* frame)
{
    auto& probe=*static_cast<Probe*>(opaque);
    if(!frame || !probe.failure.empty()) return frame;
    try {
        require(probe.frames.size()<64,"recording decoder exceeded bounded frame manifest");
        require(frame->width==1920 && frame->height==1080,"recording decoder full-size extent");
        DecodedFrame copy{};
        copy.width=frame->width;copy.height=frame->height;copy.timestamp=frame->timestamp;
        copy.format=frame->format;copy.transfer=static_cast<video_trc>(frame->trc);
        copy.fullRange=frame->full_range;copy.maxLuminance=frame->max_luminance;
        std::copy_n(frame->color_matrix,16,copy.matrix.begin());
        std::copy_n(frame->color_range_min,3,copy.rangeMin.begin());
        std::copy_n(frame->color_range_max,3,copy.rangeMax.begin());
        switch(frame->format) {
        case VIDEO_FORMAT_I420:
            copy.rowBytes={frame->width,frame->width/2,frame->width/2};
            copy.rows={frame->height,frame->height/2,frame->height/2};break;
        case VIDEO_FORMAT_NV12:
            copy.rowBytes={frame->width,frame->width,0};
            copy.rows={frame->height,frame->height/2,0};break;
        case VIDEO_FORMAT_I010:
            copy.rowBytes={frame->width*2,frame->width,frame->width};
            copy.rows={frame->height,frame->height/2,frame->height/2};break;
        case VIDEO_FORMAT_P010:
            copy.rowBytes={frame->width*2,frame->width*2,0};
            copy.rows={frame->height,frame->height/2,0};break;
        default:throw std::runtime_error("recording decoder unsupported actual plane format");
        }
        for(size_t p=0;p<copy.planes.size();++p) {
            if(!copy.rows[p]) continue;
            require(frame->data[p] && frame->linesize[p]>=copy.rowBytes[p],"recording decoder plane bounds");
            copy.planes[p].resize(static_cast<size_t>(copy.rowBytes[p])*copy.rows[p]);
            for(uint32_t row=0;row<copy.rows[p];++row)
                memcpy(copy.planes[p].data()+static_cast<size_t>(row)*copy.rowBytes[p],
                    frame->data[p]+static_cast<size_t>(row)*frame->linesize[p],copy.rowBytes[p]);
        }
        probe.frames.push_back(std::move(copy));
        if(probe.frames.size()==probe.expected)SetEvent(probe.framesComplete);
    } catch(const std::exception& error) {
        probe.failure=error.what();
        SetEvent(probe.framesComplete); // Error signal, never a fabricated media-ended event.
    }
    return frame;
}
void ended(void* opaque,calldata_t*) { SetEvent(static_cast<Probe*>(opaque)->complete); }
}
std::vector<DecodedFrame> decode_recording(const std::filesystem::path& file,bool hdr,uint32_t expectedFrames,StreamRate rate)
{
    const auto decoderOptions=rate.decoder_options();
    // Registration is per libobs session, not global decoder/provider state.
    const char* id="nr_task12_decode_observer";
    if(!obs_source_get_display_name(id)) {
        obs_source_info info{};
        info.id=id;info.type=OBS_SOURCE_TYPE_FILTER;info.output_flags=OBS_SOURCE_ASYNC_VIDEO;
        info.get_name=name;info.create=create;info.destroy=destroy;info.filter_video=decoded;
        obs_register_source(&info);
    }
    // OBS applies async filters to the closest frame at video_tick, not directly
    // in the decoder callback. The declared stream rate remains the real rate;
    // the separate decoder-only observation clock is never production evidence.
    obs_video_info video{};require(obs_get_video_info(&video),"decoder owned video info");
    video.fps_num=rate.decoder_observation_fps();video.fps_den=1;
    require(obs_reset_video(&video)==OBS_VIDEO_SUCCESS,"decoder-only observation clock reset");
    obs_set_video_levels(300,1000);
    obs_data_t* observerSettings=obs_data_create();
    obs_data_set_int(observerSettings,"expected_frames",expectedFrames);
    obs_source_t* source=obs_source_create_private("ffmpeg_source","Task12 recorded file decoder",nullptr);
    obs_source_t* filter=obs_source_create_private(id,"Task12 decoded planes",observerSettings);
    obs_data_release(observerSettings);
    const std::shared_ptr<obs_source_t> sourceOwner(source,obs_source_release);
    const std::shared_ptr<obs_source_t> filterOwner(filter,obs_source_release);
    require(source && filter,"pinned official recording decoder source");
    calldata_t query;calldata_init(&query);
    const bool observed=proc_handler_call(obs_source_get_proc_handler(filter),"nr_decode_observer",&query);
    auto* probe=static_cast<Probe*>(calldata_ptr(&query,"state"));calldata_free(&query);
    require(observed && probe,"recording decoder observer ownership");
    obs_source_filter_add(source,filter);
    auto* signals=obs_source_get_signal_handler(source);
    signal_handler_connect(signals,"media_ended",ended,probe); // Before open/play.
    obs_data_t* settings=obs_data_create();
    // This local elementary stream has no timestamp-seek index. The source's
    // non-seekable input mode avoids its local-file seek-to-zero path.
    obs_data_set_bool(settings,"is_local_file",false);
    obs_data_set_bool(settings,"seekable",false);
    obs_data_set_string(settings,"input",file.generic_string().c_str());
    obs_data_set_string(settings,"input_format","hevc");
    obs_data_set_bool(settings,"looping",false);
    obs_data_set_bool(settings,"is_hw_decoding",false);
    obs_data_set_bool(settings,"restart_on_activate",false);
    obs_data_set_bool(settings,"clear_on_media_end",false);
    obs_data_set_bool(settings,"close_when_inactive",false);
    obs_data_set_string(settings,"ffmpeg_options",decoderOptions.c_str());
    obs_source_update(source,settings);obs_data_release(settings);
    const auto wait=WaitForSingleObject(probe->complete,90000);
    const auto framesWait=WaitForSingleObject(probe->framesComplete,90000);
    obs_source_media_stop(source);
    signal_handler_disconnect(signals,"media_ended",ended,probe);
    obs_source_filter_remove(source,filter);
    const auto failure=probe->failure;
    auto frames=std::move(probe->frames); // Removal synchronizes with the exact final callback.
    std::cout<<"NR_BOOTSTRAP_DECODER_STATE {\"expected\":"<<expectedFrames<<",\"observed\":"<<frames.size()
        <<",\"stream_rate_num\":"<<rate.numerator<<",\"stream_rate_den\":"<<rate.denominator
        <<",\"observation_rate_num\":"<<video.fps_num<<",\"observation_rate_den\":"<<video.fps_den
        <<",\"media_ended\":"<<(wait==WAIT_OBJECT_0?"true":"false")
        <<",\"frame_event\":"<<(framesWait==WAIT_OBJECT_0?"true":"false")<<"}"<<std::endl;
    require(wait==WAIT_OBJECT_0,"recording decoder exact media-ended timeout");
    require(failure.empty(),failure.c_str());
    require(framesWait==WAIT_OBJECT_0 && frames.size()==expectedFrames,"recording decoder missing/extra actual frames");
    for(const auto& frame:frames) {
        const bool tenBit=frame.format==VIDEO_FORMAT_I010 || frame.format==VIDEO_FORMAT_P010;
        require(hdr?tenBit:!tenBit,"recording decoder actual bit depth");
        require(hdr?frame.transfer==VIDEO_TRC_PQ:frame.transfer!=VIDEO_TRC_PQ,
            "recording decoder actual HDR transfer");
    }
    return frames;
}
}
