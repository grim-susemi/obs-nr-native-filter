// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_lifecycle.h"
#include <Windows.h>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace nr::qa {
enum class CoreCloseStep : size_t { Stop, Worker, Generation, Pipeline, Bridge, Resources, Count };
enum class CoreCloseOutcome { Complete, Pending, Refused };
struct CoreControlReceipt {
    uint64_t request=0,pendingRequest=0;
    size_t completedSteps=0;
    bool pending=false,closed=false,callbackAttached=false,exception=false;
    lifecycle::ModelClose model{};
};
class CoreControlBackend {
public:
    virtual ~CoreControlBackend()=default;
    virtual void register_callback(void*,void(*)(void*))=0;
    virtual void remove_callback()=0;
    virtual CoreCloseOutcome close_step(CoreCloseStep)=0;
    virtual lifecycle::ModelClose diagnostics() const=0;
};
// One actual Harness owner, one pending request, and its own completion event.
// External callback/device/close operations are the only controlled CPU dependencies.
class CoreControlOwner {
    struct State final : std::enable_shared_from_this<State> {
        std::unique_ptr<CoreControlBackend> backend;
        std::shared_ptr<State> callbackOwner;
        HANDLE completed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        mutable std::mutex mutex;
        CoreControlReceipt receipt;
        uint64_t latestRequest=0;
        bool dispatching=false,detaching=false;
        explicit State(std::unique_ptr<CoreControlBackend> value):backend(std::move(value)) {
            if(!backend || !completed){if(completed)CloseHandle(completed);completed=nullptr;throw std::runtime_error("CORE control owned completion event");}
        }
        ~State(){if(completed)CloseHandle(completed);}
        static void draw(void* opaque) {
            auto keep=static_cast<State*>(opaque)->shared_from_this();
            keep->dispatch();
        }
        void start() {
            callbackOwner=shared_from_this();
            receipt.callbackAttached=true;
            backend->register_callback(this,draw);
        }
        void dispatch() {
            uint64_t sequence=0;
            size_t stage=0;
            lifecycle::ModelClose model;
            {
                std::lock_guard lock(mutex);
                if(!receipt.pending || dispatching || detaching)return;
                dispatching=true;sequence=receipt.pendingRequest;
                stage=receipt.completedSteps;model=receipt.model;
            }
            bool closed=false,exception=false,pending=false;
            try {
                while(stage<static_cast<size_t>(CoreCloseStep::Count)) {
                    const auto outcome=backend->close_step(static_cast<CoreCloseStep>(stage));
                    if(outcome!=CoreCloseOutcome::Complete){pending=outcome==CoreCloseOutcome::Pending;break;}
                    ++stage;
                    model=backend->diagnostics();
                }
                model=backend->diagnostics();
                closed=stage==static_cast<size_t>(CoreCloseStep::Count);
            }catch(...){exception=true;}
            {
                std::lock_guard lock(mutex);
                if(!pending || exception)receipt.request=sequence;
                receipt.pending=pending && !exception;receipt.closed=closed;
                receipt.completedSteps=stage;receipt.model=model;receipt.exception=exception;
                dispatching=false;if(!receipt.pending)SetEvent(completed);
            }
        }
        bool request(uint64_t sequence) {
            std::lock_guard lock(mutex);
            if(!sequence || sequence<=latestRequest || receipt.pending || detaching)return false;
            latestRequest=sequence;receipt.pendingRequest=sequence;receipt.pending=true;
            if(receipt.closed){receipt.request=sequence;receipt.pending=false;SetEvent(completed);return true;}
            ResetEvent(completed);return true;
        }
        CoreControlReceipt snapshot() const {std::lock_guard lock(mutex);return receipt;}
        void refresh() {std::lock_guard lock(mutex);if(!dispatching)receipt.model=backend->diagnostics();}
        bool detach() {
            auto keep=shared_from_this();
            {
                std::lock_guard lock(mutex);
                if(!receipt.closed || receipt.pending || dispatching || detaching)return false;
                if(!receipt.callbackAttached)return true;
                detaching=true;
            }
            try{backend->remove_callback();}
            catch(...){std::lock_guard lock(mutex);detaching=false;throw;}
            std::lock_guard lock(mutex);
            receipt.callbackAttached=false;detaching=false;callbackOwner.reset();return true;
        }
    };
    std::shared_ptr<State> state_;
public:
    explicit CoreControlOwner(std::unique_ptr<CoreControlBackend> backend)
        :state_(std::make_shared<State>(std::move(backend))){}
    void attach(){state_->start();}
    bool request(uint64_t sequence){return state_->request(sequence);}
    bool wait(DWORD milliseconds) const {return WaitForSingleObject(state_->completed,milliseconds)==WAIT_OBJECT_0;}
    CoreControlReceipt snapshot() const {return state_->snapshot();}
    void refresh(){state_->refresh();}
    bool detach(){return state_->detach();}
};
}
