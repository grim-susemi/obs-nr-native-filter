// SPDX-License-Identifier: GPL-3.0-only
// UG03 P01: completed() publishes ready before its last write through the raw
// Observation*, and its wait is only drained by the worker (settle ->
// UnregisterWaitEx) before the worker exits. The owning ObservationSink must
// therefore stay alive until the actual close has passed the worker exit.
#include "obs_native_output.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
namespace {
unsigned checks=0;
void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct DrainWitness {
    std::shared_future<void> exited;
    bool destroyed=false,exitedAtRelease=false;
};
struct Sink final:nr::model::ObservationSink {
    DrainWitness& witness;
    explicit Sink(DrainWitness& value):witness(value){}
    ~Sink() override {
        witness.destroyed=true;
        witness.exitedAtRelease=witness.exited.valid() &&
            witness.exited.wait_for(std::chrono::seconds(0))==std::future_status::ready;
    }
    nr::model::Observation* reserve(const nr::IngressSample&,const nr::pipeline::TickPlan&) noexcept override {return nullptr;}
    void capture(nr::model::Observation*,ID3D11Texture2D*,ID3D11Texture2D*,ID3D11Texture2D*) noexcept override {}
};
}
int main() try {
    DrainWitness witness;
    nr::obs_adapter::NativeOutput output(83,nr::model::Config{},{},{},{},true);
    output.observe_with(std::make_shared<Sink>(witness)); // The output is the sink's only owner.
    expect(!output.on_graphics_owner(),"unbound output accepted an off-tick render as its graphics owner");
    const nr::Settings settings{{1},true,"C:/qa/not-loaded-runtime.dll",{1},{0,0,1,1,0,-1,true}};
    const nr::FrameDescriptor frame{{64,64},{0,0,64,64},nr::PixelFormat::Bgra8,nr::ColorSpace::Rec709Srgb,
        nr::AlphaMode::Straight,80};
    // An invalid source binds the actual graphics owner and scheduler worker, no physical generation.
    const auto decision=output.prepare({1,1,1,nr::SourceValidity::Invalid,frame,settings},nullptr);
    expect(decision.placeholder.has_value(),"invalid observation stayed a placeholder");
    expect(output.on_graphics_owner(),"bound graphics thread was not recognised");
    bool foreign=true;
    std::thread([&]{foreign=output.on_graphics_owner();}).join();
    expect(!foreign,"foreign thread was recognised as the graphics owner");
    witness.exited=output.worker_exit(); // Subscribe BEFORE stop.
    expect(witness.exited.valid() && !witness.destroyed,"actual worker pipeline and sink owner");
    output.request_stop();
    bool closed=false;
    for(unsigned attempt=0;attempt<500 && !closed;++attempt) {
        closed=output.try_close();
        if(!closed) {
            expect(!witness.destroyed,"observation sink released by a refused close before the completion drain");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    expect(closed && output.closed(),"actual close completed");
    expect(witness.destroyed,"closed output released its observation sink");
    expect(witness.exitedAtRelease,"observation sink released before the worker (completion drain) exit");
    std::printf("PASS native_output_sink_drain checks=%u sink_after_worker_exit=true CPU_only=true\n",checks);
    return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
