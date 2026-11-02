// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <regex>
#include <string>
namespace nr::qa {
enum class ArtifactScenario { RecordingOutput, GhostCalibration };
namespace window_artifacts {
inline std::wstring lower(std::wstring value)
{
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    return value;
}
inline bool same(const std::filesystem::path& a,const std::filesystem::path& b)
{
    return lower(a.lexically_normal().native())==lower(b.lexically_normal().native());
}
inline bool plain_directory(const std::filesystem::path& p)
{
    const auto attributes=GetFileAttributesW(p.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
}
}
// The one admission decision for a scenario's native artifact directory. root = argv[1], the N2 stage root
// <W>/out/native-window-stage/<leaf>; directory = argv[8]. Accepted is exactly the window instance's own
// <W>/.omo/evidence/obs-nr-native-filter-20260930/task-<n>/<leaf>/artifacts/<scenario> (D3 WindowRoot.ps1:131,
// N2Runners.ps1:372), with the same leaf as the stage root and the leaf rule G uses (N2Core.ps1 Get-WindowIdentity).
// Everything else is refused: other scenarios, siblings, parents, children, '.'/'..' or trailing separators,
// other trees, non-window leaves, missing directories, and any reparse point (junction/symlink) on the path.
inline bool artifact_directory_allowed(const std::filesystem::path& root,const std::filesystem::path& directory,
    ArtifactScenario scenario)
{
    namespace fs=std::filesystem;
    using namespace window_artifacts;
    const wchar_t* name=scenario==ArtifactScenario::RecordingOutput?L"recording-output":L"ghost-calibration";
    if(!root.is_absolute() || !directory.is_absolute() || directory.compare(directory.lexically_normal())!=0 ||
        root.compare(root.lexically_normal())!=0)return false;
    const auto leaf=root.filename().native();
    static const std::wregex window(LR"(^[a-z0-9-]*window-r([0-9]+)-(n[0-9]+[ab]?|n2r)-(u[0-9]+[a-z]?)-(s[0-9]+[a-z]?)-([0-9]{8})$)");
    static const std::wregex task(LR"(^task-[0-9]+$)");
    if(!std::regex_match(leaf,window) || root.parent_path().filename().native()!=L"native-window-stage" ||
        root.parent_path().parent_path().filename().native()!=L"out")return false;
    const auto work=root.parent_path().parent_path().parent_path();
    const auto artifacts=directory.parent_path(),instance=artifacts.parent_path(),taskDirectory=instance.parent_path();
    if(directory.filename().native()!=name || artifacts.filename().native()!=L"artifacts" || instance.filename().native()!=leaf ||
        !std::regex_match(taskDirectory.filename().native(),task))return false;
    const auto tree=work/".omo"/"evidence"/"obs-nr-native-filter-20260930";
    if(!same(taskDirectory.parent_path(),tree))return false;
    for(const auto& p:{work/".omo",work/".omo"/"evidence",tree,taskDirectory,instance,artifacts,directory})
        if(!plain_directory(p))return false;
    std::error_code error;
    const auto canonical=fs::canonical(directory,error);
    return !error && same(canonical,directory);
}
}
