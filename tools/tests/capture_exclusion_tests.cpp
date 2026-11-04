#include "capture_exclusion.h"
#include <cstdio>

namespace {
int failures=0;
void check(bool ok,const char* what)
{
    if(!ok) { ++failures;std::printf("FAIL %s\n",what); }
}
DWORD affinity(HWND w)
{
    DWORD a=0xFFFFFFFFu;GetWindowDisplayAffinity(w,&a);return a;
}
HWND make(const wchar_t* cls,bool visible)
{
    return CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls,L"nr-capture-exclusion-test",WS_POPUP|(visible?WS_VISIBLE:0),
        -20000,-20000,32,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
}
bool recorded(const std::vector<HWND>& changed,HWND w)
{
    return std::find(changed.begin(),changed.end(),w)!=changed.end();
}
}

int main()
{
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"NrCaptureExclusionTest";
    if(!RegisterClassW(&wc)) { std::printf("FAIL RegisterClassW %lu\n",GetLastError());return 1; }
    HWND plain=make(wc.lpszClassName,true),preset=make(wc.lpszClassName,true),hidden=make(wc.lpszClassName,false),gone=make(wc.lpszClassName,true);
    if(!plain || !preset || !hidden || !gone) { std::printf("FAIL CreateWindowExW %lu\n",GetLastError());return 1; }
    check(SetWindowDisplayAffinity(preset,WDA_EXCLUDEFROMCAPTURE)!=0,"preset exclusion (stands for OBS's own setting)");

    std::vector<HWND> changed;
    const size_t added=nr::capture_exclusion::detail::exclude(GetCurrentProcessId(),changed);
    check(added==changed.size() && recorded(changed,plain) && recorded(changed,gone),"capturable visible windows recorded");
    check(affinity(plain)==WDA_EXCLUDEFROMCAPTURE && affinity(gone)==WDA_EXCLUDEFROMCAPTURE,"capturable visible windows excluded");
    check(affinity(hidden)==WDA_NONE && !recorded(changed,hidden),"hidden window untouched");
    check(!recorded(changed,preset),"already excluded window not recorded");
    check(nr::capture_exclusion::detail::exclude(GetCurrentProcessId(),changed)==0,"second scan changes nothing");

    DestroyWindow(gone);
    nr::capture_exclusion::detail::exclude(GetCurrentProcessId(),changed);
    check(!recorded(changed,gone) && recorded(changed,plain),"destroyed window pruned");

    const size_t count=changed.size(),restored=nr::capture_exclusion::detail::restore(changed);
    check(restored==count && changed.empty(),"restore covers every recorded window");
    check(affinity(plain)==WDA_NONE,"recorded window capturable again");
    check(affinity(preset)==WDA_EXCLUDEFROMCAPTURE,"exclusion set by someone else kept");

    DestroyWindow(plain);DestroyWindow(preset);DestroyWindow(hidden);
    if(failures) { std::printf("capture_exclusion: %d failure(s)\n",failures);return 1; }
    std::printf("capture_exclusion: PASS\n");
    return 0;
}
