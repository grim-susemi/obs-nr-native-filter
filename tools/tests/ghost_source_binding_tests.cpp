// SPDX-License-Identifier: GPL-3.0-only
// UG07 I7: the host writes nr::ghost::compiled_host_source_sha256 into every Task26 receipt as
// host_source_sha256, and the Task26 public validator binds it to the sealed source row
// (GhostDigestSupport.ps1 SourceHash). The constant must therefore be the SHA-256 of the exact
// nr_ghost_scenario.cpp bytes nr-qa-host compiles; CMake passes that path from the target's SOURCES.
#include "ghost_freeze.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace {
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::string sha256_hex(const std::vector<unsigned char>& bytes)
{
    unsigned char digest[32]{};
    expect(BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()),digest,sizeof(digest))),"CNG SHA-256 of the compiled host source");
    static constexpr char hex[]="0123456789abcdef";
    std::string out;
    for(const auto b:digest){out.push_back(hex[b>>4]);out.push_back(hex[b&15]);}
    return out;
}
}
int main(int argc,char** argv) try {
    expect(argc==2,"exact compiled host source path");
    std::ifstream file(argv[1],std::ios::binary);
    expect(static_cast<bool>(file),"compiled host source readable");
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    expect(!bytes.empty(),"compiled host source non-empty");
    const std::string actual=sha256_hex(bytes);
    const std::string_view embedded{nr::ghost::compiled_host_source_sha256};
    expect(embedded.size()==64 && embedded.find_first_not_of("0123456789abcdef")==std::string_view::npos,
        "embedded constant is a lowercase sha256");
    if(embedded!=actual) {
        std::fprintf(stderr,"FAIL embedded compiled_host_source_sha256=%.*s differs from compiled nr_ghost_scenario.cpp=%s\n",
            static_cast<int>(embedded.size()),embedded.data(),actual.c_str());
        return 1;
    }
    std::printf("PASS ghost_host_source_binding checks=%u sha256=%s CPU_only=true\n",checks,actual.c_str());
    return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
