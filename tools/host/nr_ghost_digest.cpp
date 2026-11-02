// SPDX-License-Identifier: GPL-3.0-only
#include "nr_ghost_digest.h"
#include "nr_bootstrap_images.h"
#include <windows.h>
#include <bcrypt.h>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nr::qa::ghost_digest {
namespace {
using Clock=std::chrono::steady_clock;
int64_t elapsed(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
}
void checked(NTSTATUS result,const char* operation) {
    if(result<0)throw std::runtime_error(operation);
}
}
struct Sha256::Impl {
    BCRYPT_ALG_HANDLE provider=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    std::vector<uint8_t> object;
    int64_t preparationNs=0;
    ~Impl() {
        if(hash)BCryptDestroyHash(hash);
        if(provider)BCryptCloseAlgorithmProvider(provider,0);
    }
};
Sha256::Sha256():impl_(std::make_unique<Impl>()) {
    const auto started=Clock::now();
    checked(BCryptOpenAlgorithmProvider(&impl_->provider,BCRYPT_SHA256_ALGORITHM,nullptr,0),
        "ghost SHA256 provider");
    ULONG bytes=0,returned=0,digestBytes=0;
    checked(BCryptGetProperty(impl_->provider,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&bytes),
        sizeof(bytes),&returned,0),"ghost SHA256 object length");
    if(returned!=sizeof(bytes)||!bytes)throw std::runtime_error("ghost SHA256 object bounds");
    checked(BCryptGetProperty(impl_->provider,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&digestBytes),
        sizeof(digestBytes),&returned,0),"ghost SHA256 digest length");
    if(returned!=sizeof(digestBytes)||digestBytes!=32)throw std::runtime_error("ghost full SHA256 length");
    impl_->object.resize(bytes);
    checked(BCryptCreateHash(impl_->provider,&impl_->hash,impl_->object.data(),bytes,nullptr,0,
        BCRYPT_HASH_REUSABLE_FLAG),"ghost SHA256 reusable state");
    impl_->preparationNs=elapsed(started);
}
Sha256::~Sha256()=default;
int64_t Sha256::preparation_ns() const {return impl_->preparationNs;}
Digest Sha256::image(std::span<const uint8_t> bytes,size_t logicalBytes,bool halfFloat,Timing* timing) {
    if(bytes.empty()||bytes.size()!=logicalBytes)throw std::runtime_error("ghost SHA256 full logical image");
    if(halfFloat&&!bootstrap::finite_half(bytes))throw std::runtime_error("ghost SHA256 nonfinite half image");
    return hash(bytes,timing);
}
Digest Sha256::hash(std::span<const uint8_t> bytes,Timing* timing) {
    const std::array<std::span<const uint8_t>,1> parts{bytes};
    return hash(parts,timing);
}
Digest Sha256::hash(std::span<const std::span<const uint8_t>> parts,Timing* timing) {
    Timing measured{};
    const auto begin=Clock::now();
    for(const auto bytes:parts) {
        if(bytes.size()>(std::numeric_limits<ULONG>::max)())throw std::runtime_error("ghost SHA256 payload bounds");
        measured.bytes+=bytes.size();
        if(!bytes.empty())checked(BCryptHashData(impl_->hash,const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()),0),"ghost SHA256 actual payload");
    }
    measured.hashNs=elapsed(begin);
    Digest result{};
    const auto finish=Clock::now();
    checked(BCryptFinishHash(impl_->hash,result.data(),static_cast<ULONG>(result.size()),0),
        "ghost SHA256 finalization/reset");
    measured.finishNs=elapsed(finish);
    if(timing)*timing=measured;
    return result;
}
struct Sha256Batch::Impl {
    BCRYPT_ALG_HANDLE provider=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    std::vector<uint8_t> object;
    int64_t preparationNs=0;
    ~Impl() {
        if(hash)BCryptDestroyHash(hash);
        if(provider)BCryptCloseAlgorithmProvider(provider,0);
    }
};
Sha256Batch::Sha256Batch():impl_(std::make_unique<Impl>()) {
    const auto started=Clock::now();
    checked(BCryptOpenAlgorithmProvider(&impl_->provider,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_MULTI_FLAG),
        "ghost SHA256 SIMD provider");
    BCRYPT_MULTI_OBJECT_LENGTH_STRUCT sizes{};ULONG returned=0,digestBytes=0;
    checked(BCryptGetProperty(impl_->provider,BCRYPT_MULTI_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&sizes),
        sizeof(sizes),&returned,0),"ghost SHA256 SIMD object length");
    if(returned!=sizeof(sizes)||!sizes.cbPerElement)throw std::runtime_error("ghost SHA256 SIMD object bounds");
    const uint64_t bytes=sizes.cbPerObject+static_cast<uint64_t>(capacity)*sizes.cbPerElement;
    if(bytes>(std::numeric_limits<ULONG>::max)())throw std::runtime_error("ghost SHA256 SIMD object extent");
    checked(BCryptGetProperty(impl_->provider,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&digestBytes),
        sizeof(digestBytes),&returned,0),"ghost SHA256 SIMD digest length");
    if(returned!=sizeof(digestBytes)||digestBytes!=32)throw std::runtime_error("ghost full SIMD SHA256 length");
    impl_->object.resize(static_cast<size_t>(bytes));
    checked(BCryptCreateMultiHash(impl_->provider,&impl_->hash,static_cast<ULONG>(capacity),impl_->object.data(),
        static_cast<ULONG>(bytes),nullptr,0,BCRYPT_HASH_REUSABLE_FLAG),"ghost SHA256 SIMD reusable states");
    impl_->preparationNs=elapsed(started);
}
Sha256Batch::~Sha256Batch()=default;
int64_t Sha256Batch::preparation_ns() const {return impl_->preparationNs;}
void Sha256Batch::images(std::span<const Payload> payloads,std::span<std::optional<Digest>> results,Timing* timing) {
    if(payloads.size()>capacity||payloads.size()!=results.size())throw std::runtime_error("ghost SHA256 SIMD role bounds");
    Timing measured{};const auto setup=Clock::now();
    for(size_t role=0;role<payloads.size();++role) {
        results[role].reset();const auto& p=payloads[role];
        if(!p.present) {
            if(!p.bytes.empty()||p.logicalBytes)throw std::runtime_error("ghost absent SIMD payload has bytes");
            continue;
        }
        if(p.bytes.empty()||p.bytes.size()!=p.logicalBytes||
            p.bytes.size()>(std::numeric_limits<ULONG>::max)())throw std::runtime_error("ghost SIMD full logical image");
        if(p.halfFloat&&!bootstrap::finite_half(p.bytes))throw std::runtime_error("ghost SIMD nonfinite half image");
        measured.bytes+=p.bytes.size();
    }
    std::array<BCRYPT_MULTI_HASH_OPERATION,capacity> operations{};size_t count=0;
    for(size_t role=0;role<payloads.size();++role)if(payloads[role].present) {
        const auto& bytes=payloads[role].bytes;
        operations[count++]={static_cast<ULONG>(role),BCRYPT_HASH_OPERATION_HASH_DATA,
            const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size())};
    }
    measured.setupNs=elapsed(setup);
    if(count) {
        const auto hash=Clock::now();
        checked(BCryptProcessMultiOperations(impl_->hash,BCRYPT_OPERATION_TYPE_HASH,operations.data(),
            static_cast<ULONG>(count*sizeof(operations[0])),0),"ghost SHA256 SIMD actual full payloads");
        measured.hashNs=elapsed(hash);count=0;
        for(size_t role=0;role<payloads.size();++role)if(payloads[role].present) {
            auto& output=results[role].emplace();
            operations[count++]={static_cast<ULONG>(role),BCRYPT_HASH_OPERATION_FINISH_HASH,
                output.data(),static_cast<ULONG>(output.size())};
        }
        const auto finish=Clock::now();
        checked(BCryptProcessMultiOperations(impl_->hash,BCRYPT_OPERATION_TYPE_HASH,operations.data(),
            static_cast<ULONG>(count*sizeof(operations[0])),0),"ghost SHA256 SIMD finalization and logical resets");
        measured.finishNs=elapsed(finish);
    }
    if(timing)*timing=measured;
}
std::string hex(const Digest& digest) {
    constexpr char alphabet[]="0123456789abcdef";
    std::string result(64,'0');
    for(size_t at=0;at<digest.size();++at) {
        result[at*2]=alphabet[digest[at]>>4];result[at*2+1]=alphabet[digest[at]&15];
    }
    return result;
}
bool valid_hex(std::string_view value) {
    if(value.size()!=64)return false;
    for(const char byte:value)if(!((byte>='0'&&byte<='9')||(byte>='a'&&byte<='f')))return false;
    return true;
}
}
