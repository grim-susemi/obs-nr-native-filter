// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Windows.h>
#include <string>

namespace nr::fg::vfg {
inline std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n)) return {};
    return out;
}

inline std::string wide_to_utf8(const wchar_t* s)
{
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr)) return {};
    out.pop_back();
    return out;
}
}
