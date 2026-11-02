// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_ghost_digest.h"
#include <d3d12.h>
#include <wrl/client.h>
namespace nr::qa::ghost_digest {
inline constexpr std::string_view chunkAlgorithm="sha256-chunk-root-v1";
inline constexpr uint32_t chunkBytes=4096,chunkSchema=2;
std::array<uint8_t,25> leaf_prefix(uint64_t length,uint64_t index,uint32_t bytes);
std::array<uint8_t,21> root_prefix(uint64_t length);
Digest chunk_root(Sha256&,uint64_t length,std::span<const uint8_t> canonicalLeaves);
// Records on the existing private lane; owns no queue, images, extra credits
// or worker. Caller retains these code objects through actual completion.
class ChunkPipeline final {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
public:
    ChunkPipeline(ID3D12Device*,std::span<const std::byte> shader);
    void record(ID3D12GraphicsCommandList*,ID3D12Resource* source,
        ID3D12Resource* scratch,uint32_t length,uint32_t rowBytes,
        uint32_t rowPitch,uint32_t offset,bool halfFloat) const;
};
}
