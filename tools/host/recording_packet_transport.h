// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <obs.h>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>
namespace nr::qa {
// Annex-B HEVC, one layer and one frame picture per official NVENC packet.
// first_slice_segment_in_pic_flag is the first RBSP bit after the two-byte NAL
// header; emulation prevention cannot precede that bit.
inline bool recording_hevc_single_picture(const uint8_t* data,size_t bytes,bool* idr=nullptr)
{
    if(idr)*idr=false;
    if(!data || bytes<6)return false;
    size_t pictures=0,vcl=0;
    for(size_t i=0;i+3<bytes;) {
        size_t prefix=0;
        if(data[i]==0 && data[i+1]==0 && data[i+2]==1)prefix=3;
        else if(i+4<bytes && data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1)prefix=4;
        if(!prefix){++i;continue;}
        const auto offset=i+prefix;
        if(offset+2>=bytes || (data[offset]&0x80) || !(data[offset+1]&7))return false;
        const auto layer=static_cast<unsigned>((data[offset]&1)<<5)|(data[offset+1]>>3);
        if(layer)return false;
        const auto type=(data[offset]>>1)&63;
        if(type<=31) {
            ++vcl;
            if(data[offset+2]&0x80) {
                ++pictures;
                if(idr)*idr=type==19 || type==20;
            }
            else if(!pictures)return false;
        }
        i=offset+2;
    }
    return vcl>0 && pictures==1;
}
// Public packet values only. This is not a producer or encoder-input receipt.
// In particular sys_dts_usec is not the timestamp of the copied GPU texture.
struct PacketTransportReceipt {
    size_t occurrence;
    int64_t pts,dts,dtsUsec,systemDtsUsec;
    int32_t timebaseNumerator,timebaseDenominator;
    size_t bytes;
    bool keyframe,ownedEncoder,singlePicture,idrPicture;
    uintptr_t encoderIdentity;
};
inline PacketTransportReceipt recording_packet_transport(const encoder_packet& packet,
    size_t occurrence,const obs_encoder_t* expected)
{
    bool idr=false;
    const bool single=recording_hevc_single_picture(packet.data,packet.size,&idr);
    return {occurrence,packet.pts,packet.dts,packet.dts_usec,packet.sys_dts_usec,
        packet.timebase_num,packet.timebase_den,packet.size,packet.keyframe,
        packet.encoder && packet.encoder==expected,single,idr,
        reinterpret_cast<uintptr_t>(packet.encoder)};
}
inline std::string recording_packet_transport_json(const PacketTransportReceipt& value)
{
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::boolalpha;
    out<<"{\"occurrence\":"<<value.occurrence<<",\"pts\":"<<value.pts<<",\"dts\":"<<value.dts
        <<",\"dts_usec\":"<<value.dtsUsec<<",\"sys_dts_usec\":"<<value.systemDtsUsec
        <<",\"timebase_num\":"<<value.timebaseNumerator<<",\"timebase_den\":"<<value.timebaseDenominator
        <<",\"bytes\":"<<value.bytes<<",\"keyframe\":"<<value.keyframe
        <<",\"owned_encoder\":"<<value.ownedEncoder<<",\"single_picture\":"<<value.singlePicture
        <<",\"idr_picture\":"<<value.idrPicture
        <<",\"encoder_identity\":"<<value.encoderIdentity<<"}";
    return out.str();
}
}
