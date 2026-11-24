// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>

// Only the Controller object compiled for this test gets this import substitution.
BOOL WINAPI observe_presenter_dispatch(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
#define CreateProcessW observe_presenter_dispatch
