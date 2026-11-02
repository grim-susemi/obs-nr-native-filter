// SPDX-License-Identifier: GPL-3.0-only
// Uses the accepted source/filter, native observer and original close custody.
bool run_faults(const std::filesystem::path& root,bool& retained,const std::filesystem::path& runtime)
{
    require(runtime.is_absolute(),"Task17 canonical admitted runtime required");
    // Existing real model controls are reused, not simulated by another model. The lane's
    // unresolved-ownership signal folds into the caller's retained flag exactly like the
    // recording registry: a null retained HANDLE never authorises obs_shutdown.
    if(!nr_run_model_scenario(root,retained))return false;
    obs_source_info info{};info.id="nr_task17_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
    info.get_name=fixture_name;info.create=fixture_create;info.destroy=fixture_destroy;
    info.get_width=fixture_extent;info.get_height=fixture_height;info.video_render=fixture_render;
    info.video_get_color_space=fixture_space;obs_register_source(&info);
    const nr::model::Fault cases[]{nr::model::Fault::ProducerTimeout,
        nr::model::Fault::ProducerRemoved,nr::model::Fault::CompletionSignal};
    uint64_t priorInstance=0;
    for(const auto fault:cases) {
        auto* s=new PropertyScenario;retainedProperty=s;retained=true;s->faultMode=true;
        bool graphics=false;
        try {
            require(s->trigger && s->rendered && s->closed && s->markerTrigger && s->markerActed,"Task17 original subscriptions");
            auto* settings=obs_data_create();obs_data_set_bool(settings,"nr_lifecycle_fixture",true);
            s->scope.source=obs_source_create_private(info.id,"Task17 source",settings);obs_data_release(settings);
            s->original=fixture;settings=obs_data_create();
            obs_data_set_string(settings,"nr_shader_root",(root/"bin/64bit").string().c_str());
            obs_data_set_string(settings,"nr_data_root",(root/"profile/ngx-data").string().c_str());
            obs_data_set_string(settings,"nr_runtime_path",runtime.generic_string().c_str());
            obs_data_set_bool(settings,"nr_enabled",true);obs_data_set_bool(settings,"nr_qa_observe",true);
            s->scope.filter=obs_source_create_private("susemi_nr_filter","Task17 NR",settings);
            obs_data_release(settings);require(s->scope.source && s->scope.filter && s->original,"Task17 native source/filter");
            obs_source_filter_add(s->scope.source,s->scope.filter);
            calldata_t data;
            const auto subscribe=[&] {
            obs_enter_graphics();graphics=true;
            std::array<std::shared_ptr<nr::model::QaSignal>,3> signals;
            calldata_init(&data);calldata_set_ptr(&data,"retain_dispatch_signals",&signals);
            calldata_set_ptr(&data,"retain_exit_signal",&s->exited);calldata_set_ptr(&data,"retain_context",&s->context);
            require(proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_snapshot",&data),
                "Task17 pre-trigger original subscriptions");calldata_free(&data);
            require(!s->context || s->exited,"Task17 optional initial original context/exit");
            s->returns=std::make_shared<WarmupReturns>(std::move(signals));s->returns->fault=fault;
            std::shared_ptr<nr::model::ObservationSink> sink=s->returns;
            calldata_init(&data);calldata_set_ptr(&data,"observer",&sink);
            require(proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_observe",&data),
                "Task17 exact-key observer");calldata_free(&data);
            s->scope.views[0]=obs_view_create();require(s->scope.views[0]!=nullptr,"Task17 native view");
            obs_view_set_source(s->scope.views[0],0,s->scope.source);
            obs_add_main_render_callback(property_draw,s);s->callbackAdded=true;
            obs_leave_graphics();graphics=false;
            return true;
            };
            const auto render=[&](const char* stage) {
                obs_enter_graphics();s->stage=stage;s->content=s->content%6+1;
                ResetEvent(s->rendered);SetEvent(s->trigger);obs_leave_graphics();
                require(WaitForSingleObject(s->rendered,15000)==WAIT_OBJECT_0 && !s->failed,"Task17 exact render event");
            };
            const auto validate_context=[&] {
                obs_enter_graphics();graphics=true;
                std::shared_ptr<nr::interop::Context> context;
                std::shared_ptr<nr::model::QaSignal> exited;
                calldata_t snapshot;calldata_init(&snapshot);calldata_set_ptr(&snapshot,"retain_context",&context);
                calldata_set_ptr(&snapshot,"retain_exit_signal",&exited);
                const bool sampled=proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_snapshot",&snapshot);
                calldata_free(&snapshot);
                const bool exact=sampled && context && exited && exited->owner_module() &&
                    (!s->context || s->context==context) && (!s->exited || s->exited==exited);
                if(exact) {s->context=std::move(context);s->exited=std::move(exited);}
                obs_leave_graphics();graphics=false;
                return exact;
            };
            require(nr::model::initialize_fault_fixture(subscribe,[&] {
                if(fault==nr::model::Fault::CompletionSignal)s->returns->fault=nr::model::Fault::None;
                render(fault==nr::model::Fault::CompletionSignal?"original-marker-warmup":"fault-dispatched");
            },validate_context),"Task17 explicit initialization and original context/exit identity");
            if(fault==nr::model::Fault::CompletionSignal) {
                require(s->job && s->returns->returned && s->returns->returned->wait_for(30000) &&
                    WaitForSingleObject(s->job->observed,30000)==WAIT_OBJECT_0 &&
                    s->job->created && s->job->submitted && s->job->qaFailure.load()==0,
                    "Task17 genuine prior Feature18 creation before guide-bearing omission");
                s->job.reset();s->returns->fault=fault;
                render("fault-dispatched");
            }
            auto job=s->job;
            require(job && s->returns->key==job->contract.key && s->returns->returned &&
                s->returns->returned->wait_for(30000),"Task17 exact job worker return");
            const auto code=job->qaFailure.load(std::memory_order_acquire);
            const auto expected=fault==nr::model::Fault::ProducerRemoved?0xe004u:
                fault==nr::model::Fault::ProducerTimeout?0xe005u:0xe01au;
            require(code==expected,"Task17 documented adapter return");
            const auto before=job->contract.key.historyEpoch;
            // Natural video_tick latches failure; its public snapshot is observed
            // by the next exact render event, not by a sleep or polling loop.
            render("fault-latched");
            require(s->state=="error" && s->kind!=nr::OutputKind::DelayedNr &&
                s->outputHash==s->currentHash && s->outputContent==s->content &&
                s->instance && s->instance!=priorInstance,
                "Task17 truthful failure/source fallback and fresh instance");
            obs_enter_graphics();graphics=true;
            calldata_init(&data);
            require(proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_snapshot",&data),
                "Task17 post-fault epoch");
            const auto after=static_cast<uint64_t>(calldata_int(&data,"history_epoch"));
            calldata_free(&data);obs_leave_graphics();graphics=false;
            require(after>before,"Task17 coupled failure history reset");
            const auto captures=s->counters.captures,dispatches=s->counters.dispatches;
            property_settings(*s,true,.5,runtime.generic_string());render("fault-sticky");
            require(s->state=="error" && s->counters.captures==captures && s->counters.dispatches==dispatches,
                "Task17 settings erased failure latch or dispatched");
            const bool terminal=fault==nr::model::Fault::CompletionSignal;
            const auto originalProducer=job->frame->producer;
            const auto actualProducer=originalProducer.fence->GetCompletedValue();
            std::osyncstream(std::cout)<<"NR_FAULT {\"fault\":"<<static_cast<unsigned>(fault)
                <<",\"instance\":"<<s->instance<<",\"history_before\":"<<before<<",\"history_after\":"<<after
                <<",\"code\":"<<code<<",\"publication_denied\":true,\"error_latched\":true"
                <<",\"physical_producer_required\":"<<originalProducer.value<<",\"physical_producer_done\":"<<actualProducer
                <<",\"injected_not_physical\":true,\"terminal\":"<<(terminal?"true":"false")<<"}\n"<<std::flush;
            priorInstance=s->instance;
            property_request_stop();
            if(terminal) {
                obs_enter_graphics();graphics=true;property_close_on_graphics(*s);
                require(!property_closed() && !s->sourceClosed && !job->ready.load(std::memory_order_acquire),
                    "Task17 sticky failed Signal incorrectly closed");
                obs_leave_graphics();graphics=false;
                const auto original=job->completion();
                const auto& key=job->contract.key;
                require(original.fence && original.value==1 &&
                    original.fence->GetCompletedValue()==0 &&
                    job->flow && job->evaluated && job->composed && job->submitted &&
                    job->originalMarker.state()==nr::model::OriginalMarker::State::Held &&
                    job->originalMarker.calls()==0 && !job->callbackArrived.load(),
                    "Task17 exact original withheld marker");
                const auto originalQueue=reinterpret_cast<uintptr_t>(s->context->queue());
                const auto originalContext=reinterpret_cast<uintptr_t>(s->context.get());
                const auto originalFence=reinterpret_cast<uintptr_t>(original.fence);
                std::osyncstream(std::cout)<<"NR_FAULT_TERMINAL {\"code\":"<<code
                    <<",\"retained\":true,\"normal_closed\":false,\"completion_fabricated\":false"
                    <<",\"contract\":\"missing_original_completion_marker\",\"instance\":"<<s->instance
                    <<",\"content_epoch\":"<<key.frame.contentEpoch<<",\"ingress\":"<<key.frame.ingressSequence
                    <<",\"history\":"<<key.historyEpoch<<",\"generation\":"<<key.resourceGeneration
                    <<",\"submission\":"<<key.submissionSerial<<",\"context\":"<<originalContext
                    <<",\"queue\":"<<originalQueue<<",\"fence\":"<<originalFence
                    <<",\"target\":1,\"signal_calls\":0,\"actual\":0,\"callback_arrived\":false,\"ready\":false"
                    <<",\"guide_present\":true}\n"<<std::flush;
                const auto release=[&](const nr::JobKey& key) {
                    // obs_enter_graphics alone does not change thread identity.
                    // The existing render callback forwards on the true graphics
                    // owner, even while stopping refuses the original unready job.
                    obs_enter_graphics();s->markerKey.emplace(key);s->markerAccepted=false;
                    ResetEvent(s->markerActed);SetEvent(s->markerTrigger);obs_leave_graphics();
                    require(WaitForSingleObject(s->markerActed,30000)==WAIT_OBJECT_0 && !s->failed,
                        "Task17 exact graphics marker request response");
                    return s->markerAccepted;
                };
                // Exact original job event was subscribed at creation, before
                // omission. Request is consumed on the live generation worker.
                const nr::JobKey wrong{key.frame,key.historyEpoch,key.resourceGeneration,key.submissionSerial+1};
                require(!release(wrong) && release(key) && !release(key),"Task17 exact once original marker request");
                require(WaitForSingleObject(job->observed,30000)==WAIT_OBJECT_0,"Task17 original callback observed");
                require(job->ready.load(std::memory_order_acquire) && job->callbackArrived.load() &&
                    job->completionClaimed.load() && !job->eligible && !job->published.load() &&
                    job->qaFailure.load()==code &&
                    job->originalMarker.state()==nr::model::OriginalMarker::State::Submitted &&
                    job->originalMarker.calls()==1 && job->originalMarker.thread()!=GetCurrentThreadId() &&
                    SUCCEEDED(job->originalMarker.result()) &&
                    job->completion().fence==original.fence &&
                    nr::interop::completion(1,original.fence->GetCompletedValue())==nr::interop::Completion::Complete,
                    "Task17 genuine original completion grants retirement only");
                obs_enter_graphics();graphics=true;
                calldata_init(&data);
                require(proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_snapshot",&data),
                    "Task17 released sticky filter observation");
                const bool sticky=calldata_bool(&data,"failed") &&
                    std::string(calldata_string(&data,"public_state"))=="error" &&
                    static_cast<nr::OutputKind>(calldata_int(&data,"kind"))!=nr::OutputKind::DelayedNr;
                calldata_free(&data);obs_leave_graphics();graphics=false;
                require(sticky,"Task17 release erased filter error/publication refusal");
                std::osyncstream(std::cout)<<"NR_FAULT_RELEASE {\"instance\":"<<s->instance
                    <<",\"content_epoch\":"<<key.frame.contentEpoch<<",\"ingress\":"<<key.frame.ingressSequence
                    <<",\"history\":"<<key.historyEpoch<<",\"generation\":"<<key.resourceGeneration
                    <<",\"submission\":"<<key.submissionSerial<<",\"context\":"<<originalContext
                    <<",\"queue\":"<<originalQueue<<",\"fence\":"<<originalFence<<",\"target\":1"
                    <<",\"actual\":"<<original.fence->GetCompletedValue()<<",\"first_actual_signal\":true"
                    <<",\"signal_calls\":"<<job->originalMarker.calls()<<",\"worker_thread\":"<<job->originalMarker.thread()
                    <<",\"coordinator_thread\":"<<GetCurrentThreadId()
                    <<",\"callback_arrived\":true,\"ready\":true,\"eligible\":false,\"published\":false"
                    <<",\"guide_present\":true,\"original_job_code\":"<<job->code
                    <<",\"qa_failure\":"<<code<<",\"error_latched\":true,\"duplicate_rejected\":true"
                    <<",\"release_hresult\":"<<static_cast<uint32_t>(job->originalMarker.result())<<"}\n"<<std::flush;
            }
            job.reset();s->job.reset(); // No external input/model allocation lease during SDK retirement.
            require(WaitForSingleObject(s->closed,30000)==WAIT_OBJECT_0 && property_closed(),
                "Task17 genuine original completion cleanup");
            require(property_release(),"Task17 removed exact source and SDK custody");
            std::osyncstream(std::cout)<<"NR_FAULT_CLEANUP {\"fault\":"<<static_cast<unsigned>(fault)
                <<",\"instance\":"<<priorInstance<<",\"closed\":true,\"worker_exit\":true,\"retained\":false}\n"<<std::flush;
            retained=false;
        } catch(const std::exception& error) {
            if(graphics)obs_leave_graphics();
            std::osyncstream(std::cout)<<"NR_FAULT_FAILURE "<<error.what()<<"\nNR_FAULT_RETAINED originals=true\n"<<std::flush;
            return false;
        }
    }
    return true; // Every original refusal and genuine completion/cleanup was checked.
}
bool run_shutdown(const std::filesystem::path& root,bool& retained)
{
    // Accepted finite capacity fixture holds original native consumers, rejects
    // release/budget exhaustion, then ends each exact original consumer after
    // subscribing its own marker event. No second transport or synthetic fence.
    if(!run_capacity(root,retained))return false;
    if(!run_lifecycle(root,retained,true))return false;
    std::osyncstream(std::cout)<<"NR_SHUTDOWN {\"capacity_originals_closed\":true"
        <<",\"lifecycle_native_closed\":true,\"retained\":false}\n"<<std::flush;
    return true;
}
