// SPDX-License-Identifier: GPL-3.0-only
#include "../host/nr_bootstrap_images.h"
#include <cstdio>
#include <stdexcept>
using namespace nr::qa::bootstrap;
namespace {
void expect(bool v,const char* why) { if(!v)throw std::runtime_error(why); }
void rejected(const Image& image) {
    bool refused=false;
    try { (void)content_id(image); }catch(const std::runtime_error&) { refused=true; }
    expect(refused,"blank or post-stamped wrong source passed image identity");
}
}
int main() {
    try {
        for(bool hdr:{false,true}) {
            auto first=rgb_image(fixture_bytes(1,hdr),hdr);
            const auto second=rgb_image(fixture_bytes(2,hdr),hdr);
            expect(content_id(first)==1 && content_id(second)==2,"source landmarks not independently decoded");
            for(uint32_t y=32;y<136;++y)for(uint32_t x=32;x<416;++x)
                first.pixels[y*width+x]=second.pixels[y*width+x];
            rejected(first); // Source moving landmark remains1 despite both bit rows stamped2.
            Image blank{hdr,{}};blank.pixels.resize(static_cast<size_t>(width)*height);
            rejected(blank);
        }
        for(bool hdr:{false,true}) {
            for(uint32_t id=1;id<=24;++id)
                expect(content_id(rgb_image(fixture_bytes(id,hdr),hdr))==id,"source content ID round trip failed");
            for(uint32_t id:{0u,25u}) {
                bool refused=false;
                try { (void)fixture_bytes(id,hdr); }catch(const std::runtime_error&) { refused=true; }
                expect(refused,"out-of-range source content ID accepted");
            }
        }
        for(bool hdr:{false,true}) {
            for(uint32_t k=1;k<=24;++k) {
                const auto source=rgb_image(fixture_bytes(k,hdr),hdr);
                for(uint32_t j:{k-1,k+1,k==1?2u:1u}) {
                    if(j<1||j>24||j==k)continue;
                    auto spliced=rgb_image(fixture_bytes(j,hdr),hdr);
                    for(uint32_t y=32;y<136;++y)for(uint32_t x=32;x<416;++x)
                        spliced.pixels[y*width+x]=source.pixels[y*width+x];
                    bool refused=false;
                    try { (void)content_id(spliced); }catch(const std::runtime_error&) { refused=true; }
                    if(!refused)throw std::runtime_error("cross-landmark splice accepted "+std::string(hdr?"HDR":"SDR")+" stamp="+std::to_string(k)+" landmark="+std::to_string(j));
                }
            }
        }
        std::puts("PASS bootstrap_images SDR/HDR source landmarks; blank and metadata-only wrong-source refused");
        return 0;
    }catch(const std::exception& e) { std::fprintf(stderr,"FAIL bootstrap_images %s\n",e.what());return 1; }
}
