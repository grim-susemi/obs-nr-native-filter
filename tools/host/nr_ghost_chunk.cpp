// SPDX-License-Identifier: GPL-3.0-only
#include "nr_ghost_chunk.h"
#include <algorithm>
#include <stdexcept>
namespace nr::qa::ghost_digest {
namespace {
void checked(HRESULT result,const char* operation) {
    if(FAILED(result))throw std::runtime_error(operation);
}
template<size_t N> void little(std::array<uint8_t,N>& prefix,size_t at,uint64_t value,size_t count) {
    for(size_t i=0;i<count;++i)prefix[at+i]=static_cast<uint8_t>(value>>(i*8));
}
}
std::array<uint8_t,25> leaf_prefix(uint64_t length,uint64_t index,uint32_t bytes) {
    if(!length||length>UINT64_MAX-chunkBytes+1||index>=(length+chunkBytes-1)/chunkBytes||
        bytes!=std::min<uint64_t>(chunkBytes,length-index*chunkBytes))
        throw std::runtime_error("actual chunk leaf extent");
    std::array<uint8_t,25> prefix{};
    little(prefix,1,length,8);little(prefix,9,index,8);
    little(prefix,17,chunkBytes,4);little(prefix,21,bytes,4);return prefix;
}
std::array<uint8_t,21> root_prefix(uint64_t length) {
    if(!length||length>UINT64_MAX-chunkBytes+1)throw std::runtime_error("actual chunk root extent");
    std::array<uint8_t,21> prefix{};prefix[0]=1;
    little(prefix,1,length,8);little(prefix,9,chunkBytes,4);
    little(prefix,13,(length+chunkBytes-1)/chunkBytes,8);return prefix;
}
Digest chunk_root(Sha256& sha,uint64_t length,std::span<const uint8_t> leaves) {
    const auto prefix=root_prefix(length);
    const auto count=(length+chunkBytes-1)/chunkBytes;
    if(count>SIZE_MAX/32||leaves.size()!=count*32)throw std::runtime_error("missing or extra actual chunk leaves");
    const std::array<std::span<const uint8_t>,2> parts{prefix,leaves};
    return sha.hash(parts);
}
ChunkPipeline::ChunkPipeline(ID3D12Device* device,std::span<const std::byte> shader) {
    if(!device||shader.empty())throw std::runtime_error("actual chunk code required");
    std::array<D3D12_ROOT_PARAMETER,3> parameters{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,9};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={0,0};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor={0,0};
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters=static_cast<UINT>(parameters.size());description.pParameters=parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> serialized,error;
    checked(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),
        "private chunk root signature serialization");
    checked(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
        IID_PPV_ARGS(&signature_)),"private chunk root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};compute.pRootSignature=signature_.Get();
    compute.CS={shader.data(),shader.size()};
    checked(device->CreateComputePipelineState(&compute,IID_PPV_ARGS(&pipeline_)),"private chunk pipeline");
}
void ChunkPipeline::record(ID3D12GraphicsCommandList* commands,ID3D12Resource* source,
    ID3D12Resource* scratch,uint32_t length,uint32_t rowBytes,uint32_t rowPitch,uint32_t offset,bool halfFloat) const {
    if(!commands||!source||!scratch||!length||!rowBytes||length%rowBytes||rowPitch<rowBytes||
        (halfFloat&&(length%2||rowBytes%2)))throw std::runtime_error("actual chunk row extent");
    const uint64_t end=offset+static_cast<uint64_t>(length/rowBytes-1)*rowPitch+rowBytes;
    const auto count=(static_cast<uint64_t>(length)+chunkBytes-1)/chunkBytes;
    const auto input=source->GetDesc(),output=scratch->GetDesc();
    if(input.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||output.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||
        end>UINT32_MAX||((end+3)&~3ull)>input.Width||count*32+4>output.Width||
        !(output.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        throw std::runtime_error("actual chunk owning buffer bounds");
    std::array<uint32_t,9> constants{length,0,rowBytes,rowPitch,offset,static_cast<uint32_t>(count),
        static_cast<uint32_t>(halfFloat),1,static_cast<uint32_t>(count*32)};
    commands->SetPipelineState(pipeline_.Get());commands->SetComputeRootSignature(signature_.Get());
    commands->SetComputeRootShaderResourceView(1,source->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(2,scratch->GetGPUVirtualAddress());
    commands->SetComputeRoot32BitConstants(0,static_cast<UINT>(constants.size()),constants.data(),0);
    commands->Dispatch(1,1,1);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource=scratch;commands->ResourceBarrier(1,&barrier);
    constants[7]=0;commands->SetComputeRoot32BitConstants(0,static_cast<UINT>(constants.size()),constants.data(),0);
    commands->Dispatch(static_cast<UINT>((count+127)/128),1,1);
    commands->ResourceBarrier(1,&barrier);
}
}
