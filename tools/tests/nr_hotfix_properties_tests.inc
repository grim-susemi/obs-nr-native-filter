// SPDX-License-Identifier: GPL-3.0-only
void nr_hotfix_properties(const std::string& runtime)
{
    auto* data=obs_data_create();defaults(data);
    auto* props=controls({"ready","none"});
    auto* quick=obs_properties_get(props,"nr_quick_preset");
    for(int preset=1;preset<=4;++preset) {
        obs_data_set_int(data,"nr_quick_preset",preset);
        expect(obs_property_modified(quick,data),"quick preset callback");
        const auto values=decode(data,default_runtime,runtime);
        expect(values.reason.empty() && values.strength.value==0.85f &&
            values.stabilize.temporalStrength==0.45f && values.stabilize.holdStrength==0.45f &&
            values.stabilize.holdTolerance==0.02f,"built-in preset composition and anti-flicker");
        expect(values.tuning.intensity==(preset==3?0.85f:1.0f),"built-in preset vendor intensity");
        if(preset==2)expect(values.pass2Tuning && values.pass2Tuning->intensity==1.0f,"combo pass2 intensity");
    }
    obs_data_set_double(data,"nr_strength",0.27);
    obs_data_set_double(data,"nr_intensity",2.0);
    obs_data_set_double(data,"nr_temporal_stability",0.8);
    obs_data_set_double(data,"nr_hold_strength",0.7);
    obs_data_set_int(data,"nr_quick_preset",0);
    expect(obs_property_modified(quick,data),"custom preset callback");
    defaults(data);
    const auto saved=decode(data,default_runtime,runtime);
    expect(saved.reason.empty() && saved.strength.value==0.27f && saved.tuning.intensity==2.0f &&
        saved.stabilize.temporalStrength==0.8f && saved.stabilize.holdStrength==0.7f,
        "custom and explicit saved settings survive defaults");
    obs_properties_destroy(props);obs_data_release(data);
}
