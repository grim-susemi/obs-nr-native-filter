// SPDX-License-Identifier: GPL-3.0-only
// Private nr_model implementation. The registry contains SDK leases, never
// source settings, jobs, histories, queues, outputs, or GPU completion state.
enum class SessionState { Initializing, Ready, Retiring, Retired };
struct SessionKey {
    ComPtr<IUnknown> device;
    runtime::RuntimeIdentity core,model;
    std::filesystem::path data;
    runtime::Route route=runtime::Route::DriverFallback;
    uint64_t application=0x24480451ull;
    uint32_t coreApi=static_cast<uint32_t>(NVSDK_NGX_Version_API),modelApi=0x15;
    bool compatible(const SessionKey& other) const {
        return device.Get()==other.device.Get() && core.canonicalPath==other.core.canonicalPath &&
            core.sha256==other.core.sha256 && model.canonicalPath==other.model.canonicalPath &&
            model.sha256==other.model.sha256 && data==other.data && route==other.route &&
            application==other.application && coreApi==other.coreApi && modelApi==other.modelApi;
    }
};
struct SdkSession {
    lifecycle::VendorTrace trace; // Before Driver/Module, retained on every failure.
    const SessionKey key;
    const uint64_t id;
    std::atomic<SessionState> state{SessionState::Initializing};
    uint64_t owners=1; // Registry mutex only. Observational shared_ptrs are not owners.
    std::shared_ptr<Driver> driver=std::make_shared<Driver>();
    std::shared_ptr<runtime::Module> module;
    explicit SdkSession(SessionKey input,uint64_t identity):key(std::move(input)),id(identity) {
        require(trace.bind(id,0,0,reinterpret_cast<uintptr_t>(key.device.Get())),"SDK session trace identity");
        driver->trace=&trace;
    }
};
class SessionRegistry {
    std::mutex mutex_;
    std::vector<std::shared_ptr<SdkSession>> sessions_;
    uint64_t next_=0;
public:
    struct Acquisition {std::shared_ptr<SdkSession> session;bool initialize=false;};
    Acquisition acquire(const SessionKey& key) {
        std::lock_guard lock(mutex_);
        for(const auto& session:sessions_) {
            if(session->key.device.Get()==key.device.Get()) {
                if(!session->key.compatible(key) || session->state.load()!=SessionState::Ready)return {};
                require(session->owners && session->owners!=UINT64_MAX,"bounded SDK owner count");
                ++session->owners;return {session,false};
            }
            // The NR image's process-wide IAT cannot be owned by another device/session.
            if(session->key.model.canonicalPath==key.model.canonicalPath)return {};
        }
        require(next_!=UINT64_MAX,"bounded SDK session identity");
        auto session=std::make_shared<SdkSession>(key,++next_);
        sessions_.push_back(session);return {std::move(session),true};
    }
    void publish(const std::shared_ptr<SdkSession>& session) {
        std::lock_guard lock(mutex_);
        require(session->owners==1 && session->state.load()==SessionState::Initializing &&
            session->driver && session->driver->initialized && session->module,"complete SDK initialization publication");
        session->state.store(SessionState::Ready,std::memory_order_release);
    }
    void retain(const std::shared_ptr<SdkSession>& session) {
        std::lock_guard lock(mutex_);
        session->state.store(SessionState::Retiring,std::memory_order_release);
    }
    void elect(const std::shared_ptr<SdkSession>& session,lifecycle::ModelClose& proof) {
        std::lock_guard lock(mutex_);
        require(session->owners && session->state.load()!=SessionState::Retired,"live SDK detachment lease");
        proof.sessionId=session->id;proof.ownersAtDetachment=session->owners;
        if(session->owners>1) {
            require(session->state.load()==SessionState::Ready,"shared detachment from ready SDK session");
            --session->owners;proof.sdk=lifecycle::SdkDisposition::DetachedShared;
            return;
        }
        // This owner remains responsible until actual successful retirement.
        session->state.store(SessionState::Retiring,std::memory_order_release);
        proof.sdk=lifecycle::SdkDisposition::Retiring;
    }
    void retired(const std::shared_ptr<SdkSession>& session,lifecycle::ModelClose& proof) {
        std::lock_guard lock(mutex_);
        require(session->owners==1 && session->state.load()==SessionState::Retiring &&
            proof.moduleReferenceReleased && proof.iatRestored && proof.coreReferenceReleased,
            "exact last-owner SDK reference retirement");
        const auto entry=std::find(sessions_.begin(),sessions_.end(),session);
        require(entry!=sessions_.end(),"registered SDK retirement owner");
        session->owners=0;session->state.store(SessionState::Retired,std::memory_order_release);
        sessions_.erase(entry);proof.sdk=lifecycle::SdkDisposition::Retired;
    }
};
SessionRegistry& sessions() {
    // Retained failures outlive all source objects and module teardown attempts.
    // Do not run a static destructor that would unload an unresolved SDK.
    static auto* registry=new SessionRegistry;
    return *registry;
}
SessionKey session_key(ID3D12Device* device,const Config& config,const runtime::DriverBinding& binding) {
    SessionKey key;
    checked(device->QueryInterface(IID_PPV_ARGS(&key.device)),"retained canonical device IUnknown");
    require(binding.signatureVerified && binding.identity.canonicalPath==config.driver,"verified session CORE key");
    require(runtime::inspect(config.runtime).accepted,"verified session NR key");
    key.core=binding.identity;key.model=config.runtime;
    key.data=std::filesystem::canonical(config.data);
    require(std::filesystem::is_directory(key.data),"canonical SDK session data directory");
    return key;
}
bool close_session_module(SessionRegistry& registry,const std::shared_ptr<SdkSession>& session,
    lifecycle::ModelClose& proof) {
    registry.elect(session,proof); // No SDK calls or GPU waits under registry lock.
    proof.moduleRequired=static_cast<bool>(session->module);
    proof.modulePresent=static_cast<bool>(session->module);
    if(proof.sdk==lifecycle::SdkDisposition::DetachedShared)return true;
    if(session->module && !session->module->close())return false;
    proof.modulePresent=false;proof.moduleReferenceReleased=true;proof.iatRestored=true;
    return true;
}
bool close_session_core(SessionRegistry& registry,const std::shared_ptr<SdkSession>& session,
    lifecycle::ModelClose& proof) {
    auto& driver=*session->driver;
    if(!driver.initialized && !driver.shutdownCalled)return false; // Partial admission never fabricates normal CORE retirement.
    const bool closed=driver.close();
    proof.coreResult=driver.shutdownResult;proof.coreRemaining=driver.shutdownRemaining;
    proof.moduleError=driver.releaseError;proof.coreCalled=driver.shutdownCalled;
    if(!closed)return false;
    proof.coreReferenceReleased=true;
    registry.retired(session,proof);
    // An observation alias may still retain session.trace. It must not retain
    // the SDK resources or defer reference retirement.
    session->module.reset();session->driver.reset();
    return true;
}
