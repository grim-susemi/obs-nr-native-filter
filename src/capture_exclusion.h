#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// A projector on the captured monitor is captured again, so the filter re-processes its own output
// every frame. OBS's "Hide OBS windows from screen capture" deliberately skips projectors.
namespace nr::capture_exclusion {
void enter();
void leave();
void refresh();

namespace detail {
// Records only windows changed here, so restore() never undoes an exclusion OBS itself set.
inline size_t exclude(DWORD pid,std::vector<HWND>& changed)
{
    struct Scan { DWORD pid; std::vector<HWND> windows; } scan{pid,{}};
    EnumWindows([](HWND hwnd,LPARAM param)->BOOL {
        auto& s=*reinterpret_cast<Scan*>(param);
        DWORD owner=0;GetWindowThreadProcessId(hwnd,&owner);
        if(owner==s.pid && IsWindowVisible(hwnd)) s.windows.push_back(hwnd);
        return TRUE;
    },reinterpret_cast<LPARAM>(&scan));
    changed.erase(std::remove_if(changed.begin(),changed.end(),[](HWND w) { return !IsWindow(w); }),changed.end());
    size_t added=0;
    for(HWND w:scan.windows) {
        DWORD affinity=WDA_NONE;
        if(!GetWindowDisplayAffinity(w,&affinity) || affinity!=WDA_NONE) continue;
        if(SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE)) { changed.push_back(w);++added; }
    }
    return added;
}
inline size_t restore(std::vector<HWND>& changed)
{
    size_t restored=0;
    for(HWND w:changed) {
        DWORD affinity=WDA_NONE;
        if(IsWindow(w) && GetWindowDisplayAffinity(w,&affinity) && affinity==WDA_EXCLUDEFROMCAPTURE &&
            SetWindowDisplayAffinity(w,WDA_NONE)) ++restored;
    }
    changed.clear();
    return restored;
}
}
}
