// SPDX-License-Identifier: GPL-3.0-only
#include "fg_monitor_text.h"
#include <cstdio>
#include <string>

int main()
{
    using nr::fg::vfg::utf8_to_wide;
    using nr::fg::vfg::wide_to_utf8;
    const std::wstring ids[]{
        LR"(\\?\DISPLAY#ACR1234#5&123456&0&UID1234#{abcdef})",
        L"\\\\.\\DISPLAY2",
        L"monitor-\uD55C\uAD6D",
    };
    for (const auto& id : ids) {
        const auto encoded = wide_to_utf8(id.c_str());
        if (encoded.empty() || encoded.find('\0') != std::string::npos || utf8_to_wide(encoded) != id) {
            std::fprintf(stderr, "FAIL vfg_monitor_text: monitor ID round trip\n");
            return 1;
        }
    }
    if (!wide_to_utf8(L"").empty() || !utf8_to_wide("").empty()) {
        std::fprintf(stderr, "FAIL vfg_monitor_text: auto selection must remain empty\n");
        return 1;
    }
    std::puts("PASS vfg_monitor_text");
    return 0;
}
