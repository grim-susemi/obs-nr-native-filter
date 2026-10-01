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
        std::puts("PASS bootstrap_images SDR/HDR source landmarks; blank and metadata-only wrong-source refused");
        return 0;
    }catch(const std::exception& e) { std::fprintf(stderr,"FAIL bootstrap_images %s\n",e.what());return 1; }
}
