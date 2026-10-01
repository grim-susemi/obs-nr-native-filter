// SPDX-License-Identifier: GPL-3.0-only
#include "nr_bootstrap_images.h"
#include <DirectXPackedVector.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
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
    if(y>=256 && y<320 && x>=800+id*32 && x<824+id*32)p={high,high,high,1};
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
    require(id>0 && id<=16,"bounded source content ID");
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
Image rgb_image(std::span<const uint8_t> bytes,bool hdr,bool bgra,const char* stage) {
    require(bytes.size()==static_cast<size_t>(width)*height*(hdr?8:4),"full-size RGB image bounds");
    Image image{hdr,{}};image.pixels.resize(static_cast<size_t>(width)*height);
    for(size_t i=0;i<image.pixels.size();++i)for(size_t c=0;c<4;++c) {
        if(hdr) { uint16_t half=0;memcpy(&half,bytes.data()+i*8+c*2,2);image.pixels[i][c]=DirectX::PackedVector::XMConvertHalfToFloat(half); }
        else image.pixels[i][c]=static_cast<float>(bytes[i*4+(bgra && c!=1 && c!=3?2-c:c)])/255;
        if(!std::isfinite(image.pixels[i][c])) {
            uint16_t packed=0;if(hdr)memcpy(&packed,bytes.data()+i*8+c*2,2);
            throw std::runtime_error(std::string(stage)+" nonfinite actual RGB pixel="+std::to_string(i)+
                " channel="+std::to_string(c)+" packed="+std::to_string(packed));
        }
    }
    return image;
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
    require(ids[0]==ids[1] && ids[0]>0 && ids[0]<=16,"independent source bit rows disagree");
    const auto x=804+ids[0]*32;
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
    require(ids[0]==ids[1] && ids[0]>0 && ids[0]<=16,"decoded independent source bit rows");
    require(mean_y(804+ids[0]*32,276)>threshold,"decoded metadata-only stamp without moving landmark");
    return ids[0];
}
uint64_t hash(std::span<const uint8_t> bytes) {
    uint64_t value=14695981039346656037ull;for(auto b:bytes)value=(value^b)*1099511628211ull;return value;
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
