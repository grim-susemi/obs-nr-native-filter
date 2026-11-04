// SPDX-License-Identifier: GPL-3.0-only
// CPU contract cases share the NGX ABI double defined by runtime_contract_tests.cpp.
static JobContract workJob(uint64_t serial,uint64_t history,uint32_t scale) {
    const Settings s{{1},true,"C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll",
        {1},{0,0,1,1,.75f,-1,true},2,scale};
    const FrameKey key{1,1,serial};
    return {{key,history,1,serial},{key,serial+1,serial,1,1,1,SourceValidity::Valid,
        {{64,64},{0,0,64,64},PixelFormat::Bgra8,ColorSpace::Rec709Srgb,AlphaMode::Opaque,80},s},serial+2};
}

static void workExtentTests() {
    // Given: two passes share a module and immutable settings, not tuning or history.
    const auto j1=workJob(1,1,50);
    const auto j2=workJob(2,1,50);
    const auto j3=workJob(3,2,50);
    const auto& s=j1.input.settings;
    const ModelTuning later{s.tuning.preset,s.tuning.style,s.tuning.intensity,
        s.tuning.localStructure,0,s.tuning.skinStructure,s.tuning.autoMask};
    auto module=Module::controlled(api(),{},std::make_shared<int>(1),Route::Driver);
    auto complete=std::make_shared<Completion>(Completion::Complete);
    Pass first(module),second(module);
    expectedWidth=32;
    check(first.prepare(s,s.tuning,{32,32},1,recording(1,complete))==NVSDK_NGX_Result_Success);
    auto* firstParams=latest;
    check(second.prepare(s,later,{32,32},1,recording(1,complete))==NVSDK_NGX_Result_Success);
    auto* secondParams=latest;
    check(firstParams!=secondParams && first.ready(2) && second.ready(2));
    float tone=0; unsigned width=0;
    check(firstParams->Get("DLSSNR.LocalToneStrength",&tone)==NVSDK_NGX_Result_Success && tone==.75f);
    check(secondParams->Get("DLSSNR.LocalToneStrength",&tone)==NVSDK_NGX_Result_Success && tone==0);
    check(second.prepare(s,s.tuning,{32,32},1,recording(1,complete))!=NVSDK_NGX_Result_Success);

    // When: evaluate at 50%, with the original full-size guide regions and motion scales.
    const auto base=inputs();
    const Inputs guide{base.color,base.depth,base.motion,base.output,{4,5,56,50},{3,2,55,51},
        false,false,.5f,-.5f,true};
    check(first.run(j1,guide,recording(2,complete))==NVSDK_NGX_Result_Success);
    // Then: the vendor receives the work size and the caller's untouched guides.
    check(firstParams->Get("DLSSNR.Width",&width)==NVSDK_NGX_Result_Success && width==32);
    check(firstParams->Get("DLSSNR.Height",&width)==NVSDK_NGX_Result_Success && width==32);
    check(firstParams->Get("DLSSNR.ColorSubrectWidth",&width)==NVSDK_NGX_Result_Success && width==32);
    check(firstParams->Get("DLSSNR.OutputSubrectHeight",&width)==NVSDK_NGX_Result_Success && width==32);
    check(firstParams->Get("DLSSNR.DepthSubrectBaseX",&width)==NVSDK_NGX_Result_Success && width==4);
    check(firstParams->Get("DLSSNR.MVecSubrectHeight",&width)==NVSDK_NGX_Result_Success && width==51);
    check(firstParams->Get("DLSSNR.MVecScaleX",&tone)==NVSDK_NGX_Result_Success && tone==.5f);
    check(firstParams->Get("DLSSNR.MVecScaleY",&tone)==NVSDK_NGX_Result_Success && tone==-.5f);
    const auto before=evaluates;
    check(first.run(j1,guide,recording(3,complete))!=NVSDK_NGX_Result_Success && evaluates==before);
    check(first.run(workJob(2,1,100),guide,recording(3,complete))!=NVSDK_NGX_Result_Success && evaluates==before);
    check(first.run(j2,guide,recording(3,complete))==NVSDK_NGX_Result_Success);
    check(firstParams->Get("DLSSNR.Reset",&width)==NVSDK_NGX_Result_Success && width==0);
    check(second.run(j1,guide,recording(2,complete))==NVSDK_NGX_Result_Success);
    check(secondParams->Get("DLSSNR.Reset",&width)==NVSDK_NGX_Result_Success && width==1);
    check(secondParams->Get("DLSSNR.LocalToneStrength",&tone)==NVSDK_NGX_Result_Success && tone==0);
    check(first.run(j3,guide,recording(4,complete))==NVSDK_NGX_Result_Success);
    check(firstParams->Get("DLSSNR.Reset",&width)==NVSDK_NGX_Result_Success && width==1);
    check(second.run(j2,guide,recording(3,complete))==NVSDK_NGX_Result_Success);
    check(secondParams->Get("DLSSNR.Reset",&width)==NVSDK_NGX_Result_Success && width==0);
    check(first.release() && second.release());

    // Given the same 50% settings but a wrongly prepared extent, no evaluation is recorded.
    Pass mismatch(module);expectedWidth=16;
    check(mismatch.prepare(s,s.tuning,{16,16},1,recording(1,complete))==NVSDK_NGX_Result_Success);
    const auto mismatchBefore=evaluates;
    check(mismatch.run(j1,guide,recording(2,complete))!=NVSDK_NGX_Result_Success && evaluates==mismatchBefore);
    check(mismatch.release());expectedWidth=1920;
    puts("PASS 50% work extent, tuning, separate pass reset, replay and size refusals; CPU fakes only");
}
