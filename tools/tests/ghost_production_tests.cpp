// SPDX-License-Identifier: GPL-3.0-only
#include "ghost_oracle.h"
#include "../host/nr_ghost_scenario.h"
#include "../host/nr_bootstrap_images.h"
#include "../host/nr_ghost_digest.h"
#include "../host/nr_ghost_chunk.h"
#include "ghost_sha_reference.h"
#include <DirectXPackedVector.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <bit>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string_view>
#include <future>
#include <fstream>
#include <thread>
#include <windows.h>
#include <memory>
#include <d3d12.h>
#include <wrl/client.h>
using namespace nr;
using namespace nr::ghost;
namespace {
unsigned checks=0;
void expect(bool condition,const char* why){++checks;if(!condition)throw std::runtime_error(why);}
void canvas_check() {
    const Extent working{1920,1080};
    for(uint32_t seed:{0xA17u,0xB29u})for(auto color:{FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
        const auto corpus=fixture(seed,color);
        const auto full=clean_image(corpus,40,working),leaf=clean_image(corpus,40);
        expect(full.size()==1920ull*1080,"production working extent");
        expect(decode_content(full,working)==41,"full-size independently decoded content");
        for(unsigned y=0;y<96;++y)for(unsigned x=0;x<192;++x)
            expect(full[y*working.width+x]==leaf[y*192+x],"frozen32pixel geometry/colors changed on full canvas");
        expect(decoded_landmarks(corpus,40,full,0,working),"full working-pixel landmarks");
        auto stamped=clean_image(corpus,38,working);
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<192;++x)stamped[y*working.width+x]=full[y*working.width+x];
        expect(!decoded_landmarks(corpus,40,stamped,0,working),"all header/landmark stamps hid stale modeled scene");
        const Calibration calibration{.000001,0,.000001};
        const auto clean=temporal(corpus,40,full,full,calibration,working);
        expect(clean.pass,"full-size clean temporal admission");
        const auto leafReport=temporal(corpus,40,leaf,leaf,calibration);
        expect(clean.templates.size()==leafReport.templates.size(),"working-canvas scoring omitted template");
        for(size_t i=0;i<leafReport.templates.size();++i)
            expect(clean.templates[i].denominator==leafReport.templates[i].denominator,"mask pixels were scaled or resampled");
    }
}
void templates_check() {
    const auto corpus=fixture(0xA17,FixtureColor::Sdr);
    const auto raw=clean_image(corpus,65);
    const Calibration calibration{.000001,0,.000001};
    const auto report=temporal(corpus,65,raw,raw,calibration);
    std::set<unsigned> ids;
    for(unsigned prior=0;prior<65;++prior)ids.insert(corpus.schedule[prior].contentId);
    expect(report.templates.size()==ids.size()*2,"prior distinct held-current template was silently omitted");
    unsigned heldRegions=0;
    for(const auto& t:report.templates)if(t.priorTick==64) {
        expect(t.zeroContrast&&t.denominator==0&&t.pass,"held zero contrast must be explicitly reported");
        ++heldRegions;
    }
    expect(heldRegions==2,"both held-current foreground regions required");
}
// Frozen pre-optimization equations, intentionally independent of clean_pixel
// and optimized temporal mask iteration.
Image frozen_image(const Fixture& f,unsigned tick,Extent canvas={192,96}) {
    const auto& t=f.schedule.at(tick);Image result(static_cast<size_t>(canvas.width)*canvas.height);
    const double s=f.color==FixtureColor::DarkSdr?.0625:1;
    for(unsigned y=0;y<canvas.height;++y)for(unsigned x=0;x<canvas.width;++x) {
        const unsigned phase=static_cast<unsigned>((static_cast<int>(x)+t.pan+8192)%8192);
        uint32_t state=f.seed^((phase+1)*0x9E3779B9u)^((y+1)*0x85EBCA6Bu);
        state^=state<<13;state^=state>>17;state^=state<<5;
        const double texture=static_cast<double>(state&255)/255*.125;
        Rgb c{(.125+texture)*s,(.25+texture)*s,(.1875+texture)*s};
        if(t.sameColoredReveal&&y>=28&&y<60&&x>=16&&x<144) {
            c=t.foreground;if((x+y)&1){c.r+=.0625*s;c.g+=.0625*s;c.b+=.0625*s;}
        }
        if(foreground(t,x,y)) {
            c=t.foreground;if((x+y)&1){c.r+=.0625*s;c.g+=.0625*s;c.b+=.0625*s;}
        }
        if(y==80)c={s,s,s};
        if(y>=84&&y<96&&x>=160&&x<192&&f.color!=FixtureColor::Sdr&&f.color!=FixtureColor::DarkSdr)c={4,2,1};
        if(y<4&&x<64) {
            const bool on=((t.contentId>>(x/4))&1)!=0;
            const double v=(y<2?on:!on)?.875:.125;c={v,v,v};
        }
        if(y==6&&(x==83||x==127||x==179)) {
            const unsigned code=(t.contentId*(x+1)+f.seed)&255;
            c={static_cast<double>(code)/256,static_cast<double>(255-code)/256,.375};
        }
        result[static_cast<size_t>(y)*canvas.width+x]=c;
    }
    return result;
}
TemporalReport frozen_temporal(const Fixture& f,unsigned tick,const Image& raw,const Image& output,
    const Calibration& calibration) {
    auto sub=[](Rgb a,Rgb b){return Rgb{a.r-b.r,a.g-b.g,a.b-b.b};};
    auto dot=[](Rgb a,Rgb b){return a.r*b.r+a.g*b.g+a.b*b.b;};
    const auto current=frozen_image(f,tick);TemporalReport report;report.currentTick=tick;
    std::vector<unsigned> distinct;
    for(unsigned prior=0;prior<tick;++prior) {
        const auto content=f.schedule[prior].contentId;
        if(std::find(distinct.begin(),distinct.end(),content)!=distinct.end())continue;
        distinct.push_back(content);const auto previous=frozen_image(f,prior);
        for(unsigned region=0;region<2;++region) {
            TemplateScore score{tick,prior,region,0,0,0,0,0,0,false,true};
            double norm1=0,incremental=0,rawTrail=0,total=0;
            for(unsigned y=8;y<96;++y)for(unsigned x=0;x<192;++x) {
                const auto& old=f.schedule[prior];
                const bool footprint=region==0?inside(old.bar,x,y):foreground(old,x,y)&&!inside(old.bar,x,y);
                if(!visible(x,y)||!footprint||foreground(f.schedule[tick],x,y))continue;
                const size_t p=y*192+x;const auto h=sub(previous[p],current[p]);++score.pixels;
                score.denominator+=dot(h,h);norm1+=std::abs(h.r)+std::abs(h.g)+std::abs(h.b);
                incremental+=std::max(0.0,dot(sub(output[p],raw[p]),h));
                rawTrail+=std::max(0.0,dot(sub(raw[p],current[p]),h));
                total+=std::max(0.0,dot(sub(output[p],current[p]),h));
            }
            score.zeroContrast=score.denominator==0;
            if(!score.zeroContrast) {
                score.bound=2*calibration.epsilon*norm1/score.denominator;
                score.incremental=incremental/score.denominator;score.raw=rawTrail/score.denominator;
                score.total=total/score.denominator;score.pass=score.incremental<=score.bound;
                report.maximum=std::max(report.maximum,score.incremental);
                report.maximumRaw=std::max(report.maximumRaw,score.raw);
                report.maximumTotal=std::max(report.maximumTotal,score.total);report.pass=report.pass&&score.pass;
            }
            report.templates.push_back(score);
        }
    }
    return report;
}
void parity_check() {
    auto equal=[](double a,double b){return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);};
    for(uint32_t seed:{0xA17u,0xB29u})for(auto color:{FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
        const auto corpus=fixture(seed,color);
        for(unsigned tick=0;tick<256;++tick) {
            const auto raw=frozen_image(corpus,tick);
            expect(clean_image(corpus,tick)==raw,"frozen per-pixel equations changed");
            if(!tick)continue;
            auto output=raw;const auto prior=frozen_image(corpus,tick-1);
            for(size_t p=0;p<raw.size();++p)output[p]={.98*raw[p].r+.02*prior[p].r,
                .98*raw[p].g+.02*prior[p].g,.98*raw[p].b+.02*prior[p].b};
            const Calibration calibration{.000001,.000002,.000002};
            const auto reference=frozen_temporal(corpus,tick,raw,output,calibration);
            const auto actual=temporal(corpus,tick,raw,output,calibration);
            auto read=[](const void* data,unsigned x,unsigned y) {
                return (*static_cast<const Image*>(data))[y*192+x];
            };
            const auto viewed=temporal(corpus,tick,PixelView{&raw,{192,96},read},
                PixelView{&output,{192,96},read},calibration);
            expect(actual.templates.size()==reference.templates.size(),"optimized scorer lost a prior region");
            expect(viewed.templates.size()==reference.templates.size(),"pixel-view scorer lost a prior region");
            expect(actual.pass==reference.pass&&equal(actual.maximum,reference.maximum)&&
                equal(actual.maximumRaw,reference.maximumRaw)&&equal(actual.maximumTotal,reference.maximumTotal),
                "optimized report changed frozen scalar bits");
            for(size_t i=0;i<reference.templates.size();++i) {
                const auto& a=actual.templates[i];const auto& b=reference.templates[i];
                const auto& v=viewed.templates[i];
                expect(a.currentTick==b.currentTick&&a.priorTick==b.priorTick&&a.region==b.region&&a.pixels==b.pixels&&
                    a.zeroContrast==b.zeroContrast&&a.pass==b.pass&&equal(a.denominator,b.denominator)&&
                    equal(a.bound,b.bound)&&equal(a.incremental,b.incremental)&&equal(a.raw,b.raw)&&equal(a.total,b.total),
                    "optimized template/mask/order/zeroContrast changed frozen bits");
                expect(v.pixels==b.pixels&&v.zeroContrast==b.zeroContrast&&v.pass==b.pass&&
                    equal(v.denominator,b.denominator)&&equal(v.bound,b.bound)&&equal(v.incremental,b.incremental)&&
                    equal(v.raw,b.raw)&&equal(v.total,b.total),"sparse working-pixel view changed frozen scalar bits");
            }
        }
        for(unsigned tick:{0u,32u,80u,128u,144u,224u})
            expect(clean_image(corpus,tick,{1920,1080})==frozen_image(corpus,tick,{1920,1080}),
                "full-canvas frozen pixel equations changed");
    }
}
void packing_check() {
    constexpr Extent working{1920,1080};
    uint64_t bytesCompared=0;
    for(uint32_t seed:{0xA17u,0xB29u})for(auto color:{
        FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
        const auto corpus=fixture(seed,color);
        for(bool linearSource:{false,true}) {
            if(!linearSource&&(color==FixtureColor::Hdr709||color==FixtureColor::Hdr2020))continue;
            std::printf("PACKED_PARITY_BEGIN seed=%u color=%u half=%u ticks=256 max_live_frames=2 persisted_images=0\n",
                seed,static_cast<unsigned>(color),static_cast<unsigned>(linearSource));
            std::fflush(stdout);
            for(unsigned tick=0;tick<256;++tick) {
                const auto clean=frozen_image(corpus,tick,working);
                const auto actual=nr::qa::bootstrap::ghost_fixture_bytes(corpus,tick,linearSource);
                const size_t stride=linearSource?8u:4u;
                expect(actual.size()==clean.size()*stride,"full packed source allocation/format");
                for(unsigned y=0;y<working.height;++y)for(unsigned x=0;x<working.width;++x) {
                    const size_t p=static_cast<size_t>(y)*working.width+x;
                    auto c=clean[p];
                    if(color==FixtureColor::Hdr2020)c={
                        1.6604910021*c.r-.5876411388*c.g-.0728498633*c.b,
                        -.1245504745*c.r+1.1328998971*c.g-.0083494226*c.b,
                        -.0181507634*c.r-.1005788980*c.g+1.1187296614*c.b};
                    const double a=x<128&&y>=960&&y<1040?0:1,factor=corpus.referenceWhiteNits/80;
                    const auto gamma=[](double v){return v<=.0031308?12.92*v:1.055*std::pow(std::max(v,0.0),1/2.4)-.055;};
                    const std::array<double,4> values{linearSource?c.r*factor*a:gamma(c.r)*a,
                        linearSource?c.g*factor*a:gamma(c.g)*a,linearSource?c.b*factor*a:gamma(c.b)*a,a};
                    for(size_t channel=0;channel<4;++channel) {
                        if(linearSource) {
                            const auto expected=DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>(values[channel]));
                            uint16_t packed=0;memcpy(&packed,actual.data()+p*8+channel*2,2);
                            if(packed!=expected)throw std::runtime_error("full frozen half-byte/signed-zero packing mismatch");
                        }else {
                            const auto expected=static_cast<uint8_t>(std::lround(std::clamp(values[channel],0.0,1.0)*255));
                            if(actual[p*4+channel]!=expected)throw std::runtime_error("full frozen RGBA8 packing mismatch");
                        }
                    }
                }
                bytesCompared+=actual.size();
            }
            std::printf("PACKED_PARITY_TRANSPORT_PASS seed=%u color=%u half=%u ticks=256 bytes=%llu\n",
                seed,static_cast<unsigned>(color),static_cast<unsigned>(linearSource),
                static_cast<unsigned long long>(working.width)*working.height*(linearSource?8ull:4ull)*256);
            std::fflush(stdout);
        }
    }
    std::printf("PACKED_PARITY bytes=%llu seeds=2 ticks=256 transports=6 width=1920 height=1080 CPU_only=true\n",
        static_cast<unsigned long long>(bytesCompared));
}
void rgb_unpack_check() {
    namespace pixels=nr::qa::bootstrap;
    const size_t count=static_cast<size_t>(pixels::width)*pixels::height;
    uint64_t compared=0,rejections=0,bmpBytes=0;
    for(bool half:{false,true})for(bool bgra:{false,true}) {
        const size_t stride=half?8u:4u;
        std::vector<uint8_t> storage(count*stride+1);
        std::span<uint8_t> bytes(storage.data()+1,count*stride); // Deliberately unaligned.
        for(size_t i=0;i<count;++i)for(size_t c=0;c<4;++c) {
            if(half) {
                auto word=static_cast<uint16_t>((i+c*16381)&65535);
                if((word&0x7c00)==0x7c00)word=0x8000;
                memcpy(bytes.data()+i*8+c*2,&word,2);
            }else bytes[i*4+c]=static_cast<uint8_t>(i+c*53);
        }
        const auto actual=pixels::rgb_image(bytes,half,bgra,"UNPACK");
        pixels::Image reference{half,{}};reference.pixels.resize(count);
        for(size_t i=0;i<count;++i)for(size_t c=0;c<4;++c) {
            float value=0;
            if(half) {
                uint16_t word=0;memcpy(&word,bytes.data()+i*8+c*2,2);
                value=DirectX::PackedVector::XMConvertHalfToFloat(word);
            }else value=static_cast<float>(bytes[i*4+(bgra&&c!=1&&c!=3?2-c:c)])/255;
            reference.pixels[i][c]=value;
            expect(std::bit_cast<uint32_t>(actual.pixels[i][c])==std::bit_cast<uint32_t>(value),
                "SIMD unpack changed scalar RGBA float bits including zero/subnormal/final pixel");
            ++compared;
        }
        const auto path=std::filesystem::temp_directory_path()/("nr-rgb-parity-"+
            std::to_string(GetCurrentProcessId())+"-"+std::to_string(half)+"-"+std::to_string(bgra)+".bmp");
        expect(!std::filesystem::exists(path),"RGB parity artifact already exists");
        pixels::save_bmp(path,actual);
        std::ifstream file(path,std::ios::binary);
        BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};
        file.read(reinterpret_cast<char*>(&header),sizeof(header));
        file.read(reinterpret_cast<char*>(&info),sizeof(info));
        expect(header.bfType==0x4d42&&header.bfOffBits==54&&header.bfSize==54+count*4&&
            info.biSize==40&&info.biWidth==1920&&info.biHeight==-1080&&info.biPlanes==1&&
            info.biBitCount==32&&info.biCompression==BI_RGB,"BMP scalar header differs");
        for(const auto& p:reference.pixels) {
            const float peak=std::max({p[0],p[1],p[2],0.0f});
            std::array<uint8_t,4> expected{},encoded{};
            for(size_t c=0;c<3;++c) {
                float v=half?p[c]/(1+peak):p[c];
                if(half)v=v<=.0031308f?v*12.92f:1.055f*std::pow(std::max(v,0.0f),1/2.4f)-.055f;
                expected[2-c]=static_cast<uint8_t>(std::lround(std::clamp(v,0.0f,1.0f)*255));
            }
            expected[3]=255;file.read(reinterpret_cast<char*>(encoded.data()),4);
            expect(file.good()&&encoded==expected,"BMP bytes changed independent scalar tone-map/control");
            bmpBytes+=4;
        }
        expect(file.peek()==std::char_traits<char>::eof(),"BMP unexpected tail");
        std::printf("RGB_UNPACK_FINITE half=%u bgra=%u pixels=%zu float_bits_exact=true unaligned=true final_pixel=true bmp=%s\n",
            static_cast<unsigned>(half),static_cast<unsigned>(bgra),count,path.string().c_str());
    }
    // Cover every nonfinite encoding with rotating first-invalid RGBA lanes;
    // add every channel at the final pixel to cover ordered late rejection.
    std::vector<uint8_t> bytes(count*8);
    const auto rejects=[&](uint16_t word,size_t pixel,size_t channel) {
        memcpy(bytes.data()+pixel*8+channel*2,&word,2);
        bool refused=false;
        try{(void)pixels::rgb_image(bytes,true,false,"UNPACK");}
        catch(const std::runtime_error& error) {
            refused=error.what()==std::string("UNPACK nonfinite actual RGB pixel=")+std::to_string(pixel)+
                " channel="+std::to_string(channel)+" packed="+std::to_string(word);
        }
        expect(refused,"nonfinite RGBA scalar rejection/order/diagnostic changed");
        const uint16_t zero=0;memcpy(bytes.data()+pixel*8+channel*2,&zero,2);++rejections;
    };
    for(unsigned word=0;word<65536;++word)if((word&0x7c00)==0x7c00)
        rejects(static_cast<uint16_t>(word),0,word%4);
    for(size_t channel=0;channel<4;++channel)for(uint16_t word:{uint16_t{0x7c00},uint16_t{0xfc00},
        uint16_t{0x7c01},uint16_t{0xfe00}})rejects(word,count-1,channel);
    std::printf("RGB_UNPACK_PARITY float_components=%llu half_encodings=65536 byte_codes=256 nonfinite_rejections=%llu bmp_pixel_bytes=%llu CPU_only=true\n",
        static_cast<unsigned long long>(compared),static_cast<unsigned long long>(rejections),
        static_cast<unsigned long long>(bmpBytes));
}
void timing_check() {
    const Extent working{1920,1080};
    const auto corpus=fixture(0xA17,FixtureColor::Sdr);
    for(unsigned tick:{1u,65u,255u}) {
        const auto began=std::chrono::steady_clock::now();
        const auto raw=clean_image(corpus,tick,working);
        const auto generated=std::chrono::steady_clock::now();
        const Calibration calibration{.000001,0,.000001};
        const auto report=temporal(corpus,tick,raw,raw,calibration,working);
        const auto scored=std::chrono::steady_clock::now();
        struct View { const Image& image;unsigned stride; };
        const View pixels{raw,working.width};
        auto read=[](const void* data,unsigned x,unsigned y) {
            const auto& view=*static_cast<const View*>(data);return view.image[y*view.stride+x];
        };
        const auto sparse=temporal(corpus,tick,PixelView{&pixels,working,read},PixelView{&pixels,working,read},calibration);
        const auto viewed=std::chrono::steady_clock::now();
        expect(report.pass,"timed clean CPU corpus rejected");
        expect(sparse.pass&&sparse.templates.size()==report.templates.size(),"timed sparse-view records changed");
        unsigned zero=0;
        for(const auto& region:report.templates)if(region.zeroContrast)++zero;
        std::printf("CPU_GHOST_TIMING seed=2583 tick=%u width=1920 height=1080 templates=%zu zero_contrast=%u generation_ns=%lld temporal_ns=%lld view_ns=%lld CPU_only=true physical_calibration=false\n",
            tick,report.templates.size(),zero,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(generated-began).count()),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(scored-generated).count()),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(viewed-scored).count()));
    }
}
void admission_check() {
    namespace pixels=nr::qa::bootstrap;
    const auto equal=[](double a,double b){return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);};
    std::array<double,256> transfer{};
    for(unsigned code=0;code<256;++code) {
        const double value=static_cast<double>(code)/255;
        transfer[code]=value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4);
    }
    uint64_t compared=0;
    for(uint32_t seed:{0xA17u,0xB29u})for(auto color:{
        FixtureColor::Sdr,FixtureColor::DarkSdr,FixtureColor::Hdr709,FixtureColor::Hdr2020}) {
        const auto corpus=fixture(seed,color);
        for(bool half:{false,true}) {
            if(!half&&(color==FixtureColor::Hdr709||color==FixtureColor::Hdr2020))continue;
            const double factor=80/corpus.referenceWhiteNits;
            const auto frozenMaximum=[&](std::span<const uint8_t> bytes,const Image& clean) {
                double reference=0;
                const auto* input=bytes.data();const auto* expected=clean.data();const auto* gamma=transfer.data();
                for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x) {
                    Rgb c{};
                    if(half) {
                        uint16_t a=0,b=0,d=0;memcpy(&a,input,2);memcpy(&b,input+2,2);memcpy(&d,input+4,2);
                        c={DirectX::PackedVector::XMConvertHalfToFloat(a),DirectX::PackedVector::XMConvertHalfToFloat(b),
                            DirectX::PackedVector::XMConvertHalfToFloat(d)};input+=8;
                    }else {c={gamma[input[0]],gamma[input[1]],gamma[input[2]]};input+=4;}
                    if(color==FixtureColor::Hdr2020)c={
                        .6274038959*c.r+.3292830384*c.g+.0433130657*c.b,
                        .0690972894*c.r+.9195403951*c.g+.0113623155*c.b,
                        .0163914389*c.r+.0880133079*c.g+.8955952532*c.b};
                    c={c.r*factor,c.g*factor,c.b*factor};
                    const auto e=x<128&&y>=960&&y<1040?Rgb{}:*expected;
                    ++expected;
                    reference=std::max(reference,std::max({std::abs(c.r-e.r),std::abs(c.g-e.g),std::abs(c.b-e.b)}));
                }
                return reference;
            };
            std::printf("RAW_PARITY_BEGIN seed=%u color=%u half=%u ticks=256 max_live_frames=2 persisted_images=0\n",
                seed,static_cast<unsigned>(color),static_cast<unsigned>(half));std::fflush(stdout);
            for(unsigned tick=0;tick<256;++tick) {
                const auto clean=frozen_image(corpus,tick,{1920,1080});
                auto bytes=pixels::ghost_fixture_bytes(corpus,tick,half);
                const auto started=std::chrono::steady_clock::now();
                pixels::RawScanCounts counts;
                const double actual=pixels::ghost_raw_maximum(corpus,tick,half,bytes,1,&counts);
                const auto optimized=std::chrono::steady_clock::now();
                const double reference=frozenMaximum(bytes,clean);
                const auto finished=std::chrono::steady_clock::now();
                expect(equal(actual,reference),"full raw calibration changed frozen maximum bits");
                expect(counts.matches==2043200&&counts.mismatches==0&&counts.special==30400,
                    "prescribed full-word reuse omitted pixels or changed special/transparent boundaries");
                compared+=bytes.size();
                if(tick==0) {
                    expect(equal(pixels::ghost_raw_maximum(corpus,tick,half,bytes,actual),actual),
                        "unchanged raw fixture failed its own frozen maximum");
                    std::printf("RAW_SCAN_TIMING seed=%u color=%u half=%u optimized_ns=%lld reference_ns=%lld matches=%llu mismatches=%llu special=%llu CPU_only=true\n",
                        seed,static_cast<unsigned>(color),static_cast<unsigned>(half),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(optimized-started).count()),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished-optimized).count()),
                        static_cast<unsigned long long>(counts.matches),static_cast<unsigned long long>(counts.mismatches),
                        static_cast<unsigned long long>(counts.special));
                    const size_t outside=(400u*1920u+400u)*(half?8u:4u);
                    const size_t stride=half?8u:4u;
                    std::array<uint8_t,8> saved{};memcpy(saved.data(),bytes.data()+outside,stride);
                    size_t other=outside;
                    for(unsigned x=512;x<1024;++x) {
                        const size_t at=(400u*1920u+x)*stride;
                        if(std::memcmp(bytes.data()+outside,bytes.data()+at,stride)!=0){other=at;break;}
                    }
                    expect(other!=outside,"wrong-coordinate palette control has no distinct input");
                    memcpy(bytes.data()+outside,bytes.data()+other,stride);
                    const double wrongCoordinate=frozenMaximum(bytes,clean);
                    pixels::RawScanCounts mismatch;
                    const uint64_t mismatchHash=pixels::hash(bytes);
                    expect(equal(pixels::ghost_raw_maximum(corpus,tick,half,bytes,wrongCoordinate,&mismatch),wrongCoordinate)&&
                        mismatch.mismatches>0,"valid palette membership replaced coordinate-specific scalar admission");
                    expect(pixels::hash(bytes)==mismatchHash,"finite mismatch normalization changed actual bytes");
                    memcpy(bytes.data()+outside,saved.data(),stride);
                    for(const auto [x,y]:std::array<std::pair<unsigned,unsigned>,8>{{
                        {191,95},{192,95},{191,96},{0,80},{127,960},{128,960},{127,1039},{127,1040}}}) {
                        const size_t at=(static_cast<size_t>(y)*1920+x)*stride;
                        memcpy(saved.data(),bytes.data()+at,stride);
                        if(half) {
                            uint16_t value=0;memcpy(&value,bytes.data()+at,2);
                            value^=1;memcpy(bytes.data()+at,&value,2);
                        }else bytes[at]^=1;
                        const double expected=frozenMaximum(bytes,clean);
                        expect(equal(pixels::ghost_raw_maximum(corpus,tick,half,bytes,expected),expected),
                            "boundary finite mismatch did not use unchanged scalar evaluator");
                        memcpy(bytes.data()+at,saved.data(),stride);
                    }
                    const auto rejects=[&](const char* why) {
                        bool rejected=false;
                        try{(void)pixels::ghost_raw_maximum(corpus,tick,half,bytes,actual);}
                        catch(const std::runtime_error&){rejected=true;}
                        expect(rejected,why);
                    };
                    if(half) {
                        uint16_t old=0;memcpy(&old,bytes.data()+outside,2);
                        const uint16_t white=0x3c00;memcpy(bytes.data()+outside,&white,2);
                        rejects("outside-mask finite RGB error escaped frozen raw bound");
                        memcpy(bytes.data()+outside,&old,2);
                        for(size_t channel=0;channel<4;++channel) {
                            uint16_t original=0;memcpy(&original,bytes.data()+outside+channel*2,2);
                            for(uint16_t nonfinite:{uint16_t{0x7c00},uint16_t{0xfe00}}) {
                                memcpy(bytes.data()+outside+channel*2,&nonfinite,2);
                                rejects("outside-mask nonfinite half channel escaped complete validity scan");
                            }
                            memcpy(bytes.data()+outside+channel*2,&original,2);
                        }
                        memcpy(&old,bytes.data()+outside+6,2);
                        const uint16_t zero=0;memcpy(bytes.data()+outside+6,&zero,2);
                        rejects("outside-mask original alpha changed");memcpy(bytes.data()+outside+6,&old,2);
                        const size_t transparent=(980u*1920u+40u)*8+6;
                        memcpy(&old,bytes.data()+transparent,2);
                        const uint16_t negativeZero=0x8000;memcpy(bytes.data()+transparent,&negativeZero,2);
                        expect(equal(pixels::ghost_raw_maximum(corpus,tick,half,bytes,actual),actual),
                            "alpha signed zero changed raw scalar maximum");
                        memcpy(bytes.data()+transparent,&old,2);
                        const size_t transparentRgb=(980u*1920u+40u)*8;
                        std::array<uint8_t,8> originalWord{};memcpy(originalWord.data(),bytes.data()+transparentRgb,8);
                        for(size_t channel=0;channel<4;++channel)memcpy(bytes.data()+transparentRgb+channel*2,&negativeZero,2);
                        const uint64_t signedHash=pixels::hash(bytes);
                        expect(equal(pixels::ghost_raw_maximum(corpus,tick,half,bytes,reference),frozenMaximum(bytes,clean)),
                            "transparent RGB/alpha signedzero changed frozen scalar result");
                        expect(pixels::hash(bytes)==signedHash,"signedzero actual bytes were normalized");
                        memcpy(bytes.data()+transparentRgb,originalWord.data(),8);
                    }else {
                        const auto channel=bytes[outside];bytes[outside]=255;
                        rejects("outside-mask finite RGB error escaped frozen raw bound");bytes[outside]=channel;
                        const auto old=bytes[outside+3];bytes[outside+3]=0;
                        rejects("outside-mask original alpha changed");bytes[outside+3]=old;
                    }
                    const uint64_t original=pixels::hash(bytes);
                    const auto hashes=pixels::hash_three({bytes,bytes,bytes});
                    expect(hashes[0]==original&&hashes[1]==original&&hashes[2]==original,
                        "full-size interleaved hashes changed byte order");
                }
            }
            std::printf("RAW_PARITY_TRANSPORT_PASS seed=%u color=%u half=%u ticks=256\n",
                seed,static_cast<unsigned>(color),static_cast<unsigned>(half));std::fflush(stdout);
        }
    }
    for(size_t size:{size_t{0},size_t{1},size_t{17},size_t{4096}}) {
        std::array<std::vector<uint8_t>,3> bytes;
        for(size_t stream=0;stream<3;++stream) {
            bytes[stream].resize(size);
            for(size_t at=0;at<size;++at)bytes[stream][at]=static_cast<uint8_t>(at*37+stream*53);
        }
        const auto hashes=pixels::hash_three({bytes[0],bytes[1],bytes[2]});
        for(size_t stream=0;stream<3;++stream)
            expect(hashes[stream]==pixels::hash(bytes[stream]),"independent hash stream changed original byte order");
    }
    std::printf("RAW_PARITY bytes=%llu seeds=2 ticks=256 transports=6 width=1920 height=1080 CPU_only=true\n",
        static_cast<unsigned long long>(compared));
}
void hash_service_check() {
    namespace pixels=nr::qa::bootstrap;
    expect(pixels::parallel_hash_available(),"actual CPU AVX512F/DQ or OS XState missing");
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    const auto release=[](uint8_t* p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<uint8_t,decltype(release)> guarded(
        static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)),release);
    expect(guarded!=nullptr,"bounded hash guard-page allocation");
    DWORD previous=0;
    expect(VirtualProtect(guarded.get()+page,page,PAGE_NOACCESS,&previous)!=FALSE,"hash no-access boundary");
    for(size_t size:{size_t{0},size_t{1},size_t{7},size_t{8},size_t{9},size_t{15},size_t{16},size_t{17},page}) {
        auto* end=guarded.get()+page;
        for(size_t at=0;at<size;++at)(end-size)[at]=static_cast<uint8_t>(at*37+size);
        std::array<std::span<const uint8_t>,8> streams{};
        for(size_t lane=0;lane<streams.size();++lane) {
            const size_t count=lane==7?0:lane%2?size/2:size;
            streams[lane]={end-count,count};
        }
        const auto actual=pixels::hash_eight(streams),fallback=pixels::hash_eight(streams,false);
        for(size_t lane=0;lane<streams.size();++lane)
            expect(actual[lane]==pixels::hash(streams[lane])&&fallback[lane]==actual[lane],
                "guard-boundary empty/tail/variable stream overread or scalar mismatch");
        if(size%2==0) {
            const auto bytes=std::span<const uint8_t>{end-size,size};bool reference=true;
            for(size_t at=0;at<size;at+=2) {
                uint16_t word=0;memcpy(&word,bytes.data()+at,2);
                reference=reference&&(word&0x7c00)!=0x7c00;
            }
            expect(pixels::finite_half(bytes)==reference,"half-finiteness guard-boundary overread/mismatch");
        }
    }
    for(size_t size:{size_t{0},size_t{1},size_t{7},size_t{8},size_t{9},size_t{15},size_t{16},size_t{17},size_t{257},size_t{4097}}) {
        std::array<std::vector<uint8_t>,8> data;
        std::array<std::span<const uint8_t>,8> streams{};
        for(size_t lane=0;lane<data.size();++lane) {
            const size_t count=lane==7?0:lane%2?size/2:size;
            data[lane].resize(count+1);
            for(size_t at=0;at<data[lane].size();++at)data[lane][at]=static_cast<uint8_t>(at*37+lane*53);
            streams[lane]={data[lane].data()+1,count};
        }
        const auto hashes=pixels::hash_eight(streams);
        const auto fallback=pixels::hash_eight(streams,false);
        for(size_t lane=0;lane<streams.size();++lane)
            expect(hashes[lane]==pixels::hash(streams[lane])&&fallback[lane]==hashes[lane],
                "SIMD or scalar fallback changed original byte order/tail/empty stream");
        if(size) {
            data[0].back()^=0x51;
            const auto changed=pixels::hash_eight(streams);
            expect(changed[0]==pixels::hash(streams[0])&&changed[0]!=hashes[0],"actual last-byte corruption was not rehashed");
            for(size_t lane=1;lane<streams.size();++lane)
                expect(changed[lane]==hashes[lane],"one mutated stream changed an independent lane");
        }
    }
    const auto scalarFinite=[](std::span<const uint8_t> bytes) {
        for(size_t at=0;at<bytes.size();at+=2) {
            uint16_t word=0;memcpy(&word,bytes.data()+at,2);
            if((word&0x7c00)==0x7c00)return false;
        }
        return true;
    };
    std::array<uint16_t,13> words{};
    for(unsigned value=0;value<65536;++value) {
        words.fill(static_cast<uint16_t>(value));
        const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(words.data()),sizeof(words)};
        expect(pixels::finite_half(bytes)==scalarFinite(bytes),"full half exponent predicate changed finite/NaN/Inf/signedzero");
    }
    for(size_t position=0;position<words.size();++position)for(uint16_t value:{uint16_t{0x7c00},uint16_t{0xfc00},uint16_t{0x7c01},uint16_t{0xffff}}) {
        words.fill(0x8000);words[position]=value;
        std::array<uint8_t,sizeof(words)+1> unaligned{};
        memcpy(unaligned.data()+1,words.data(),sizeof(words));
        expect(!pixels::finite_half({unaligned.data()+1,sizeof(words)}),"nonfinite vector/tail/alpha position escaped full scan");
    }
    for(uint32_t seed:{0xA17u,0xB29u})for(bool half:{false,true}) {
        const auto corpus=fixture(seed,FixtureColor::Sdr);
        auto source=pixels::ghost_fixture_bytes(corpus,79,half);
        auto changed=source;changed.back()^=0x55;
        const std::array<std::span<const uint8_t>,8> streams{source,changed,source,changed,source,changed,
            std::span<const uint8_t>{source}.subspan(1),std::span<const uint8_t>{changed}.first(source.size()/4)};
        const auto started=std::chrono::steady_clock::now();
        const auto hashes=pixels::hash_eight(streams);
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        for(size_t lane=0;lane<streams.size();++lane)
            expect(hashes[lane]==pixels::hash(streams[lane]),"full-size actual independent FNV result changed");
        if(half)expect(pixels::finite_half(source)==scalarFinite(source),"full-size half validity changed");
        std::printf("HASH_SERVICE_TIMING seed=%u half=%u longest_bytes=%llu eight_stream_ns=%lld max_live_frames=2 archive=0\n",
            seed,static_cast<unsigned>(half),static_cast<unsigned long long>(source.size()),static_cast<long long>(elapsed));
    }
}
void hash_shape_check() {
    namespace pixels=nr::qa::bootstrap;
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    const auto release=[](uint8_t* p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<uint8_t,decltype(release)> guarded(
        static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)),release);
    expect(guarded!=nullptr,"three-stream guard-page allocation");
    DWORD previous=0;
    expect(VirtualProtect(guarded.get()+page,page,PAGE_NOACCESS,&previous)!=FALSE,"three-stream no-access boundary");
    for(size_t at=0;at<page;++at)guarded.get()[at]=static_cast<uint8_t>(at*53+1);
    for(size_t size:{size_t{0},size_t{1},size_t{7},size_t{8},size_t{9},size_t{15},size_t{16},size_t{17},page}) {
        const std::span<const uint8_t> bytes{guarded.get()+page-size,size};
        std::array<std::span<const uint8_t>,8> streams{bytes,bytes,bytes};
        const auto actual=pixels::hash_eight(streams),fallback=pixels::hash_eight(streams,false);
        for(size_t lane=0;lane<streams.size();++lane)
            expect(actual[lane]==pixels::hash(streams[lane])&&fallback[lane]==actual[lane],
                "exact three-stream kernel overread empty/unaligned/tail or changed byte order");
        streams[3]={guarded.get()+page-1,1};
        const auto guide=pixels::hash_eight(streams);
        for(size_t lane=0;lane<streams.size();++lane)
            expect(guide[lane]==pixels::hash(streams[lane]),"one actual guide byte was omitted by shape dispatch");
        streams[1]=bytes.first(size/2);
        const auto unequal=pixels::hash_eight(streams);
        for(size_t lane=0;lane<streams.size();++lane)
            expect(unequal[lane]==pixels::hash(streams[lane]),"unequal actual stream was truncated by shape dispatch");
    }
    constexpr std::array<std::pair<FixtureColor,bool>,6> transports{{
        {FixtureColor::Sdr,false},{FixtureColor::Sdr,true},{FixtureColor::DarkSdr,false},
        {FixtureColor::DarkSdr,true},{FixtureColor::Hdr709,true},{FixtureColor::Hdr2020,true}}};
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto& [color,half]:transports) {
        const auto corpus=fixture(seed,color);
        auto source=pixels::ghost_fixture_bytes(corpus,79,half);
        auto changed=source;changed.back()^=0x55;
        const std::array<std::span<const uint8_t>,8> streams{source,changed,source};
        const auto started=std::chrono::steady_clock::now();
        const auto hashes=pixels::hash_eight(streams);
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        for(size_t lane=0;lane<streams.size();++lane)
            expect(hashes[lane]==pixels::hash(streams[lane]),"working-size three-stream transport/corrupt actual byte changed");
        expect(hashes[0]!=hashes[1],"full-size corruption was cached or omitted");
        std::printf("HASH_SHAPE_TIMING seed=%u color=%u half=%u bytes=%llu three_stream_ns=%lld max_live_frames=2 archive=0\n",
            seed,static_cast<unsigned>(color),static_cast<unsigned>(half),static_cast<unsigned long long>(source.size()),
            static_cast<long long>(elapsed));
    }
}
void hash_limb_check() {
    namespace pixels=nr::qa::bootstrap;
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    const auto release=[](uint8_t* p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<uint8_t,decltype(release)> guarded(
        static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)),release);
    expect(guarded!=nullptr,"limb hash guard-page allocation");
    DWORD previous=0;
    expect(VirtualProtect(guarded.get()+page,page,PAGE_NOACCESS,&previous)!=FALSE,"limb hash no-access boundary");
    for(unsigned pattern:{0u,255u,37u}) {
        for(size_t at=0;at<page;++at)guarded.get()[at]=static_cast<uint8_t>(pattern==37?at*37+53:pattern);
        for(size_t size:{size_t{0},size_t{1},size_t{7},size_t{8},size_t{9},size_t{15},size_t{16},size_t{17},page}) {
            auto* end=guarded.get()+page;std::array<std::span<const uint8_t>,8> streams{};
            for(size_t lane=0;lane<streams.size();++lane) {
                const size_t count=lane==7?0:lane%2?size/2:size;
                streams[lane]={end-count,count};
            }
            const auto actual=pixels::hash_eight(streams),fallback=pixels::hash_eight(streams,false);
            for(size_t lane=0;lane<streams.size();++lane)
                expect(actual[lane]==pixels::hash(streams[lane])&&fallback[lane]==actual[lane],
                    "exact limb product changed independent empty/tail/variable protected stream");
        }
    }
    constexpr std::array<std::pair<FixtureColor,bool>,6> transports{{
        {FixtureColor::Sdr,false},{FixtureColor::Sdr,true},{FixtureColor::DarkSdr,false},
        {FixtureColor::DarkSdr,true},{FixtureColor::Hdr709,true},{FixtureColor::Hdr2020,true}}};
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto& [color,half]:transports) {
        const auto corpus=fixture(seed,color);auto source=pixels::ghost_fixture_bytes(corpus,79,half);
        auto changed=source;changed.back()^=0x55;
        const std::array<std::span<const uint8_t>,8> streams{source,changed,source,changed,source,changed,
            std::span<const uint8_t>{source}.subspan(1),std::span<const uint8_t>{changed}.first(source.size()/4)};
        const auto started=std::chrono::steady_clock::now();
        const auto actual=pixels::hash_eight(streams);
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        for(size_t lane=0;lane<streams.size();++lane)
            expect(actual[lane]==pixels::hash(streams[lane]),"full transport limb FNV reordered or omitted actual byte");
        expect(actual[0]!=actual[1],"limb hash cached or omitted actual corruption");
        std::printf("HASH_LIMB_TIMING seed=%u color=%u half=%u bytes=%llu eight_stream_ns=%lld max_live_frames=2 archive=0\n",
            seed,static_cast<unsigned>(color),static_cast<unsigned>(half),static_cast<unsigned long long>(source.size()),
            static_cast<long long>(elapsed));
    }
}
void hash_profile_check() {
    namespace pixels=nr::qa::bootstrap;
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    const auto release=[](uint8_t* p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<uint8_t,decltype(release)> guarded(
        static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)),release);
    expect(guarded!=nullptr,"profile hash guard-page allocation");
    DWORD previous=0;
    expect(VirtualProtect(guarded.get()+page,page,PAGE_NOACCESS,&previous)!=FALSE,"profile no-access boundary");
    for(unsigned pattern:{0u,255u,37u}) {
        for(size_t at=0;at<page;++at)guarded.get()[at]=static_cast<uint8_t>(pattern==37?at*37+53:pattern);
        for(size_t size:{size_t{0},size_t{1},size_t{7},size_t{8},size_t{9},size_t{15},size_t{16},size_t{17},page}) {
            const std::array<std::array<size_t,8>,5> profiles{{
                {size,size,size,size,size,size,size,size},
                {size,size,size,size,size,size,size,size/4},
                {size,size,size,size,size,size,0,0},
                {size,size,size,size/2,size/2,size/2,size/2,size/4},
                {size,size,size,size/2,size/2,size/2,0,0}}};
            for(const auto& lengths:profiles) {
                std::array<std::span<const uint8_t>,8> streams{};
                for(size_t lane=0;lane<streams.size();++lane)streams[lane]={guarded.get()+page-lengths[lane],lengths[lane]};
                const auto actual=pixels::hash_eight(streams),fallback=pixels::hash_eight(streams,false);
                for(size_t lane=0;lane<streams.size();++lane)
                    expect(actual[lane]==pixels::hash(streams[lane])&&actual[lane]==fallback[lane],
                        "dense scalar profile reset/reordered/overread an independent actual stream");
            }
        }
    }
    constexpr std::array<std::pair<FixtureColor,bool>,6> transports{{
        {FixtureColor::Sdr,false},{FixtureColor::Sdr,true},{FixtureColor::DarkSdr,false},
        {FixtureColor::DarkSdr,true},{FixtureColor::Hdr709,true},{FixtureColor::Hdr2020,true}}};
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto& [color,half]:transports) {
        const auto corpus=fixture(seed,color);auto source=pixels::ghost_fixture_bytes(corpus,79,half);
        auto changed=source;changed.back()^=0x55;
        const size_t analysis=half?source.size()/2:source.size(),cost=analysis/4;
        for(bool reset:{false,true}) {
            std::array<std::span<const uint8_t>,8> streams{source,changed,source,
                std::span<const uint8_t>{changed}.first(analysis),std::span<const uint8_t>{source}.first(analysis),
                std::span<const uint8_t>{changed}.first(analysis),
                reset?std::span<const uint8_t>{}:std::span<const uint8_t>{source}.first(analysis),
                reset?std::span<const uint8_t>{}:std::span<const uint8_t>{changed}.first(cost)};
            const auto started=std::chrono::steady_clock::now();const auto actual=pixels::hash_eight(streams);
            const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
            for(size_t lane=0;lane<streams.size();++lane)
                expect(actual[lane]==pixels::hash(streams[lane]),"actual full transport/guide length profile hash changed");
            expect(actual[0]!=actual[1],"profile kernel omitted or cached actual corrupted last byte");
            std::printf("HASH_PROFILE_TIMING seed=%u color=%u half=%u reset=%u bytes=%llu eight_stream_ns=%lld max_live_frames=2 archive=0\n",
                seed,static_cast<unsigned>(color),static_cast<unsigned>(half),static_cast<unsigned>(reset),
                static_cast<unsigned long long>(source.size()),static_cast<long long>(elapsed));
        }
    }
}
using nr::qa::test::reference_sha256;
void codec_capacity_check() {
    namespace pixels=nr::qa::bootstrap;
    // The production plane recipes and vector growth, not decoder/image proof.
    // No pixels from this allocation-only check qualify physical Task26.
    for(bool hdr:{false,true}) {
        std::vector<nr::qa::DecodedFrame> before,decoded;before.reserve(40);decoded.reserve(32);
        const auto frame=[&](bool planar) {
            nr::qa::DecodedFrame value{};value.width=1920;value.height=1080;
            value.format=hdr?(planar?VIDEO_FORMAT_I010:VIDEO_FORMAT_P010):
                (planar?VIDEO_FORMAT_I420:VIDEO_FORMAT_NV12);
            value.fullRange=false;value.transfer=hdr?VIDEO_TRC_PQ:VIDEO_TRC_DEFAULT;
            const uint32_t b=hdr?2u:1u;
            value.rowBytes=planar?std::array<uint32_t,3>{1920*b,960*b,960*b}:
                std::array<uint32_t,3>{1920*b,1920*b,0};
            value.rows=planar?std::array<uint32_t,3>{1080,540,540}:std::array<uint32_t,3>{1080,540,0};
            for(size_t p=0;p<3;++p)value.planes[p].resize(static_cast<size_t>(value.rowBytes[p])*value.rows[p]);
            return value;
        };
        for(unsigned i=0;i<40;++i)before.push_back(frame(false));
        for(unsigned i=0;i<32;++i)decoded.push_back(frame(true));
        uint64_t retained=before.capacity()*sizeof(before.front())+decoded.capacity()*sizeof(decoded.front());
        for(const auto* group:{&before,&decoded})for(const auto& value:*group)
            for(const auto& plane:value.planes)retained+=plane.capacity();
        std::array<pixels::Image,3> pictures{};
        for(auto& picture:pictures){picture.pixels.resize(1920ull*1080);retained+=picture.pixels.capacity()*sizeof(picture.pixels.front());}
        uint64_t temporary=0;
        {
            const auto rgb=pixels::decoded_image(decoded.front(),hdr);
            const nr::ghost::Image canonicalPixels(rgb.pixels.size());
            const std::vector<uint8_t> bitmap(1920ull*1080*4);
            temporary=rgb.pixels.capacity()*sizeof(rgb.pixels.front())+
                canonicalPixels.capacity()*sizeof(canonicalPixels.front())+bitmap.capacity();
            expect(rgb.pixels.capacity()==1920ull*1080&&canonicalPixels.capacity()==rgb.pixels.capacity(),
                "actual codec RGB/canonical vector capacity recipe");
        } // All conversion/BMP temporaries end before one-frame upload/root work.
        const auto logical=hdr?6220800ull:3110400ull;
        expect(temporary==91238400&&retained>=logical*72+99532800,
            "matched codec phase payload envelope drift");
        std::printf("CODEC_CPU_CAPACITY hdr=%u preencode_count=40 preencode_vector_capacity=%zu decoded_count=32 decoded_vector_capacity=%zu retained_capacity=%llu old_conversion_BMP_peak=%llu after_conversion_BMP_overlap=0 upload_frame_logical=%llu decoder_vendor_overhead=unchanged_unknown physical_image_acceptance=false GPU_dispatch=0\n",
            static_cast<unsigned>(hdr),before.capacity(),decoded.capacity(),static_cast<unsigned long long>(retained),
            static_cast<unsigned long long>(temporary),static_cast<unsigned long long>(logical));
    }
}
void chunk_controls_check() {
    namespace digest=nr::qa::ghost_digest;
    const std::array<uint8_t,3> actual{'a','b','c'};
    std::vector<uint8_t> leaf{0};
    const auto append=[](std::vector<uint8_t>& out,uint64_t value,unsigned n) {
        for(unsigned i=0;i<n;++i)out.push_back(static_cast<uint8_t>(value>>(i*8)));
    };
    append(leaf,3,8);append(leaf,0,8);append(leaf,4096,4);append(leaf,3,4);
    leaf.insert(leaf.end(),actual.begin(),actual.end());
    const auto leafHash=reference_sha256(leaf);digest::Sha256 root;
    const auto expected=digest::chunk_root(root,actual.size(),leafHash);
    expect(digest::hex(expected)=="9dd576b9473e6bbe8291778e45a06ee0687c2a5996f1f68e64c7f0a66fef2307",
        "independent fixed-width abc chunk-root KAT");
    for(size_t at:{size_t{0},size_t{1},size_t{9},size_t{17},size_t{21}}) {
        leaf[at]^=1;const auto changed=reference_sha256(leaf);leaf[at]^=1;
        expect(changed!=leafHash&&digest::chunk_root(root,actual.size(),changed)!=expected,
            "altered leaf tag/length/index/chunk-size/tail extent accepted");
    }
    std::vector<uint8_t> rootMessage{1};append(rootMessage,3,8);append(rootMessage,4096,4);append(rootMessage,1,8);
    rootMessage.insert(rootMessage.end(),leafHash.begin(),leafHash.end());
    for(size_t at:{size_t{0},size_t{1},size_t{9},size_t{13}}) {
        rootMessage[at]^=1;
        expect(reference_sha256(rootMessage)!=expected,"altered root tag/length/chunk-size/leaf count accepted");
        rootMessage[at]^=1;
    }
    bool rejected=false;
    try{(void)digest::chunk_root(root,actual.size(),std::span<const uint8_t>{leafHash}.first(31));}
    catch(const std::runtime_error&){rejected=true;}
    expect(rejected,"truncated canonical leaf control accepted");
    const std::array<std::span<const uint8_t>,3> planes{
        std::span<const uint8_t>{actual}.first(1),std::span<const uint8_t>{actual}.subspan(1,1),
        std::span<const uint8_t>{actual}.last(1)};
    std::vector<uint8_t> ordered;
    for(const auto plane:planes)ordered.insert(ordered.end(),plane.begin(),plane.end());
    expect(ordered==std::vector<uint8_t>(actual.begin(),actual.end()),"actual owned codec logical plane concatenation");
    std::printf("CHUNK_CHANGED_CONTROLS algorithm=%s schema=%u full_known_root=true altered_fixed_fields_rejected=true GPU_dispatch=0 old96_not_replayed=true\n",
        digest::chunkAlgorithm.data(),digest::chunkSchema);
}
void chunk_check() {
    namespace digest=nr::qa::ghost_digest;
    digest::Sha256 root;
    const std::string_view abc="abc";
    const auto abcBytes=std::span{reinterpret_cast<const uint8_t*>(abc.data()),abc.size()};
    expect(digest::hex(reference_sha256(abcBytes))==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "independent chunk SHA anchor");
    auto scalar=[&](std::span<const uint8_t> bytes) {
        std::vector<uint8_t> leaves;
        for(size_t at=0,index=0;at<bytes.size();at+=4096,++index) {
            const auto count=std::min<size_t>(4096,bytes.size()-at);
            std::vector<uint8_t> message{0};
            const auto append=[&](uint64_t value,unsigned n) {
                for(unsigned i=0;i<n;++i)message.push_back(static_cast<uint8_t>(value>>(i*8)));
            };
            append(bytes.size(),8);append(index,8);append(4096,4);append(count,4);
            const auto prefix=digest::leaf_prefix(bytes.size(),index,static_cast<uint32_t>(count));
            expect(message.size()==25&&std::equal(message.begin(),message.end(),prefix.begin()),
                "leaf prefix fixed widths/order");
            message.insert(message.end(),bytes.begin()+at,bytes.begin()+at+count);
            const auto value=reference_sha256(message);leaves.insert(leaves.end(),value.begin(),value.end());
        }
        std::vector<uint8_t> message{1};
        const auto append=[&](uint64_t value,unsigned n) {
            for(unsigned i=0;i<n;++i)message.push_back(static_cast<uint8_t>(value>>(i*8)));
        };
        append(bytes.size(),8);append(4096,4);append((bytes.size()+4095)/4096,8);
        const auto prefix=digest::root_prefix(bytes.size());
        expect(message.size()==21&&std::equal(message.begin(),message.end(),prefix.begin()),
            "root prefix fixed widths/order");
        message.insert(message.end(),leaves.begin(),leaves.end());
        const auto reference=reference_sha256(message);
        expect(digest::chunk_root(root,bytes.size(),leaves)==reference,"ordered full256 root differs from scalar");
        for(size_t at:{size_t{0},leaves.size()/2,leaves.size()-1}) {
            leaves[at]^=1;
            expect(digest::chunk_root(root,bytes.size(),leaves)!=reference,"corrupt first/interior/final leaf accepted");
            leaves[at]^=1;
        }
        if(leaves.size()>32) {
            std::swap_ranges(leaves.begin(),leaves.begin()+32,leaves.end()-32);
            expect(digest::chunk_root(root,bytes.size(),leaves)!=reference,"reordered leaves accepted");
            std::swap_ranges(leaves.begin(),leaves.begin()+32,leaves.end()-32);
            std::copy_n(leaves.begin(),32,leaves.end()-32);
            expect(digest::chunk_root(root,bytes.size(),leaves)!=reference,"duplicate leaves accepted");
        }
        bool rejected=false;
        try{(void)digest::chunk_root(root,bytes.size(),std::span<const uint8_t>{leaves}.first(leaves.size()-1));}
        catch(const std::runtime_error&){rejected=true;}
        expect(rejected,"partial canonical leaf accepted");
        return reference;
    };
    for(size_t length:{size_t{1},size_t{30},size_t{31},size_t{32},size_t{39},size_t{55},
        size_t{63},size_t{64},size_t{65},size_t{4095},size_t{4096},size_t{4097},size_t{8191},size_t{8192},size_t{8193}}) {
        std::vector<uint8_t> bytes(length);
        for(size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<uint8_t>(i*37+(i>>8));
        const auto original=scalar(bytes);
        expect(original!=reference_sha256(bytes),"chunk root mislabeled as flat SHA");
        for(size_t at:{size_t{0},length/2,length-1}) {
            bytes[at]^=1;expect(scalar(bytes)!=original,"actual chunk payload corruption omitted");bytes[at]^=1;
        }
        const auto split=length/2;
        std::vector<uint8_t> swapped(bytes.begin()+split,bytes.end());
        swapped.insert(swapped.end(),bytes.begin(),bytes.begin()+split);
        if(swapped!=bytes)expect(scalar(swapped)!=original,"codec plane order omitted");
    }
    for(uint64_t length:{uint64_t{0},UINT64_MAX}) {
        bool rejected=false;try{(void)digest::root_prefix(length);}catch(const std::runtime_error&){rejected=true;}
        expect(rejected,"absent/overflowing payload assigned root");
    }
    const auto wide=digest::leaf_prefix(0x100000001ull,0x100000ull,1);
    expect(wide[1]==1&&wide[5]==1&&wide[9]==0&&wide[11]==16&&wide[21]==1,
        "LE64 length/index truncated by CPU prefix");
    std::printf("CHUNK_SCALAR_PROOF algorithm=%s schema=%u prefix_bytes=25,21 chunk_bytes=4096 gpu_dispatch=0\n",
        digest::chunkAlgorithm.data(),digest::chunkSchema);
}
void sha256_check() {
    namespace digest=nr::qa::ghost_digest;
    digest::Sha256 context;
    const std::array<std::pair<std::string_view,std::string_view>,3> known{{
        {"","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}}};
    for(const auto& [text,answer]:known) {
        const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(text.data()),text.size()};
        expect(digest::hex(reference_sha256(bytes))==answer,"independent SHA known answer");
        expect(digest::hex(context.hash(bytes))==answer,"CNG full SHA known answer/reset");
    }
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    const auto release=[](uint8_t* p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<uint8_t,decltype(release)> guarded(
        static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)),release);
    expect(guarded!=nullptr,"SHA protected payload allocation");DWORD previous=0;
    expect(VirtualProtect(guarded.get()+page,page,PAGE_NOACCESS,&previous)!=FALSE,"SHA no-access tail");
    for(size_t at=0;at<page;++at)guarded.get()[at]=static_cast<uint8_t>(at*37+53);
    for(size_t size:{size_t{0},size_t{1},size_t{55},size_t{56},size_t{63},size_t{64},size_t{65},size_t{127},size_t{128},size_t{129},page}) {
        const std::span<const uint8_t> bytes{guarded.get()+page-size,size};
        expect(context.hash(bytes)==reference_sha256(bytes),"SHA actual unaligned/block/final protected tail");
    }
    std::array<std::vector<uint8_t>,17> roles;
    std::array<digest::Digest,17> original{};
    for(size_t role=0;role<roles.size();++role) {
        roles[role].resize(193+role);
        for(size_t at=0;at<roles[role].size();++at)roles[role][at]=static_cast<uint8_t>(at*37+role*53);
        original[role]=context.hash(roles[role]);
        expect(original[role]==reference_sha256(roles[role]),"all17 present role full independent SHA");
    }
    for(size_t role=0;role<roles.size();++role)for(size_t location:{size_t{0},size_t{97},roles[role].size()-1}) {
        roles[role][location]^=0x41;
        for(size_t peer=0;peer<roles.size();++peer) {
            const auto actual=context.hash(roles[peer]);
            expect(actual==reference_sha256(roles[peer]),"per-role first/interior/last actual corruption SHA");
            expect(peer==role?actual!=original[peer]:actual==original[peer],"17role digest independence/cached payload");
        }
        roles[role][location]^=0x41;
    }
    std::array<uint16_t,17> halves{};halves.fill(0x3c00);
    auto halfBytes=std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(halves.data()),sizeof(halves)};
    const auto opaque=context.image(halfBytes,halfBytes.size(),true);
    halves.back()=0x8000;
    expect(context.image(halfBytes,halfBytes.size(),true)!=opaque&&context.hash(halfBytes)==reference_sha256(halfBytes),
        "actual half alpha/signedzero final bits were omitted");
    for(size_t position=0;position<halves.size();++position)for(uint16_t bad:{uint16_t{0x7c00},uint16_t{0xfc00},uint16_t{0x7c01}}) {
        halves.fill(0x3c00);halves[position]=bad;bool rejected=false;
        try{(void)context.image(halfBytes,halfBytes.size(),true);}catch(const std::runtime_error&){rejected=true;}
        expect(rejected,"nonfinite full half/alpha/vector/tail accepted");
    }
    bool truncated=false;try{(void)context.image(halfBytes,halfBytes.size()+2,false);}catch(const std::runtime_error&){truncated=true;}
    expect(truncated,"truncated logical stage row accepted");
    constexpr std::array<std::pair<FixtureColor,bool>,6> transports{{
        {FixtureColor::Sdr,false},{FixtureColor::Sdr,true},{FixtureColor::DarkSdr,false},
        {FixtureColor::DarkSdr,true},{FixtureColor::Hdr709,true},{FixtureColor::Hdr2020,true}}};
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto& [color,half]:transports) {
        const auto fixtureData=fixture(seed,color);
        auto source=nr::qa::bootstrap::ghost_fixture_bytes(fixtureData,79,half);
        auto changed=source;changed.back()^=1;
        const auto baseline=context.image(source,source.size(),half);
        expect(baseline==reference_sha256(source),"working-size six-transport independent SHA");
        for(size_t role=0;role<digest::roles.size();++role) {
            const size_t wanted=role==0?0:role==16?changed.size()-1:changed.size()/2+role;
            const size_t at=half?(wanted&~size_t{1}):wanted;
            changed[at]^=static_cast<uint8_t>(role+1);
            const auto actual=context.image(changed,changed.size(),half);
            expect(actual==reference_sha256(changed)&&actual!=baseline,"actual full-size17role corruption/same-address reset");
            changed[at]^=static_cast<uint8_t>(role+1);
        }
    }
    expect(digest::valid_hex(digest::hex(original[0]))&&!digest::valid_hex("12")&&!digest::valid_hex(std::string(64,'A')),
        "machine full32byte/64lowerhex schema");
}
void sha_multi_check() {
    namespace digest=nr::qa::ghost_digest;
    digest::Sha256Batch batch;digest::Sha256 serial;
    constexpr size_t count=digest::Sha256Batch::capacity;
    std::array<digest::Payload,count> inputs{};
    std::array<std::optional<digest::Digest>,count> results{};
    std::array<std::vector<uint8_t>,count> bytes;
    for(size_t role=0;role<count;++role) {
        bytes[role].resize(193+role);
        for(size_t at=0;at<bytes[role].size();++at)bytes[role][at]=static_cast<uint8_t>(at*37+role*53);
        inputs[role]={bytes[role],bytes[role].size(),false,true};
    }
    const auto verify=[&] {
        batch.images(inputs,results);
        for(size_t role=0;role<count;++role)
            expect(inputs[role].present?results[role]&&*results[role]==reference_sha256(inputs[role].bytes):!results[role],
                "SIMD full independent SHA/role presence/logical reset");
    };
    verify();
    for(size_t role=0;role<count;++role)for(size_t at:{size_t{0},size_t{97},bytes[role].size()-1}) {
        const auto before=results[role];bytes[role][at]^=0x41;verify();
        expect(results[role]!=before,"SIMD omitted actual per-role first/interior/last corruption");
        bytes[role][at]^=0x41;verify();
    }
    for(size_t absent:{size_t{3},size_t{8},size_t{9},size_t{14},size_t{18}}) {
        const auto original=inputs[absent];inputs[absent]={};verify();inputs[absent]=original;verify();
    }
    constexpr std::string_view abc="abc",longAnswer="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    inputs[0]={{reinterpret_cast<const uint8_t*>(abc.data()),abc.size()},abc.size(),false,true};
    inputs[1]={{reinterpret_cast<const uint8_t*>(longAnswer.data()),longAnswer.size()},longAnswer.size(),false,true};
    verify();
    expect(digest::hex(*results[0])=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SIMD standard abc SHA");
    expect(digest::hex(*results[1])=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","SIMD standard two-block SHA");
    SYSTEM_INFO system{};GetSystemInfo(&system);const size_t page=system.dwPageSize;
    const auto release=[](uint8_t* p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<uint8_t,decltype(release)> guarded(
        static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)),release);
    expect(guarded!=nullptr,"SIMD protected bounds allocation");DWORD previous=0;
    expect(VirtualProtect(guarded.get()+page,page,PAGE_NOACCESS,&previous)!=FALSE,"SIMD actual no-access tail");
    for(size_t at=0;at<page;++at)guarded.get()[at]=static_cast<uint8_t>(at*37+53);
    constexpr std::array<size_t,10> boundaries{1,55,56,63,64,65,127,128,129,193};
    for(size_t pass=0;pass<boundaries.size();++pass) {
        for(size_t role=0;role<count;++role) {
            const auto size=role==count-1?page:boundaries[(pass+role)%boundaries.size()];
            inputs[role]={{guarded.get()+page-size,size},size,false,true};
        }
        verify();
    }
    std::array<uint16_t,17> halves{};halves.fill(0x3c00);
    const std::span<const uint8_t> halfBytes{reinterpret_cast<const uint8_t*>(halves.data()),sizeof(halves)};
    for(size_t role=0;role<count;++role) {
        const auto saved=inputs[role];inputs[role]={halfBytes,halfBytes.size(),true,true};verify();
        halves.back()=0x8000;verify();halves.back()=0x3c00;
        for(size_t at=0;at<halves.size();++at)for(uint16_t bad:{uint16_t{0x7c00},uint16_t{0xfc00},uint16_t{0x7c01}}) {
            halves[at]=bad;bool rejected=false;
            try{batch.images(inputs,results);}catch(const std::runtime_error&){rejected=true;}
            expect(rejected,"SIMD accepted nonfinite half/alpha/tail");
            halves[at]=0x3c00;
        }
        verify();inputs[role]=saved;verify();
    }
    const auto saved=inputs[0];
    for(unsigned invalid=0;invalid<3;++invalid) {
        inputs[0]=saved;
        if(invalid==0)++inputs[0].logicalBytes;
        if(invalid==1)inputs[0].present=false;
        if(invalid==2)inputs[0]={{},1,false,true};
        bool rejected=false;try{batch.images(inputs,results);}catch(const std::runtime_error&){rejected=true;}
        expect(rejected,"SIMD logical truncation/absent bytes/empty surrogate accepted");
    }
    inputs[0]=saved;verify();inputs.fill({});verify();
    constexpr std::array<std::pair<FixtureColor,bool>,6> transports{{
        {FixtureColor::Sdr,false},{FixtureColor::Sdr,true},{FixtureColor::DarkSdr,false},
        {FixtureColor::DarkSdr,true},{FixtureColor::Hdr709,true},{FixtureColor::Hdr2020,true}}};
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto& [color,half]:transports) {
        auto source=nr::qa::bootstrap::ghost_fixture_bytes(fixture(seed,color),79,half),changed=source;
        changed.back()^=1;
        expect(serial.hash(source)==reference_sha256(source),"SIMD full transport independent scalar anchor");
        for(size_t role=0;role<count;++role) {
            const auto& actual=role&1?changed:source;
            const auto length=role%3==0?actual.size():actual.size()/2-(role&~size_t{1});
            inputs[role]={{actual.data(),length},length,half,true};
        }
        for(unsigned phase=0;phase<3;++phase) {
            const auto vectors=inputs[8],cost=inputs[9];
            if(phase==1){inputs[8]={};inputs[9]={};}
            batch.images(inputs,results);
            for(size_t role=0;role<count;++role)
                expect(inputs[role].present?results[role]&&*results[role]==serial.image(inputs[role].bytes,inputs[role].logicalBytes,half):
                    !results[role],"SIMD full six-transport reset/ordinary/late byte equivalence");
            inputs[8]=vectors;inputs[9]=cost;
        }
    }
}
void sha_profile_check(bool batched=false) {
    namespace digest=nr::qa::ghost_digest;
    using Microsoft::WRL::ComPtr;
    using Clock=std::chrono::steady_clock;
    const auto ns=[](Clock::time_point begin){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();};
    const auto requireHr=[](HRESULT result,const char* operation){if(FAILED(result))throw std::runtime_error(operation);};
    ComPtr<ID3D12Device> device;
    requireHr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"SHA experiment actual private device");
    const auto luid=device->GetAdapterLuid();
    std::printf("SHA_PROFILE_DEVICE luid_high=%ld luid_low=%lu single_cpu_worker=true credits=3 physical_acceptance=false\n",
        static_cast<long>(luid.HighPart),static_cast<unsigned long>(luid.LowPart));
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};
    requireHr(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"SHA experiment actual copy queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    requireHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"SHA experiment copy allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    requireHr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"SHA experiment copy list");
    requireHr(commands->Close(),"SHA experiment prepared list");
    ComPtr<ID3D12Fence> fence;requireHr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"SHA experiment real fence");
    const auto closeEvent=[](void* value){if(value)CloseHandle(static_cast<HANDLE>(value));};
    std::unique_ptr<void,decltype(closeEvent)> completed(CreateEventW(nullptr,TRUE,FALSE,nullptr),closeEvent);
    expect(completed!=nullptr,"SHA experiment exact event");
    const auto allocate=[&](D3D12_HEAP_TYPE type,uint64_t bytes) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        requireHr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&buffer)),"SHA experiment bounded role buffer");
        return buffer;
    };
    constexpr size_t pixels=1920ull*1080;
    constexpr std::array<std::pair<FixtureColor,bool>,6> transports{{
        {FixtureColor::Sdr,false},{FixtureColor::Sdr,true},{FixtureColor::DarkSdr,false},
        {FixtureColor::DarkSdr,true},{FixtureColor::Hdr709,true},{FixtureColor::Hdr2020,true}}};
    uint64_t copyValue=0,envelope=0;bool firstProvider=true;
    nr::qa::GhostCredits credits;
    for(uint32_t seed:{0xA17u,0xB29u})for(const auto& [color,half]:transports) {
        const auto corpus=fixture(seed,color);
        const auto analysisCorpus=fixture(seed,color==FixtureColor::DarkSdr?FixtureColor::DarkSdr:FixtureColor::Sdr);
        const auto source8=nr::qa::bootstrap::ghost_fixture_bytes(analysisCorpus,79,false);
        const auto source16=nr::qa::bootstrap::ghost_fixture_bytes(corpus,79,true);
        for(unsigned phase=0;phase<4;++phase) {
            const auto setupBegin=Clock::now();
            std::array<size_t,17> sizes{},offsets{};
            uint64_t bytes=0;
            for(size_t role=0;role<sizes.size();++role) {
                const bool present=phase==0?role<11:phase==1?role!=8&&role!=9:true;
                const size_t stride=role<=4?8u:role==9?1u:role==10||role>=11?(half?8u:4u):4u;
                sizes[role]=present?pixels*stride:0;offsets[role]=static_cast<size_t>(bytes);bytes+=sizes[role];
            }
            const size_t codecBytes=phase==3?pixels*(half?3u:3u)/2*(half?2u:1u):0;
            const size_t codecOffset=static_cast<size_t>(bytes);bytes+=codecBytes*2;
            expect(bytes<=529268736,"existing bounded readback byte ceiling");
            std::vector<uint8_t> normal(static_cast<size_t>(bytes));
            for(size_t role=0;role<sizes.size();++role)if(sizes[role]) {
                auto* target=normal.data()+offsets[role];const auto& source=sizes[role]==source16.size()?source16:source8;
                memcpy(target,source.data(),sizes[role]);
                if(role==7) {
                    for(size_t at=0;at<sizes[role];at+=2) {
                        const auto value=DirectX::PackedVector::XMConvertFloatToHalf(static_cast<float>((at/2)%31)*.03125f);
                        memcpy(target+at,&value,2);
                    }
                }
                for(size_t at=0;at<sizes[role];at+=2)
                    target[at]^=static_cast<uint8_t>(role+1); // Distinct complete payloads; half exponent bits stay finite.
            }
            if(codecBytes) {
                for(size_t at=0;at<codecBytes*2;++at)normal[codecOffset+at]=static_cast<uint8_t>(at*37+seed);
                normal[codecOffset+codecBytes]^=0x53;
            }
            const auto roleSetup=ns(setupBegin);
            auto upload=allocate(D3D12_HEAP_TYPE_UPLOAD,bytes),mapped=allocate(D3D12_HEAP_TYPE_READBACK,bytes);
            D3D12_RANGE empty{0,0};void* uploaded=nullptr;
            requireHr(upload->Map(0,&empty,&uploaded),"SHA experiment upload map");
            memcpy(uploaded,normal.data(),normal.size());D3D12_RANGE written{0,normal.size()};upload->Unmap(0,&written);
            std::array<digest::Digest,17> normalResult{};
            for(bool readback:{false,true}) {
                const auto reserved=Clock::now();
                const FrameKey frame{991,1,++envelope};const auto token=credits.reserve(frame,1);
                expect(token.has_value(),"same three-credit experiment owner");
                int64_t copyNs=0;
                if(readback) {
                    requireHr(allocator->Reset(),"SHA experiment allocator reuse");
                    requireHr(commands->Reset(allocator.Get(),nullptr),"SHA experiment list reuse");
                    commands->CopyBufferRegion(mapped.Get(),0,upload.Get(),0,bytes);
                    requireHr(commands->Close(),"SHA experiment copy close");
                    ++copyValue;ResetEvent(static_cast<HANDLE>(completed.get()));
                    requireHr(fence->SetEventOnCompletion(copyValue,static_cast<HANDLE>(completed.get())),
                        "SHA experiment event before copy");
                    const auto copyBegin=Clock::now();
                    ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);
                    const auto signaled=queue->Signal(fence.Get(),copyValue);
                    auto waited=WaitForSingleObject(static_cast<HANDLE>(completed.get()),30000);
                    if(FAILED(signaled)||waited!=WAIT_OBJECT_0) {
                        std::printf("SHA_PROFILE_RETAINED fence=%llu signal_hr=%ld wait=%lu credits=%u closure=false\n",
                            static_cast<unsigned long long>(copyValue),static_cast<long>(signaled),
                            static_cast<unsigned long>(waited),credits.occupied());
                        std::fflush(stdout);
                        // No unwind or release while the submitted copy is unresolved.
                        waited=WaitForSingleObject(static_cast<HANDLE>(completed.get()),INFINITE);
                    }
                    if(waited!=WAIT_OBJECT_0||fence->GetCompletedValue()==UINT64_MAX||
                        fence->GetCompletedValue()<copyValue) {
                        std::printf("SHA_PROFILE_RETAINED completion_unproven=true credits=%u closure=false\n",credits.occupied());
                        std::fflush(stdout);
                        const auto retained=CreateEventW(nullptr,TRUE,FALSE,nullptr);
                        if(retained)WaitForSingleObject(retained,INFINITE);
                        // The existing completion event remains owned even if retention-event allocation fails.
                        for(;;)WaitForSingleObject(static_cast<HANDLE>(completed.get()),INFINITE);
                    }
                    copyNs=ns(copyBegin);upload.Reset();
                }
                const auto providerBegin=Clock::now();
                auto hasher=batched?nullptr:std::make_unique<digest::Sha256>();
                auto multi=batched?std::make_unique<digest::Sha256Batch>():nullptr;
                const auto providerNs=ns(providerBegin);
                const auto mapBegin=Clock::now();void* address=nullptr;D3D12_RANGE range{0,normal.size()};
                if(readback)requireHr(mapped->Map(0,&range,&address),"SHA experiment actual READBACK map");
                const auto mapNs=ns(mapBegin);
                const auto* base=readback?static_cast<const uint8_t*>(address):normal.data();
                digest::Timing total{};std::array<digest::Digest,17> results{};
                std::array<std::string,17> hexadecimal{};
                const auto hashBegin=Clock::now();
                digest::Timing burst{};
                if(batched) {
                    std::array<digest::Payload,digest::Sha256Batch::capacity> payloads{};
                    std::array<std::optional<digest::Digest>,digest::Sha256Batch::capacity> outputs{};
                    for(size_t role=0;role<sizes.size();++role)if(sizes[role]) {
                        const bool halfFloat=role<=4||role==7||((role==10||role>=11)&&half);
                        payloads[role]={{base+offsets[role],sizes[role]},sizes[role],halfFloat,true};
                    }
                    if(codecBytes)for(size_t stage=0;stage<2;++stage)
                        payloads[17+stage]={{base+codecOffset+stage*codecBytes,codecBytes},codecBytes,false,true};
                    multi->images(payloads,outputs,&total);
                    for(size_t role=0;role<sizes.size();++role)if(sizes[role])results[role]=*outputs[role];
                    if(codecBytes)for(size_t stage=0;stage<2;++stage)
                        expect(outputs[17+stage]&&digest::valid_hex(digest::hex(*outputs[17+stage])),"SIMD actual codec payload output");
                    burst.bytes=codecBytes*2;total.bytes-=burst.bytes;
                }else for(size_t role=0;role<sizes.size();++role)if(sizes[role]) {
                    digest::Timing timing;
                    const bool halfFloat=role<=4||role==7||((role==10||role>=11)&&half);
                    results[role]=hasher->image({base+offsets[role],sizes[role]},sizes[role],halfFloat,&timing);
                    total.hashNs+=timing.hashNs;total.finishNs+=timing.finishNs;total.setupNs+=timing.setupNs;
                    total.bytes+=timing.bytes;
                }
                if(codecBytes&&!batched)for(size_t stage=0;stage<2;++stage) {
                    digest::Timing timing;const auto value=hasher->hash({base+codecOffset+stage*codecBytes,codecBytes},&timing);
                    burst.hashNs+=timing.hashNs;burst.finishNs+=timing.finishNs;burst.bytes+=timing.bytes;
                    expect(digest::valid_hex(digest::hex(value)),"retained actual codec burst result");
                }
                const auto hashTotal=ns(hashBegin);const auto serializeBegin=Clock::now();
                auto* record=obs_data_create();obs_data_set_string(record,"identity_digest_algorithm","sha256");
                auto* stages=obs_data_array_create();
                for(size_t role=0;role<sizes.size();++role) {
                    auto* value=obs_data_create();obs_data_set_string(value,"role",digest::roles[role].data());
                    obs_data_set_bool(value,"present",sizes[role]!=0);obs_data_set_int(value,"bytes",static_cast<int64_t>(sizes[role]));
                    if(sizes[role]) {
                        hexadecimal[role]=digest::hex(results[role]);obs_data_set_string(value,"digest",hexadecimal[role].c_str());
                    }
                    auto* key=obs_data_create();
                    obs_data_set_int(key,"instance",991);obs_data_set_int(key,"content_epoch",1);
                    obs_data_set_int(key,"ingress",static_cast<int64_t>(envelope));obs_data_set_int(key,"history",1);
                    obs_data_set_int(key,"generation",1);obs_data_set_int(key,"submission",static_cast<int64_t>(envelope));
                    obs_data_set_obj(value,"frame_key",key);obs_data_set_obj(value,"job_key",key);obs_data_release(key);
                    auto* domain=obs_data_create();
                    const size_t stride=role<=4?8u:role==9?1u:role==10||role>=11?(half?8u:4u):4u;
                    obs_data_set_int(domain,"width",1920);obs_data_set_int(domain,"height",1080);
                    obs_data_set_int(domain,"row_bytes",static_cast<int64_t>(1920*stride));obs_data_set_int(domain,"rows",1080);
                    obs_data_set_int(domain,"storage_dxgi",role<=4?10:role==7?34:role==8?38:role==9?62:
                        role==5||role==6?87:(half?10:28));
                    obs_data_set_string(domain,"transfer",role==1||role==3?"model_proxy":role==7?"working_pixel_motion":
                        role==8?"signed_1_32_native_flow":role==9?"native_cost":half?"scrgb_linear":"srgb");
                    obs_data_set_string(domain,"primaries","rec709");obs_data_set_string(domain,"alpha","declared_profile");
                    obs_data_set_double(domain,"reference_white_nits",80);obs_data_set_double(domain,"oracle_reference_white_nits",corpus.referenceWhiteNits);
                    obs_data_set_double(domain,"exposure",1);obs_data_set_obj(value,"domain",domain);obs_data_release(domain);
                    obs_data_array_push_back(stages,value);obs_data_release(value);
                }
                obs_data_set_array(record,"stages",stages);obs_data_array_release(stages);
                const std::string serialized=obs_data_get_json(record);obs_data_release(record);
                const auto serialization=ns(serializeBegin);const auto unmapBegin=Clock::now();
                if(readback)mapped->Unmap(0,&empty);
                const auto unmapNs=ns(unmapBegin);
                const auto providerCloseBegin=Clock::now();hasher.reset();multi.reset();
                const auto providerCloseNs=ns(providerCloseBegin);
                expect(credits.release(*token,{copyValue,fence->GetCompletedValue(),copyValue,fence->GetCompletedValue(),
                    true,true,true,true}),"actual copy/map/CPU-finished credit release");
                const auto reserveAge=ns(reserved);
                if(readback) {
                    for(size_t role=0;role<sizes.size();++role)if(sizes[role])
                        expect(results[role]==normalResult[role],"mapped actual full payload differs from normal memory");
                }
                if(!readback)normalResult=results;
                std::array<uint64_t,17> baseline{};const auto baselineBegin=Clock::now();
                // One-time unchanged full17 scalar FNV baseline, not native dual hashing.
                void* baselineMapped=nullptr;
                if(readback)requireHr(mapped->Map(0,&range,&baselineMapped),"SHA experiment separate baseline map");
                const auto* baselineBytes=readback?static_cast<const uint8_t*>(baselineMapped):normal.data();
                for(size_t role=0;role<sizes.size();++role)if(sizes[role])
                    baseline[role]=nr::qa::bootstrap::hash({baselineBytes+offsets[role],sizes[role]});
                if(readback)mapped->Unmap(0,&empty);
                const auto baselineNs=ns(baselineBegin);
                std::printf("SHA_COMPLETE_PROFILE seed=%u color=%u half=%u phase=%u mapped=%u roles=%u bytes=%llu cold_first=%u provider_ns=%lld role_setup_ns=%lld copy_ns=%lld map_ns=%lld hash_ns=%lld finish_ns=%lld complete_hash_ns=%lld serialization_ns=%lld codec_burst_ns=%lld codec_bytes=%llu unmap_ns=%lld provider_close_ns=%lld reserve_age_ns=%lld baseline_full17_fnv_ns=%lld current_budget_reference_ns=4317900 acceptance=false\n",
                    seed,static_cast<unsigned>(color),static_cast<unsigned>(half),phase,static_cast<unsigned>(readback),
                    static_cast<unsigned>(std::count_if(sizes.begin(),sizes.end(),[](size_t n){return n!=0;})),
                    static_cast<unsigned long long>(total.bytes),static_cast<unsigned>(firstProvider),
                    static_cast<long long>(providerNs),static_cast<long long>(roleSetup),static_cast<long long>(copyNs),
                    static_cast<long long>(mapNs),static_cast<long long>(total.hashNs),static_cast<long long>(total.finishNs),
                    static_cast<long long>(hashTotal),static_cast<long long>(serialization),
                    static_cast<long long>(burst.hashNs+burst.finishNs),static_cast<unsigned long long>(burst.bytes),
                    static_cast<long long>(unmapNs),static_cast<long long>(providerCloseNs),
                    static_cast<long long>(reserveAge),static_cast<long long>(baselineNs));
                std::printf("SHA_COMPLETE_ROLES %s\n",serialized.c_str());
                std::printf("SHA_MECHANISM batched=%u codec_in_same_batch=%u finite_validation_ns=%lld hash_and_finish_include_codec=%u\n",
                    static_cast<unsigned>(batched),static_cast<unsigned>(batched&&codecBytes!=0),
                    static_cast<long long>(total.setupNs),static_cast<unsigned>(batched));
                for(size_t role=0;role<sizes.size();++role)if(sizes[role])
                    std::printf("SHA_BASELINE_ROLE role=%zu bytes=%llu fnv=%llu\n",role,
                        static_cast<unsigned long long>(sizes[role]),static_cast<unsigned long long>(baseline[role]));
                firstProvider=false;
            }
        }
    }
    expect(credits.occupied()==0,"experiment exact map/code/credit retirement");
    std::printf("SHA_COMPLETE_PROFILE_CLOSED maps=true credits=0 one_experiment=true physical_acceptance=false\n");
}
void codec_rate_check() {
    const nr::qa::StreamRate legacy;
    expect(legacy.valid()&&legacy.decoder_options()=="framerate=1"&&legacy.decoder_observation_fps()==60,
        "accepted Task12 default decoder stream/observation descriptor changed");
    for(const nr::qa::StreamRate rate:std::array<nr::qa::StreamRate,2>{{{60,1},{60000,1001}}}) {
        expect(rate.valid()&&rate.decoder_observation_fps()==600,"required rational stream/independent observation descriptor");
        expect(rate.decoder_options()==(rate.numerator==60?"framerate=60":"framerate=60000/1001"),
            "actual official decoder rational input option relabeled stream");
        for(uint32_t steps=0;steps<=32;++steps) {
            const uint64_t delta=static_cast<uint64_t>(steps)*rate.denominator*1000000000ull/rate.numerator;
            expect(rate.timestamp_delta(delta,steps)&&rate.timestamp_delta(delta+1,steps),
                "actual integer nanosecond rational rounding rejected");
            expect(!rate.timestamp_delta(delta+2,steps),"actual missing/reordered rational timestamp admitted");
        }
        expect(!rate.timestamp_delta(0,33),"unbounded decoded timestamp descriptor admitted");
    }
    for(const nr::qa::StreamRate bad:std::array<nr::qa::StreamRate,4>{{{0,1},{60,0},{60000,1000},{1,2}}}) {
        expect(!bad.valid()&&!bad.timestamp_delta(0,0),"unsupported decoder rational selector admitted");
        bool refused=false;try{(void)bad.decoder_options();}catch(const std::invalid_argument&){refused=true;}
        expect(refused,"invalid official decoder option escaped boundary admission");
    }
}
void pixel_domain_check() {
    namespace pixels=nr::qa::bootstrap;
    const auto decode=[](double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};
    const auto same=[](double a,double b){return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);};
    for(unsigned code=0;code<256;++code) {
        const std::array<uint8_t,4> bytes{static_cast<uint8_t>(code),static_cast<uint8_t>(255-code),
            static_cast<uint8_t>(code^0x55),255};
        const auto rgb=pixels::packed_rgb(bytes,0,false,true),bgr=pixels::packed_rgb(bytes,0,false,true,true);
        expect(same(rgb.r,decode(static_cast<double>(bytes[0])/255))&&
            same(rgb.g,decode(static_cast<double>(bytes[1])/255))&&
            same(rgb.b,decode(static_cast<double>(bytes[2])/255)),"RGBA8 actual gamma-to-linear equations changed");
        expect(bgr.r==rgb.b&&bgr.g==rgb.g&&bgr.b==rgb.r,"actual BGRA channel order changed");
    }
    for(float value:{-.25f,0.0f,.003f,.04f,.125f,.375f,.625f,.875f,1.0f,2.0f,12.5f}) {
        const std::array<uint16_t,4> words{
            DirectX::PackedVector::XMConvertFloatToHalf(value),
            DirectX::PackedVector::XMConvertFloatToHalf(value+.0625f),
            DirectX::PackedVector::XMConvertFloatToHalf(value+.125f),0x3c00};
        const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(words.data()),sizeof(words)};
        const auto raw=pixels::packed_rgb(bytes,0,true,false),gamma=pixels::packed_rgb(bytes,0,true,true);
        const std::array<double,3> actual{raw.r,raw.g,raw.b},converted{gamma.r,gamma.g,gamma.b};
        for(size_t channel=0;channel<3;++channel) {
            const double reference=DirectX::PackedVector::XMConvertHalfToFloat(words[channel]);
            expect(same(actual[channel],reference),"linear HDR/negative half was normalized or clipped");
            expect(same(converted[channel],decode(reference)),"gamma half was decoded as linear or quantized to eight bits");
        }
    }
    const std::array<uint16_t,4> before{0x3600,0x3600,0x3600,0x3c00},after{0x3900,0x3900,0x3900,0x3c00};
    const auto a=pixels::packed_rgb({reinterpret_cast<const uint8_t*>(before.data()),sizeof(before)},0,true,true);
    const auto b=pixels::packed_rgb({reinterpret_cast<const uint8_t*>(after.data()),sizeof(after)},0,true,true);
    expect(same(b.r-a.r,decode(.625)-decode(.375))&&b.r-a.r!=.625-.375,
        "same-job SDR model edit subtraction stayed in gamma magnitudes");
}
void credits_check() {
    nr::qa::GhostCredits credits;
    const auto a=credits.reserve({41,1,1},1),b=credits.reserve({41,1,2},2),c=credits.reserve({41,1,3},3);
    expect(a&&b&&c,"three total observation credits across generations");
    expect(!credits.reserve({41,1,4},4),"fourth/per-generation evidence credit admitted");
    std::array<uint8_t,64> original{},owned{};
    for(size_t i=0;i<original.size();++i)original[i]=static_cast<uint8_t>(i*3+1);
    const auto expected=original;
    std::promise<void> copied;const auto completion=copied.get_future();
    std::thread producer([&]{owned=original;copied.set_value();});
    const auto ready=completion.wait_for(std::chrono::seconds(5));producer.join();
    expect(ready==std::future_status::ready,"pre-subscribed exact copy event");
    original.fill(0xB2);expect(owned==expected&&owned!=original,"owned observation aliased overwritten original");
    std::span<const uint8_t> mappedView{owned};
    expect(nr::qa::bootstrap::hash(mappedView)==nr::qa::bootstrap::hash(expected)&&
        nr::qa::bootstrap::hash(mappedView)!=nr::qa::bootstrap::hash(original),
        "readonly owned readback view followed overwritten original pixels");
    nr::qa::GhostCredits::Receipt receipt{7,6,9,9,true,true,true,true};
    expect(!credits.release(*a,receipt),"pending producer released observation");
    receipt.producerDone=7;receipt.copyDone=8;
    expect(!credits.release(*a,receipt),"pending readback released observation");
    receipt.copyDone=UINT64_MAX;
    expect(!credits.release(*a,receipt),"failed sentinel released observation");
    receipt.copyDone=9;receipt.dispatchReturned=false;
    expect(!credits.release(*a,receipt),"GPU return substituted for dispatch return");
    receipt.dispatchReturned=true;receipt.sourceDetached=false;
    expect(!credits.release(*a,receipt),"external input binding released after session close");
    receipt.sourceDetached=true;receipt.mapsClosed=false;
    expect(!credits.release(*a,receipt),"open observation map released credit");
    mappedView={};expect(mappedView.empty(),"mapped view outlived its close boundary");
    receipt.mapsClosed=true;receipt.cpuFinished=false;
    expect(!credits.release(*a,receipt),"CPU consumer overwritten before completion");
    receipt.cpuFinished=true;expect(credits.release(*a,receipt),"real completed detached credit not released");
    const auto newer=credits.reserve({41,2,4},4);
    expect(newer&&newer->serial>a->serial&&newer->frame!=a->frame,"reused address changed immutable serial/key");
    expect(!credits.release(*a,receipt),"old generation/serial released newer envelope");
    expect(credits.release(*newer,receipt)&&credits.release(*b,receipt)&&credits.release(*c,receipt)&&
        credits.occupied()==0,"exact once cooperative credit cleanup");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("production probe selector");
        if(std::string_view(argv[1])=="canvas")canvas_check();
        else if(std::string_view(argv[1])=="templates")templates_check();
        else if(std::string_view(argv[1])=="timing")timing_check();
        else if(std::string_view(argv[1])=="parity")parity_check();
        else if(std::string_view(argv[1])=="credits")credits_check();
        else if(std::string_view(argv[1])=="packing")packing_check();
        else if(std::string_view(argv[1])=="rgb-unpack")rgb_unpack_check();
        else if(std::string_view(argv[1])=="admission")admission_check();
        else if(std::string_view(argv[1])=="pixel-domain")pixel_domain_check();
        else if(std::string_view(argv[1])=="codec-rate")codec_rate_check();
        else if(std::string_view(argv[1])=="hash-service")hash_service_check();
        else if(std::string_view(argv[1])=="hash-shape")hash_shape_check();
        else if(std::string_view(argv[1])=="hash-limb")hash_limb_check();
        else if(std::string_view(argv[1])=="hash-profile")hash_profile_check();
        else if(std::string_view(argv[1])=="sha256")sha256_check();
        else if(std::string_view(argv[1])=="chunk-proof")chunk_check();
        else if(std::string_view(argv[1])=="chunk-controls")chunk_controls_check();
        else if(std::string_view(argv[1])=="codec-capacity")codec_capacity_check();
        else if(std::string_view(argv[1])=="sha-multi")sha_multi_check();
        else if(std::string_view(argv[1])=="sha-profile")sha_profile_check();
        else if(std::string_view(argv[1])=="sha-multi-profile")sha_profile_check(true);
        else throw std::runtime_error("unknown production probe");
        const bool gpu=std::string_view(argv[1])=="sha-profile"||std::string_view(argv[1])=="sha-multi-profile";
        std::printf("PASS ghost_production checks=%u CPU_only=%s physical_calibration=false\n",checks,gpu?"false":"true");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL ghost_production %s checks=%u\n",error.what(),checks);return 1;}
}
