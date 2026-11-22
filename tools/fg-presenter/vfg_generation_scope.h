// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <exception>

namespace nr::fg::vfg {
// Declare GPU and queue owners outside both callbacks. Even if work throws
// after submitting a list, shutdown decides whether those owners may unwind.
template<class Work, class Shutdown>
void run_generation(const Work& work, const Shutdown& shutdown, bool preserveOwnerOnFault = true)
{
    if (!preserveOwnerOnFault) {
        work();
        shutdown(false);
        return;
    }
    std::exception_ptr fault;
    try {
        work();
    } catch (...) {
        fault = std::current_exception();
    }
    const bool userClosed = shutdown(static_cast<bool>(fault));
    if (fault && !userClosed) std::rethrow_exception(fault);
}
} // namespace nr::fg::vfg
