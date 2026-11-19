// SPDX-License-Identifier: GPL-3.0-only
void passes_status_controls()
{
    for(const auto& status:std::array<nr::model::PassStatus,6>{{
            {1,1,0,0},{1,0,0,0},{2,2,0,0},{2,1,2,0xbad00001u},{2,0,0,0},{1,0,2,0xbad00001u}}}) {
        // Given an independent requested/effective/failed worker status.
        Labels labels;labels.passStatus=status;labels.passesText="machine-observed-counts";
        // When actual OBS properties are built.
        auto* props=controls({"ready",status.failedPass?"passFailed":"none"},labels);
        auto* row=obs_properties_get(props,"nr_passes_status");
        // Then only the requested/actual visibility rule adds a read-only information row.
        expect(bool(row)==(status.requested>1 || status.effective<status.requested),"passes row visibility");
        if(row)expect(obs_property_text_type(row)==OBS_TEXT_INFO && !obs_property_enabled(row),"passes row is read-only");
        expect(obs_properties_get(props,"nr_status") && obs_properties_get(props,"nr_reason"),"status/reason rows preserved");
        obs_properties_destroy(props);
    }
    expect(status(nr::Status::Applied,nr::Reason::PassFailed).reason=="passFailed" &&
        status(nr::Status::Bypassed,nr::Reason::WorkScaleTooSmall).reason=="workScaleTooSmall","new machine reason codes");
}
