// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_model.h"
#include <graphics/graphics.h>
namespace nr::obs_adapter {
struct Capture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> original11, analysis11;
    Microsoft::WRL::ComPtr<ID3D12Resource> original12, analysis12;
};
class NativeOutput final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    NativeOutput(uint64_t,model::Config,std::vector<std::byte>,std::vector<std::byte>,std::vector<std::byte>,bool deterministicQa=false);
    ~NativeOutput();
    NativeOutput(const NativeOutput&)=delete;
    NativeOutput& operator=(const NativeOutput&)=delete;
    Selection tick(const IngressSample&,ID3D11Texture2D*,ID3D11Texture2D*);
    void request_stop();
    bool try_close();
    bool closed() const;
    std::shared_future<void> worker_exit() const;
    std::shared_ptr<model::Job> current_job() const;
    std::shared_ptr<model::Job> selected_job() const;
    std::shared_ptr<const Capture> selected_raw() const;
    std::shared_ptr<interop::Context> context() const;
    std::shared_ptr<interop::Bridge> transport() const;
    Epochs epochs() const;
};
}
