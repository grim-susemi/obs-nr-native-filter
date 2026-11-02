// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_recording_decode.h"
#include <array>
#include <cstdint>
#include <stdexcept>
namespace nr::qa {
struct ProgramMarker {
    static constexpr uint32_t x=1800,y=956,width=112,height=8,cell=2;
    static constexpr uint32_t bits=112,cells=224,columns=56;
    static constexpr uint32_t sync=0xd593,guard=0xb62d91a8;
    using Cells=std::array<uint8_t,cells>;
    static uint32_t wire(uint32_t epoch,uint32_t ingress,uint32_t phase=0) {
        if(!epoch || epoch>255 || !ingress || ingress>256 || phase>2)
            throw std::runtime_error("program witness declared schedule");
        return (epoch<<20)|(ingress<<4)|phase;
    }
    static uint32_t checksum(uint32_t id) {
        uint32_t value=0xffffffff;
        for(unsigned byte=0;byte<4;++byte) {
            value^=(id>>(byte*8))&255;
            for(unsigned bit=0;bit<8;++bit)
                value=(value>>1)^(0xedb88320u&static_cast<uint32_t>(-static_cast<int32_t>(value&1)));
        }
        return ~value;
    }
    static bool bit(uint32_t id,uint32_t index) {
        if(index<16)return ((sync>>index)&1)!=0;
        if(index<48)return ((id>>(index-16))&1)!=0;
        if(index<80)return ((checksum(id)>>(index-48))&1)!=0;
        return ((guard>>(index-80))&1)!=0;
    }
    static Cells encode(uint32_t id) {
        if(!id)throw std::runtime_error("program witness zero wire");
        Cells result{};
        for(uint32_t i=0;i<bits;++i) {
            result[2*i]=bit(id,i)?1u:0u;result[2*i+1]=result[2*i]^1u;
        }
        return result;
    }
    template<class Code> static uint32_t read(const DecodedFrame& frame,Code code) {
        if(frame.width!=1920 || frame.height!=1080 ||
            (frame.format!=VIDEO_FORMAT_NV12 && frame.format!=VIDEO_FORMAT_P010 &&
                frame.format!=VIDEO_FORMAT_I420 && frame.format!=VIDEO_FORMAT_I010))
            throw std::runtime_error("program witness plane format/extent");
        const bool ten=frame.format==VIDEO_FORMAT_P010 || frame.format==VIDEO_FORMAT_I010;
        const int32_t minimum=ten?4:8;
        const bool planar=frame.format==VIDEO_FORMAT_I420 || frame.format==VIDEO_FORMAT_I010;
        uint32_t words[4]{};
        for(uint32_t i=0;i<bits;++i) {
            int32_t sum[2]{};
            for(uint32_t side=0;side<2;++side) {
                const auto index=i*2+side;
                const auto cx=(x+(index%columns)*cell)/2,cy=(y+(index/columns)*cell)/2;
                sum[side]=static_cast<int32_t>(code(frame,1,planar?cx:cx*2,cy));
            }
            const auto difference=sum[0]-sum[1];
            if(difference>-minimum && difference<minimum)
                throw std::runtime_error("program witness corrupt complementary cells");
            const bool value=difference>0;
            const auto word=i<16?0u:i<48?1u:i<80?2u:3u;
            const auto offset=i<16?i:i<48?i-16:i<80?i-48:i-80;
            if(value)words[word]|=1u<<offset;
        }
        if(words[0]!=sync || words[3]!=guard || !words[1] || words[2]!=checksum(words[1]))
            throw std::runtime_error("program witness corrupt codeword");
        return words[1];
    }
};
}
