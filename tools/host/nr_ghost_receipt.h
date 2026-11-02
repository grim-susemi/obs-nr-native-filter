// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <ostream>
#include <syncstream>
#include <stdexcept>
#include <string_view>
#include "nr_ghost_digest.h"

namespace nr::qa {
// One bounded record on the existing drain, then the joined codec caller.
// No owning strings, growing stream buffer or retained serialization cache.
class GhostDigestRecord final {
    std::array<char,16384> bytes_{};
    std::array<bool,16> first_{};
    size_t size_=0,depth_=0;
    void append(std::string_view value) {
        if(value.size()>bytes_.size()-size_)throw std::runtime_error("bounded digest receipt capacity");
        memcpy(bytes_.data()+size_,value.data(),value.size());size_+=value.size();
    }
    void element() {
        if(!depth_)return;
        if(!first_[depth_-1])append(",");
        first_[depth_-1]=false;
    }
    void quote(std::string_view value) {
        append("\"");
        for(char c:value) {
            if(c=='\"'||c=='\\')append("\\");
            if(static_cast<unsigned char>(c)<32)throw std::runtime_error("digest receipt control character");
            append({&c,1});
        }
        append("\"");
    }
public:
    static constexpr size_t capacity=16384;
    void object() {
        if(depth_==first_.size())throw std::runtime_error("digest receipt nesting");
        append("{");first_[depth_++]=true;
    }
    void end_object() {if(!depth_)throw std::runtime_error("digest receipt object");--depth_;append("}");}
    void array(std::string_view name) {
        key(name);append("[");
        if(depth_==first_.size())throw std::runtime_error("digest receipt nesting");
        first_[depth_++]=true;
    }
    void array_object() {element();object();}
    void end_array() {if(!depth_)throw std::runtime_error("digest receipt array");--depth_;append("]");}
    void key(std::string_view name) {element();quote(name);append(":");}
    void text(std::string_view name,std::string_view value) {key(name);quote(value);}
    void boolean(std::string_view name,bool value) {key(name);append(value?"true":"false");}
    template<class T> void number(std::string_view name,T value) {
        key(name);std::array<char,64> number{};
        const auto result=std::to_chars(number.data(),number.data()+number.size(),value);
        if(result.ec!=std::errc{})throw std::runtime_error("digest receipt number");
        append({number.data(),static_cast<size_t>(result.ptr-number.data())});
    }
    void digest(std::string_view name,const ghost_digest::Digest& value) {
        std::array<char,64> hex{};
        constexpr std::string_view digits="0123456789abcdef";
        for(size_t i=0;i<value.size();++i){hex[i*2]=digits[value[i]>>4];hex[i*2+1]=digits[value[i]&15];}
        text(name,{hex.data(),hex.size()});
    }
    std::string_view view() const {
        if(depth_)throw std::runtime_error("unfinished digest receipt");
        return {bytes_.data(),size_};
    }
};
inline void emit_ghost_digest(std::ostream& output,std::string_view prefix,const GhostDigestRecord& record) {
    std::array<std::byte,65536> storage{};
    std::pmr::monotonic_buffer_resource arena(storage.data(),storage.size(),std::pmr::null_memory_resource());
    using Stream=std::basic_osyncstream<char,std::char_traits<char>,std::pmr::polymorphic_allocator<char>>;
    Stream stream(output,std::pmr::polymorphic_allocator<char>(&arena));
    stream<<prefix<<record.view()<<'\n';stream.emit();
    if(!stream.good())throw std::runtime_error("bounded digest receipt output");
}
inline constexpr size_t ghost_digest_serializer_capacity=sizeof(GhostDigestRecord)+65536+
    sizeof(std::pmr::monotonic_buffer_resource)+
    sizeof(std::basic_osyncstream<char,std::char_traits<char>,std::pmr::polymorphic_allocator<char>>);
}
