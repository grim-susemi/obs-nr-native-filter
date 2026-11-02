// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "recording_occurrence.h"
#include "nr_recording_decode.h"
namespace nr::qa {
struct RecordingDecodedOccurrence {
    size_t delivery,render;
    std::shared_ptr<const RecordingProducer> producer;
};
inline RecordingDecodedOccurrence recording_decoded_occurrence(size_t ordinal,size_t decodedCount,
    const std::vector<size_t>& packetDeliveries,const RecordingOccurrences& ledger)
{
    if(decodedCount!=packetDeliveries.size() || ordinal>=decodedCount)
        throw std::runtime_error("recording decoded packet occurrence cardinality");
    const auto index=packetDeliveries[ordinal];
    if(index>=ledger.deliveries.size())throw std::runtime_error("recording decoded foreign delivery");
    const auto& delivery=ledger.deliveries[index];
    if(!delivery.render || delivery.unresolved || delivery.repetition ||
        *delivery.render>=ledger.renders.size() || delivery.epoch!=ledger.renders[*delivery.render].epoch)
        throw std::runtime_error("recording decoded unresolved producer occurrence");
    return {index,*delivery.render,ledger.renders[*delivery.render].producer};
}
template<class Code>
std::pair<uint64_t,uint64_t> recording_lossless_codes(const DecodedFrame& decoded,
    const DecodedFrame& raw,Code code)
{
    if(decoded.width!=raw.width || decoded.height!=raw.height || !raw.width || !raw.height ||
        raw.width%2 || raw.height%2)
        throw std::runtime_error("recording decoded lossless extent");
    uint64_t errors=0,compared=0;
    for(uint32_t y=0;y<raw.height;++y)for(uint32_t x=0;x<raw.width;++x) {
        if(code(decoded,0,x,y)!=code(raw,0,x,y))++errors;++compared;
    }
    const bool planar=decoded.format==VIDEO_FORMAT_I420 || decoded.format==VIDEO_FORMAT_I010;
    for(uint32_t y=0;y<raw.height/2;++y)for(uint32_t x=0;x<raw.width/2;++x)for(size_t c=0;c<2;++c) {
        if(code(decoded,planar?c+1:1,planar?x:x*2+static_cast<uint32_t>(c),y)!=
            code(raw,1,x*2+static_cast<uint32_t>(c),y))++errors;
        ++compared;
    }
    return {errors,compared};
}
}
