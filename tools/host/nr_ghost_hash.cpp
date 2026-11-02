// SPDX-License-Identifier: GPL-3.0-only
// QA-only integer lanes. Caller first verifies AVX512F/DQ and OS XState.
#include "nr_bootstrap_images.h"
#include <immintrin.h>
#include <algorithm>
#include <bit>
#include <cstring>
namespace nr::qa::bootstrap {
std::array<uint64_t,8> hash_eight_avx512(const std::array<std::span<const uint8_t>,8>& bytes) {
    static_assert(std::endian::native==std::endian::little);
    constexpr uint64_t offset=14695981039346656037ull,prime=1099511628211ull;
    auto value=_mm512_set1_epi64(std::bit_cast<int64_t>(offset));
    const auto multiplier=_mm512_set1_epi64(static_cast<int64_t>(prime));
    const auto byteMask=_mm512_set1_epi64(255);
    size_t maximum=0;for(const auto stream:bytes)maximum=std::max(maximum,stream.size());
    std::array<uint64_t,8> addresses{};
    for(size_t lane=0;lane<bytes.size();++lane)addresses[lane]=reinterpret_cast<uintptr_t>(bytes[lane].data());
    auto pointers=_mm512_loadu_si512(addresses.data());
    const auto stride=_mm512_set1_epi64(8);
    size_t at=0;
    while(at<maximum) {
        __mmask8 full=0;bool partial=false;size_t limit=maximum;
        for(size_t lane=0;lane<bytes.size();++lane) {
            if(at>=bytes[lane].size())continue;
            if(bytes[lane].size()-at<8){partial=true;continue;}
            full=static_cast<__mmask8>(full|(1u<<lane));
            limit=std::min(limit,bytes[lane].size()&~size_t{7});
        }
#define NR_HASH_BYTE(mask) \
            value=_mm512_mask_mullo_epi64(value,mask, \
                _mm512_xor_si512(value,_mm512_and_si512(word,byteMask)),multiplier); \
            word=_mm512_srli_epi64(word,8)
        if(!partial) {
            // One mask remains valid through this contiguous prefix. Gather
            // complete actual words directly; no per-word dynamic memcpy calls.
            for(;at<limit;at+=8) {
                auto word=_mm512_mask_i64gather_epi64(_mm512_setzero_si512(),full,pointers,nullptr,1);
                NR_HASH_BYTE(full);NR_HASH_BYTE(full);NR_HASH_BYTE(full);NR_HASH_BYTE(full);
                NR_HASH_BYTE(full);NR_HASH_BYTE(full);NR_HASH_BYTE(full);NR_HASH_BYTE(full);
                pointers=_mm512_add_epi64(pointers,stride);
            }
        }else {
            std::array<uint64_t,8> words{};std::array<__mmask8,8> active{};
            for(size_t lane=0;lane<bytes.size();++lane) {
                if(at>=bytes[lane].size())continue;
                const size_t count=std::min(size_t{8},bytes[lane].size()-at);
                memcpy(&words[lane],bytes[lane].data()+at,count);
                for(size_t byte=0;byte<count;++byte)
                    active[byte]=static_cast<__mmask8>(active[byte]|(1u<<lane));
            }
            auto word=_mm512_loadu_si512(words.data());
            for(size_t byte=0;byte<8;++byte) {
                value=_mm512_mask_mullo_epi64(value,active[byte],
                _mm512_xor_si512(value,_mm512_and_si512(word,byteMask)),multiplier);
                word=_mm512_srli_epi64(word,8);
            }
            at+=8;pointers=_mm512_add_epi64(pointers,stride);
        }
#undef NR_HASH_BYTE
    }
    std::array<uint64_t,8> result{};_mm512_storeu_si512(result.data(),value);return result;
}
}
