// SPDX-License-Identifier: GPL-3.0-only
#include "nr_bootstrap_images.h"
#include <DirectXPackedVector.h>
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace nr::qa::bootstrap {
namespace {
void require(bool v,const char* why) { if(!v) throw std::runtime_error(why); }
double pq(double c) {
    const double a=std::pow(std::clamp(c,0.0,1.0),1.0/78.84375);
    return 10000*std::pow(std::max(a-0.8359375,0.0)/(18.8515625-18.6875*a),1.0/0.1593017578125)/80;
}
double mean(const Image& image,uint32_t x,uint32_t y) {
    double value=0;
    for(uint32_t dy=0;dy<8;++dy)for(uint32_t dx=0;dx<8;++dx) {
        const auto& p=image.pixels[(y+dy)*width+x+dx];
        value+=(p[0]+p[1]+p[2])/3;
    }
    return value/64;
}
}
std::array<float,4> fixture_pixel(uint32_t x,uint32_t y,uint32_t id,bool hdr) {
    const uint32_t shifted=x+id;
    uint32_t noise=shifted*0x9e3779b9u^y*0x85ebca6bu;noise^=noise>>16;
    const float tile=static_cast<float>(((shifted/32)^(y/32))&1u);
    std::array<float,4> p{.12f+.18f*tile+static_cast<float>(noise&31)/1024,
        .17f+.18f*tile+static_cast<float>((noise>>5)&31)/1024,
        .22f+.18f*tile+static_cast<float>((noise>>10)&31)/1024,1};
    if(hdr)for(size_t c=0;c<3;++c)p[c]*=3;
    const float high=hdr?3.75f:.85f,low=hdr?.05f:.03f;
    if((y>=32 && y<72)||(y>=96 && y<136)) {
        if(x>=32 && x<416) {
            const auto bit=(x-32)/48;
            if((x-32)%48<40) { const auto v=(id&(1u<<bit))?high:low;p={v,v,v,1}; }
        }
        if(x>=640 && x<680)p={high,high,high,1};
        if(x>=704 && x<744)p={low,low,low,1};
    }
    if(y>=256 && y<320 && x>=800 && x<1360)p={low,low,low,1};
    if(y>=256 && y<320 && x>=800+id*landmark_pitch && x<824+id*landmark_pitch)p={high,high,high,1};
    if(hdr && x>=1400 && x<1600) {
        if(y<200)p={12.5f,6.25f,3.125f,1}; // 1000/500/250 nits, scRGB80.
        else if(y>=240 && y<400)p={3.75f,3.75f,3.75f,1};
        else if(y>=440 && y<600)p={1.875f,.625f,.25f,1};
    }
    if(y>=960 && y<1040 && x<512) {
        p[3]=static_cast<float>(x/128)/3;
        for(size_t c=0;c<3;++c)p[c]*=p[3];
    }
    return p;
}
std::vector<uint8_t> fixture_bytes(uint32_t id,bool hdr) {
    require(id>0 && id<=max_content_id,"bounded source content ID");
    std::vector<uint8_t> bytes(static_cast<size_t>(width)*height*(hdr?8:4));
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const auto p=fixture_pixel(x,y,id,hdr);const auto offset=(static_cast<size_t>(y)*width+x)*(hdr?8:4);
        for(size_t c=0;c<4;++c) {
            if(hdr) { const auto half=DirectX::PackedVector::XMConvertFloatToHalf(p[c]);memcpy(bytes.data()+offset+c*2,&half,2); }
            else bytes[offset+c]=static_cast<uint8_t>(std::lround(p[c]*255));
        }
    }
    return bytes;
}
std::vector<uint8_t> ghost_fixture_bytes(const ghost::Fixture& f,unsigned tick,bool linearSource) {
    require(tick<256&&(linearSource||f.color==ghost::FixtureColor::Sdr||f.color==ghost::FixtureColor::DarkSdr),
        "frozen full-size packing selector");
    const auto& t=f.schedule.at(tick);
    const double scale=f.color==ghost::FixtureColor::DarkSdr?.0625:1;
    const auto pack=[&](ghost::Rgb c,double a) {
        if(f.color==ghost::FixtureColor::Hdr2020)c={
            1.6604910021*c.r-.5876411388*c.g-.0728498633*c.b,
            -.1245504745*c.r+1.1328998971*c.g-.0083494226*c.b,
            -.0181507634*c.r-.1005788980*c.g+1.1187296614*c.b};
        const auto gamma=[](double v){return v<=.0031308?12.92*v:1.055*std::pow(std::max(v,0.0),1/2.4)-.055;};
        const double factor=f.referenceWhiteNits/80;
        const std::array<double,4> value{linearSource?c.r*factor*a:gamma(c.r)*a,
            linearSource?c.g*factor*a:gamma(c.g)*a,linearSource?c.b*factor*a:gamma(c.b)*a,a};
        uint64_t bits=0;auto* bytes=reinterpret_cast<uint8_t*>(&bits);
        for(size_t channel=0;channel<4;++channel) {
            if(linearSource) {
                const auto half=DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(value[channel]));
                memcpy(bytes+channel*2,&half,2);
            }else bytes[channel]=static_cast<uint8_t>(std::lround(std::clamp(value[channel],0.0,1.0)*255));
        }
        return bits;
    };
    std::array<uint64_t,256> background{},transparent{};
    for(size_t code=0;code<256;++code) {
        const double texture=static_cast<double>(code)/255*.125;
        const ghost::Rgb c{(.125+texture)*scale,(.25+texture)*scale,(.1875+texture)*scale};
        background[code]=pack(c,1);transparent[code]=pack(c,0);
    }
    auto checker=t.foreground;
    checker.r+=.0625*scale;checker.g+=.0625*scale;checker.b+=.0625*scale;
    const std::array<uint64_t,2> foreground{pack(t.foreground,1),pack(checker,1)};
    const auto hud=pack({scale,scale,scale},1),highlight=pack({4,2,1},1);
    const std::array<uint64_t,2> header{pack({.125,.125,.125},1),pack({.875,.875,.875},1)};
    std::array<uint64_t,3> landmarks{};
    constexpr std::array<unsigned,3> locations{83,127,179};
    for(size_t i=0;i<locations.size();++i) {
        const auto code=(t.contentId*(locations[i]+1)+f.seed)&255;
        landmarks[i]=pack({static_cast<double>(code)/256,static_cast<double>(255-code)/256,.375},1);
    }
    const size_t stride=linearSource?8u:4u;
    std::vector<uint8_t> result(static_cast<size_t>(width)*height*stride);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const auto phase=static_cast<unsigned>((static_cast<int>(x)+t.pan+8192)%8192);
        uint32_t state=f.seed^((phase+1)*0x9E3779B9u)^((y+1)*0x85EBCA6Bu);
        state^=state<<13;state^=state>>17;state^=state<<5;
        uint64_t bits=(x<128&&y>=960&&y<1040?transparent:background)[state&255];
        if(x<192&&y<96) {
            if((t.sameColoredReveal&&y>=28&&y<60&&x>=16&&x<144)||ghost::foreground(t,x,y))
                bits=foreground[(x+y)&1];
            if(y>=84&&x>=160&&f.color!=ghost::FixtureColor::Sdr&&f.color!=ghost::FixtureColor::DarkSdr)
                bits=highlight;
            if(y<4&&x<64)bits=header[(y<2?((t.contentId>>(x/4))&1):!((t.contentId>>(x/4))&1))?1:0];
            if(y==6)for(size_t i=0;i<locations.size();++i)if(x==locations[i])bits=landmarks[i];
        }
        if(y==80)bits=hud;
        memcpy(result.data()+(static_cast<size_t>(y)*width+x)*stride,&bits,stride);
    }
    return result;
}
double ghost_raw_maximum(const ghost::Fixture& f,unsigned tick,bool linearSource,
    std::span<const uint8_t> bytes,double bound,RawScanCounts* output) {
    require(tick<256&&(linearSource||f.color==ghost::FixtureColor::Sdr||f.color==ghost::FixtureColor::DarkSdr)&&
        std::isfinite(bound)&&bound>=0,"raw calibration selector/bound");
    require(bytes.size()==static_cast<size_t>(width)*height*(linearSource?8u:4u),
        "complete packed raw calibration bounds");
    const auto& t=f.schedule.at(tick);
    const double scale=f.color==ghost::FixtureColor::DarkSdr?.0625:1;
    const double factor=80/f.referenceWhiteNits;
    std::array<ghost::Rgb,256> background{};
    std::array<double,256> transfer{};
    for(unsigned code=0;code<256;++code) {
        const double texture=static_cast<double>(code)/255*.125;
        background[code]={(.125+texture)*scale,(.25+texture)*scale,(.1875+texture)*scale};
        const double v=static_cast<double>(code)/255;
        transfer[code]=v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);
    }
    const auto* palette=background.data();const auto* gamma=transfer.data();
    const auto* source=bytes.data();
    static_assert(sizeof(double)==sizeof(uint64_t)&&std::numeric_limits<double>::is_iec559);
    const auto absolute=[](double value) {
        return std::bit_cast<double>(std::bit_cast<uint64_t>(value)&~(uint64_t{1}<<63));
    };
    double maximum=0;uint64_t alphaErrors=0;
    RawScanCounts local;auto& counts=output?*output:local;counts={};
    const auto evaluate=[&](uint64_t packed,ghost::Rgb expected,bool transparent) {
        ghost::Rgb actual{};
        if(linearSource) {
            require((packed&0x7c00)!=0x7c00&&((packed>>16)&0x7c00)!=0x7c00&&
                ((packed>>32)&0x7c00)!=0x7c00&&((packed>>48)&0x7c00)!=0x7c00,
                "nonfinite full packed raw channel");
            actual={DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(packed)),
                DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(packed>>16)),
                DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(packed>>32))};
            if(DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(packed>>48))!=(transparent?0:1))
                ++alphaErrors;
        }else {
            const auto* value=reinterpret_cast<const uint8_t*>(&packed);
            actual={gamma[value[0]],gamma[value[1]],gamma[value[2]]};
            if(value[3]!=(transparent?0:255))++alphaErrors;
        }
        if(f.color==ghost::FixtureColor::Hdr2020)actual={
            .6274038959*actual.r+.3292830384*actual.g+.0433130657*actual.b,
            .0690972894*actual.r+.9195403951*actual.g+.0113623155*actual.b,
            .0163914389*actual.r+.0880133079*actual.g+.8955952532*actual.b};
        actual={actual.r*factor,actual.g*factor,actual.b*factor};
        return std::max({absolute(actual.r-expected.r),absolute(actual.g-expected.g),
            absolute(actual.b-expected.b)});
    };
    struct Cached {uint64_t word;double error;};
    static_assert(sizeof(Cached)==16);
    std::array<Cached,256> cached{};
    const double encodingFactor=f.referenceWhiteNits/80;
    for(unsigned code=0;code<256;++code) {
        auto c=palette[code];
        if(f.color==ghost::FixtureColor::Hdr2020)c={
            1.6604910021*c.r-.5876411388*c.g-.0728498633*c.b,
            -.1245504745*c.r+1.1328998971*c.g-.0083494226*c.b,
            -.0181507634*c.r-.1005788980*c.g+1.1187296614*c.b};
        const auto gammaEncode=[](double v){return v<=.0031308?12.92*v:
            1.055*std::pow(std::max(v,0.0),1/2.4)-.055;};
        const std::array<double,4> value{linearSource?c.r*encodingFactor*1.0:gammaEncode(c.r)*1.0,
            linearSource?c.g*encodingFactor*1.0:gammaEncode(c.g)*1.0,
            linearSource?c.b*encodingFactor*1.0:gammaEncode(c.b)*1.0,1};
        uint64_t word=0;auto* packed=reinterpret_cast<uint8_t*>(&word);
        for(size_t channel=0;channel<4;++channel) {
            if(linearSource) {
                const auto half=DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(value[channel]));
                memcpy(packed+channel*2,&half,2);
            }else packed[channel]=static_cast<uint8_t>(std::lround(std::clamp(value[channel],0.0,1.0)*255));
        }
        cached[code]={word,evaluate(word,palette[code],false)};
    }
    const auto* entries=cached.data();const size_t stride=linearSource?8u:4u;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const auto phase=static_cast<unsigned>((static_cast<int>(x)+t.pan+8192)%8192);
        uint32_t state=f.seed^((phase+1)*0x9E3779B9u)^((y+1)*0x85EBCA6Bu);
        state^=state<<13;state^=state>>17;state^=state<<5;
        const auto code=state&255;const auto& entry=entries[code];
        uint64_t word=0;memcpy(&word,source,stride);source+=stride;
        const bool special=(x<192&&y<96)||y==80;
        const bool transparent=x<128&&y>=960&&y<1040;
        double error=0;
        if(!special&&!transparent&&word==entry.word) {
            ++counts.matches;error=entry.error;
        }else {
            if(special||transparent)++counts.special;else ++counts.mismatches;
            auto expected=special?ghost::clean_pixel(f,t,x,y):palette[code];
            if(transparent)expected={0,0,0};
            error=evaluate(word,expected,transparent);
        }
        maximum=std::max(maximum,error);
    }
    require(alphaErrors==0,"full packed original alpha differs from fixture");
    require(maximum<=bound,"complete actual raw/color admission");
    return maximum;
}
Image rgb_image(std::span<const uint8_t> bytes,bool hdr,bool bgra,const char* stage) {
    require(bytes.size()==static_cast<size_t>(width)*height*(hdr?8:4),"full-size RGB image bounds");
    Image image{hdr,{}};image.pixels.resize(static_cast<size_t>(width)*height);
    const auto zero=_mm_setzero_si128();
    if(hdr) {
        const auto exponentMask=_mm_set1_epi32(0x7c00),mantissaMask=_mm_set1_epi32(0x03ff);
        for(size_t i=0;i<image.pixels.size();++i) {
            const auto words=_mm_unpacklo_epi16(
                _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes.data()+i*8)),zero);
            const auto exponent=_mm_and_si128(words,exponentMask);
            if(_mm_movemask_epi8(_mm_cmpeq_epi32(exponent,exponentMask))) {
                for(size_t c=0;c<4;++c) {
                    uint16_t packed=0;memcpy(&packed,bytes.data()+i*8+c*2,2);
                    if((packed&0x7c00)==0x7c00)
                        throw std::runtime_error(std::string(stage)+" nonfinite actual RGB pixel="+std::to_string(i)+
                            " channel="+std::to_string(c)+" packed="+std::to_string(packed));
                }
            }
            const auto sign=_mm_slli_epi32(_mm_and_si128(words,_mm_set1_epi32(0x8000)),16);
            const auto mantissa=_mm_and_si128(words,mantissaMask);
            const auto normal=_mm_add_epi32(_mm_slli_epi32(_mm_and_si128(words,_mm_set1_epi32(0x7fff)),13),
                _mm_set1_epi32(0x38000000));
            // Every half subnormal is a normal float. This exact power-of-two
            // scaling also produces +0; apply the original sign bits afterward.
            const auto small=_mm_castps_si128(_mm_mul_ps(_mm_cvtepi32_ps(mantissa),_mm_set1_ps(0x1p-24f)));
            const auto isSmall=_mm_cmpeq_epi32(exponent,zero);
            const auto value=_mm_or_si128(sign,_mm_or_si128(_mm_and_si128(isSmall,small),
                _mm_andnot_si128(isSmall,normal)));
            _mm_storeu_ps(image.pixels[i].data(),_mm_castsi128_ps(value));
        }
    }else {
        for(size_t i=0;i<image.pixels.size();++i) {
            int32_t packed=0;memcpy(&packed,bytes.data()+i*4,4);
            auto channels=_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(packed),zero),zero);
            if(bgra)channels=_mm_shuffle_epi32(channels,_MM_SHUFFLE(3,0,1,2));
            _mm_storeu_ps(image.pixels[i].data(),_mm_cvtepi32_ps(channels));
            // Preserve the scalar float division, not a reciprocal multiply.
            for(size_t c=0;c<4;++c)image.pixels[i][c]=image.pixels[i][c]/255;
        }
    }
    return image;
}
ghost::Rgb packed_rgb(std::span<const uint8_t> bytes,size_t pixel,bool half,bool gamma,bool bgra) {
    const size_t stride=half?8u:4u;
    require(pixel<bytes.size()/stride,"actual packed RGB pixel bounds");
    static const auto transfer=[] {
        std::array<double,256> values{};
        for(size_t code=0;code<values.size();++code) {
            const double v=static_cast<double>(code)/255;
            values[code]=v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);
        }
        return values;
    }();
    std::array<double,3> rgb{};
    for(size_t channel=0;channel<rgb.size();++channel) {
        const size_t source=bgra&&channel!=1?2-channel:channel;
        if(half) {
            uint16_t word=0;memcpy(&word,bytes.data()+pixel*stride+source*2,2);
            const double v=DirectX::PackedVector::XMConvertHalfToFloat(word);
            rgb[channel]=gamma?(v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4)):v;
        }else {
            const auto code=bytes[pixel*stride+source];
            rgb[channel]=gamma?transfer[code]:static_cast<float>(code)/255;
        }
    }
    return {rgb[0],rgb[1],rgb[2]};
}
uint32_t code(const DecodedFrame& frame,size_t plane,uint32_t x,uint32_t y) {
    const bool ten=frame.format==VIDEO_FORMAT_I010 || frame.format==VIDEO_FORMAT_P010;
    const auto at=static_cast<size_t>(y)*frame.rowBytes[plane]+x*(ten?2:1);
    require(at+(ten?2:1)<=frame.planes[plane].size(),"actual YUV sample bounds");
    if(!ten)return frame.planes[plane][at];
    uint16_t value=0;memcpy(&value,frame.planes[plane].data()+at,2);
    return frame.format==VIDEO_FORMAT_P010?value>>6:value;
}
Image decoded_image(const DecodedFrame& frame,bool hdr) {
    require(frame.width==width && frame.height==height && !frame.fullRange,"decoded image size/range");
    const bool planar=frame.format==VIDEO_FORMAT_I420 || frame.format==VIDEO_FORMAT_I010;
    const double scale=hdr?4:1,kr=hdr?.2627:.2126,kb=hdr?.0593:.0722;
    Image image{hdr,{}};image.pixels.resize(static_cast<size_t>(width)*height);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const double yy=(code(frame,0,x,y)-16*scale)/(219*scale);
        const double cb=(code(frame,1,planar?x/2:(x/2)*2,y/2)-128*scale)/(224*scale);
        const double cr=(code(frame,planar?2:1,planar?x/2:(x/2)*2+1,y/2)-128*scale)/(224*scale);
        double r=yy+2*(1-kr)*cr,b=yy+2*(1-kb)*cb,g=(yy-kr*r-kb*b)/(1-kr-kb);
        if(hdr) {
            r=pq(r);g=pq(g);b=pq(b);
            // Independent Rec2020 RGB -> XYZ -> Rec709 RGB, D65.
            const double xx=.6369580483*r+.1446169036*g+.1688809752*b;
            const double zz=.0280726930*g+1.0609850577*b;
            const double lum=.2627002120*r+.6779980715*g+.0593017165*b;
            r=3.2409699419*xx-1.5373831776*lum-.4986107603*zz;
            g=-.9692436363*xx+1.8759675015*lum+.0415550574*zz;
            b=.0556300797*xx-.2039769589*lum+1.0569715142*zz;
        }
        image.pixels[static_cast<size_t>(y)*width+x]={static_cast<float>(r),static_cast<float>(g),static_cast<float>(b),1};
    }
    return image;
}
uint32_t content_id(const Image& image) {
    require(image.pixels.size()==static_cast<size_t>(width)*height,"content image bounds");
    const double high=mean(image,652,44),low=mean(image,716,44);
    require(high-low>(image.hdr?.05:.05),"blank/low-contrast source anchors");
    const double threshold=(high+low)/2;
    uint32_t ids[2]{};
    for(unsigned row=0;row<2;++row)for(unsigned bit=0;bit<8;++bit)
        if(mean(image,44+bit*48,44+row*64)>threshold)ids[row]|=1u<<bit;
    require(ids[0]==ids[1] && ids[0]>0 && ids[0]<=max_content_id,"independent source bit rows disagree");
    const auto x=804+ids[0]*landmark_pitch;
    require(mean(image,x,276)>threshold,"metadata/bit stamp without same-ID moving source landmark");
    return ids[0];
}
uint32_t content_id(const DecodedFrame& frame) {
    auto mean_y=[&](uint32_t x,uint32_t y) {
        double sum=0;for(uint32_t dy=0;dy<8;++dy)for(uint32_t dx=0;dx<8;++dx)sum+=code(frame,0,x+dx,y+dy);
        return sum/64;
    };
    const double high=mean_y(652,44),low=mean_y(716,44);
    require(high-low>4,"blank decoded/preencode source anchors");
    const double threshold=(high+low)/2;uint32_t ids[2]{};
    for(unsigned row=0;row<2;++row)for(unsigned bit=0;bit<8;++bit)
        if(mean_y(44+bit*48,44+row*64)>threshold)ids[row]|=1u<<bit;
    require(ids[0]==ids[1] && ids[0]>0 && ids[0]<=max_content_id,"decoded independent source bit rows");
    require(mean_y(804+ids[0]*landmark_pitch,276)>threshold,"decoded metadata-only stamp without moving landmark");
    return ids[0];
}
bool parallel_hash_available() {
    if(!IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE)||
        (GetEnabledXStateFeatures()&0xe6)!=0xe6)return false;
    int bits[4]{};__cpuidex(bits,7,0);
    return (static_cast<unsigned>(bits[1])&0x30000)==0x30000;
}
std::array<uint64_t,8> hash_eight_avx512(const std::array<std::span<const uint8_t>,8>&);
std::array<uint64_t,8> hash_eight(const std::array<std::span<const uint8_t>,8>& bytes,bool parallel) {
    bool onlyThree=bytes[0].size()==bytes[1].size()&&bytes[0].size()==bytes[2].size();
    for(size_t lane=3;lane<bytes.size();++lane)onlyThree=onlyThree&&bytes[lane].empty();
    if(parallel&&onlyThree) {
        const auto first=hash_three({bytes[0],bytes[1],bytes[2]});
        std::array<uint64_t,8> result{};result.fill(14695981039346656037ull);
        std::copy(first.begin(),first.end(),result.begin());return result;
    }
    bool scalarProfile=true;size_t maximum=0;
    for(const auto stream:bytes)maximum=std::max(maximum,stream.size());
    const auto activeAt=[&](size_t at) {
        unsigned mask=0;
        for(size_t lane=0;lane<bytes.size();++lane)if(bytes[lane].size()>at)mask|=1u<<lane;
        return mask;
    };
    const auto supported=[](unsigned mask){return mask==0||mask==7||mask==63||mask==127||mask==255;};
    scalarProfile=supported(activeAt(0));
    for(const auto stream:bytes)scalarProfile=scalarProfile&&supported(activeAt(stream.size()));
    if(parallel&&scalarProfile) {
        constexpr uint64_t offset=14695981039346656037ull,prime=1099511628211ull;
        uint64_t a=offset,b=offset,c=offset,d=offset,e=offset,f=offset,g=offset,h=offset;
        size_t at=0;
#define NR_FNV_THREE() a=(a^bytes[0][at])*prime; b=(b^bytes[1][at])*prime; c=(c^bytes[2][at])*prime
#define NR_FNV_SIX() NR_FNV_THREE(); d=(d^bytes[3][at])*prime; e=(e^bytes[4][at])*prime; f=(f^bytes[5][at])*prime
        while(at<maximum) {
            const auto active=activeAt(at);size_t limit=maximum;
            for(const auto stream:bytes)if(stream.size()>at)limit=std::min(limit,stream.size());
            switch(active) {
            case 255:
#pragma loop(no_vector)
                for(;at<limit;++at){NR_FNV_SIX();g=(g^bytes[6][at])*prime;h=(h^bytes[7][at])*prime;}break;
            case 127:
#pragma loop(no_vector)
                for(;at<limit;++at){NR_FNV_SIX();g=(g^bytes[6][at])*prime;}break;
            case 63:
#pragma loop(no_vector)
                for(;at<limit;++at){NR_FNV_SIX();}break;
            case 7:
#pragma loop(no_vector)
                for(;at<limit;++at){NR_FNV_THREE();}break;
            default:require(false,"scalar hash stream profile");
            }
        }
#undef NR_FNV_SIX
#undef NR_FNV_THREE
        return {a,b,c,d,e,f,g,h};
    }
    // This generic translation unit has no ISA flag. No specialized instruction
    // is reached before the actual CPU AND OS-state admission.
    static const bool available=parallel_hash_available();
    if(parallel&&available)return hash_eight_avx512(bytes);
    std::array<uint64_t,8> result{};
    for(size_t lane=0;lane<bytes.size();++lane)result[lane]=hash(bytes[lane]);
    return result;
}
bool finite_half(std::span<const uint8_t> bytes) {
    require(bytes.size()%2==0,"half-finiteness byte bounds");
    const auto exponents=_mm_set1_epi16(0x7c00);size_t at=0;
    for(;at+16<=bytes.size();at+=16) {
        const auto values=_mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes.data()+at));
        if(_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_and_si128(values,exponents),exponents)))return false;
    }
    for(;at<bytes.size();at+=2) {
        uint16_t word=0;memcpy(&word,bytes.data()+at,2);
        if((word&0x7c00)==0x7c00)return false;
    }
    return true;
}
uint64_t hash(std::span<const uint8_t> bytes) {
    uint64_t value=14695981039346656037ull;for(auto b:bytes)value=(value^b)*1099511628211ull;return value;
}
std::array<uint64_t,3> hash_three(const std::array<std::span<const uint8_t>,3>& bytes) {
    require(bytes[0].size()==bytes[1].size()&&bytes[0].size()==bytes[2].size(),"three byte-hash stream bounds");
    uint64_t a=14695981039346656037ull,b=a,c=a;
    const auto* first=bytes[0].data();const auto* second=bytes[1].data();const auto* third=bytes[2].data();
    for(size_t at=0;at<bytes[0].size();++at) {
        a=(a^first[at])*1099511628211ull;
        b=(b^second[at])*1099511628211ull;
        c=(c^third[at])*1099511628211ull;
    }
    return {a,b,c};
}
void save_bmp(const std::filesystem::path& path,const Image& image) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width)*height*4);
    for(size_t i=0;i<image.pixels.size();++i) {
        auto p=image.pixels[i];
        const float peak=std::max({p[0],p[1],p[2],0.0f});
        for(size_t c=0;c<3;++c) {
            float value=image.hdr?p[c]/(1+peak):p[c];
            if(image.hdr)value=value<=.0031308f?value*12.92f:1.055f*std::pow(std::max(value,0.0f),1/2.4f)-.055f;
            pixels[i*4+2-c]=static_cast<uint8_t>(std::lround(std::clamp(value,0.0f,1.0f)*255));
        }
        pixels[i*4+3]=255;
    }
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-static_cast<LONG>(height);
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header),sizeof(header));file.write(reinterpret_cast<const char*>(&info),sizeof(info));
    file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    require(file.good(),"actual image artifact write");
}
}
