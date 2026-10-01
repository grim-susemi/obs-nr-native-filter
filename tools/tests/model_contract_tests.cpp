// SPDX-License-Identifier: GPL-3.0-only
#include "frame_pipeline.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
struct Owner final : nr::pipeline::Processor {
    const std::thread::id graphics=std::this_thread::get_id();
    std::atomic<unsigned> stops{0};
    std::atomic<bool> workerOwned{false};
    nr::pipeline::Processed process(const nr::pipeline::Work& work,std::stop_token) override {
        return {work.job,false,{}};
    }
    void worker_stopping() noexcept override {
        workerOwned.store(std::this_thread::get_id()!=graphics);
        stops.fetch_add(1);
    }
};
int main() try {
    auto owner=std::make_shared<Owner>();
    nr::pipeline::FramePipeline pipeline({111,1,1,1},owner);
    const auto exited=pipeline.worker_exit(); // Subscribe BEFORE stop.
    pipeline.request_stop();
    if(exited.wait_for(std::chrono::seconds(5))!=std::future_status::ready ||
        owner->stops.load()!=1 || !owner->workerOwned.load() || !pipeline.try_close())
        throw std::runtime_error("worker retirement did not precede exact exit");
    if(!pipeline.try_close() || owner->stops.load()!=1) throw std::runtime_error("retirement repeated after close");
    std::puts("PASS production scheduler worker retirement before subscribed exit; CPU only, no model/GPU claim");
    return 0;
} catch(const std::exception& error) { std::puts(error.what()); return 1; }
