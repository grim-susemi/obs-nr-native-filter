#include "capture_exclusion.h"
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>

namespace nr::capture_exclusion {
namespace {
std::atomic<int> holders{0};
std::atomic<bool> queued{false};
std::atomic<uint64_t> lastRefresh{0};
std::vector<HWND> uiThreadChanged;

void apply(void*)
{
    queued.store(false,std::memory_order_release);
    if(holders.load(std::memory_order_acquire)>0) {
        if(const size_t added=detail::exclude(GetCurrentProcessId(),uiThreadChanged))
            blog(LOG_INFO,"[obs-nr-filter] hid %zu OBS window(s) from screen capture while Native NR runs on a display capture",added);
    } else if(!uiThreadChanged.empty()) {
        blog(LOG_INFO,"[obs-nr-filter] restored screen capture of %zu OBS window(s)",detail::restore(uiThreadChanged));
    }
}
void schedule()
{
    if(!queued.exchange(true,std::memory_order_acq_rel)) obs_queue_task(OBS_TASK_UI,apply,nullptr,false);
}
}
void enter() { holders.fetch_add(1,std::memory_order_acq_rel);schedule(); }
void leave() { holders.fetch_sub(1,std::memory_order_acq_rel);schedule(); }
void refresh()
{
    const uint64_t now=os_gettime_ns();
    if(now-lastRefresh.load(std::memory_order_relaxed)<500000000ull) return;
    lastRefresh.store(now,std::memory_order_relaxed);
    schedule();
}
}
