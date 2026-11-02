// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_bootstrap_images.h"
#include <DirectXPackedVector.h>
#include <cmath>
#include <cstring>
namespace nr::qa {
// Reuse independently decoded source bit rows AND moving landmark. Additional
// content is authored at ingress, never stamped onto the filter/recording.
inline std::vector<uint8_t> recording_fixture_bytes(uint32_t id,bool hdr)
{
    auto bytes=bootstrap::fixture_bytes(id,hdr);
    for(uint32_t y=420;y<900;++y)for(uint32_t x=32;x<1700;++x) {
        const bool cut=id>=8;
        const bool hud=y<452 && ((x/8+y/8)%3==0);
        const bool moving=x>=240+id*24 && x<320+id*24 && y>=520 && y<700;
        const float detail=static_cast<float>((x+id*3+y*7)%17)/4096;
        std::array<float,4> p{(cut?.03f:.01f)+detail,(cut?.01f:.03f)+detail,.02f+detail,1};
        if(hud)p={.7f,.7f,.7f,1};
        if(moving)p={.3f,.15f,.05f,1};
        if(hdr)for(size_t c=0;c<3;++c)p[c]*=3;
        const auto offset=(static_cast<size_t>(y)*bootstrap::width+x)*(hdr?8:4);
        for(size_t c=0;c<4;++c) {
            if(hdr) {
                const auto half=DirectX::PackedVector::XMConvertFloatToHalf(p[c]);
                memcpy(bytes.data()+offset+c*2,&half,2);
            }else bytes[offset+c]=static_cast<uint8_t>(std::lround(p[c]*255));
        }
    }
    return bytes;
}
}
