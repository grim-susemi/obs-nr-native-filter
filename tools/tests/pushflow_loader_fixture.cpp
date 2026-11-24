// SPDX-License-Identifier: GPL-3.0-only
// Loadable PE only: no DirectML exports or runtime dependency.
extern "C" __declspec(dllexport) int pushflow_loader_fixture() noexcept { return 0; }
