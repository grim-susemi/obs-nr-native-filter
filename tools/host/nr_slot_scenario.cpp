// SPDX-License-Identifier: GPL-3.0-only
#include "nr_slot_scenario.h"
#include "interop.h"
#include <obs.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
namespace {
using namespace nr;
void require(bool v,const char* why) { if(!v) throw std::runtime_error(why); }
struct Event {
    HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Event() { require(value!=nullptr,"slot QA event"); }
    ~Event() { CloseHandle(value); }
    void wait() { require(WaitForSingleObject(value,15000)==WAIT_OBJECT_0,"slot QA exact event timeout"); }
};
std::vector<std::byte> bytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);require(file.good(),"slot shader");
    const auto size=file.tellg();require(size>0,"slot shader empty");
    std::vector<std::byte> result(static_cast<size_t>(size));file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),size);require(file.good(),"slot shader read");return result;
}
bool same(const interop::Ownership& a,const interop::Ownership& b) {
    // Fence completions can advance independently; compare all owned mutations.
    return a.key==b.key && a.phase==b.phase && a.producer==b.producer && a.worker==b.worker &&
        a.consumer==b.consumer && a.consumerOpen==b.consumerOpen && a.cancelled==b.cancelled && a.failed==b.failed;
}
struct Harness {
    Event acted,initialDiscarded,reusedReady,exited,producerDone,initialProducer;
    std::atomic<unsigned> action{0};
    std::atomic<bool> failed{false};
    std::shared_ptr<interop::Context> context;
    std::unique_ptr<interop::Bridge> bridge;
    gs_texture_t* texture=nullptr;
    std::vector<interop::Ticket> tickets;
    std::vector<std::byte> vertex,pixel;
    std::thread worker;
    explicit Harness(const std::filesystem::path& root):
        vertex(bytes(root/"bin/64bit/nr_interop_vertex.cso")),pixel(bytes(root/"bin/64bit/nr_interop_pixel.cso")) {}
    JobKey key(uint64_t serial) const { return {{121,1,serial},1,1,serial}; }
    void execute(unsigned next) { ResetEvent(acted.value);action.store(next);acted.wait();require(!failed,"slot graphics action"); }
    ID3D11Texture2D* borrowed() const { return static_cast<ID3D11Texture2D*>(gs_texture_get_obj(texture)); }
    void init() {
        texture=gs_texture_create(16,16,GS_RGBA,1,nullptr,0);require(texture!=nullptr,"slot OBS texture");
        Microsoft::WRL::ComPtr<ID3D11Device> renderer;borrowed()->GetDevice(&renderer);
        context=interop::Context::create(renderer.Get());
        const FrameDescriptor descriptor{{16,16},{0,0,16,16},PixelFormat::Rgba8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80};
        bridge=std::make_unique<interop::Bridge>(context,1,descriptor,vertex,pixel);
        require(SUCCEEDED(bridge->producer_fence()->SetEventOnCompletion(3,initialProducer.value)),"initial producer subscription");
        for(size_t i=0;i<3;++i) {
            const auto ticket=bridge->capture(key(i+1),borrowed());
            require(ticket && ticket->slot==i,"old first-reusable entry");tickets.push_back(*ticket);
        }
        const auto before=bridge->ownership(2);
        require(!bridge->capture_at(2,key(4),borrowed()),"busy targeted slot accepted");
        require(!bridge->capture_at(3,key(4),borrowed()),"out-of-bounds slot accepted");
        require(same(before,bridge->ownership(2)),"refusal mutated captured owner");
    }
    void verify() {
        require(bridge->quiescent(),"exact initial producer/discard completion");
        // All three are freshly reusable. The old scan would pick0; reservation2 must bind2.
        const auto fourth=bridge->capture_at(2,key(4),borrowed());
        require(fourth && fourth->slot==2 && bridge->ownership(2).producer==4,"target redirected or refusal advanced cursor");
        tickets.push_back(*fourth);
        const auto before=bridge->ownership(2);
        require(!bridge->capture_at(2,key(5),borrowed()),"busy target fell back to free0");
        require(same(before,bridge->ownership(2)),"busy target changed key/phase/fences");
        const auto fifth=bridge->capture(key(5),borrowed());
        require(fifth && fifth->slot==0 && bridge->ownership(0).producer==5,"old entry changed or rejected serial consumed");
        tickets.push_back(*fifth);
        const auto empty=bridge->ownership(1);
        require(!bridge->capture_at(SIZE_MAX,key(6),borrowed()),"invalid target accepted");
        require(!bridge->capture_at(1,key(6),nullptr),"invalid borrowed source accepted");
        require(same(empty,bridge->ownership(1)),"invalid target mutated another slot");
        const auto sixth=bridge->capture_at(1,key(6),borrowed());
        require(sixth && sixth->slot==1 && bridge->ownership(1).producer==6,"invalid refusal changed cursor/serial");
        tickets.push_back(*sixth);
        std::cout<<"NR_BOOTSTRAP_SLOT {\"targeted_slot\":2,\"lower_reusable_slot\":0,\"old_api_slot\":0,"
            "\"refusal_ownership_unchanged\":true,\"producer_cursor\":6,\"serial_not_consumed\":true}"<<std::endl;
    }
    void close() {
        require(!worker.joinable() && bridge->close(),"normal physical slot close");
        bridge.reset();context.reset();gs_texture_destroy(texture);texture=nullptr;
        std::cout<<"NR_BOOTSTRAP_SLOT_CLEANUP {\"closed\":true,\"worker_exit\":true,\"retained\":false}"<<std::endl;
    }
};
void draw(void* opaque,uint32_t,uint32_t) {
    auto& h=*static_cast<Harness*>(opaque);const auto action=h.action.exchange(0);if(!action)return;
    try { if(action==1)h.init();else if(action==2)h.verify();else h.close(); }
    catch(const std::exception& e) { h.failed=true;std::cout<<"NR_BOOTSTRAP_FAILURE slot "<<e.what()<<std::endl; }
    SetEvent(h.acted.value);
}
}
bool nr_run_slot_scenario(const std::filesystem::path& root,bool& retained) {
    auto h=std::make_unique<Harness>(root);bool callback=false;
    try {
        obs_add_main_render_callback(draw,h.get());callback=true;h->execute(1);
        require(SUCCEEDED(h->bridge->producer_fence()->SetEventOnCompletion(6,h->producerDone.value)),"slot producer subscription");
        h->worker=std::thread([&] {
            try {
                for(size_t i=0;i<3;++i)require(h->bridge->discard_recording(h->tickets[i]),"initial discard");
                SetEvent(h->initialDiscarded.value);
                h->reusedReady.wait();
                for(size_t i=3;i<h->tickets.size();++i)require(h->bridge->discard_recording(h->tickets[i]),"reused discard");
            } catch(const std::exception& e) { h->failed=true;std::cout<<"NR_BOOTSTRAP_FAILURE slot worker "<<e.what()<<std::endl; }
            SetEvent(h->exited.value);
        });
        h->initialDiscarded.wait();
        h->initialProducer.wait();h->execute(2);h->producerDone.wait();
        SetEvent(h->reusedReady.value);h->exited.wait();h->worker.join();require(!h->failed,"slot worker failed");
        h->execute(3);obs_remove_main_render_callback(draw,h.get());return true;
    } catch(const std::exception& e) {
        std::cout<<"NR_BOOTSTRAP_FAILURE slot host "<<e.what()<<std::endl;
        if(callback)obs_remove_main_render_callback(draw,h.get());
        if(h->worker.joinable() || (h->bridge && !h->bridge->quiescent())) {
            retained=true;(void)h.release();std::cout<<"NR_BOOTSTRAP_RETAINED slot=true"<<std::endl;
        } else { obs_enter_graphics();gs_texture_destroy(h->texture);h->texture=nullptr;obs_leave_graphics(); }
        return false;
    }
}
