// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_ghost_digest.h"
#include "nr_instance_json.h"
#include <d3d12.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace nr::qa {
// Bytes are the final composed model output, tightly packed by the scenario's
// existing completed-GPU readback. The raw filename is relative to the JSON.
inline void write_model_digest(const std::filesystem::path& path,
    std::span<const uint8_t> bytes,const D3D12_RESOURCE_DESC& texture)
{
    const bool hdr=texture.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    if((texture.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && !hdr) ||
        bytes.size()!=texture.Width*texture.Height*(hdr?8:4))
        throw std::runtime_error("NR_OUTPUT_DIGEST_FORMAT: expected packed RGBA8 or RGBA16F");
    ghost_digest::Sha256 hasher;
    const auto digest=ghost_digest::hex(hasher.image(bytes,bytes.size(),false));
    auto raw=path; raw.replace_extension(".raw");
    if(raw==path)throw std::runtime_error("NR_OUTPUT_DIGEST_PATH: JSON and raw paths coincide");
    std::ofstream pixels(raw,std::ios::binary|std::ios::trunc);
    if(!pixels)throw std::runtime_error("NR_OUTPUT_DIGEST_RAW_OPEN");
    pixels.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    pixels.close();
    if(!pixels)throw std::runtime_error("NR_OUTPUT_DIGEST_RAW_WRITE");
    std::ofstream json(path,std::ios::binary|std::ios::trunc);
    if(!json)throw std::runtime_error("NR_OUTPUT_DIGEST_JSON_OPEN");
    json<<"{\"sha256\":\""<<digest<<"\",\"width\":"<<texture.Width
        <<",\"height\":"<<texture.Height<<",\"format\":\""
        <<(hdr?"DXGI_FORMAT_R16G16B16A16_FLOAT":"DXGI_FORMAT_R8G8B8A8_UNORM")<<"\",\"raw_path\":";
    write_instance_json_string(json,raw.filename().string().c_str());
    json<<"}\n"; json.close();
    if(!json)throw std::runtime_error("NR_OUTPUT_DIGEST_JSON_WRITE");
}
}
