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
#include <map>
#include <sstream>
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

enum class Fixture { Standard, Hotfix, PacingAdaptive, SplitTelemetry, Parity };

void wait_initialized(const std::wstring& logPath, Controller& controller)
{
    const auto deadline = clock_ns() + 15'000'000'000ULL;
    while (clock_ns() < deadline) {
        {
            std::ifstream ifs(logPath);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.find("event=initialized backend=nvidia-vfg-native") != std::string::npos) return;
        }
        expect(controller.alive(), "presenter exited before initialization");
        Sleep(5);
    }
    throw std::runtime_error("timed out waiting for presenter initialization");
}

void run_multiplier(uint32_t mult, const std::wstring& presenterPath, const std::wstring& logPath,
    const std::wstring& monitorId, uint32_t cadence = 0, Quality quality = Quality::Medium, Fixture fixture = Fixture::Standard)
{
    const bool hotfix = fixture == Fixture::Hotfix, wiring = fixture == Fixture::PacingAdaptive;
    const bool split = fixture == Fixture::SplitTelemetry, parity = fixture == Fixture::Parity;
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

    ipc::Shape shape{hotfix ? 2560u : wiring || split ? 1280u : 1920u, hotfix ? 1440u : wiring || split ? 720u : 1080u,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    shape.splitCompare = split;
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

    const auto mappingKey = LR"(Local\nr-vfg-smoke-)" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(mult);
    ipc::Producer producer(dev.Get(), mappingKey, shape);
    LaunchOptions opts{};
    opts.multiplier = mult;
    opts.quality = quality;
    opts.monitorId = monitorId;
    opts.pacingRepair = wiring;
    opts.adaptiveFg = wiring;
    opts.adaptiveTargetFps = wiring ? 60u : 0u;
    Controller controller(opts);
    if (split) {
        controller.set_hud_lock(1);
        controller.set_pacing_repair(1);
        controller.set_adaptive_fg(1);
        controller.set_adaptive_target_fps(90);
    }

    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, shape, desc.AdapterLuid), "controller.offer");

    const DWORD waitHandshake = WaitForSingleObject(producer.control_event(), 10000);
    expect(waitHandshake == WAIT_OBJECT_0 && producer.status() == ipc::Status::Ok, "IPC handshake ready");
    if (parity) wait_initialized(logPath, controller);

    std::vector<uint32_t> row(shape.width * 64, 0xFF2080FFu);
    std::vector<uint32_t> background;
    if (cadence || parity) {
        background.resize(static_cast<size_t>(shape.width) * shape.height, 0xFF202020u);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, background.data(), shape.width * 4, 0);
    }
    const auto start = clock_ns();
    uint32_t last = UINT32_MAX, accepted = 0, repeated = 0;
    const uint32_t warmupCount = parity ? 16 : 0;
    const uint32_t count = parity ? (warmupCount + 120) : hotfix ? 360 : wiring ? 180 : cadence ? 96 : 45;
    uint64_t measureStartNs = 0;
    HANDLE paceTimer = parity
        ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)
        : nullptr;
    struct TimerClose { HANDLE h; ~TimerClose() { if (h) CloseHandle(h); } } paceTimerGuard{paceTimer};
    for (uint32_t f = 0; f < count; ++f) {
        const auto ts = start + uint64_t{f} * 1'000'000'000 / 60;
        if (cadence || parity) {
            while (clock_ns() < ts) {
                const auto rem = ts - (std::min)(ts, clock_ns());
                if (parity && paceTimer) {
                    if (rem <= 300'000ULL) { YieldProcessor(); continue; }
                    LARGE_INTEGER due{};
                    due.QuadPart = -static_cast<LONGLONG>((rem - 200'000ULL) / 100ULL);
                    if (SetWaitableTimer(paceTimer, &due, 0, nullptr, nullptr, FALSE) &&
                        WaitForSingleObject(paceTimer, static_cast<DWORD>(rem / 1'000'000ULL + 2)) != WAIT_FAILED)
                        continue;
                }
                if (rem > 2'000'000) Sleep(1);
                else SwitchToThread();
            }
        }
        if (parity && f == warmupCount) {
            measureStartNs = clock_ns();
            std::printf("PARITY_WINDOW mult=%ux start_ns=%llu first_serial=%u last_serial=%u\n",
                mult, static_cast<unsigned long long>(measureStartNs), warmupCount + 1, count);
        }
        const uint32_t unique = wiring ? (f / 4 * 2 + (f % 4 != 0)) : cadence ? f * cadence / 60 : f;
        const bool repeat = unique == last;
        if (!repeat) {
            if ((cadence || parity) && last != UINT32_MAX) {
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
        const auto pub = producer.publish(tex.Get(), ts, now, !hotfix && !wiring && !parity && cadence && f == 48,
            cadence ? ts : 0, split ? tex.Get() : nullptr, 50);
        expect(pub == ipc::Status::Ok || pub == ipc::Status::Dropped, "publish");
        if (pub == ipc::Status::Ok) {
            ++accepted;
            if (repeat) ++repeated;
        }
        if (!cadence && !parity) Sleep(16);
    }
    if (parity) Sleep(300);

    controller.stop();
    Sleep(100);
    for (int i = 0; i < 60 && controller.alive(); ++i) Sleep(50);
    expect(!controller.alive(), "presenter still alive after stop");
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
    expect(events("event=error") == 0, "presenter logged an error");
    if (split) {
        std::map<std::string, std::string> ingress;
        unsigned originals = 0, repeats = 0, mismatched = 0, wrongRepeat = 0;
        std::istringstream lines(content);
        for (std::string line; std::getline(lines, line);) {
            const bool input = line.starts_with("event=input ");
            const bool original = line.starts_with("event=present_original ");
            const bool repeat = line.starts_with("event=present_repeat_split ");
            if (!input && !original && !repeat) continue;
            const auto key = line.find("generation=");
            const auto keyEnd = line.find(" b_qpc_ns=");
            const auto values = line.find("eff_mult=");
            expect(key != std::string::npos && keyEnd > key && keyEnd != std::string::npos &&
                values != std::string::npos, "frame identity and A/B fields");
            const auto id = line.substr(key, keyEnd - key);
            const auto snapshot = line.substr(values);
            if (input) {
                expect(ingress.emplace(id, snapshot).second, "one input per frame");
                continue;
            }
            expect(line.find(" present_id=0 ") == std::string::npos, "observed actual present");
            originals += original;
            repeats += repeat;
            if (const auto it = ingress.find(id); it == ingress.end() || it->second != snapshot) ++mismatched;
            const auto flags = "hud_lock=1 shot_cut=0 pacing_repair=1 adaptive_fg=1 adaptive_target_fps=90";
            if (repeat && (!snapshot.starts_with("eff_mult=1 pacing_shift_ns=0 ") ||
                snapshot.find(flags) == std::string::npos)) ++wrongRepeat;
            if (original) expect(snapshot.find(flags) != std::string::npos, "original reports applied live flags");
        }
        std::printf("split telemetry originals=%u repeats=%u mismatched=%u wrong_repeat=%u\n",
            originals, repeats, mismatched, wrongRepeat);
        expect(originals && repeats && !mismatched && !wrongRepeat, "input and real split/original presents share actual frame telemetry");
    }
    if (wiring) {
        bool repaired = false, adaptivePair = false;
        std::istringstream lines(content);
        for (std::string line; std::getline(lines, line);) {
            if (line.starts_with("event=present_original ")) {
                constexpr std::string_view key = "pacing_shift_ns=";
                const auto shift = line.find(key);
                if (shift != std::string::npos && std::stoll(line.substr(shift + key.size())) != 0) repaired = true;
            }
            if (line.starts_with("event=stage_queued ") && line.find("eff_mult=2 ") != std::string::npos)
                adaptivePair = true;
        }
        expect(repaired && adaptivePair, "transport cadence repair and adaptive 2x pair reach the actual presenter");
    }
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
        const bool wiring = argc == 4 && std::wstring_view(argv[3]) == L"--pacing-adaptive";
        const bool split = argc == 4 && std::wstring_view(argv[3]) == L"--split-telemetry";
        const bool hotfix = argc == 4 && std::wstring_view(argv[3]) == L"--1440p-4x";
        const bool high = argc == 4 && std::wstring_view(argv[3]) == L"--1440p-4x-high";
        const bool parity = argc >= 4 && std::wstring_view(argv[3]) == L"--parity";
#ifdef OBS_NR_VFG_QA_ACTION
        constexpr bool allowPrimary = false;
#else
        const bool allowPrimary = (argc == 4 && std::wstring_view(argv[3]) == L"--allow-primary") ||
            (parity && argc == 5 && std::wstring_view(argv[4]) == L"--allow-primary");
#endif
        expect(argc == 3 || cadence || wiring || split || hotfix || high ||
            (parity && (argc == 4 || allowPrimary)) || (allowPrimary && argc == 4),
            "usage: vfg_native_smoke_tests <presenter.exe> <log_dir> [--cadence|--pacing-adaptive|--split-telemetry|--1440p-4x|--1440p-4x-high|--parity [--allow-primary]|--allow-primary]");
        const std::wstring presenter = argv[1];
        const std::filesystem::path logDir = argv[2];
        const auto monId = secondary_monitor_id();
        expect(allowPrimary || !monId.empty(), "no active secondary display");
        expect(SetEnvironmentVariableW(L"OBS_NR_VFG_QA_MONITOR", monId.empty() ? nullptr : monId.c_str()) != FALSE,
            "set QA monitor override");
        if (parity) {
            std::filesystem::create_directories(logDir);
            run_multiplier(2, presenter, (logDir / L"parity-2x.log").wstring(), monId, 0, Quality::Medium, Fixture::Parity);
            run_multiplier(4, presenter, (logDir / L"parity-4x.log").wstring(), monId, 0, Quality::Medium, Fixture::Parity);
            return 0;
        }
        if (split) {
            std::filesystem::create_directories(logDir);
            run_multiplier(2, presenter, (logDir / L"native-split-telemetry.log").wstring(), monId, 30,
                Quality::Medium, Fixture::SplitTelemetry);
            return 0;
        }
        if (wiring) {
            run_multiplier(4, presenter, (logDir / L"pacing-adaptive.log").wstring(), monId, 30, Quality::Medium, Fixture::PacingAdaptive);
            return 0;
        }
        if (high) {
            run_multiplier(4, presenter, (logDir / L"1440p-4x-mode-2.log").wstring(), monId, 60, Quality::High, Fixture::Hotfix);
            return 0;
        }
        if (hotfix) {
            for (const auto quality : {Quality::Medium, Quality::Low})
                run_multiplier(4, presenter, (logDir / (L"1440p-4x-mode-" +
                    std::to_wstring(static_cast<unsigned>(quality)) + L".log")).wstring(), monId, 60, quality, Fixture::Hotfix);
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
