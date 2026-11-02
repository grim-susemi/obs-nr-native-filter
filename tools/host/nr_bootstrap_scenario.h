// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <d3d11.h>
#include <vector>
void nr_register_bootstrap_fixture();
std::vector<uint8_t> nr_bootstrap_read_texture(ID3D11Texture2D*,bool hdr);
std::vector<uint8_t> nr_bootstrap_read_texture(ID3D11Texture2D*,bool hdr,uint32_t width,uint32_t height);
bool nr_run_bootstrap_scenario(const std::filesystem::path&,const std::filesystem::path&,bool& retained);
