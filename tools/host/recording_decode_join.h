// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <locale>
#include <sstream>
#include <string>
namespace nr::qa {
// Read-only diagnostic. Its result never participates in the original lookup or require.
template<class Frames,class Rows,class ContentId>
std::string recording_decode_join(unsigned lane,bool hdr,bool nrOff,uint32_t imageId,
    uint32_t planesId,uint64_t decodedTimestamp,const Frames& preencode,const Rows& rows,ContentId contentId) {
    const auto before=std::find_if(preencode.begin(),preencode.end(),
        [&](const auto& frame){return contentId(frame)==imageId;});
    const bool beforeFound=before!=preencode.end();
    const auto pair=std::find_if(rows.begin(),rows.end(),[&](const auto& row){
        return beforeFound && row.programId==imageId && row.clock==before->timestamp;
    });
    const bool pairFound=pair!=rows.end();
    const bool programFound=std::any_of(rows.begin(),rows.end(),
        [&](const auto& row){return row.programId==imageId;});
    const bool clockFound=beforeFound && std::any_of(rows.begin(),rows.end(),
        [&](const auto& row){return row.clock==before->timestamp;});
    const auto kindPass=[&](const auto& row){
        return nrOff?row.kind==0:
            (row.kind==3 && row.genuine) || (row.kind==2 && row.betweenViews);
    };
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::boolalpha;
    out<<"{\"lane\":"<<lane<<",\"hdr\":"<<hdr<<",\"nrOff\":"<<nrOff
        <<",\"decoderImageId\":"<<imageId<<",\"decoderPlanesId\":"<<planesId
        <<",\"decodedTimestamp\":"<<decodedTimestamp
        <<",\"independentIdsEqual\":"<<(imageId==planesId)
        <<",\"preencodeIdFound\":"<<beforeFound<<",\"before\":";
    if(beforeFound)out<<"{\"id\":"<<contentId(*before)<<",\"timestamp\":"<<before->timestamp<<"}";
    else out<<"null";
    out<<",\"rowProgramIdFound\":"<<programFound<<",\"rowClockFound\":"<<clockFound
        <<",\"exactFoundPair\":"<<pairFound<<",\"firstPairKindSatisfied\":"<<(pairFound && kindPass(*pair))
        <<",\"originalJoinKindSatisfied\":"<<(pairFound && kindPass(*pair))
        <<",\"firstPairIndex\":";
    if(pairFound)out<<(pair-rows.begin());else out<<"null";
    out<<",\"preencodeCount\":"<<preencode.size()<<",\"preencodePrinted\":"<<std::min<size_t>(16,preencode.size())
        <<",\"preencodeTruncated\":"<<(preencode.size()>16)<<",\"preencode\":[";
    for(size_t i=0;i<std::min<size_t>(16,preencode.size());++i) {
        if(i)out<<",";
        out<<"{\"index\":"<<i<<",\"id\":"<<contentId(preencode[i])
            <<",\"timestamp\":"<<preencode[i].timestamp<<"}";
    }
    out<<"],\"rowCount\":"<<rows.size()<<",\"rowsPrinted\":"<<std::min<size_t>(16,rows.size())
        <<",\"rowsTruncated\":"<<(rows.size()>16)<<",\"rows\":[";
    for(size_t i=0;i<std::min<size_t>(16,rows.size());++i) {
        const auto& row=rows[i];if(i)out<<",";
        out<<"{\"index\":"<<i<<",\"programId\":"<<row.programId<<",\"sourceId\":"<<row.sourceId
            <<",\"selected\":"<<row.selected<<",\"ingress\":"<<row.ingress<<",\"clock\":"<<row.clock
            <<",\"tick\":"<<row.tick<<",\"kind\":"<<row.kind<<",\"genuine\":"<<row.genuine
            <<",\"betweenViews\":"<<row.betweenViews<<",\"programIdEquals\":"<<(row.programId==imageId)
            <<",\"clockEquals\":";
        if(beforeFound)out<<(row.clock==before->timestamp);else out<<"null";
        out<<",\"pairEquals\":"<<(beforeFound && row.programId==imageId && row.clock==before->timestamp)
            <<",\"kindSatisfied\":"<<kindPass(row)<<"}";
    }
    out<<"]}";return out.str();
}
}
