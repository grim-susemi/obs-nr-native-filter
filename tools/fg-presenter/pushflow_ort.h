// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): Based on PushFPS (GPL-3.0-or-later), modified 2026-11-22 for the OBS presenter.
#pragma once
#include "pushflow_motion.h"
#include "pushflow_policy.h"
#include "pushflow_tensor_contract.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <dml_provider_factory.h>
#include <array>
#include <cstdio>
#include <functional>

namespace nr::fg::vfg::pushflow {
using Microsoft::WRL::ComPtr;
using veyra::pushflow::motion::Geometry;
using Beat = std::function<void()>;

struct Tensor {
    ComPtr<ID3D12Resource> buffer;
    void* allocation{};
    OrtValue* value{};
    UINT elements{};
};

class OrtState {
    const OrtApi* api_{};
    const OrtDmlApi* dml_{};
    OrtEnv* env_{};
    ComPtr<IDMLDevice> device_;
    OrtMemoryInfo* memory_{};
    OrtSession* session_{};
    OrtAllocator* pool_{};
    OrtIoBinding* binding_{};
    OrtRunOptions* runOptions_{};
    bool poisoned_{};
    FILE* log_;

    bool create_session(const fs::path&, ID3D12CommandQueue*, const std::array<std::pair<std::string, int64_t>, 4>&);
    bool release_session();
    bool validate_output(size_t index, size_t slot);
    bool status(const char*, OrtStatus*) noexcept;

    // Only this leaf function has SEH. Its parameters and locals must not
    // need C++ unwinding; callers own every smart pointer and string.
    template<class F, class... Args>
    static bool seh(DWORD& code, F function, Args... args) noexcept
    {
        __try { function(args...); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); return false; }
    }
    template<class F, class... Args>
    bool invoke(const char* name, F function, Args... args) noexcept
    {
        if (poisoned_) return false;
        DWORD code{};
        if (seh(code, function, args...)) return true;
        poisoned_ = true;
        std::fprintf(log_, "event=pushflow_native_exception operation=%s code=0x%08lX\n", name, code);
        std::fflush(log_);
        return false;
    }
    template<class F, class... Args>
    bool call(const char* name, F function, Args... args) noexcept
    {
        OrtStatus* error{};
        return invoke(name, [function, &error](Args... input) { error = function(input...); }, args...) &&
            status(name, error);
    }

public:
    explicit OrtState(FILE* log) : log_(log) {}
    OrtState(const OrtState&) = delete;
    OrtState& operator=(const OrtState&) = delete;
    bool load(const fs::path&, ID3D12Device*);
    bool open(const fs::path&, const ModelFacts&, const Geometry&, ID3D12CommandQueue*, const Beat&);
    bool bind(ID3D12Device*);
    bool run();
    bool sync();
    bool release();
    bool poisoned() const noexcept { return poisoned_; }
    std::array<Tensor, 5> tensors;
    std::array<std::string, 5> names;
    Shape inputShape{};
    size_t outputCount{};
    ShaderPath shader{ShaderPath::Unpack};
};
} // namespace nr::fg::vfg::pushflow
