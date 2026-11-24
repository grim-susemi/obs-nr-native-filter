// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#include <cstdio>
#include <memory>
#include <string_view>

namespace nr::fg::vfg {
struct AnrEvaluation {
    ID3D12Resource *color, *motion, *depth, *output, *disable;
    uint64_t frameId;
    unsigned count, index;
    bool reset;
};

class AnrFeature {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit AnrFeature(FILE*);
    ~AnrFeature();
    unsigned open(ID3D12Device*, std::wstring_view path, bool optIn);
    bool create(ID3D12GraphicsCommandList*, unsigned width, unsigned height);
    bool evaluate(ID3D12GraphicsCommandList*, const AnrEvaluation&);
    [[nodiscard]] bool close();
};
} // namespace nr::fg::vfg
