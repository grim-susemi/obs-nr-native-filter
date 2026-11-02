// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <cstdint>
#include <vector>
#include "nr_contract.h"
#include <atomic>
#include <array>
namespace nr::qa {
class GhostCredits {
public:
    struct Token {
        size_t index;
        uint64_t serial;
        FrameKey frame;
        uint64_t generation;
        bool operator==(const Token&) const=default;
    };
    struct Receipt {
        uint64_t producerRequired,producerDone,copyRequired,copyDone;
        bool dispatchReturned,sourceDetached,mapsClosed,cpuFinished;
    };
private:
    struct Credit {
        std::atomic<bool> occupied{false};
        std::optional<Token> identity;
    };
    std::array<Credit,3> credits_;
    uint64_t serial_=0;
public:
    std::optional<Token> reserve(const FrameKey& frame,uint64_t generation) noexcept {
        if(!frame.valid()||!generation||serial_==UINT64_MAX)return {};
        for(size_t i=0;i<credits_.size();++i) {
            bool empty=false;
            if(!credits_[i].occupied.compare_exchange_strong(empty,true,std::memory_order_acquire))continue;
            const Token token{i,++serial_,frame,generation};
            credits_[i].identity.emplace(token);return token;
        }
        return {};
    }
    bool release(const Token& token,const Receipt& receipt) noexcept {
        if(token.index>=credits_.size())return false;
        auto& credit=credits_[token.index];
        if(!credit.occupied.load(std::memory_order_acquire)||!credit.identity||*credit.identity!=token||
            receipt.producerDone==UINT64_MAX||receipt.copyDone==UINT64_MAX||
            receipt.producerDone<receipt.producerRequired||receipt.copyDone<receipt.copyRequired||
            !receipt.dispatchReturned||!receipt.sourceDetached||!receipt.mapsClosed||!receipt.cpuFinished)return false;
        credit.occupied.store(false,std::memory_order_release);return true;
    }
    unsigned occupied() const noexcept {
        unsigned count=0;
        for(const auto& credit:credits_)if(credit.occupied.load(std::memory_order_acquire))++count;
        return count;
    }
};
}
std::vector<uint8_t> nr_ghost_fixture_bytes(uint32_t seed,uint32_t color,uint32_t tick,bool linearSource);
bool nr_ghost_storage_metadata();
bool nr_ghost_complete_service(const std::filesystem::path&,const std::filesystem::path&);
bool nr_run_ghost_scenario(const std::filesystem::path&,const std::filesystem::path&,bool& retained);
bool nr_run_ghost_baseline(const std::filesystem::path&,const std::filesystem::path&,bool& retained);
