// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../host/nr_ghost_digest.h"
#include <array>
#include <bit>
#include <cstring>
#include <span>
namespace nr::qa::test {
inline nr::qa::ghost_digest::Digest reference_sha256(std::span<const uint8_t> input) {
    constexpr std::array<uint32_t,64> constants{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<uint32_t,8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const auto block=[&](const uint8_t* bytes) {
        std::array<uint32_t,64> words{};
        for(size_t i=0;i<16;++i)words[i]=(uint32_t{bytes[i*4]}<<24)|(uint32_t{bytes[i*4+1]}<<16)|
            (uint32_t{bytes[i*4+2]}<<8)|bytes[i*4+3];
        for(size_t i=16;i<words.size();++i) {
            const auto x=words[i-15],y=words[i-2];
            words[i]=words[i-16]+(std::rotr(x,7)^std::rotr(x,18)^(x>>3))+words[i-7]+
                (std::rotr(y,17)^std::rotr(y,19)^(y>>10));
        }
        auto a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for(size_t i=0;i<words.size();++i) {
            const uint32_t first=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^((~e)&g))+constants[i]+words[i];
            const uint32_t second=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;
        }
        const std::array<uint32_t,8> after{a,b,c,d,e,f,g,h};
        for(size_t i=0;i<state.size();++i)state[i]+=after[i];
    };
    size_t at=0;for(;at+64<=input.size();at+=64)block(input.data()+at);
    std::array<uint8_t,128> tail{};const size_t remainder=input.size()-at;
    if(remainder)memcpy(tail.data(),input.data()+at,remainder);
    tail[remainder]=0x80;const size_t finalBytes=remainder<56?64u:128u;
    const uint64_t bits=static_cast<uint64_t>(input.size())*8;
    for(size_t i=0;i<8;++i)tail[finalBytes-1-i]=static_cast<uint8_t>(bits>>(i*8));
    block(tail.data());if(finalBytes==128)block(tail.data()+64);
    nr::qa::ghost_digest::Digest result{};
    for(size_t i=0;i<state.size();++i)for(size_t byte=0;byte<4;++byte)
        result[i*4+byte]=static_cast<uint8_t>(state[i]>>(24-byte*8));
    return result;
}
}
