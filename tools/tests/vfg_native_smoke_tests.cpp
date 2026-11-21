// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nr::fg;
using namespace nr::fg::vfg;

namespace {
void expect(bool ok, const char* why)
{
    if (!ok) throw std::runtime_error(why);
}

std::wstring secondary_monitor_id()
{
    std::wstring found;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* out = reinterpret_cast<std::wstring*>(lp);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi) || (mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
        for (DWORD i = 0;; ++i) {
            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(mi.szDevice, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) break;
            if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                *out = dd.DeviceID;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

void run_multiplier(uint32_t mult, const std::wstring& presenterPath, const std::wstring& logPath,
    const std::wstring& monitorId, uint32_t cadence = 0, Quality quality = Quality::Medium, bool hotfix = false)
{
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenterPath.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logPath.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_RUNTIME", nullptr); // Verify auto-discovery of Veyra 2.1.9 runtime!

    const auto rtDir = Controller::find_runtime_dir();
    expect(!rtDir.empty(), "Controller::find_runtime_dir() returned empty");

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &dev, &fl, &ctx)), "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    expect(SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)), "adapter");

    const ipc::Shape shape{hotfix ? 2560u : 1920u, hotfix ? 1440u : 1080u,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = shape.width;
    td.Height = shape.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = shape.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    expect(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)), "CreateTexture2D");

    const auto key = LR"(Local\nr-vfg-smoke-)" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mult);
    ipc::Producer producer(dev.Get(), key, shape);
    LaunchOptions opts{};
    opts.multiplier = mult;
    opts.quality = quality;
    opts.monitorId = monitorId;
    Controller controller(opts);

    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, shape, desc.AdapterLuid), "controller.offer");

    const DWORD waitHandshake = WaitForSingleObject(producer.control_event(), 10000);
    expect(waitHandshake == WAIT_OBJECT_0 && producer.status() == ipc::Status::Ok, "IPC handshake ready");

    std::vector<uint32_t> row(shape.width * 64, 0xFF2080FFu);
    std::vector<uint32_t> background;
    if (cadence) {
        background.resize(static_cast<size_t>(shape.width) * shape.height, 0xFF202020u);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, background.data(), shape.width * 4, 0);
    }
    const auto start = clock_ns();
    uint32_t last = UINT32_MAX, accepted = 0, repeated = 0;
    const uint32_t count = hotfix ? 360 : cadence ? 96 : 45;
    for (uint32_t f = 0; f < count; ++f) {
        const auto ts = start + uint64_t{f} * 1'000'000'000 / 60;
        if (cadence) {
            while (clock_ns() < ts) {
                if (ts - (std::min)(ts, clock_ns()) > 2'000'000) Sleep(1);
                else SwitchToThread();
            }
        }
        const uint32_t unique = cadence ? f * cadence / 60 : f;
        const bool repeat = unique == last;
        if (!repeat) {
            if (cadence && last != UINT32_MAX) {
                const uint32_t oldX = (last * 40) % (shape.width - 128);
                const D3D11_BOX oldBox{oldX, 200, 0, oldX + 64, 264, 1};
                ctx->UpdateSubresource(tex.Get(), 0, &oldBox, background.data(), 64 * 4, 0);
            }
            const uint32_t x = (unique * 40) % (shape.width - 128);
            const D3D11_BOX box{x, 200, 0, x + 64, 264, 1};
            ctx->UpdateSubresource(tex.Get(), 0, &box, row.data(), 64 * 4, 0);
        }
        last = unique;
        const auto now = clock_ns();
        const auto pub = producer.publish(tex.Get(), ts, now, !hotfix && cadence && f == 48, cadence ? ts : 0);
        expect(pub == ipc::Status::Ok || pub == ipc::Status::Dropped, "publish");
        if (pub == ipc::Status::Ok) {
            ++accepted;
            if (repeat) ++repeated;
        }
        if (!cadence) Sleep(16);
    }

    controller.stop();
    Sleep(100);
    for (int i = 0; i < 60 && controller.alive(); ++i) Sleep(50);
    expect(producer.retire() == ipc::Status::Ok, "producer.retire");

    std::ifstream ifs(logPath);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    expect(content.find("event=initialized backend=nvidia-vfg-native") != std::string::npos, "missing nvidia-vfg-native init");
    const auto events = [&](std::string_view name) {
        size_t c = 0, pos = 0;
        while ((pos = content.find(name, pos)) != std::string::npos) { ++c; pos += name.size(); }
        return c;
    };
    const auto subCount = events("event=present_vfg_subframe");
    const auto origCount = events("event=present_original");
    if (cadence)
        std::printf("CADENCE hz=%u accepted=%u duplicate_writes=%u\n", cadence, accepted, repeated);
    if (hotfix) {
        const auto mode = static_cast<unsigned>(quality);
        expect(content.find("size=2560x1440 quality=" + std::to_string(mode) + " ") != std::string::npos,
            "wrong resolution or mode in presenter");
        std::printf("HOTFIX quality=%u late=%zu pair_late=%zu busy=%zu original_late=%zu synthetic=%zu original=%zu\n", mode,
            events("event=drop_vfg_subframe_late"), events("event=drop_vfg_pair_late"),
            events("event=drop_vfg_present_busy"), events("event=drop_original_late"), subCount, origCount);
    }
    expect(subCount >= (mult - 1) * 5, "insufficient VFG interpolated subframes presented");
    expect(origCount >= 5, "insufficient original frames presented");
    std::printf("PASS vfg_native_smoke multiplier=%ux subframes=%zu originals=%zu\n", mult, subCount, origCount);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        const bool cadence = argc == 4 && std::wstring_view(argv[3]) == L"--cadence";
        const bool hotfix = argc == 4 && std::wstring_view(argv[3]) == L"--1440p-4x";
        const bool high = argc == 4 && std::wstring_view(argv[3]) == L"--1440p-4x-high";
        expect(argc == 3 || cadence || hotfix || high,
            "usage: vfg_native_smoke_tests <presenter.exe> <log_dir> [--cadence|--1440p-4x|--1440p-4x-high]");
        const std::wstring presenter = argv[1];
        const std::filesystem::path logDir = argv[2];
        const auto monId = secondary_monitor_id();
        expect(!monId.empty(), "no active secondary display");
        expect(SetEnvironmentVariableW(L"OBS_NR_VFG_QA_MONITOR", monId.c_str()) != FALSE, "set secondary display");
        if (high) {
            run_multiplier(4, presenter, (logDir / L"1440p-4x-mode-2.log").wstring(), monId, 60, Quality::High, true);
            return 0;
        }
        if (hotfix) {
            for (const auto quality : {Quality::Medium, Quality::Low})
                run_multiplier(4, presenter, (logDir / (L"1440p-4x-mode-" +
                    std::to_wstring(static_cast<unsigned>(quality)) + L".log")).wstring(), monId, 60, quality, true);
            return 0;
        }
        if (cadence) {
            for (const auto hz : {30u, 40u, 60u})
                run_multiplier(2, presenter, (logDir / (L"cadence-" + std::to_wstring(hz) + L".log")).wstring(), monId, hz);
            return 0;
        }
        run_multiplier(2, presenter, (logDir / L"smoke-2x.log").wstring(), monId);
        run_multiplier(3, presenter, (logDir / L"smoke-3x.log").wstring(), monId);
        run_multiplier(4, presenter, (logDir / L"smoke-4x.log").wstring(), monId);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_native_smoke: %s\n", e.what());
        return 1;
    }
}
