// SPDX-License-Identifier: GPL-3.0-only
// UG03P3 (W1 blocker): the N2 window passes artifactRoot = <instanceRoot>/artifacts/<scenario>
// (D3 WindowRoot.ps1:131, N2Runners.ps1:372) and the host stage root as argv[1]
// (<W>/out/native-window-stage/<leaf>). The host must accept exactly that instance path for its own
// scenario and refuse everything else. Real directories, junctions and paths in a private temp tree.
#include "nr_window_artifacts.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
using nr::qa::ArtifactScenario;
namespace {
unsigned checks=0;
void expect(bool value,const std::string& why){++checks;if(!value)throw std::runtime_error(why);}
void junction(const fs::path& link,const fs::path& target)
{
    const std::wstring command=L"cmd /d /c mklink /J \""+link.wstring()+L"\" \""+target.wstring()+L"\" >nul";
    expect(_wsystem(command.c_str())==0 && fs::exists(link),"test junction created: "+link.string());
}
}
int main() try {
    const auto base=fs::canonical(fs::temp_directory_path())/("nr-window-artifact-test-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(GetTickCount64()));
    const auto w=base/"w";
    const auto tree=w/".omo"/"evidence"/"obs-nr-native-filter-20260930";
    const std::string leaf="native-window-r34-n2-u3e-s3e-20261019";
    const auto stage=w/"out"/"native-window-stage"/leaf;
    const auto instance=tree/"task-16"/leaf;
    const auto recording=instance/"artifacts"/"recording-output";
    const auto ghost=instance/"artifacts"/"ghost-calibration";
    for(const auto& d:{stage,recording,ghost,instance/"artifacts"/"recording-output-2",recording/"nested"})fs::create_directories(d);
    struct Cleanup{fs::path root;std::vector<fs::path> links;~Cleanup(){for(auto& l:links)RemoveDirectoryW(l.c_str());std::error_code e;fs::remove_all(root,e);}} cleanup{base,{}};
    const auto allowed=[&](const fs::path& root,const fs::path& dir,ArtifactScenario s){return nr::qa::artifact_directory_allowed(root,dir,s);};
    // Accepted: exactly this window's own scenario directory, either separator style.
    expect(allowed(stage,recording,ArtifactScenario::RecordingOutput),"Task16 window artifact directory accepted");
    expect(allowed(stage,ghost,ArtifactScenario::GhostCalibration),"Task26 window artifact directory accepted");
    expect(allowed(fs::path(stage.generic_string()),fs::path(recording.generic_string()),ArtifactScenario::RecordingOutput),
        "forward-slash runner form accepted");
    // Refused: other scenario, sibling, parent, child.
    expect(!allowed(stage,ghost,ArtifactScenario::RecordingOutput),"other scenario refused (recording)");
    expect(!allowed(stage,recording,ArtifactScenario::GhostCalibration),"other scenario refused (ghost)");
    expect(!allowed(stage,instance/"artifacts"/"recording-output-2",ArtifactScenario::RecordingOutput),"sibling refused");
    expect(!allowed(stage,instance/"artifacts",ArtifactScenario::RecordingOutput),"parent refused");
    expect(!allowed(stage,recording/"nested",ArtifactScenario::RecordingOutput),"child refused");
    expect(!allowed(stage,instance,ArtifactScenario::RecordingOutput),"instance root itself refused");
    // Refused: traversal, trailing separator, relative, wrong case.
    expect(!allowed(stage,recording/".."/"recording-output",ArtifactScenario::RecordingOutput),"dot-dot traversal refused");
    expect(!allowed(stage,ghost/".."/"recording-output",ArtifactScenario::RecordingOutput),"cross-scenario traversal refused");
    expect(!allowed(stage,instance/"artifacts"/"."/"recording-output",ArtifactScenario::RecordingOutput),"dot segment refused");
    expect(!allowed(stage,fs::path(recording.generic_string()+"/"),ArtifactScenario::RecordingOutput),"trailing separator refused");
    expect(!allowed(stage,fs::path("artifacts/recording-output"),ArtifactScenario::RecordingOutput),"relative path refused");
    expect(!allowed(stage,instance/"ARTIFACTS"/"recording-output",ArtifactScenario::RecordingOutput),"non-exact case refused");
    // Refused: not a window instance / wrong tree / stage-instance identity mismatch.
    const std::string badLeaf="native-window-x";
    fs::create_directories(w/"out"/"native-window-stage"/badLeaf);fs::create_directories(tree/"task-16"/badLeaf/"artifacts"/"recording-output");
    expect(!allowed(w/"out"/"native-window-stage"/badLeaf,tree/"task-16"/badLeaf/"artifacts"/"recording-output",ArtifactScenario::RecordingOutput),
        "non-window leaf refused");
    const auto otherTree=w/".omo"/"evidence"/"other-tree"/"task-16"/leaf/"artifacts"/"recording-output";fs::create_directories(otherTree);
    expect(!allowed(stage,otherTree,ArtifactScenario::RecordingOutput),"other evidence tree refused");
    const auto noTask=tree/"notask"/leaf/"artifacts"/"recording-output";fs::create_directories(noTask);
    expect(!allowed(stage,noTask,ArtifactScenario::RecordingOutput),"non task-N parent refused");
    const std::string leafB="native-window-r35-n2-u3e-s3e-20261019";
    fs::create_directories(w/"out"/"native-window-stage"/leafB);
    expect(!allowed(w/"out"/"native-window-stage"/leafB,recording,ArtifactScenario::RecordingOutput),"stage/instance leaf mismatch refused");
    fs::create_directories(w/"elsewhere"/leaf);
    expect(!allowed(w/"elsewhere"/leaf,recording,ArtifactScenario::RecordingOutput),"stage root outside out/native-window-stage refused");
    expect(!allowed(stage,ghost/"missing",ArtifactScenario::GhostCalibration) &&
        !allowed(w/"out"/"native-window-stage"/leafB,tree/"task-16"/leafB/"artifacts"/"recording-output",ArtifactScenario::RecordingOutput),
        "missing directory refused");
    // Refused: the pre-window fixed folders (old rule) even under the derived work root.
    const auto oldGhost=tree/"task-26"/"ghost-artifacts"/"attempt-1";fs::create_directories(oldGhost);
    expect(!allowed(stage,oldGhost,ArtifactScenario::GhostCalibration),"old ghost-artifacts/attempt-* folder refused");
    // Refused: junctions at the leaf and above it (names all valid).
    const auto outside=base/"outside"/"recording-output";fs::create_directories(outside);
    const std::string leafC="native-window-r36-n2-u3e-s3e-20261019",leafD="native-window-r37-n2-u3e-s3e-20261019";
    for(const auto& l:{leafC,leafD})fs::create_directories(w/"out"/"native-window-stage"/l);
    fs::create_directories(tree/"task-16"/leafC/"artifacts");
    junction(tree/"task-16"/leafC/"artifacts"/"recording-output",outside);cleanup.links.push_back(tree/"task-16"/leafC/"artifacts"/"recording-output");
    expect(!allowed(w/"out"/"native-window-stage"/leafC,tree/"task-16"/leafC/"artifacts"/"recording-output",ArtifactScenario::RecordingOutput),
        "junction scenario directory refused");
    fs::create_directories(tree/"task-16"/leafD);
    junction(tree/"task-16"/leafD/"artifacts",base/"outside");cleanup.links.push_back(tree/"task-16"/leafD/"artifacts");
    expect(fs::is_directory(tree/"task-16"/leafD/"artifacts"/"recording-output"),"junction target reachable");
    expect(!allowed(w/"out"/"native-window-stage"/leafD,tree/"task-16"/leafD/"artifacts"/"recording-output",ArtifactScenario::RecordingOutput),
        "junction artifacts directory refused");
    // n2r leaf (PowerShell N2Core accepts n2r): own scenario dirs accepted; n2x and n3r leaves refused.
    const std::string leafR="native-window-r38-n2r-u3e-s3g-20261029";
    const auto stageR=w/"out"/"native-window-stage"/leafR;
    const auto instanceR=tree/"task-38"/leafR;
    const auto recordingR=instanceR/"artifacts"/"recording-output";
    const auto ghostR=instanceR/"artifacts"/"ghost-calibration";
    for(const auto& d:{stageR,recordingR,ghostR})fs::create_directories(d);
    expect(allowed(stageR,recordingR,ArtifactScenario::RecordingOutput),"n2r recording-output directory accepted");
    expect(allowed(stageR,ghostR,ArtifactScenario::GhostCalibration),"n2r ghost-calibration directory accepted");
    for(const std::string bad:{"native-window-r38-n2x-u3e-s3g-20261029","native-window-r38-n3r-u3e-s3g-20261029"}){
        const auto badStage=w/"out"/"native-window-stage"/bad;
        const auto badRecording=tree/"task-38"/bad/"artifacts"/"recording-output";
        fs::create_directories(badStage);fs::create_directories(badRecording);
        expect(!allowed(badStage,badRecording,ArtifactScenario::RecordingOutput),"non-n2r leaf refused: "+bad);
    }
    std::printf("PASS window_artifact_path checks=%u exact_instance_scenario_only=true CPU_only=true\n",checks);
    return 0;
} catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
