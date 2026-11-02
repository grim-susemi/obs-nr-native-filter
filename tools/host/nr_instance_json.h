// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ostream>
namespace nr::qa {
inline void write_instance_json_string(std::ostream& out,const char* value)
{
    constexpr char hex[]="0123456789abcdef";
    out.put('"');
    if(value)for(const auto* p=reinterpret_cast<const unsigned char*>(value);*p;++p) {
        if(*p=='"' || *p=='\\') { out.put('\\');out.put(static_cast<char>(*p)); }
        else if(*p<0x20) { out<<"\\u00"<<hex[*p>>4]<<hex[*p&15]; }
        else out.put(static_cast<char>(*p));
    }
    out.put('"');
}
}
