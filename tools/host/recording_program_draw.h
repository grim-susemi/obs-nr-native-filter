// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "recording_program_marker.h"
#include "recording_program_palette.h"
#include <DirectXPackedVector.h>
#include <graphics/graphics.h>
#include <cstring>
namespace nr::qa {
class ProgramMarkerDraw {
    gs_texture_t* texture_=nullptr;
    const bool hdr_;
    std::array<std::array<uint8_t,ProgramMarker::width*ProgramMarker::height*8>,768> prepared_{};
public:
    explicit ProgramMarkerDraw(bool hdr):hdr_(hdr) {
        for(uint32_t phase=0;phase<3;++phase)for(uint32_t ingress=1;ingress<=256;++ingress) {
            const auto cells=ProgramMarker::encode(ProgramMarker::wire(1,ingress,phase));
            auto& bytes=prepared_[phase*256+ingress-1];
            const uint32_t pixel=hdr?8u:4u;
            for(uint32_t y=0;y<ProgramMarker::height;++y)for(uint32_t x=0;x<ProgramMarker::width;++x) {
                const auto index=(y/2)*ProgramMarker::columns+x/2;
                const auto symbol=cells[index];
                for(uint32_t channel=0;channel<4;++channel) {
                    const auto offset=(y*ProgramMarker::width+x)*pixel+channel*(hdr?2u:1u);
                    if(hdr) {
                        const auto half=ProgramPalette::hdr[symbol][channel];
                        std::memcpy(bytes.data()+offset,&half,2);
                    }else bytes[offset]=ProgramPalette::sdr[symbol][channel];
                }
            }
        }
        obs_enter_graphics();
        texture_=gs_texture_create(ProgramMarker::width,ProgramMarker::height,
            hdr?GS_RGBA16F:GS_RGBA_UNORM,1,nullptr,GS_DYNAMIC);
        obs_leave_graphics();
        if(!texture_)throw std::runtime_error("program witness persistent texture");
    }
    ~ProgramMarkerDraw() {
        obs_enter_graphics();gs_texture_destroy(texture_);obs_leave_graphics();
    }
    void draw(uint32_t wire,uintptr_t actualProgramTarget) {
        if((wire>>20)!=1 || reinterpret_cast<uintptr_t>(gs_get_render_target())!=actualProgramTarget ||
            gs_get_color_space()!=(hdr_?GS_CS_709_EXTENDED:GS_CS_SRGB) ||
            (hdr_ && obs_get_video_sdr_white_level()!=300))
            throw std::runtime_error("program witness wrong main resource/epoch");
        const auto ingress=(wire>>4)&0xffff,phase=wire&15;
        if(!ingress || ingress>256 || phase>2)throw std::runtime_error("program witness unprepared code");
        gs_texture_set_image(texture_,prepared_[phase*256+ingress-1].data(),
            ProgramMarker::width*(hdr_?8:4),false);
        const bool srgb=gs_framebuffer_srgb_enabled();gs_enable_framebuffer_srgb(false);
        gs_blend_state_push();gs_enable_blending(false);gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(ProgramMarker::x),static_cast<float>(ProgramMarker::y),0);
        auto* effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),texture_);
        while(gs_effect_loop(effect,"DrawAlphaDivide"))
            gs_draw_sprite(texture_,0,ProgramMarker::width,ProgramMarker::height);
        gs_matrix_pop();gs_blend_state_pop();gs_enable_framebuffer_srgb(srgb);
    }
};
}
