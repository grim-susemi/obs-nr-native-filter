// SPDX-License-Identifier: GPL-2.0-or-later
// Included inside the accepted host's anonymous namespace, after run_capacity.
// Uses the original fixture, renderer, WarmupReturns and production proc APIs.
std::string property_compact(const char* text)
{
    std::string result;
    bool quoted=false,escaped=false;
    for(const char* p=text;*p;++p) {
        const char c=*p;
        if(quoted || (c!=' ' && c!='\r' && c!='\n' && c!='\t'))result+=c;
        if(escaped)escaped=false;
        else if(quoted && c=='\\')escaped=true;
        else if(c=='"')quoted=!quoted;
    }
    return result;
}
struct PropertyScenario {
    Sampler scope;
    HANDLE trigger=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE rendered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE closed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE capacity=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE markerTrigger=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE markerActed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::optional<nr::JobKey> markerKey;
    bool markerAccepted=false;
    Fixture* original=nullptr;
    std::shared_ptr<WarmupReturns> returns;
    std::shared_ptr<nr::model::Job> job;
    std::shared_ptr<nr::model::QaSignal> exited;
    std::shared_ptr<nr::interop::Context> context;
    bool nativeOriginal=false,sourceClosed=false,sourcesReleased=false;
    nr::lifecycle::CloseSnapshot closure{};
    nr::pipeline::Counters counters{};
    const char* stage="created";
    bool stopping=false,failed=false,callbackAdded=false;
    uint64_t instance=0,sequence=0;
    unsigned content=1;
    uint32_t outputContent=0,rawContent=0;
    bool restore=false;
    bool faultMode=false;
    bool ordinary=false;
    uint64_t outputHash=0,currentHash=0;
    uint64_t selectedTick=0,selectedRevision=0;
    std::string state,reason;
    nr::OutputKind kind=nr::OutputKind::CurrentSource;
    ~PropertyScenario() {
        if(trigger)CloseHandle(trigger);
        if(rendered)CloseHandle(rendered);
        if(closed)CloseHandle(closed);
        if(capacity)CloseHandle(capacity);
        if(markerTrigger)CloseHandle(markerTrigger);
        if(markerActed)CloseHandle(markerActed);
    }
};
PropertyScenario* retainedProperty=nullptr;
void property_draw(void*,uint32_t,uint32_t);
bool property_retained() { return retainedProperty!=nullptr; }
bool property_closed()
{
    if(!retainedProperty)return true;
    const auto& s=*retainedProperty;
    return s.stopping && s.sourceClosed && (!s.nativeOriginal ||
        (s.exited && s.exited->wait_for(0) && s.closure.instance==s.instance &&
            s.closure.workerExited && nr::lifecycle::close_complete(s.closure)));
}
void property_request_stop()
{
    if(!retainedProperty)return;
    auto& s=*retainedProperty;
    obs_enter_graphics();
    try {
        if(!s.stopping) {
            if(s.scope.filter) {
                calldata_t data;calldata_init(&data);
                const bool capacityUnsubscribed=proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_qa_capacity_watch",&data);
                calldata_free(&data);require(capacityUnsubscribed,"property capacity unsubscribe before closure");
                calldata_init(&data);
                const bool sampled=proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_sampler_snapshot",&data);
                const auto instance=static_cast<uint64_t>(calldata_int(&data,"instance"));calldata_free(&data);
                require(sampled && instance && (!s.instance || s.instance==instance),"property pre-stop original identity");
                s.instance=instance;
                std::shared_ptr<nr::interop::Context> context;
                std::shared_ptr<nr::model::QaSignal> exited;
                calldata_init(&data);calldata_set_ptr(&data,"retain_context",&context);
                calldata_set_ptr(&data,"retain_exit_signal",&exited);
                const bool snapshot=proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_snapshot",&data);
                calldata_free(&data);
                require(snapshot && (!s.context || s.context==context) && (!s.exited || s.exited==exited) &&
                    (!context || exited),"property pre-stop exact original context and worker subscription");
                s.context=std::move(context);s.exited=std::move(exited);
                s.nativeOriginal=static_cast<bool>(s.context) || static_cast<bool>(s.exited);
                // NativeOutput and Jobs keep non-owning Observation pointers. Unsubscribe
                // future reservations now, but keep returns/job storage through real exit.
                std::shared_ptr<nr::model::ObservationSink> observer;
                calldata_init(&data);calldata_set_ptr(&data,"observer",&observer);
                const bool unsubscribed=proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_observe",&data);
                calldata_free(&data);require(unsubscribed,"property original observer unsubscribe");
            } else s.sourceClosed=true; // No source procedure or worker was created.
            s.stopping=true;
            if(s.scope.filter && !s.callbackAdded) {
                obs_add_main_render_callback(property_draw,&s);s.callbackAdded=true;
            }
            if(property_closed() && s.closed)SetEvent(s.closed);
        }
    } catch(...) {obs_leave_graphics();throw;}
    obs_leave_graphics();
}
void property_close_on_graphics(PropertyScenario& s)
{
    if(s.sourceClosed)return;
    calldata_t data;calldata_init(&data);
    const bool called=proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_stop",&data);
    const bool closed=calldata_bool(&data,"closed");calldata_free(&data);
    require(called,"property original stop procedure");
    if(!closed)return;
    if(s.nativeOriginal) {
        calldata_init(&data);calldata_set_ptr(&data,"snapshot",&s.closure);
        const bool sampled=proc_handler_call(obs_get_proc_handler(),"nr_retirement_snapshot",&data);
        const bool normal=calldata_bool(&data,"normal_closed");calldata_free(&data);
        require(sampled,"property original closure snapshot");
        if(!normal || s.closure.instance!=s.instance || !s.closure.workerExited ||
            !nr::lifecycle::close_complete(s.closure) || !s.exited || !s.exited->wait_for(0))return;
    }
    s.sourceClosed=true;
    if(property_closed() && s.closed)SetEvent(s.closed);
}
bool property_release()
{
    if(!retainedProperty)return true;
    auto* s=retainedProperty;
    obs_enter_graphics();
    if(!property_closed()) {obs_leave_graphics();return false;}
    if(!s->sourcesReleased) {
        if(s->callbackAdded)obs_remove_main_render_callback(property_draw,s);
        s->callbackAdded=false;
        for(auto*& view:s->scope.views)if(view) {obs_view_destroy(view);view=nullptr;}
        if(s->scope.source && s->scope.filter)obs_source_filter_remove(s->scope.source,s->scope.filter);
        obs_source_release(s->scope.filter);s->scope.filter=nullptr;
        obs_source_release(s->scope.source);s->scope.source=nullptr;
        s->sourcesReleased=true;
    }
    obs_leave_graphics();
    // libobs destruction queue may take graphics. Never wait while holding it.
    // OBS 32.2.2: false also means no audio/video output; otherwise the
    // return reports tasks processed, not success. Closure is proved below.
    const bool destructionQueueTasksProcessed=obs_wait_for_destroy_queue();
    std::osyncstream(std::cout)<<"NR_PROPERTY_CLOSE_CHECK {\"phase\":\"destruction-queue\",\"instance\":"<<s->instance
        <<",\"tasks_processed_or_unavailable\":"<<(destructionQueueTasksProcessed?"true":"false")<<"}\n"<<std::flush;
    obs_enter_graphics();
    if(s->nativeOriginal) {
        nr::lifecycle::CloseSnapshot after{};
        calldata_t data;calldata_init(&data);calldata_set_ptr(&data,"snapshot",&after);
        const bool sampled=proc_handler_call(obs_get_proc_handler(),"nr_retirement_snapshot",&data);
        const bool normal=calldata_bool(&data,"normal_closed");calldata_free(&data);
        const bool workerSignal=s->exited && s->exited->wait_for(0);
        const bool complete=nr::lifecycle::close_complete(after);
        lifecycle_write_closure(after,"property-post-destruction-observed",0);
        std::osyncstream(std::cout)<<"NR_PROPERTY_CLOSE_CHECK {\"phase\":\"post-source-destruction\",\"expected_instance\":"<<s->instance
            <<",\"snapshot_instance\":"<<after.instance<<",\"sampled\":"<<(sampled?"true":"false")
            <<",\"normal_closed\":"<<(normal?"true":"false")<<",\"source_detached\":"<<(after.sourceDetached?"true":"false")
            <<",\"worker_exited\":"<<(after.workerExited?"true":"false")<<",\"close_complete\":"<<(complete?"true":"false")
            <<",\"original_worker_signal\":"<<(workerSignal?"true":"false")<<"}\n"<<std::flush;
        if(!sampled || !normal || after.instance!=s->instance || !after.sourceDetached ||
            !after.workerExited || !nr::lifecycle::close_complete(after) || !s->exited || !s->exited->wait_for(0)) {
            obs_leave_graphics();return false;
        }
        s->closure=after;
        lifecycle_write_closure(after,"post-source-destruction",0);
    }
    retainedProperty=nullptr;
    delete s; // Original observer, job, context and handles outlive closure and detachment.
    obs_leave_graphics();return true;
}

void property_settings(PropertyScenario& s,bool enabled,double strength,const std::string& path)
{
    auto* settings=obs_source_get_settings(s.scope.filter);
    obs_data_set_bool(settings,"nr_enabled",enabled);
    obs_data_set_double(settings,"nr_strength",strength);
    obs_data_set_string(settings,"nr_runtime_path",path.c_str());
    obs_source_update(s.scope.filter,settings);
    obs_data_release(settings);
}
void property_record(PropertyScenario& s,const char* stage,uint64_t tick,uint64_t outputHash,
    uint64_t currentHash,uint64_t rawHash,const nr::JobKey* selected,calldata_t* sample,calldata_t* native)
{
    auto* settings=obs_source_get_settings(s.scope.filter);
    auto* owner=obs_source_get_settings(s.scope.source);
    std::ostringstream row;
    row<<"NR_PROPERTY {\"run\":\""<<(s.restore?"restore":"save")<<"\",\"stage\":\""<<stage
        <<"\",\"sequence\":"<<++s.sequence<<",\"pid\":"<<GetCurrentProcessId()<<",\"source_uuid\":";
    nr::qa::write_instance_json_string(row,obs_source_get_uuid(s.scope.source));
    row<<",\"filter_uuid\":";nr::qa::write_instance_json_string(row,obs_source_get_uuid(s.scope.filter));
    row<<",\"instance\":"<<s.instance<<",\"tick\":"<<tick<<",\"source_enabled\":"
        <<(obs_source_enabled(s.scope.filter)?"true":"false")
        <<",\"settings\":"<<property_compact(obs_data_get_json(settings))
        <<",\"owner_settings\":"<<property_compact(obs_data_get_json(owner))
        <<",\"state\":";nr::qa::write_instance_json_string(row,s.state.c_str());
    row<<",\"reason\":";nr::qa::write_instance_json_string(row,s.reason.c_str());
    row<<",\"kind\":"<<static_cast<unsigned>(s.kind)<<",\"current_content\":"<<s.content
        <<",\"output_content\":"<<s.outputContent<<",\"raw_content\":"<<s.rawContent
        <<",\"output_hash\":\""<<outputHash<<"\",\"current_hash\":\""<<currentHash
        <<"\",\"raw_hash\":\""<<rawHash<<"\",\"selected_job\":";
    capacity_key(row,selected);
    row<<",\"settings_revision\":"<<(native?calldata_int(native,"settings_revision"):0)
        <<",\"applied_settings_revision\":"<<(native?calldata_int(native,"applied_settings_revision"):0)
        <<",\"content_epoch\":"<<(sample?calldata_int(sample,"content_epoch"):0)
        <<",\"history_epoch\":"<<(native?calldata_int(native,"history_epoch"):0)
        <<",\"resource_generation\":"<<(native?calldata_int(native,"resource_generation"):0)
        <<",\"captures\":"<<s.counters.captures<<",\"dispatches\":"<<s.counters.dispatches
        <<",\"selections\":"<<s.counters.selections<<",\"ordinary_physical_complete\":"
        <<(s.ordinary?"true":"false")<<",\"selected_tick\":"<<s.selectedTick
        <<",\"selected_revision\":"<<s.selectedRevision<<"}\n";
    std::osyncstream(std::cout)<<row.str()<<std::flush;
    obs_data_release(owner);obs_data_release(settings);
}
void property_draw(void* opaque,uint32_t,uint32_t)
{
    auto& s=*static_cast<PropertyScenario*>(opaque);
    try {
        if(WaitForSingleObject(s.markerTrigger,0)==WAIT_OBJECT_0) {
            require(s.markerKey.has_value(),"original marker exact graphics request");
            calldata_t request;calldata_init(&request);calldata_set_ptr(&request,"key",&*s.markerKey);
            const bool called=proc_handler_call(obs_source_get_proc_handler(s.scope.filter),
                "nr_qa_original_marker_release",&request);
            s.markerAccepted=calldata_bool(&request,"accepted");calldata_free(&request);
            require(called,"original marker graphics procedure");
            SetEvent(s.markerActed);
        }
        if(s.stopping) {
            property_close_on_graphics(s);
            return;
        }
        if(WaitForSingleObject(s.trigger,0)!=WAIT_OBJECT_0)return;
        fixture=s.original;s.original->content=s.content;
        const auto bytes=lifecycle_view(s.scope.views[0],false,1920);
        s.outputContent=lifecycle_image_id(bytes,false,1920);s.rawContent=0;
        const auto outputHash=nr::qa::bootstrap::hash(bytes);
        const auto currentHash=nr::qa::bootstrap::hash(nr::qa::bootstrap::fixture_bytes(s.content,false));
        s.outputHash=outputHash;s.currentHash=currentHash;s.ordinary=false;
        s.selectedTick=0;s.selectedRevision=0;
        calldata_t sample,native;calldata_init(&sample);calldata_init(&native);
        require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_sampler_snapshot",&sample),
            "property source-bound sampler");
        s.instance=static_cast<uint64_t>(calldata_int(&sample,"instance"));
        s.job.reset();
        std::shared_ptr<nr::model::Job> selected;
        std::shared_ptr<const nr::obs_adapter::Capture> raw;
        calldata_set_ptr(&native,"counters",&s.counters);calldata_set_ptr(&native,"retain_job",&s.job);
        calldata_set_ptr(&native,"retain_selected_job",&selected);calldata_set_ptr(&native,"retain_raw",&raw);
        require(proc_handler_call(obs_source_get_proc_handler(s.scope.filter),"nr_native_snapshot",&native),
            "property actual native snapshot");
        s.state=calldata_string(&native,"public_state");s.reason=calldata_string(&native,"public_reason");
        s.kind=static_cast<nr::OutputKind>(calldata_int(&native,"kind"));
        uint64_t rawHash=0;
        if(raw) {
            require(raw->input.key.instanceId==s.instance,"property raw foreign source");
            const auto rawBytes=nr_bootstrap_read_texture(raw->original11.Get(),false,1920,1080);
            rawHash=nr::qa::bootstrap::hash(rawBytes);s.rawContent=lifecycle_image_id(rawBytes,false,1920);
        }
        if(s.kind==nr::OutputKind::DelayedNr) {
            require(selected && raw && selected->contract.key.frame==raw->input.key,
                "property selected exact native source frame");
            const auto physical=selected->completion();
            require(selected->evaluated && selected->composed && selected->submitted && physical.fence &&
                nr::interop::completion(physical.value,physical.fence->GetCompletedValue())==nr::interop::Completion::Complete,
                "property selected exact physical completion");
            require(s.state=="ready","property truthful ready image");
            require(s.outputContent==s.rawContent,"property selected pixels foreign/stale source phase");
            s.ordinary=selected->flow && !selected->flow->reset && outputHash!=rawHash;
            s.selectedTick=selected->contract.input.canonicalTick;
            s.selectedRevision=selected->contract.input.settings.revision.value;
        }
        if(s.kind==nr::OutputKind::PrimingRaw)
            require(s.state=="priming" && outputHash==currentHash,"property visible current-source priming");
        property_record(s,s.stage,nr::sampler::canonical_tick(obs_get_video_frame_time(),obs_get_frame_interval_ns()),outputHash,
            currentHash,rawHash,s.kind==nr::OutputKind::DelayedNr?&selected->contract.key:nullptr,&sample,&native);
        calldata_free(&native);calldata_free(&sample);
        require(s.faultMode || s.state!="error","property actual native error");
        SetEvent(s.rendered);
    } catch(const std::exception& error) {
        s.failed=true;std::osyncstream(std::cout)<<"NR_PROPERTY_FAILURE "<<error.what()<<'\n'<<std::flush;
        SetEvent(s.rendered);
    }
}
bool run_property(const std::filesystem::path& root,bool& retained,bool restore)
{
    auto* s=new PropertyScenario;retainedProperty=s;retained=true;s->restore=restore;
    bool graphics=false;
    try {
        require(s->trigger && s->rendered && s->closed && s->capacity,"property pre-action render/close subscriptions");
        obs_source_info info{};info.id="nr_task15_fixture";info.type=OBS_SOURCE_TYPE_INPUT;
        info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
        info.get_name=fixture_name;info.create=fixture_create;info.destroy=fixture_destroy;
        info.get_width=fixture_extent;info.get_height=fixture_height;info.video_render=fixture_render;
        info.video_get_color_space=fixture_space;obs_register_source(&info);
        const auto file=root/"profile/task15-source.json";
        const auto runtime=std::filesystem::canonical("C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll").generic_string();
        if(restore) {
            auto* saved=obs_data_create_from_json_file(file.string().c_str());
            require(saved!=nullptr,"property original source file");
            s->scope.source=obs_load_source(saved);obs_data_release(saved);
            require(s->scope.source!=nullptr,"property genuine OBS source load");
            s->scope.filter=obs_source_get_filter_by_name(s->scope.source,"Task15 NR");
        } else {
            require(!std::filesystem::exists(file),"property refuses existing save file");
            auto* settings=obs_data_create();obs_data_set_bool(settings,"nr_lifecycle_fixture",true);
            obs_data_set_string(settings,"foreign_owner","preserve source owner");
            s->scope.source=obs_source_create_private(info.id,"Task15 source",settings);obs_data_release(settings);
            settings=obs_data_create();
            obs_data_set_string(settings,"nr_shader_root",(root/"bin/64bit").string().c_str());
            obs_data_set_string(settings,"nr_data_root",(root/"profile/ngx-data").string().c_str());
            obs_data_set_bool(settings,"nr_qa_observe",true);obs_data_set_bool(settings,"nr_qa_capacity_evidence",true);
            obs_data_set_bool(settings,"nr_enabled",true);
            obs_data_set_double(settings,"nr_strength",.375);obs_data_set_string(settings,"nr_runtime_path",runtime.c_str());
            auto* unrelated=obs_data_create();obs_data_set_string(unrelated,"text","keep \" exact \\ UTF-8");
            obs_data_set_int(unrelated,"integer",731);obs_data_set_bool(unrelated,"flag",true);
            obs_data_set_obj(settings,"foreign_owner",unrelated);obs_data_release(unrelated);
            s->scope.filter=obs_source_create_private("susemi_nr_filter","Task15 NR",settings);
            obs_data_release(settings);
            require(s->scope.source && s->scope.filter,"property actual source/filter create");
            obs_source_filter_add(s->scope.source,s->scope.filter);
        }
        s->original=fixture;require(s->scope.source && s->scope.filter && s->original,"property restored source/filter");
        // No update precedes this restore receipt: defaults/admission must not normalize user values.
        s->state="unobserved";s->reason="unobserved";property_record(*s,"loaded",0,0,0,0,nullptr,nullptr,nullptr);
        auto* settings=obs_source_get_settings(s->scope.filter);
        require(std::string(obs_data_get_string(settings,"nr_runtime_path"))==runtime &&
            obs_data_get_double(settings,"nr_strength")==.375 &&
            obs_data_get_bool(settings,"nr_enabled")==!restore && obs_source_enabled(s->scope.filter)==!restore,
            "property exact source restore controls");obs_data_release(settings);
        if(restore) {
            obs_source_set_enabled(s->scope.filter,true);property_settings(*s,true,.375,runtime);
        }
        obs_enter_graphics();graphics=true;
        std::array<std::shared_ptr<nr::model::QaSignal>,3> signals;
        calldata_t data;calldata_init(&data);calldata_set_ptr(&data,"retain_dispatch_signals",&signals);
        calldata_set_ptr(&data,"retain_exit_signal",&s->exited);
        calldata_set_ptr(&data,"retain_context",&s->context);
        require(proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_snapshot",&data),
            "property pre-trigger original worker and return subscriptions");calldata_free(&data);
        require(!s->context || s->exited,"property original native worker subscription");
        s->returns=std::make_shared<WarmupReturns>(std::move(signals));
        std::shared_ptr<nr::model::ObservationSink> sink=s->returns;
        calldata_init(&data);calldata_set_ptr(&data,"observer",&sink);
        require(proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_observe",&data),
            "property exact original pre-submit observer");calldata_free(&data);
        s->scope.views[0]=obs_view_create();require(s->scope.views[0]!=nullptr,"property actual original view");
        obs_view_set_source(s->scope.views[0],0,s->scope.source);
        obs_add_main_render_callback(property_draw,s);s->callbackAdded=true;
        obs_leave_graphics();graphics=false;
        const auto trigger=[&](const char* stage) {
            obs_enter_graphics();s->stage=stage;s->content=s->content%6+1;
            ResetEvent(s->rendered);SetEvent(s->trigger);obs_leave_graphics();
            require(WaitForSingleObject(s->rendered,15000)==WAIT_OBJECT_0 && !s->failed,"property exact render event");
            if(s->job) {
                require(WaitForSingleObject(s->job->observed,30000)==WAIT_OBJECT_0 &&
                    s->returns->key==s->job->contract.key && s->returns->returned &&
                    s->returns->returned->wait_for(30000),"property exact original job and scheduler return");
                const auto physical=s->job->completion();
                if(s->job->submitted)require(physical.fence && physical.value &&
                    nr::interop::completion(physical.value,physical.fence->GetCompletedValue())==nr::interop::Completion::Complete,
                    "property original physical completion");
                s->job.reset(); // Release only after exact original return and physical completion.
            }
        };
        const auto ready=[&](const char* stage) {
            for(unsigned frame=0;frame<64;++frame) {
                trigger(stage);
                if(s->kind==nr::OutputKind::DelayedNr && s->ordinary)return;
            }
            require(false,"property finite genuine native ready not reached");
        };
        trigger("initial-prime");require(s->kind==nr::OutputKind::PrimingRaw,"property initial visible prime");
        ready("initial-ready");
        const auto disabled=[&](const char* first,const char* second,const char* expected) {
            const auto captures=s->counters.captures,dispatches=s->counters.dispatches;
            trigger(first);trigger(second);
            require(s->counters.captures==captures && s->counters.dispatches==dispatches &&
                s->state=="bypassed" && s->reason==expected && s->kind==nr::OutputKind::CurrentSource &&
                s->outputHash==s->currentHash,
                "property bypass captured/submitted/composed or wrong reason");
        };
        const auto reenable=[&](bool sourceEnable,bool replacement) {
            obs_enter_graphics();graphics=true;
            calldata_t watch;calldata_init(&watch);
            const bool sampled=proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_native_snapshot",&watch);
            const auto generation=calldata_int(&watch,"resource_generation");calldata_free(&watch);
            require(sampled && generation>0,"property pre-transition exact resource generation");
            ResetEvent(s->capacity);
            calldata_init(&watch);calldata_set_ptr(&watch,"event",s->capacity);
            // Source enable alone is a content/history cut, not resource replacement.
            calldata_set_int(&watch,"min_generation",generation+(replacement?1:0));
            const bool called=proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_qa_capacity_watch",&watch);
            const bool subscribed=calldata_bool(&watch,"subscribed");calldata_free(&watch);
            require(called && subscribed,"property pre-transition exact capacity subscription");
            if(sourceEnable)obs_source_set_enabled(s->scope.filter,true);
            property_settings(*s,true,.375,runtime);
            obs_leave_graphics();graphics=false;
            // Natural video_tick performs prepare/collect; no fixture render,
            // capture or submission is triggered while retirement is unresolved.
            const auto observed=WaitForSingleObject(s->capacity,30000);
            obs_enter_graphics();graphics=true;
            calldata_init(&watch);
            const bool unsubscribed=proc_handler_call(obs_source_get_proc_handler(s->scope.filter),"nr_qa_capacity_watch",&watch);
            calldata_free(&watch);
            obs_leave_graphics();graphics=false;
            require(unsubscribed,"property capacity unsubscribe before handle release");
            require(observed==WAIT_OBJECT_0,"property exact natural-tick retirement capacity event");
        };
        property_settings(*s,false,.375,runtime);disabled("configured-off-a","configured-off-b","disabled");
        reenable(false,true);trigger("configured-on-prime");
        require(s->kind==nr::OutputKind::PrimingRaw,"property configured reenable no stale output");ready("configured-on-ready");
        obs_source_set_enabled(s->scope.filter,false);property_settings(*s,true,.375,runtime);
        disabled("source-off-a","source-off-b","disabled");
        reenable(true,false);
        trigger("source-on-prime");require(s->kind==nr::OutputKind::PrimingRaw,"property source reenable no stale output");
        ready("source-on-ready");
        property_settings(*s,true,1.25,runtime);disabled("invalid-strength-a","invalid-strength-b","invalidStrength");
        property_settings(*s,true,.375,"relative.dll");disabled("invalid-path-a","invalid-path-b","runtimeChangeRequiresRecreate");
        reenable(false,true);trigger("recovered-prime");
        require(s->kind==nr::OutputKind::PrimingRaw,"property invalid recovery primes");ready("recovered-ready");
        property_settings(*s,false,.375,runtime);obs_source_set_enabled(s->scope.filter,false);
        disabled("persist-off-a","persist-off-b","disabled");
        if(!restore) {
            obs_enter_graphics();graphics=true;
            auto* saved=obs_save_source(s->scope.source);require(saved!=nullptr,"property actual OBS source save");
            require(obs_data_save_json(saved,file.string().c_str()),"property source profile write");obs_data_release(saved);
            property_record(*s,"saved",0,0,0,0,nullptr,nullptr,nullptr);
            obs_leave_graphics();graphics=false;
        }
        property_request_stop();
        require(WaitForSingleObject(s->closed,30000)==WAIT_OBJECT_0 && property_closed(),
            "property exact original graphics/worker closure");
        obs_enter_graphics();graphics=true;
        lifecycle_write_closure(s->closure,"task15-property-closed",0);
        property_record(*s,"closed",0,0,0,0,nullptr,nullptr,nullptr);
        obs_leave_graphics();graphics=false;
        require(property_release(),"property exact post-source-destruction closure");
        retained=false;
        return true;
    } catch(const std::exception& error) {
        if(graphics)obs_leave_graphics();
        std::osyncstream(std::cout)<<"NR_PROPERTY_FAILURE "<<error.what()
            <<"\nNR_PROPERTY_RETAINED originals=true\n"<<std::flush;
        // No release, shutdown, process control or normal-close claim on uncertain originals.
        return false;
    }
}
