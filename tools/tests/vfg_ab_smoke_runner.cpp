// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nr::fg;
using namespace nr::fg::vfg;

namespace {
namespace fs = std::filesystem;

struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
};

void expect(bool ok, const char* why)
{
    if (!ok) throw std::runtime_error(why);
}

std::string read_text(const fs::path& path)
{
    std::ifstream ifs(path);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

size_t count_tokens(const std::string& text, std::string_view token)
{
    size_t c = 0, pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        ++c;
        pos += token.size();
    }
    return c;
}

std::string find_line(const std::string& text, std::string_view prefix, std::string_view extra = {})
{
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (line.starts_with(prefix) && (extra.empty() || line.find(extra) != std::string::npos)) return line;
    }
    return {};
}

std::string field_value(const std::string& line, std::string_view key)
{
    const auto pos = line.find(key);
    if (pos == std::string::npos) return {};
    const auto start = pos + key.size();
    const auto end = line.find_first_of(" \r\n", start);
    return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::vector<std::wstring> list_starting_markers(const fs::path& dir)
{
    std::vector<std::wstring> out;
    std::error_code ec;
    const bool exists = fs::exists(dir, ec);
    expect(!ec, "check PushFlow marker directory existence");
    if (!exists) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (name == L"pushflow-starting.txt" ||
            (name.starts_with(L"pushflow-starting-") && name.ends_with(L".txt"))) {
            out.push_back(name);
        }
    }
    expect(!ec, "enumerate PushFlow marker directory");
    return out;
}

size_t count_starting_markers(const fs::path& dir)
{
    return list_starting_markers(dir).size();
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

struct GpuFixture {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    LUID luid{};
    ipc::Shape shape{};
    ComPtr<ID3D11Texture2D> tex;
    std::vector<uint32_t> scratch;
    std::vector<uint32_t> boxScratch;

    GpuFixture(uint32_t w, uint32_t h) : scratch(size_t{w} * h), boxScratch(size_t{64} * 64)
    {
        D3D_FEATURE_LEVEL fl{};
        expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &dev, &fl, &ctx)), "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        expect(SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc)), "adapter");
        luid = desc.AdapterLuid;
        shape = {w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = shape.format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        expect(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)), "CreateTexture2D");
    }

    void fill_frame(uint32_t bg, uint32_t fg, uint32_t boxX)
    {
        std::fill(scratch.begin(), scratch.end(), bg);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, scratch.data(), shape.width * 4, 0);
        std::fill(boxScratch.begin(), boxScratch.end(), fg);
        const D3D11_BOX box{boxX, shape.height / 2 - 32, 0, boxX + 64, shape.height / 2 + 32, 1};
        ctx->UpdateSubresource(tex.Get(), 0, &box, boxScratch.data(), 64 * 4, 0);
    }
};

void wait_log_token(const fs::path& logPath, std::string_view token, Controller& controller, uint64_t timeoutMs = 15000)
{
    const auto deadline = clock_ns() + timeoutMs * 1'000'000ULL;
    while (clock_ns() < deadline) {
        const auto txt = read_text(logPath);
        if (txt.find(token) != std::string::npos) return;
        expect(controller.alive(), "presenter exited before expected log token");
        Sleep(5);
    }
    throw std::runtime_error("timed out waiting for log token");
}

void pace_to(uint64_t targetNs)
{
    static thread_local struct HighResTimer {
        HANDLE h{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
        ~HighResTimer() { if (h) CloseHandle(h); }
    } timer;
    for (;;) {
        const uint64_t now = clock_ns();
        if (now >= targetNs) return;
        const auto rem = targetNs - now;
        if (rem <= 300'000ULL) { YieldProcessor(); continue; }
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((rem - 200'000ULL) / 100ULL);
        if (timer.h && SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer.h, static_cast<DWORD>(rem / 1'000'000ULL + 2)) != WAIT_FAILED)
            continue;
        if (rem > 2'000'000ULL) Sleep(1);
        else SwitchToThread();
    }
}

void wait_controller_exit(Controller& controller)
{
    controller.stop();
    Sleep(50);
    for (int i = 0; i < 80 && controller.alive(); ++i) Sleep(50);
    expect(!controller.alive(), "presenter still alive after stop");
}

void publish_ok_or_dropped(ipc::Producer& producer, ID3D11Texture2D* tex, uint64_t ts, uint64_t obsTick = 0)
{
    const auto pub = producer.publish(tex, ts, clock_ns(), false, obsTick, nullptr, 50);
    expect(pub == ipc::Status::Ok || pub == ipc::Status::Dropped, "publish");
}

std::wstring unique_mapping(std::wstring_view tag)
{
    static uint32_t seq = 0;
    return LR"(Local\nr-ab-)" + std::wstring(tag) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(++seq);
}

void run_shot_change(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId)
{
    const auto log = logDir / L"ab-shot-change.log";
    fs::remove(log);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    GpuFixture gfx(1280, 720);
    ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"shot"), gfx.shape);
    LaunchOptions opts{};
    opts.multiplier = 2;
    opts.gpuShotChange = 1;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
    wait_log_token(log, "event=initialized backend=nvidia-vfg-native", controller);

    const auto start = clock_ns();
    for (uint32_t f = 0; f < 60; ++f) {
        const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 60;
        pace_to(ts);
        if (f >= 20 && f < 35) gfx.fill_frame(0xFFFFFFFFu, 0xFF101010u, (f * 32) % 1000);
        else gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 32) % 1000);
        publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
    }
    Sleep(300);
    wait_controller_exit(controller);
    expect(producer.retire() == ipc::Status::Ok, "retire");

    const auto content = read_text(log);
    expect(content.find("gpu_shot_change=1") != std::string::npos, "gpu_shot_change=1 missing");
    expect(count_tokens(content, "shot_cut=1") >= 2, "expected shot_cut=1 on cut frames");
    expect(count_tokens(content, "event=present_vfg_subframe ") >= 10, "insufficient synthetic subframes");
    expect(count_tokens(content, "event=error") == 0, "unexpected error in shot-change");
    std::printf("PASS ab_shot_change cuts=%zu subframes=%zu\n",
        count_tokens(content, "shot_cut=1"), count_tokens(content, "event=present_vfg_subframe "));
}

void run_hud_lock(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId)
{
    const auto log = logDir / L"ab-hud-lock.log";
    fs::remove(log);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    GpuFixture gfx(1280, 720);
    ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"hud"), gfx.shape);
    LaunchOptions opts{};
    opts.multiplier = 2;
    opts.gpuShotChange = 1;
    opts.hudLock = 0;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
    wait_log_token(log, "event=initialized backend=nvidia-vfg-native", controller);

    const auto start = clock_ns();
    for (uint32_t f = 0; f < 90; ++f) {
        if (f == 30) controller.set_hud_lock(1);
        if (f == 60) controller.set_hud_lock(0);
        const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 60;
        pace_to(ts);
        if (f >= 45 && f < 55) gfx.fill_frame(0xFFFFFFFFu, 0xFF202020u, (f * 32) % 1000);
        else gfx.fill_frame(0xFF202020u, 0xFFFFFFFFu, (f * 32) % 1000);
        publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
    }
    Sleep(300);
    wait_controller_exit(controller);
    expect(producer.retire() == ipc::Status::Ok, "retire");

    const auto content = read_text(log);
    expect(count_tokens(content, "event=offer ") == 1, "must stay on same PID without restart");
    const auto p0 = content.find("hud_lock=0 ");
    const auto p1 = content.find("hud_lock=1 ", p0 == std::string::npos ? 0 : p0);
    const auto p2 = content.find("hud_lock=0 ", p1 == std::string::npos ? 0 : p1);
    expect(p0 != std::string::npos && p1 != std::string::npos && p2 != std::string::npos,
        "hud_lock must toggle live 0 -> 1 -> 0");
    expect(count_tokens(content, "shot_cut=1") >= 1, "expected shot_cut=1 during hud-lock run");
    expect(count_tokens(content, "event=present_vfg_subframe ") >= 20, "insufficient synthetic subframes");
    expect(count_tokens(content, "event=error") == 0, "unexpected error in hud-lock");
    std::printf("PASS ab_hud_lock subframes=%zu cuts=%zu\n",
        count_tokens(content, "event=present_vfg_subframe "), count_tokens(content, "shot_cut=1"));
}

void run_pacing_repair(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId)
{
    const auto log = logDir / L"ab-pacing-repair.log";
    fs::remove(log);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    GpuFixture gfx(1280, 720);
    ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pacing"), gfx.shape);
    LaunchOptions opts{};
    opts.multiplier = 2;
    opts.pacingRepair = 0;
    opts.adaptiveFg = 0;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
    wait_log_token(log, "event=initialized backend=nvidia-vfg-native", controller);

    const auto start = clock_ns();
    uint32_t lastUnique = UINT32_MAX;
    uint32_t serial = 0;
    const uint32_t ticksPerSeg = 112;
    for (uint32_t seg = 0; seg < 3; ++seg) {
        const uint32_t repairOn = (seg == 1) ? 1u : 0u;
        controller.set_pacing_repair(repairOn);
        const uint32_t segFirstSerial = serial + 1;
        for (uint32_t k = 0; k < ticksPerSeg; ++k) {
            const uint32_t tick = seg * ticksPerSeg + k;
            const auto ts = start + uint64_t{tick} * 1'000'000'000ULL / 60;
            pace_to(ts);
            const uint32_t unique = tick / 4 * 2 + (tick % 4 != 0);
            if (unique != lastUnique) {
                gfx.fill_frame(0xFF202020u, 0xFF20A0FFu, (unique * 40) % (gfx.shape.width - 128));
                lastUnique = unique;
            }
            const auto pub = producer.publish(gfx.tex.Get(), ts, clock_ns(), false, ts, nullptr, 50);
            expect(pub == ipc::Status::Ok || pub == ipc::Status::Dropped, "publish");
            if (pub == ipc::Status::Ok) ++serial;
        }
        std::printf("PACING_SEG seg=%u repair=%u first_serial=%u last_serial=%u\n",
            seg + 1, repairOn, segFirstSerial, serial);
    }
    Sleep(300);
    wait_controller_exit(controller);
    expect(producer.retire() == ipc::Status::Ok, "retire");

    const auto content = read_text(log);
    expect(count_tokens(content, "event=offer ") == 1, "must stay on same PID");
    expect(count_tokens(content, "event=present_vfg_subframe ") >= 30, "insufficient synthetic subframes");
    expect(count_tokens(content, "event=error") == 0, "unexpected error in pacing-repair");
    std::printf("PASS ab_pacing_repair_run subframes=%zu\n", count_tokens(content, "event=present_vfg_subframe "));
}

void run_adaptive(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId)
{
    const auto log = logDir / L"ab-adaptive.log";
    fs::remove(log);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    GpuFixture gfx(1280, 720);
    ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"adaptive"), gfx.shape);
    LaunchOptions opts{};
    opts.multiplier = 4;
    opts.adaptiveFg = 1;
    opts.adaptiveTargetFps = 120;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
    wait_log_token(log, "event=initialized backend=nvidia-vfg-native", controller);

    uint64_t cursorNs = clock_ns();
    uint32_t serial = 0;
    const struct SegSpec { uint32_t fps; uint32_t frames; } segs[]{{30, 60}, {60, 120}, {30, 60}};
    for (uint32_t s = 0; s < 3; ++s) {
        const uint64_t stepNs = 1'000'000'000ULL / segs[s].fps;
        const uint32_t firstSerial = serial + 1;
        for (uint32_t f = 0; f < segs[s].frames; ++f) {
            cursorNs += stepNs;
            pace_to(cursorNs);
            gfx.fill_frame(0xFF181818u, 0xFFFF8020u, ((serial + 1) * 32) % (gfx.shape.width - 128));
            const auto pub = producer.publish(gfx.tex.Get(), cursorNs, clock_ns(), false, 0, nullptr, 50);
            expect(pub == ipc::Status::Ok || pub == ipc::Status::Dropped, "publish");
            if (pub == ipc::Status::Ok) ++serial;
        }
        std::printf("ADAPTIVE_SEG seg=%u fps=%u first_serial=%u last_serial=%u\n",
            s + 1, segs[s].fps, firstSerial, serial);
    }
    Sleep(300);
    wait_controller_exit(controller);
    expect(producer.retire() == ipc::Status::Ok, "retire");

    const auto content = read_text(log);
    expect(count_tokens(content, "event=offer ") == 1, "must stay on same PID");
    expect(count_tokens(content, "event=error") == 0, "unexpected error in adaptive");
    std::printf("PASS ab_adaptive_run subframes=%zu\n", count_tokens(content, "event=present_vfg_subframe "));
}

void run_pushflow_smoke(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime)
{
    for (const uint32_t mult : {2u, 3u, 4u}) {
        const auto log = logDir / (L"ab-pushflow-" + std::to_wstring(mult) + L"x.log");
        fs::remove(log);
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
        GpuFixture gfx(1280, 720);
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pf"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = mult;
        opts.gpuShotChange = 1;
        opts.hudLock = 1;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
        wait_log_token(log, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 40; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            if (f >= 20 && f < 28) gfx.fill_frame(0xFFFFFFFFu, 0xFF101010u, (f * 32) % 1000);
            else gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 32) % 1000);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire");

        const auto content = read_text(log);
        expect(content.find("event=initialized backend=pushflow") != std::string::npos, "pushflow init missing");
        expect(content.find("gpu_shot_change=1") != std::string::npos, "gpu_shot_change=1 missing");
        expect(content.find("hud_lock=1") != std::string::npos, "hud_lock=1 missing");
        expect(count_tokens(content, "shot_cut=1") >= 1, "shot_cut=1 missing in PushFlow");
        expect(count_tokens(content, "event=present_vfg_subframe ") >= (mult - 1) * 5, "insufficient PushFlow subframes");
        expect(count_tokens(content, "event=error") == 0, "unexpected error in PushFlow smoke");
        std::printf("PASS ab_pushflow mult=%ux subframes=%zu cuts=%zu\n",
            mult, count_tokens(content, "event=present_vfg_subframe "), count_tokens(content, "shot_cut=1"));
    }

    // Same-PID live HUD Lock 0 -> 1 -> 0 toggle on PushFlow
    {
        const auto log = logDir / L"ab-pushflow-hud-toggle.log";
        fs::remove(log);
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
        GpuFixture gfx(1280, 720);
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfhud"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.gpuShotChange = 1;
        opts.hudLock = 0;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer pf hud");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake pf hud");
        wait_log_token(log, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 60; ++f) {
            if (f == 20) controller.set_hud_lock(1);
            if (f == 40) controller.set_hud_lock(0);
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            if (f >= 28 && f < 34) gfx.fill_frame(0xFFFFFFFFu, 0xFF202020u, (f * 32) % 1000);
            else gfx.fill_frame(0xFF202020u, 0xFFFFFFFFu, (f * 32) % 1000);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire pf hud");

        const auto content = read_text(log);
        expect(count_tokens(content, "event=offer ") == 1, "PushFlow HUD toggle must stay on same PID");
        const auto p0 = content.find("hud_lock=0 ");
        const auto p1 = content.find("hud_lock=1 ", p0 == std::string::npos ? 0 : p0);
        const auto p2 = content.find("hud_lock=0 ", p1 == std::string::npos ? 0 : p1);
        expect(p0 != std::string::npos && p1 != std::string::npos && p2 != std::string::npos,
            "PushFlow hud_lock must toggle live 0 -> 1 -> 0");
        expect(count_tokens(content, "shot_cut=1") >= 1, "expected shot_cut=1 during PushFlow hud-toggle");
        expect(count_tokens(content, "event=present_vfg_subframe ") >= 10, "insufficient PushFlow hud-toggle subframes");
        expect(count_tokens(content, "event=error") == 0, "unexpected error in PushFlow hud-toggle");
        std::printf("PASS ab_pushflow_hud_toggle subframes=%zu cuts=%zu\n",
            count_tokens(content, "event=present_vfg_subframe "), count_tokens(content, "shot_cut=1"));
    }
}

void run_pushflow_stalled_esc(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    const auto log = logDir / L"ab-pushflow-stalled-esc.log";
    fs::remove(log);
    fs::remove_all(marksDir);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", L"2500");

    GpuFixture gfx(640, 360);
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfesc"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
        wait_log_token(log, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 30; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
            if (read_text(log).find("event=present_vfg_subframe ") != std::string::npos && f >= 5) break;
        }
        wait_log_token(log, "event=present_vfg_subframe ", controller);
        const auto offerLine = find_line(read_text(log), "event=offer ");
        const auto hwndVal = std::stoull(field_value(offerLine, "hwnd="), nullptr, 16);
        const auto escSentNs = clock_ns();
        expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
            "PostMessage VK_ESCAPE");
        const auto exitDeadline = escSentNs + 2'200'000'000ULL;
        while (controller.alive() && clock_ns() < exitDeadline) Sleep(10);
        const auto escElapsedMs = (clock_ns() - escSentNs) / 1'000'000ULL;
        expect(!controller.alive(), "stalled Esc presenter did not force-exit within 2.2s");
        expect(controller.await_exit_cleanup(500), "stalled Esc exit cleanup timed out");
        expect(controller.user_close_marker_cleaned(), "stalled Esc marker cleanup failed");
        expect(!controller.can_recover(), "UserClosed must disable relaunch");
        expect(producer.retire() == ipc::Status::Ok, "retire");
        const auto c1 = read_text(log);
        expect(c1.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
            "expected UserClosed force terminate");
        expect(count_tokens(c1, "event=controller_auto_fallback_1x_relaunch") == 0, "expected 0 relaunches");
        expect(count_starting_markers(marksDir) == 0, "pushflow starting marker must be cleaned on Esc");
        expect(!fs::exists(marksDir / L"pushflow-crash-quarantine.txt"), "Esc must not write quarantine");
        std::printf("PASS ab_pushflow_stalled_esc exit_ms=%llu\n", static_cast<unsigned long long>(escElapsedMs));
    }

    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    const auto log2 = logDir / L"ab-pushflow-stalled-esc-reopen.log";
    fs::remove(log2);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log2.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfreopen"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer reopen");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake reopen");
        wait_log_token(log2, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 35; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire reopen");

        const auto c2 = read_text(log2);
        expect(c2.find("event=pushflow_previous_crash_detected") == std::string::npos,
            "stalled Esc reopen must NOT trigger false crash quarantine");
        expect(count_tokens(c2, "event=present_vfg_subframe ") >= 5, "reopen must resume PushFlow synthesis");
        expect(count_tokens(c2, "event=error") == 0, "unexpected error on reopen");
        std::printf("PASS ab_pushflow_stalled_esc_reopen subframes=%zu\n",
            count_tokens(c2, "event=present_vfg_subframe "));
    }
}

void run_pushflow_userstop_stalled(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    fs::remove_all(marksDir);
    GpuFixture gfx(640, 360);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());

    // Explicit Controller::stop(true) during stalled PushFlow teardown must stamp UserClosed & clean starting marker
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", L"2500");
    const auto logStop = logDir / L"ab-pushflow-userstop-stalled.log";
    fs::remove(logStop);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logStop.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfstop"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer userstop");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake userstop");
        wait_log_token(logStop, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 25; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
            if (read_text(logStop).find("event=present_vfg_subframe ") != std::string::npos && f >= 5) break;
        }
        wait_log_token(logStop, "event=present_vfg_subframe ", controller);
        const auto stopStart = clock_ns();
        controller.stop(true);
        const auto stopDeadline = stopStart + 2'200'000'000ULL;
        while (controller.alive() && clock_ns() < stopDeadline) Sleep(10);
        const auto stopMs = (clock_ns() - stopStart) / 1'000'000ULL;
        expect(!controller.alive() && stopMs < 2200, "Controller::stop(true) must force-exit stalled PushFlow");
        expect(controller.await_exit_cleanup(500), "userstop exit cleanup timed out");
        expect(controller.user_close_marker_cleaned(), "userstop marker cleanup failed");
        expect(producer.retire() == ipc::Status::Ok, "retire userstop");
        const auto cs = read_text(logStop);
        expect(cs.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
            "Controller::stop(true) must preserve UserClosed on force terminate");
        expect(count_starting_markers(marksDir) == 0, "pushflow starting marker must be cleaned on user stop");
        expect(!fs::exists(marksDir / L"pushflow-crash-quarantine.txt"), "user stop must not write quarantine");
        std::printf("PASS ab_pushflow_userstop_stalled exit_ms=%llu\n", static_cast<unsigned long long>(stopMs));
    }

    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    const auto logStopReopen = logDir / L"ab-pushflow-userstop-reopen.log";
    fs::remove(logStopReopen);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logStopReopen.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfstop-re"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer userstop reopen");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake userstop reopen");
        wait_log_token(logStopReopen, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 35; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire userstop reopen");

        const auto cr = read_text(logStopReopen);
        expect(cr.find("event=pushflow_previous_crash_detected") == std::string::npos,
            "user stop reopen must NOT trigger false crash quarantine");
        expect(count_tokens(cr, "event=present_vfg_subframe ") >= 5, "reopen after user stop must resume PushFlow");
        expect(count_tokens(cr, "event=error") == 0, "unexpected error on user stop reopen");
        std::printf("PASS ab_pushflow_userstop_reopen subframes=%zu\n",
            count_tokens(cr, "event=present_vfg_subframe "));
    }
}

void run_pushflow_midrun_crash(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    const auto log = logDir / L"ab-pushflow-midrun-crash.log";
    fs::remove(log);
    fs::remove_all(marksDir);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", L"2500");

    GpuFixture gfx(640, 360);
    {
        auto producer = std::make_unique<ipc::Producer>(gfx.dev.Get(), unique_mapping(L"pfmid"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer->connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
        expect(WaitForSingleObject(producer->control_event(), 10000) == WAIT_OBJECT_0, "handshake");
        wait_log_token(log, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 160; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 60;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 16) % 480);
            publish_ok_or_dropped(*producer, gfx.tex.Get(), ts);
            if (f >= 135 && count_tokens(read_text(log), "event=present_vfg_subframe ") >= 125) break;
        }
        Sleep(150);
        const auto preKill = read_text(log);
        const auto steadySub = count_tokens(preKill, "event=present_vfg_subframe ");
        expect(steadySub >= 125, "expected >= 125 steady PushFlow inferences before kill");
        expect(count_starting_markers(marksDir) == 0,
            "pushflow starting marker must already be cleared after 120 steady inferences");

        const auto oldOffer = find_line(preKill, "event=offer ", "epoch=1");
        const auto oldPid = std::stoul(field_value(oldOffer, "pid="));
        Handle child{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, oldPid)};
        expect(child.value != nullptr, "open PushFlow child for mid-run kill");
        expect(TerminateProcess(child.value, 9) != FALSE, "TerminateProcess mid-run");
        expect(WaitForSingleObject(child.value, 2000) == WAIT_OBJECT_0, "child reaped");
        const auto retiredGen = producer->generation();
        expect(producer->retire() == ipc::Status::Ok, "retire crashed generation");
        producer.reset();
        for (int i = 0; i < 40 && ipc::Producer::generation_retained(retiredGen); ++i) Sleep(5);

        expect(controller.can_recover(), "controller must be eligible for 1x recovery");
        expect(controller.begin_async_fallback_relaunch(), "begin async fallback relaunch");
        const auto recDeadline = clock_ns() + 10'000'000'000ULL;
        bool reconnected = false;
        uint32_t recFrame = 0;
        while (clock_ns() < recDeadline) {
            if (!reconnected && controller.poll_async_fallback_relaunch() == RelaunchState::Ready) {
                producer = std::make_unique<ipc::Producer>(gfx.dev.Get(), unique_mapping(L"pfmid-rec"), gfx.shape);
                const auto recOffer = producer->connect(controller.peer());
                expect(controller.offer(recOffer, gfx.shape, gfx.luid), "offer recovery 1x");
                expect(WaitForSingleObject(producer->control_event(), 10000) == WAIT_OBJECT_0, "handshake recovery");
                reconnected = true;
            }
            if (reconnected) {
                if (recFrame == 0) {
                    gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, 16);
                    publish_ok_or_dropped(*producer, gfx.tex.Get(), clock_ns());
                    ++recFrame;
                }
                const auto txt = read_text(log);
                const auto recLine = find_line(txt, "event=offer ", "epoch=2");
                if (!recLine.empty()) {
                    const auto gen = "generation=" + field_value(recLine, "generation=") + " ";
                    if (!find_line(txt, "event=present_original ", gen).empty()) break;
                }
            }
            Sleep(16);
        }
        Sleep(30);
        expect(reconnected, "async fallback relaunch never reached Ready");
        const auto recTxt = read_text(log);
        const auto recOfferLine = find_line(recTxt, "event=offer ", "epoch=2");
        expect(!recOfferLine.empty(), "missing epoch=2 offer");
        const auto newPid = std::stoul(field_value(recOfferLine, "pid="));
        expect(newPid != oldPid && field_value(recOfferLine, "mode=") == "1x", "recovered PID/1x mismatch");
        expect(fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
            "Controller must persist pushflow-crash-quarantine.txt on mid-run crash");

        const auto recHwnd = std::stoull(field_value(recOfferLine, "hwnd="), nullptr, 16);
        expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(recHwnd)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
            "Esc on recovered 1x window");
        const auto escDeadline = clock_ns() + 2'200'000'000ULL;
        while (controller.alive() && clock_ns() < escDeadline) Sleep(10);
        expect(!controller.alive(), "stalled Esc on recovered 1x window did not exit in time");
        expect(controller.await_exit_cleanup(500), "recovered 1x Esc exit cleanup timed out");
        expect(producer->retire() == ipc::Status::Ok, "retire recovered");
        const auto afterRecEsc = read_text(log);
        expect(afterRecEsc.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
            "expected UserClosed force-terminate on recovered 1x window");
        expect(count_tokens(afterRecEsc, "event=controller_auto_fallback_1x_relaunch") == 1,
            "expected exactly 1 relaunch total");
        expect(fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
            "pushflow-crash-quarantine.txt must survive Esc force-terminate of recovered 1x window");
    }

    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    wchar_t selfExe[4096]{};
    const auto selfLen = GetModuleFileNameW(nullptr, selfExe, ARRAYSIZE(selfExe));
    expect(selfLen > 0 && selfLen < ARRAYSIZE(selfExe), "locate vfg_ab_smoke_runner.exe");
    std::wstring cmd = L"\"" + std::wstring(selfExe) + L"\" \"" + presenter + L"\" \"" + logDir.wstring() +
        L"\" pushflow-midrun-crash-manual";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::fflush(stdout);
    expect(CreateProcessW(selfExe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi) != FALSE,
        "spawn pushflow-midrun-crash-manual");
    const Handle childProc{pi.hProcess}, childThread{pi.hThread};
    expect(WaitForSingleObject(childProc.value, 60000) == WAIT_OBJECT_0, "manual reopen child timed out");
    DWORD childCode = 1;
    expect(GetExitCodeProcess(childProc.value, &childCode) != FALSE && childCode == 0,
        "pushflow-midrun-crash-manual failed");
    std::printf("PASS ab_pushflow_midrun_crash\n");
}

void run_pushflow_midrun_crash_manual(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    GpuFixture gfx(640, 360);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_TEARDOWN_MS", nullptr);
    const auto logManual1 = logDir / L"ab-pushflow-manual1.log";
    fs::remove(logManual1);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logManual1.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfm1"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer manual1");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake manual1");
        wait_log_token(logManual1, "event=initialized ", controller);
        const auto start = clock_ns();
        for (uint32_t f = 0; f < 30; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 60;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 16) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(200);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire manual1");
        const auto m1 = read_text(logManual1);
        expect(m1.find("event=pushflow_previous_crash_detected") != std::string::npos,
            "1st manual reopen must detect previous crash quarantine");
        expect(!fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
            "1st manual reopen must consume quarantine");
        expect(count_tokens(m1, "event=present_original ") > 0 &&
            count_tokens(m1, "event=present_vfg_subframe ") == 0,
            "1st manual reopen must degrade once to 1x original");
    }

    const auto logManual2 = logDir / L"ab-pushflow-manual2.log";
    fs::remove(logManual2);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logManual2.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfm2"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer manual2");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake manual2");
        wait_log_token(logManual2, "event=initialized backend=pushflow", controller);
        const auto start = clock_ns();
        for (uint32_t f = 0; f < 40; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 16) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire manual2");
        const auto m2 = read_text(logManual2);
        expect(m2.find("event=pushflow_previous_crash_detected") == std::string::npos,
            "2nd manual reopen must not trigger crash quarantine");
        expect(count_tokens(m2, "event=present_vfg_subframe ") >= 5, "2nd manual reopen must resume PushFlow");
        expect(count_tokens(m2, "event=error") == 0, "unexpected error in manual2");
    }
    std::printf("PASS ab_pushflow_midrun_crash_manual\n");
}

void run_pushflow_empty(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId)
{
    const auto log = logDir / L"ab-pushflow-empty.log";
    const auto emptyRt = logDir / L"empty-pushflow-rt";
    fs::remove(log);
    fs::create_directories(emptyRt);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", emptyRt.c_str());

    GpuFixture gfx(640, 360);
    ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfempty"), gfx.shape);
    LaunchOptions opts{};
    opts.backend = Backend::PushFlow;
    opts.pushflowTier = PushFlowTier::Fast;
    opts.multiplier = 2;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake");
    wait_log_token(log, "event=pushflow_runtime_missing_fallback_1x", controller);
    const auto start = clock_ns();
    for (uint32_t f = 0; f < 30; ++f) {
        const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 60;
        pace_to(ts);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 16) % 480);
        publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
    }
    Sleep(200);
    wait_controller_exit(controller);
    expect(producer.retire() == ipc::Status::Ok, "retire");

    const auto content = read_text(log);
    expect(content.find("event=pushflow_runtime_missing_fallback_1x") != std::string::npos,
        "missing pushflow_runtime_missing_fallback_1x");
    expect(count_tokens(content, "event=present_original ") > 0 &&
        count_tokens(content, "event=present_vfg_subframe ") == 0,
        "empty PushFlow runtime must present 1x original only");
    std::printf("PASS ab_pushflow_empty originals=%zu\n", count_tokens(content, "event=present_original "));
}

void run_stalled_log_esc(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    const auto log = logDir / L"ab-stalled-log-esc.log";
    fs::remove(log);
    fs::remove_all(marksDir);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_LOG_LOCK_MS", L"5000");

    GpuFixture gfx(640, 360);
    ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfloglock"), gfx.shape);
    LaunchOptions opts{};
    opts.backend = Backend::PushFlow;
    opts.pushflowTier = PushFlowTier::Fast;
    opts.pushflowRuntimeDir = pfRuntime;
    opts.multiplier = 2;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer log lock");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake log lock");
    wait_log_token(log, "event=test_log_locked", controller);

    const auto offerLine = find_line(read_text(log), "event=offer ");
    const auto hwndVal = std::stoull(field_value(offerLine, "hwnd="), nullptr, 16);
    const auto escSentNs = clock_ns();
    expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
        "PostMessage VK_ESCAPE during locked log");
    const auto exitDeadline = escSentNs + 2'200'000'000ULL;
    while (controller.alive() && clock_ns() < exitDeadline) Sleep(10);
    const auto escElapsedMs = (clock_ns() - escSentNs) / 1'000'000ULL;
    expect(!controller.alive(), "presenter holding FILE* lock did not force-exit within 2.2s");
    expect(controller.await_exit_cleanup(500), "locked log Esc exit cleanup timed out");
    expect(controller.user_close_marker_cleaned(), "locked log Esc marker cleanup failed");
    expect(producer.retire() == ipc::Status::Ok, "retire log lock");

    const auto c = read_text(log);
    expect(c.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
        "expected unshared shutdown_force_terminate diagnostic");
    expect(count_starting_markers(marksDir) == 0, "pushflow starting marker must be cleaned even when worker holds log lock");
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_LOG_LOCK_MS", nullptr);
    std::printf("PASS ab_stalled_log_esc exit_ms=%llu\n", static_cast<unsigned long long>(escElapsedMs));
}

struct BlockingLogPipe {
    std::wstring pipeName;
    HANDLE pipe{};
    HANDLE stopEvent{};
    std::string captured;
    std::mutex mutex;
    std::atomic_bool tokenSeen{false};
    std::thread reader;

    BlockingLogPipe(std::wstring_view tag, std::string stopAfterToken)
    {
        static uint32_t seq = 0;
        pipeName = LR"(\\.\pipe\obs-nr-stalled-log-)" + std::wstring(tag) + L"-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++seq);
        pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, nullptr);
        expect(pipe != INVALID_HANDLE_VALUE, "CreateNamedPipeW for blocking log pipe");
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        expect(stopEvent != nullptr, "CreateEventW for blocking log pipe");
        reader = std::thread([this, token = std::move(stopAfterToken)] {
            if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
            char buf[64]{};
            DWORD got{};
            while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
                DWORD avail{};
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
                if (!avail) {
                    Sleep(5);
                    continue;
                }
                if (!ReadFile(pipe, buf, static_cast<DWORD>(std::min<DWORD>(avail, sizeof(buf))), &got, nullptr) || !got)
                    break;
                bool pauseNow = false;
                {
                    std::lock_guard lock(mutex);
                    captured.append(buf, got);
                    const auto pos = token.empty() ? std::string::npos : captured.find(token);
                    if (pos != std::string::npos && captured.find('\n', pos) != std::string::npos) {
                        tokenSeen.store(true, std::memory_order_release);
                        pauseNow = true;
                    }
                }
                if (pauseNow) {
                    WaitForSingleObject(stopEvent, INFINITE);
                    break;
                }
            }
        });
    }

    DWORD avail_bytes() const
    {
        DWORD avail{};
        if (pipe != INVALID_HANDLE_VALUE && PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
            return avail;
        return 0;
    }

    std::string snapshot()
    {
        std::lock_guard lock(mutex);
        return captured;
    }

    void close()
    {
        if (stopEvent) SetEvent(stopEvent);
        if (pipe && pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (reader.joinable()) reader.join();
        if (stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
    }

    ~BlockingLogPipe() { close(); }
};

struct BlockedWriteWitness {
    DWORD sample1AvailBytes{};
    DWORD sample2AvailBytes{};
    uint64_t dwellMs{};
    uint32_t framesFed{};
};

BlockedWriteWitness saturate_and_confirm_blocked_write(BlockingLogPipe& logPipe, GpuFixture& gfx,
    ipc::Producer& producer, Controller& controller, uint32_t startFrame = 1)
{
    const auto confirmSustainedBlock = [&](uint32_t framesFed) -> std::optional<BlockedWriteWitness> {
        const auto avail1 = logPipe.avail_bytes();
        if (avail1 == 0 || !controller.log_write_in_flight()) return std::nullopt;
        const auto t0 = clock_ns();
        Sleep(80);
        const auto dwellMs = (clock_ns() - t0) / 1'000'000ULL;
        const auto avail2 = logPipe.avail_bytes();
        if (controller.log_write_in_flight() && avail2 == avail1 && dwellMs >= 70) {
            return BlockedWriteWitness{avail1, avail2, dwellMs, framesFed};
        }
        return std::nullopt;
    };

    const auto start = clock_ns();
    const auto deadline = start + 6'000'000'000ULL;
    uint32_t f = 0;
    while (clock_ns() < deadline) {
        if (const auto witnessed = confirmSustainedBlock(f)) return *witnessed;
        const auto ts = start + uint64_t{f} * 16'666'667ULL;
        pace_to(ts);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, ((startFrame + f) * 20) % 480);
        const auto pub = producer.publish(gfx.tex.Get(), ts, clock_ns(), false, 0, nullptr, 50);
        if (pub != ipc::Status::Ok && pub != ipc::Status::Dropped) {
            if (const auto witnessed = confirmSustainedBlock(f)) return *witnessed;
            throw std::runtime_error("publish failed without sustained kernel pipe WriteFile block");
        }
        ++f;
    }
    throw std::runtime_error("failed to observe real kernel pipe WriteFile block (avail=" +
        std::to_string(logPipe.avail_bytes()) + " in_flight=" +
        std::to_string(controller.log_write_in_flight() ? 1 : 0) + ")");
}

void write_pipe_witness_log(const fs::path& path, const std::string& header, const std::string& pipeSnapshot)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << header << "\n--- PIPE CAPTURE BEFORE BLOCK ---\n" << pipeSnapshot << "\n";
    out.close();
    expect(out.good(), "write pipe witness log");
}

void run_watchdog_stalled_pipe_log(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    fs::remove_all(marksDir);
    BlockingLogPipe logPipe(L"wd", "event=present_original ");
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logPipe.pipeName.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());

    GpuFixture gfx(640, 360);
    auto producer = std::make_unique<ipc::Producer>(gfx.dev.Get(), unique_mapping(L"pfwdpipe"), gfx.shape);
    LaunchOptions opts{};
    opts.backend = Backend::PushFlow;
    opts.pushflowTier = PushFlowTier::Fast;
    opts.pushflowRuntimeDir = pfRuntime;
    opts.multiplier = 2;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto oldPid = controller.peer().pid;
    const auto offer = producer->connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer watchdog pipe");
    expect(WaitForSingleObject(producer->control_event(), 10000) == WAIT_OBJECT_0, "handshake watchdog pipe");

    // Feed warmup frames until the first present_original line is read from the pipe, then reader pauses.
    const auto warmStart = clock_ns();
    uint32_t warmFrame = 0;
    const auto waitDeadline = warmStart + 15'000'000'000ULL;
    while (!logPipe.tokenSeen.load(std::memory_order_acquire) && clock_ns() < waitDeadline) {
        expect(controller.alive(), "presenter exited before present_original on pipe");
        const auto ts = warmStart + uint64_t{warmFrame} * 16'666'667ULL;
        pace_to(ts);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (warmFrame * 20) % 480);
        publish_ok_or_dropped(*producer, gfx.tex.Get(), ts);
        ++warmFrame;
    }
    expect(logPipe.tokenSeen.load(std::memory_order_acquire), "timed out waiting for present_original on pipe");
    expect(count_starting_markers(marksDir) > 0, "pushflow starting marker must be armed before watchdog");

    const auto witness = saturate_and_confirm_blocked_write(logPipe, gfx, *producer, controller, warmFrame);
    expect(controller.alive(), "presenter should still be blocked in WriteFile when witness observed");

    const auto stallStart = clock_ns();
    const auto deadline = stallStart + 4'500'000'000ULL;
    while (controller.alive() && clock_ns() < deadline) Sleep(10);
    const auto watchdogElapsedMs = (clock_ns() - stallStart) / 1'000'000ULL;
    expect(!controller.alive(), "no-input watchdog must terminate despite blocked kernel WriteFile");
    expect(controller.await_exit_cleanup(500), "watchdog exit cleanup must remain bounded despite blocked pipe");
    expect(controller.exit_reason() == ExitReason::Watchdog, "watchdog must preserve ExitReason::Watchdog");
    expect(count_starting_markers(marksDir) > 0, "watchdog must NOT clear starting marker");
    const auto pipeSnap = logPipe.snapshot();
    expect(producer->retire() == ipc::Status::Ok, "retire watchdog pipe");
    producer.reset();
    logPipe.close();

    write_pipe_witness_log(logDir / L"ab-watchdog-stalled-pipe-log.log",
        "WITNESS scenario=watchdog sample1_avail=" + std::to_string(witness.sample1AvailBytes) +
        " sample2_avail=" + std::to_string(witness.sample2AvailBytes) +
        " pipe_avail_bytes=" + std::to_string(witness.sample2AvailBytes) +
        " dwell_ms=" + std::to_string(witness.dwellMs) +
        " frames_fed=" + std::to_string(witness.framesFed) +
        " log_write_in_flight=1 old_pid=" + std::to_string(oldPid) +
        " exit_ms=" + std::to_string(watchdogElapsedMs) + " exit_reason=3",
        pipeSnap);

    const auto recLog = logDir / L"ab-watchdog-stalled-pipe-recovery.log";
    fs::remove(recLog);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", recLog.c_str());

    expect(controller.can_recover() && controller.begin_async_fallback_relaunch(),
        "watchdog must remain eligible for 1x fallback recovery");
    bool reachedReady = false;
    const auto recDeadline = clock_ns() + 8'000'000'000ULL;
    while (clock_ns() < recDeadline) {
        if (controller.poll_async_fallback_relaunch() == RelaunchState::Ready) {
            reachedReady = true;
            break;
        }
        Sleep(10);
    }
    expect(reachedReady, "watchdog recovery must reach RelaunchState::Ready");
    expect(fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
        "watchdog recovery must persist crash quarantine");
    const auto newPid = controller.peer().pid;
    expect(newPid != 0 && newPid != oldPid, "watchdog recovery must launch a new presenter PID");

    {
        ipc::Producer recProducer(gfx.dev.Get(), unique_mapping(L"pfwdrec"), gfx.shape);
        const auto recOffer = recProducer.connect(controller.peer());
        expect(controller.offer(recOffer, gfx.shape, gfx.luid), "offer watchdog 1x recovery");
        expect(WaitForSingleObject(recProducer.control_event(), 10000) == WAIT_OBJECT_0,
            "handshake watchdog 1x recovery");
        wait_log_token(recLog, "event=initialized ", controller);
        const auto recStart = clock_ns();
        for (uint32_t f = 0; f < 25; ++f) {
            const auto ts = recStart + uint64_t{f} * 16'666'667ULL;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(recProducer, gfx.tex.Get(), ts);
        }
        wait_log_token(recLog, "event=present_original ", controller);
        wait_controller_exit(controller);
        expect(recProducer.retire() == ipc::Status::Ok, "retire watchdog 1x recovery");
    }
    const auto recText = read_text(recLog);
    expect(count_tokens(recText, "event=present_original ") >= 5 &&
        count_tokens(recText, "event=present_vfg_subframe ") == 0,
        "watchdog recovered child must present 1x original frames only");
    std::printf("PASS ab_watchdog_stalled_pipe_log old_pid=%lu new_pid=%lu pipe_avail=%lu dwell_ms=%llu exit_ms=%llu orig=%zu\n",
        oldPid, newPid, witness.sample2AvailBytes, static_cast<unsigned long long>(witness.dwellMs),
        static_cast<unsigned long long>(watchdogElapsedMs),
        count_tokens(recText, "event=present_original "));
}

void run_pushflow_pipe_close_case(const std::wstring& presenter, const fs::path& logDir,
    const std::wstring& monId, const std::wstring& pfRuntime, const fs::path& marksDir, bool useControllerStop)
{
    fs::remove_all(marksDir);
    const std::wstring tag = useControllerStop ? L"userstop" : L"esc";
    const auto witnessPath = logDir / (useControllerStop ? L"ab-pushflow-userstop-stalled-pipe-log.log"
                                                         : L"ab-pushflow-esc-stalled-pipe-log.log");
    const auto reopenPath = logDir / (useControllerStop ? L"ab-pushflow-userstop-stalled-pipe-log-reopen.log"
                                                        : L"ab-pushflow-esc-stalled-pipe-log-reopen.log");
    BlockingLogPipe logPipe(tag, "event=present_original ");
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logPipe.pipeName.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());

    GpuFixture gfx(640, 360);
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(useControllerStop ? L"pfustoppipe" : L"pfescpipe"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto oldPid = controller.peer().pid;
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer pipe close");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake pipe close");

        const auto warmStart = clock_ns();
        uint32_t warmFrame = 0;
        const auto waitDeadline = warmStart + 15'000'000'000ULL;
        while (!logPipe.tokenSeen.load(std::memory_order_acquire) && clock_ns() < waitDeadline) {
            expect(controller.alive(), "presenter exited before present_original on pipe");
            const auto ts = warmStart + uint64_t{warmFrame} * 16'666'667ULL;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (warmFrame * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
            ++warmFrame;
        }
        expect(logPipe.tokenSeen.load(std::memory_order_acquire), "timed out waiting for present_original on pipe");
        expect(count_starting_markers(marksDir) > 0, "pushflow starting marker must exist before close");

        const auto witness = saturate_and_confirm_blocked_write(logPipe, gfx, producer, controller, warmFrame);
        expect(controller.alive(), "presenter should still be blocked in WriteFile before close");

        const auto closeSentNs = clock_ns();
        if (useControllerStop) {
            controller.request_stop(true);
        } else {
            const auto offerLine = find_line(logPipe.snapshot(), "event=offer ");
            const auto hwndVal = std::stoull(field_value(offerLine, "hwnd="), nullptr, 16);
            expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
                "PostMessage VK_ESCAPE while pipe WriteFile blocked");
        }
        const auto exitDeadline = closeSentNs + 2'200'000'000ULL;
        while (controller.alive() && clock_ns() < exitDeadline) Sleep(10);
        const auto closeElapsedMs = (clock_ns() - closeSentNs) / 1'000'000ULL;
        if (useControllerStop) controller.stop(true);
        expect(!controller.alive(), "user close must exit within 2.2s even while pipe WriteFile blocked");
        expect(controller.await_exit_cleanup(500), "user close exit cleanup must remain bounded despite blocked pipe");
        expect(controller.user_close_marker_cleaned(), "user close marker cleanup must report success");
        expect(controller.exit_reason() == ExitReason::UserClosed, "user close must preserve UserClosed");
        expect(count_starting_markers(marksDir) == 0,
            "pushflow starting marker must be cleaned independently of blocked pipe log");
        expect(!fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
            "user close must not write crash quarantine");
        const auto pipeSnap = logPipe.snapshot();
        expect(producer.retire() == ipc::Status::Ok, "retire pipe close");
        logPipe.close();
        write_pipe_witness_log(witnessPath,
            "WITNESS scenario=" + std::string(useControllerStop ? "userstop" : "esc") +
            " sample1_avail=" + std::to_string(witness.sample1AvailBytes) +
            " sample2_avail=" + std::to_string(witness.sample2AvailBytes) +
            " pipe_avail_bytes=" + std::to_string(witness.sample2AvailBytes) +
            " dwell_ms=" + std::to_string(witness.dwellMs) +
            " frames_fed=" + std::to_string(witness.framesFed) +
            " log_write_in_flight=1 old_pid=" + std::to_string(oldPid) +
            " exit_ms=" + std::to_string(closeElapsedMs) + " exit_reason=1 marker_cleaned=1",
            pipeSnap);
        std::printf("PASS ab_pushflow_%s_stalled_pipe_log pid=%lu pipe_avail=%lu dwell_ms=%llu exit_ms=%llu\n",
            useControllerStop ? "userstop" : "esc", oldPid, witness.sample2AvailBytes,
            static_cast<unsigned long long>(witness.dwellMs),
            static_cast<unsigned long long>(closeElapsedMs));
    }

    fs::remove(reopenPath);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", reopenPath.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(useControllerStop ? L"pfustoppipe-re" : L"pfescpipe-re"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer pipe close reopen");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0,
            "handshake pipe close reopen");
        wait_log_token(reopenPath, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 35; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire pipe close reopen");

        const auto cr = read_text(reopenPath);
        expect(cr.find("event=pushflow_previous_crash_detected") == std::string::npos,
            "reopen after blocked-pipe user close must NOT trigger false crash quarantine");
        expect(count_tokens(cr, "event=present_vfg_subframe ") >= 5,
            "reopen after blocked-pipe user close must resume PushFlow synthesis");
        expect(count_tokens(cr, "event=error") == 0, "unexpected error on reopen");
        std::printf("PASS ab_pushflow_%s_stalled_pipe_log_reopen subframes=%zu\n",
            useControllerStop ? "userstop" : "esc", count_tokens(cr, "event=present_vfg_subframe "));
    }
}

void run_pushflow_overlapping_markers(const std::wstring& presenter, const fs::path& logDir,
    const std::wstring& monId, const std::wstring& pfRuntime, const fs::path& marksDir)
{
    fs::remove_all(marksDir);
    const auto logA = logDir / L"ab-pushflow-overlap-a.log";
    const auto logB = logDir / L"ab-pushflow-overlap-b.log";
    const auto logC = logDir / L"ab-pushflow-overlap-c.log";
    fs::remove(logA);
    fs::remove(logB);
    fs::remove(logC);

    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    GpuFixture gfx(640, 360);
    // 1. Start session A (PushFlow 2x)
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logA.c_str());
    ipc::Producer prodA(gfx.dev.Get(), unique_mapping(L"ovA"), gfx.shape);
    LaunchOptions optsA{};
    optsA.backend = Backend::PushFlow;
    optsA.pushflowTier = PushFlowTier::Fast;
    optsA.pushflowRuntimeDir = pfRuntime;
    optsA.multiplier = 2;
    optsA.monitorId = monId;
    Controller ctrlA(optsA);
    const auto offA = prodA.connect(ctrlA.peer());
    expect(ctrlA.offer(offA, gfx.shape, gfx.luid), "offer overlap A");
    expect(WaitForSingleObject(prodA.control_event(), 10000) == WAIT_OBJECT_0, "handshake overlap A");
    wait_log_token(logA, "event=initialized backend=pushflow", ctrlA);
    for (uint32_t f = 0; f < 15; ++f) {
        pace_to(clock_ns() + 16'666'667ULL);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
        publish_ok_or_dropped(prodA, gfx.tex.Get(), clock_ns());
        if (count_starting_markers(marksDir) == 1 &&
            count_tokens(read_text(logA), "event=present_vfg_subframe ") >= 3) break;
    }
    wait_log_token(logA, "event=present_vfg_subframe ", ctrlA);
    const auto markersAfterA = list_starting_markers(marksDir);
    expect(markersAfterA.size() == 1, "overlap A must arm 1 starting marker");
    const std::wstring markerA = markersAfterA[0];

    // 2. Start overlapping session B (PushFlow 2x) while A is active
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logB.c_str());
    ipc::Producer prodB(gfx.dev.Get(), unique_mapping(L"ovB"), gfx.shape);
    LaunchOptions optsB = optsA;
    Controller ctrlB(optsB);
    const auto offB = prodB.connect(ctrlB.peer());
    expect(ctrlB.offer(offB, gfx.shape, gfx.luid), "offer overlap B");
    expect(WaitForSingleObject(prodB.control_event(), 10000) == WAIT_OBJECT_0, "handshake overlap B");
    wait_log_token(logB, "event=initialized backend=pushflow", ctrlB);
    for (uint32_t f = 0; f < 15; ++f) {
        pace_to(clock_ns() + 16'666'667ULL);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
        publish_ok_or_dropped(prodB, gfx.tex.Get(), clock_ns());
        if (count_starting_markers(marksDir) == 2 &&
            count_tokens(read_text(logB), "event=present_vfg_subframe ") >= 3) break;
    }
    wait_log_token(logB, "event=present_vfg_subframe ", ctrlB);
    const auto markersAfterB = list_starting_markers(marksDir);
    expect(markersAfterB.size() == 2, "overlap A + B must hold 2 isolated starting markers");
    const std::wstring markerB = (markersAfterB[0] == markerA) ? markersAfterB[1] : markersAfterB[0];
    expect(markerA != markerB && fs::exists(marksDir / markerA) && fs::exists(marksDir / markerB),
        "overlap A and B must have distinct named starting markers");

    // 3. Close A cleanly via VK_ESCAPE (UserClosed): exact markerA is removed, exact markerB MUST survive untouched!
    const auto offerLineA = find_line(read_text(logA), "event=offer ");
    const auto hwndA = std::stoull(field_value(offerLineA, "hwnd="), nullptr, 16);
    expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndA)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
        "Esc overlap A");
    const auto exitDeadlineA = clock_ns() + 2'500'000'000ULL;
    while (ctrlA.alive() && clock_ns() < exitDeadlineA) Sleep(10);
    expect(!ctrlA.alive() && ctrlA.await_exit_cleanup(1000) && ctrlA.user_close_marker_cleaned(),
        "overlap A UserClosed cleanup must complete");
    expect(prodA.retire() == ipc::Status::Ok, "retire overlap A");
    expect(!fs::exists(marksDir / markerA) && fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 1,
        "closing A via UserClosed must remove exact markerA and preserve exact markerB");
    expect(!fs::exists(marksDir / L"pushflow-crash-quarantine.txt"), "UserClosed A must not write quarantine");

    // 4. Crash B mid-init without writing quarantine (simulating host exit before fallback quarantine):
    //    B's exact surviving starting markerB MUST remain on disk and force C to degrade to 1x!
    const auto offerLineB = find_line(read_text(logB), "event=offer ");
    const auto pidB = std::stoul(field_value(offerLineB, "pid="));
    Handle childB{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pidB)};
    expect(childB.value != nullptr, "open overlap B presenter");
    expect(TerminateProcess(childB.value, 9) != FALSE, "TerminateProcess overlap B");
    expect(WaitForSingleObject(childB.value, 2000) == WAIT_OBJECT_0, "overlap B reaped");
    expect(ctrlB.await_exit_cleanup(1000), "overlap B observer settles");
    expect(prodB.retire() == ipc::Status::Ok, "retire overlap B");
    expect(fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 1,
        "crashed B exact markerB must survive on disk");

    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logC.c_str());
    ipc::Producer prodC(gfx.dev.Get(), unique_mapping(L"ovC"), gfx.shape);
    Controller ctrlC(optsA);
    const auto offC = prodC.connect(ctrlC.peer());
    expect(ctrlC.offer(offC, gfx.shape, gfx.luid), "offer overlap C");
    expect(WaitForSingleObject(prodC.control_event(), 10000) == WAIT_OBJECT_0, "handshake overlap C");
    const auto startC = clock_ns();
    for (uint32_t f = 0; f < 30; ++f) {
        const auto ts = startC + uint64_t{f} * 1'000'000'000ULL / 30;
        pace_to(ts);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
        publish_ok_or_dropped(prodC, gfx.tex.Get(), ts);
    }
    Sleep(200);
    wait_controller_exit(ctrlC);
    expect(prodC.retire() == ipc::Status::Ok, "retire overlap C");
    const auto cTxt = read_text(logC);
    expect(cTxt.find("event=pushflow_previous_crash_detected") != std::string::npos,
        "C must detect B's surviving crash marker after A closed via UserClosed");
    expect(count_tokens(cTxt, "event=present_original ") >= 20 &&
        count_tokens(cTxt, "event=present_vfg_subframe ") == 0,
        "C must degrade to 1x original after B crash");
    expect(!fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 0,
        "C consumed exact markerB cleanly");
    std::printf("PASS ab_pushflow_overlapping_markers markerA=%ls markerB=%ls pidB=%lu orig=%zu\n",
        markerA.c_str(), markerB.c_str(), pidB, count_tokens(cTxt, "event=present_original "));
}

void run_pushflow_cross_process_helper(const std::wstring& presenter, const fs::path& logDir,
    const std::wstring& monId, const std::wstring& pfRuntime, const fs::path& marksDir)
{
    auto readEnvW = [](const wchar_t* k) -> std::wstring {
        wchar_t buf[512]{};
        const auto n = GetEnvironmentVariableW(k, buf, ARRAYSIZE(buf));
        return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring{};
    };
    const auto readyName = readEnvW(L"OBS_NR_XPROC_READY_EVENT");
    const auto holdName = readEnvW(L"OBS_NR_XPROC_HOLD_EVENT");
    const auto infoPath = fs::path(readEnvW(L"OBS_NR_XPROC_INFO_FILE"));
    expect(!readyName.empty() && !holdName.empty() && !infoPath.empty(), "helper env vars required");
    Handle readyEv{OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str())};
    Handle holdEv{OpenEventW(SYNCHRONIZE, FALSE, holdName.c_str())};
    expect(readyEv.value && holdEv.value, "open cross-process sync events");

    const auto logB = logDir / L"ab-pushflow-xproc-b.log";
    fs::remove(logB);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logB.c_str());
    GpuFixture gfx(640, 360);
    ipc::Producer prodB(gfx.dev.Get(), unique_mapping(L"xpB"), gfx.shape);
    LaunchOptions optsB{};
    optsB.backend = Backend::PushFlow;
    optsB.pushflowTier = PushFlowTier::Fast;
    optsB.pushflowRuntimeDir = pfRuntime;
    optsB.multiplier = 2;
    optsB.monitorId = monId;
    Controller ctrlB(optsB);
    const auto offB = prodB.connect(ctrlB.peer());
    expect(ctrlB.offer(offB, gfx.shape, gfx.luid), "offer xproc B");
    expect(WaitForSingleObject(prodB.control_event(), 10000) == WAIT_OBJECT_0, "handshake xproc B");
    wait_log_token(logB, "event=initialized backend=pushflow", ctrlB);
    for (uint32_t f = 0; f < 20; ++f) {
        pace_to(clock_ns() + 16'666'667ULL);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
        publish_ok_or_dropped(prodB, gfx.tex.Get(), clock_ns());
        if (!list_starting_markers(marksDir).empty() &&
            count_tokens(read_text(logB), "event=present_vfg_subframe ") >= 3) break;
    }
    wait_log_token(logB, "event=present_vfg_subframe ", ctrlB);
    const auto markers = list_starting_markers(marksDir);
    expect(markers.size() == 1, "xproc B must arm 1 starting marker");
    const auto offerLineB = find_line(read_text(logB), "event=offer ");
    const auto pidB = std::stoul(field_value(offerLineB, "pid="));
    {
        std::ofstream out(infoPath, std::ios::binary | std::ios::trunc);
        out << pidB << '\n';
        for (wchar_t wc : markers[0]) out << static_cast<char>(wc);
        out << '\n';
    }
    SetEvent(readyEv.value);
    // Keep feeding frames periodically so presenter watchdog (2000ms) does not fire until host kills or holdEv clears.
    uint32_t frame = 20;
    while (WaitForSingleObject(holdEv.value, 33) == WAIT_TIMEOUT && ctrlB.alive()) {
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (frame * 20) % 480);
        publish_ok_or_dropped(prodB, gfx.tex.Get(), clock_ns());
        ++frame;
    }
}

void run_pushflow_cross_process_markers(const std::wstring& presenter, const fs::path& logDir,
    const std::wstring& monId, const std::wstring& pfRuntime, const fs::path& marksDir)
{
    fs::remove_all(marksDir);
    const auto logA = logDir / L"ab-pushflow-xproc-a.log";
    const auto logA2 = logDir / L"ab-pushflow-xproc-a2.log";
    const auto logC = logDir / L"ab-pushflow-xproc-c.log";
    const auto infoFile = logDir / L"xproc-b-info.txt";
    fs::remove(logA);
    fs::remove(logA2);
    fs::remove(logC);
    fs::remove(infoFile);

    const auto selfPid = std::to_wstring(GetCurrentProcessId());
    const auto readyName = LR"(Local\obs-nr-xp-ready-)" + selfPid;
    const auto holdName = LR"(Local\obs-nr-xp-hold-)" + selfPid;
    Handle readyEv{CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())};
    Handle holdEv{CreateEventW(nullptr, TRUE, FALSE, holdName.c_str())};
    expect(readyEv.value && holdEv.value, "create cross-process events");

    wchar_t selfExe[4096]{};
    const auto selfLen = GetModuleFileNameW(nullptr, selfExe, ARRAYSIZE(selfExe));
    expect(selfLen && selfLen < ARRAYSIZE(selfExe), "query self exe path");

    struct HelperEnvGuard {
        ~HelperEnvGuard()
        {
            SetEnvironmentVariableW(L"OBS_NR_XPROC_READY_EVENT", nullptr);
            SetEnvironmentVariableW(L"OBS_NR_XPROC_HOLD_EVENT", nullptr);
            SetEnvironmentVariableW(L"OBS_NR_XPROC_INFO_FILE", nullptr);
        }
    } envGuard;
    SetEnvironmentVariableW(L"OBS_NR_XPROC_READY_EVENT", readyName.c_str());
    SetEnvironmentVariableW(L"OBS_NR_XPROC_HOLD_EVENT", holdName.c_str());
    SetEnvironmentVariableW(L"OBS_NR_XPROC_INFO_FILE", infoFile.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());

    auto cmd = L"\"" + std::wstring(selfExe, selfLen) + L"\" \"" + presenter + L"\" \"" +
        logDir.wstring() + L"\" pushflow-cross-process-helper";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    expect(CreateProcessW(selfExe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi) != FALSE, "launch cross-process host-B helper");
    Handle hostBProc{pi.hProcess}, hostBThread{pi.hThread};
    struct HostBCleanup {
        HANDLE proc{};
        DWORD pidB{};
        ~HostBCleanup()
        {
            if (pidB) {
                Handle pB{OpenProcess(PROCESS_TERMINATE, FALSE, pidB)};
                if (pB.value) TerminateProcess(pB.value, 9);
            }
            if (proc) TerminateProcess(proc, 9);
        }
    } hostBCleanup{hostBProc.value, 0};

    expect(WaitForSingleObject(readyEv.value, 15000) == WAIT_OBJECT_0, "wait for cross-process host-B ready");
    std::ifstream infoIn(infoFile);
    std::string pidBStr, markerBStr;
    expect(std::getline(infoIn, pidBStr) && std::getline(infoIn, markerBStr) && !markerBStr.empty(),
        "read cross-process host-B info");
    const DWORD pidB = std::stoul(pidBStr);
    hostBCleanup.pidB = pidB;
    const std::wstring markerB(markerBStr.begin(), markerBStr.end());
    expect(fs::exists(marksDir / markerB), "host-B exact markerB must exist on disk");

    // Host A starts 2x PushFlow in this separate process against the SAME LOCALAPPDATA.
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logA.c_str());
    GpuFixture gfx(640, 360);
    LaunchOptions optsA{};
    optsA.backend = Backend::PushFlow;
    optsA.pushflowTier = PushFlowTier::Fast;
    optsA.pushflowRuntimeDir = pfRuntime;
    optsA.multiplier = 2;
    optsA.monitorId = monId;
    std::wstring markerA;
    {
        ipc::Producer prodA(gfx.dev.Get(), unique_mapping(L"xpA"), gfx.shape);
        Controller ctrlA(optsA);
        const auto offA = prodA.connect(ctrlA.peer());
        expect(ctrlA.offer(offA, gfx.shape, gfx.luid), "offer xproc A");
        expect(WaitForSingleObject(prodA.control_event(), 10000) == WAIT_OBJECT_0, "handshake xproc A");
        wait_log_token(logA, "event=initialized backend=pushflow", ctrlA);
        for (uint32_t f = 0; f < 15; ++f) {
            pace_to(clock_ns() + 16'666'667ULL);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(prodA, gfx.tex.Get(), clock_ns());
            if (count_starting_markers(marksDir) == 2 &&
                count_tokens(read_text(logA), "event=present_vfg_subframe ") >= 3) break;
        }
        wait_log_token(logA, "event=present_vfg_subframe ", ctrlA);
        const auto m2 = list_starting_markers(marksDir);
        expect(m2.size() == 2, "xproc host-A + host-B must hold 2 isolated markers");
        markerA = (m2[0] == markerB) ? m2[1] : m2[0];
        expect(markerA != markerB && fs::exists(marksDir / markerA) && fs::exists(marksDir / markerB),
            "host-A and host-B markers must be distinct");

        // Host-A UserClosed -> exact markerA removed, exact markerB from host-B MUST survive!
        const auto offerLineA = find_line(read_text(logA), "event=offer ");
        const auto hwndA = std::stoull(field_value(offerLineA, "hwnd="), nullptr, 16);
        expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndA)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
            "Esc xproc A");
        const auto dl = clock_ns() + 2'500'000'000ULL;
        while (ctrlA.alive() && clock_ns() < dl) Sleep(10);
        expect(!ctrlA.alive() && ctrlA.await_exit_cleanup(1000) && ctrlA.user_close_marker_cleaned(),
            "xproc A UserClosed cleanup");
        expect(prodA.retire() == ipc::Status::Ok, "retire xproc A");
    }
    expect(!fs::exists(marksDir / markerA) && fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 1,
        "xproc host-A UserClosed must delete ONLY markerA and preserve live host-B markerB");

    // Host-A REOPENS 2x PushFlow while separate host-B is STILL active: must succeed 2x AND preserve markerB!
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logA2.c_str());
    {
        ipc::Producer prodA2(gfx.dev.Get(), unique_mapping(L"xpA2"), gfx.shape);
        Controller ctrlA2(optsA);
        const auto offA2 = prodA2.connect(ctrlA2.peer());
        expect(ctrlA2.offer(offA2, gfx.shape, gfx.luid), "offer xproc A2");
        expect(WaitForSingleObject(prodA2.control_event(), 10000) == WAIT_OBJECT_0, "handshake xproc A2");
        wait_log_token(logA2, "event=initialized backend=pushflow", ctrlA2);
        for (uint32_t f = 0; f < 15; ++f) {
            pace_to(clock_ns() + 16'666'667ULL);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(prodA2, gfx.tex.Get(), clock_ns());
            if (count_tokens(read_text(logA2), "event=present_vfg_subframe ") >= 3) break;
        }
        wait_log_token(logA2, "event=present_vfg_subframe ", ctrlA2);
        const auto a2Txt = read_text(logA2);
        expect(a2Txt.find("event=pushflow_previous_crash_detected") == std::string::npos,
            "host-A reopen while live host-B runs must NOT misclassify markerB as crash");
        expect(fs::exists(marksDir / markerB),
            "host-A reopen must preserve live host-B markerB");

        const auto offerLineA2 = find_line(a2Txt, "event=offer ");
        const auto hwndA2 = std::stoull(field_value(offerLineA2, "hwnd="), nullptr, 16);
        expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndA2)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
            "Esc xproc A2");
        const auto dl2 = clock_ns() + 2'500'000'000ULL;
        while (ctrlA2.alive() && clock_ns() < dl2) Sleep(10);
        expect(!ctrlA2.alive() && ctrlA2.await_exit_cleanup(1000) && ctrlA2.user_close_marker_cleaned(),
            "xproc A2 UserClosed cleanup");
        expect(prodA2.retire() == ipc::Status::Ok, "retire xproc A2");
    }
    expect(fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 1,
        "after host-A2 closes, live host-B markerB still survives");

    // Simulate abrupt startup crash of host-B (kill both host-B process and presenter pidB with no quarantine/fallback).
    {
        Handle presB{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pidB)};
        expect(presB.value != nullptr, "open host-B presenter for crash injection");
        expect(TerminateProcess(presB.value, 9) != FALSE, "TerminateProcess host-B presenter");
        expect(WaitForSingleObject(presB.value, 2000) == WAIT_OBJECT_0, "reap host-B presenter");
        expect(TerminateProcess(hostBProc.value, 9) != FALSE, "TerminateProcess host-B helper");
        expect(WaitForSingleObject(hostBProc.value, 2000) == WAIT_OBJECT_0, "reap host-B helper");
        hostBCleanup.pidB = 0;
        hostBCleanup.proc = nullptr;
    }
    expect(fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 1,
        "crashed host-B exact markerB must remain on disk");

    // Host-C opens 2x -> detects host-B's crash, degrades to 1x originals, and consumes exact markerB!
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logC.c_str());
    ipc::Producer prodC(gfx.dev.Get(), unique_mapping(L"xpC"), gfx.shape);
    Controller ctrlC(optsA);
    const auto offC = prodC.connect(ctrlC.peer());
    expect(ctrlC.offer(offC, gfx.shape, gfx.luid), "offer xproc C");
    expect(WaitForSingleObject(prodC.control_event(), 10000) == WAIT_OBJECT_0, "handshake xproc C");
    const auto startC = clock_ns();
    for (uint32_t f = 0; f < 30; ++f) {
        const auto ts = startC + uint64_t{f} * 1'000'000'000ULL / 30;
        pace_to(ts);
        gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
        publish_ok_or_dropped(prodC, gfx.tex.Get(), ts);
    }
    Sleep(200);
    wait_controller_exit(ctrlC);
    expect(prodC.retire() == ipc::Status::Ok, "retire xproc C");
    const auto cTxt = read_text(logC);
    expect(cTxt.find("event=pushflow_previous_crash_detected") != std::string::npos,
        "host-C must detect crashed host-B markerB");
    expect(count_tokens(cTxt, "event=present_original ") >= 20 &&
        count_tokens(cTxt, "event=present_vfg_subframe ") == 0,
        "host-C must degrade to 1x original after host-B crash");
    expect(!fs::exists(marksDir / markerB) && count_starting_markers(marksDir) == 0,
        "host-C consumed exact markerB cleanly");
    std::printf("PASS ab_pushflow_cross_process_markers markerA=%ls markerB=%ls pidB=%lu orig=%zu\n",
        markerA.c_str(), markerB.c_str(), pidB, count_tokens(cTxt, "event=present_original "));
}

void run_anr_ab(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& mockDllPath)
{
    static constexpr std::string_view companions[]{
        "nr_fg.ini", "path_table.json", "size_table.json",
        "adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
        "programs/fg-lite-any-coopmat-pq10/pack.nrx", "programs/fg-lite-any-coopmat-srgb8/pack.nrx",
        "programs/fg-lite-any-dp4a-pq10/pack.nrx", "programs/fg-lite-any-dp4a-srgb8/pack.nrx",
        "programs/fg-lite-any-packed-pq10/pack.nrx", "programs/fg-lite-any-packed-srgb8/pack.nrx",
        "programs/fg-lite-any-portable-linalg-pq10/pack.nrx", "programs/fg-lite-any-portable-linalg-srgb8/pack.nrx",
        "programs/fg-lite-any-portable-pq10/pack.nrx", "programs/fg-lite-any-portable-srgb8/pack.nrx",
        "programs/fg-lite-any-portable-xmx-pq10/pack.nrx", "programs/fg-lite-any-portable-xmx-srgb8/pack.nrx",
    };
    const auto addonRoot = logDir / L"anr-ab-addon";
    std::error_code ec;
    fs::remove_all(addonRoot, ec);
    fs::create_directories(addonRoot);
    struct AddonCleanup {
        fs::path root;
        ~AddonCleanup() { std::error_code e; fs::remove_all(root, e); }
    } cleanup{addonRoot};

    const auto stagedDll = addonRoot / L"nvngx_dlssg.dll";
    fs::copy_file(mockDllPath, stagedDll, fs::copy_options::overwrite_existing);
    for (const auto rel : companions) {
        const auto p = addonRoot / fs::path(rel);
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << "inert test companion\n";
        expect(static_cast<bool>(f), "stage ANR A/B companion");
    }

    // Session 1: live HUD Lock 0 -> 1 -> 0 on same ANR Mock presenter PID (<= 80 evals limit)
    {
        const auto log = logDir / L"ab-anr-hud.log";
        fs::remove(log);
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        GpuFixture gfx(640, 360);
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"anrhud"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::AnrExperimental;
        opts.experimentalAnrOptIn = 1;
        opts.anrDllPath = stagedDll.wstring();
        opts.multiplier = 2;
        opts.gpuShotChange = 0;
        opts.hudLock = 0;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer anr hud");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake anr hud");
        wait_log_token(log, "event=initialized backend=anr-experimental", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 45; ++f) {
            if (f == 15) controller.set_hud_lock(1);
            if (f == 30) controller.set_hud_lock(0);
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF202020u, 0xFFFFFFFFu, (f * 16) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire anr hud");

        const auto c = read_text(log);
        expect(c.find("event=initialized backend=anr-experimental") != std::string::npos, "ANR init missing");
        expect(count_tokens(c, "event=offer ") == 1, "ANR HUD toggle must stay on same PID");
        const auto p0 = c.find("hud_lock=0 ");
        const auto p1 = c.find("hud_lock=1 ", p0 == std::string::npos ? 0 : p0);
        const auto p2 = c.find("hud_lock=0 ", p1 == std::string::npos ? 0 : p1);
        expect(p0 != std::string::npos && p1 != std::string::npos && p2 != std::string::npos,
            "ANR hud_lock must toggle live 0 -> 1 -> 0");
        expect(count_tokens(c, "event=present_vfg_subframe ") >= 10, "insufficient ANR HUD subframes");
        expect(count_tokens(c, "event=error") == 0, "unexpected error in ANR HUD toggle");
        std::printf("PASS ab_anr_hud_toggle subframes=%zu\n", count_tokens(c, "event=present_vfg_subframe "));
    }

    // Session 2: reopen ANR Mock presenter with gpuShotChange=1 and verify cut classification + synthesis
    {
        const auto log = logDir / L"ab-anr-shot-reopen.log";
        fs::remove(log);
        SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
        SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
        GpuFixture gfx(640, 360);
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"anrshot"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::AnrExperimental;
        opts.experimentalAnrOptIn = 1;
        opts.anrDllPath = stagedDll.wstring();
        opts.multiplier = 2;
        opts.gpuShotChange = 1;
        opts.hudLock = 1;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer anr shot");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake anr shot");
        wait_log_token(log, "event=initialized backend=anr-experimental", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 35; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            if (f >= 15 && f < 22) gfx.fill_frame(0xFFFFFFFFu, 0xFF101010u, (f * 16) % 480);
            else gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 16) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire anr shot");

        const auto c = read_text(log);
        expect(c.find("event=initialized backend=anr-experimental") != std::string::npos, "ANR shot init missing");
        expect(c.find("gpu_shot_change=1") != std::string::npos, "ANR gpu_shot_change=1 missing");
        expect(count_tokens(c, "shot_cut=1") >= 1, "expected shot_cut=1 on ANR reopen");
        expect(count_tokens(c, "event=present_vfg_subframe ") >= 10, "insufficient ANR shot subframes");
        expect(count_tokens(c, "event=error") == 0, "unexpected error in ANR shot reopen");
        std::printf("PASS ab_anr_shot_reopen cuts=%zu subframes=%zu\n",
            count_tokens(c, "shot_cut=1"), count_tokens(c, "event=present_vfg_subframe "));
    }
}

// Drive the public IPC/controller boundary. Source PTS and RGB values define
// the oracle; never infer a repeat or the expected shot cut from the log.
void run_pair_probe(const std::wstring& presenter, const fs::path& log, const std::wstring& mon)
{
    const auto name = log.stem().string();
    const bool exact = name == "dedup-exact";
    const bool noisy = name == "dedup-noisy", adaptive = name == "dedup-60to30-adaptive", fixed = name == "dedup-60to30-fixed";
    const bool live = name == "live-anchor", shot = name == "2-deep-repeat-shot-change";
    const bool blocked = name == "bank-wait-timeout", resize = name == "2-deep-resize", discontinuity = name == "2-deep-discontinuity";
    const bool cutover = discontinuity || resize || blocked;
    const auto mode = adaptive ? DedupMode::Adaptive60To30 : fixed ? DedupMode::Fixed60To30 :
        noisy || shot ? DedupMode::Auto : DedupMode::OffExact;

    fs::remove(log);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    GpuFixture gpu(1280, 720);
    LaunchOptions opts{};
    opts.lowLatency = 0;
    opts.monitorId = mon;
    opts.dedupMode = static_cast<uint32_t>(mode);
    opts.transportIntervalNs = 16'666'667;
    opts.gpuShotChange = shot;
    opts.liveAnchor = live;
    ipc::Producer producer(gpu.dev.Get(), unique_mapping(L"pair"), gpu.shape);
    Controller ctl(opts);
    expect(ctl.offer(producer.connect(ctl.peer()), gpu.shape, gpu.luid) &&
        WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "pair probe bind");
    wait_log_token(log, "event=initialized", ctl);
    const auto send = [&](unsigned rgb, uint64_t pts) {
        const auto pixel = 0xff000000u | rgb * 0x010101u;
        gpu.fill_frame(pixel, pixel, 0);
        expect(producer.publish(gpu.tex.Get(), pts, clock_ns()) == ipc::Status::Ok, "pair probe publish");
    };
    const auto wait_line = [&](std::string_view key, unsigned timeout) {
        const auto end = clock_ns() + uint64_t{timeout} * 1'000'000;
        do { if (read_text(log).find(key) != std::string::npos) return; pace_to(clock_ns() + 250'000); } while (clock_ns() < end);
        throw std::runtime_error(std::string("timed out waiting for ") + std::string(key));
    };
    const auto event = [&](std::string_view kind, unsigned serial) {
        return "event=" + std::string(kind) + " generation=" + std::to_string(producer.generation()) + " b=" + std::to_string(serial) + " ";
    };
    ComPtr<ID3D12Fence> release;
    ComPtr<ID3D11DeviceContext4> gatedContext;
    ComPtr<ID3D11Fence> gate;
    if (blocked) {
        // Create the shared fence before streaming; insert the wait while two pairs are live.
        ComPtr<ID3D11Device5> d11;
        ComPtr<ID3D12Device> d12;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        Handle shared;
        expect(SUCCEEDED(gpu.dev.As(&d11)) && SUCCEEDED(gpu.ctx.As(&gatedContext)) && SUCCEEDED(gpu.dev.As(&dxgi)) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate))) &&
            SUCCEEDED(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared.value)) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))) &&
            SUCCEEDED(d12->OpenSharedHandle(shared.value, IID_PPV_ARGS(&release))), "install shared GPU gate");
    }
    unsigned count = 0;
    if (cutover || shot) {
        const auto basePts = clock_ns();
        send(32, basePts);
        wait_line(event("present_original", 1), 3000);
        send(224, basePts + 16'666'667);
        wait_line("event=pair_admitted b=2 ", 1000);
        if (shot) send(226, basePts + 25'000'000);
        send(215, basePts + 33'333'333);
        const unsigned last = shot ? 4 : 3;
        wait_line("event=pair_admitted b=" + std::to_string(last) + " ", 1000);
        if (shot) wait_line(event("present_original", last), 1000);
        if (resize) {
            expect(ctl.request_resize(), "request resize");
            const auto end = clock_ns() + 2'000'000'000;
            while (!ctl.resize_drained() && clock_ns() < end) Sleep(1);
            expect(ctl.resize_drained(), "resize drain acknowledgement");
            GpuFixture resizedGpu(960, 540);
            ipc::Producer resized(resizedGpu.dev.Get(), unique_mapping(L"resized"), resizedGpu.shape);
            expect(ctl.offer(resized.connect(ctl.peer()), resizedGpu.shape, resizedGpu.luid) &&
                WaitForSingleObject(resized.control_event(), 10000) == WAIT_OBJECT_0, "resize offer");
            const auto readyEnd = clock_ns() + 3'000'000'000;
            while (count_tokens(read_text(log), "event=initialized") < 2 && clock_ns() < readyEnd) Sleep(1);
            expect(count_tokens(read_text(log), "event=initialized") == 2, "resized backend ready");
            resizedGpu.fill_frame(0xff303030, 0xff303030, 0);
            expect(resized.publish(resizedGpu.tex.Get(), basePts + 50'000'000, clock_ns()) == ipc::Status::Ok, "resized anchor");
            wait_line("event=present_original generation=" + std::to_string(resized.generation()) + " b=1 ", 3000);
        } else if (discontinuity || blocked) {
            if (blocked) expect(SUCCEEDED(gatedContext->Wait(gate.Get(), 1)), "delay consumer queue");
            send(96, basePts + 1); // backwards PTS; read by IPC before the image is ready
            if (blocked) {
                try { wait_line("reason=bank_wait_timeout", 1950); }
                catch (...) { (void)release->Signal(1); throw; }
                expect(SUCCEEDED(release->Signal(1)), "release GPU gate");
                wait_line("event=bank_wait_recovered", 500);
            }
            const unsigned anchor = blocked ? 5 : 4;
            if (blocked) send(80, basePts + 16'666'668);
            wait_line(event("present_original", anchor), 1200);
            send(120, basePts + (blocked ? 33'333'335 : 16'666'668));
            wait_line(event("present_original", anchor + 1), 1200);
        }
    } else {
        count = live ? 120u : 48u;
        const auto basePts = clock_ns() + 2'000'000;
        const auto phase = fixed || adaptive;
        for (unsigned i = 0; i < count; ++i) {
            const auto ts = basePts + uint64_t{i} * 1'000'000'000 / 60;
            const unsigned id = phase ? i < 16 ? i / 2 : i < 32 ? 8 + (i - 16) / 4 : 12 + i - 32 : noisy || exact ? i / 3 : i;
            const unsigned rgb = (id % 2 ? 200u : 32u) + (noisy || exact ? i % 3 : 0);
            pace_to(live ? basePts + (ts - basePts) * 1002 / 1000 : ts);
            send(rgb, ts);
        }
        wait_line(event("input", count), 2000);
        wait_line(event("present_original", noisy ? count - 2 : fixed ? count - 1 : count), 2000);
    }
    wait_controller_exit(ctl);
    const auto report = read_text(log);
    expect(count_tokens(report, "event=error") == unsigned(blocked), "unexpected error count in pair probe");
    std::vector<bool> repeats(count + 1), originals(count + 1), skipped(count + 1);
    uint64_t lastFence = 0, timeoutMs = 0, recover = 0;
    unsigned retired = 0, reuse = 0;
    bool boundary = false, overlap = false, locked = false, cut = false, afterCut = false, repeatSlot = false;
    unsigned cutSlot = 2;
    double phaseMs = 0, ppm = 0;
    std::istringstream lines(report);
    const auto field_u64 = [](const std::string& line, const char* key) {
        return std::stoull(field_value(line, " " + std::string(key) + "="));
    };
    for (std::string line; std::getline(lines, line);) {
        const auto serial = field_value(line, " b=");
        const auto b = serial.empty() ? 0ULL : std::stoull(serial);
        if (line.starts_with("event=pair_admitted")) {
            const auto prior = field_u64(line, "prior_fence");
            expect(field_u64(line, "completed") >= prior, "bank reused before consumer completion");
            reuse += prior > 0;
            if (b == (shot ? 4u : 3u) && field_u64(line, "depth") == 2) overlap = true;
            if (shot && b == 2) cutSlot = static_cast<unsigned>(field_u64(line, "shot_slot"));
        } else if (line.starts_with("event=pair_retired")) {
            const auto fence = field_u64(line, "fence");
            if (resize && boundary && fence == 1) lastFence = 0;
            expect(fence > lastFence, "retire sequence must increase");
            lastFence = fence;
            if (line.find(resize ? "reason=resize" : "reason=discontinuity") != std::string::npos) {
                if (!boundary) expect(field_u64(line, "depth") == 2, "discontinuity did not drain two pairs");
                boundary = true;
                ++retired;
            }
        } else if (line.starts_with("event=present_vfg_subframe")) {
            expect(!boundary || (b != 2 && b != 3), "stale synthetic after drain");
            if (shot && b == 2) cut = field_u64(line, "shot_cut") == 1 && field_u64(line, "shot_slot") == cutSlot;
            if (shot && b == 4) afterCut = field_u64(line, "shot_cut") == 0 && field_u64(line, "a") == 2;
        } else if (line.starts_with("event=repeat_input")) {
            if (b <= count) repeats[static_cast<size_t>(b)] = true;
            if (shot && b == 3) repeatSlot = field_u64(line, "shot_slot") != cutSlot;
        } else if (b && b <= count && line.starts_with("event=present_original")) originals[static_cast<size_t>(b)] = true;
        else if (b && b <= count && line.starts_with("event=decimate_input")) skipped[static_cast<size_t>(b)] = true;
        else if (line.starts_with("event=error reason=bank_wait_timeout")) {
            timeoutMs = field_u64(line, "waited_ms");
            expect(field_u64(line, "completed") < field_u64(line, "fence"), "test did not delay a consumer fence");
        } else if (line.starts_with("event=bank_wait_recovered")) ++recover;
        else if (line.starts_with("[live-anchor]") && field_u64(line, "locked")) {
            locked = true;
            phaseMs = std::stod(line.substr(line.find("phaseMs=") + 8));
            ppm = std::stod(line.substr(line.find("clockDriftPpm=") + 14));
        }
    }
    if (cutover) {
        expect(overlap && retired == 2 && (resize || reuse), "must retire both banks and resume cleanly");
        expect(!blocked || (timeoutMs > 0 && timeoutMs <= 2000 && recover == 1), "bounded timeout and recovery required");
        std::printf("PASS %s depth=2 retired=%u stale_subframes=0 reused_early=0 wait_ms=%llu recovered=%llu errors=%u\n", name.c_str(), retired, timeoutMs, recover, unsigned(blocked));
    } else if (shot) {
        expect(overlap && cut && afterCut && repeatSlot, "overlapped repeat clobbered a cut or its predecessor");
        std::printf("PASS %s gpu_shot_change=1 depth=2 repeat_b=3 cut_b=2 shot_cut=1 next_b=4 next_shot_cut=0 errors=0\n", name.c_str());
    } else if (live) {
        expect(locked && phaseMs > 10 && phaseMs < 25 && ppm > 300 && ppm < 4200, "live phase and implanted +2000ppm source drift");
        std::printf("PASS live-anchor locked=1 phaseMs=%.3f clockDriftPpm=%.2f errors=0\n", phaseMs, ppm);
    } else {
        for (unsigned i = 0; i < count; ++i) {
            const bool unique = exact || (noisy ? i % 3 == 0 : fixed || i < 16 ? i % 2 == 0 : i >= 32 || i % 4 == 0);
            expect(originals[i + 1] == unique && ((repeats[i + 1] || skipped[i + 1]) != unique), "dedup decision differs from source sequence");
        }
        expect(!count_tokens(report, "[live-anchor]") && count_tokens(report, "[content-dedup]") > 0, "dedup telemetry with live anchor disabled");
        std::printf("PASS %s mode=%u samples=%u originals=%zu repeats=%zu decimated=%zu errors=0\n", name.c_str(), static_cast<unsigned>(mode), count,
            size_t(std::count(originals.begin(), originals.end(), true)), size_t(std::count(repeats.begin(), repeats.end(), true)), size_t(std::count(skipped.begin(), skipped.end(), true)));
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 4, "usage: vfg_ab_smoke_runner <presenter.exe> <log_dir> <mode>");
        const std::wstring presenter = argv[1];
        const fs::path logDir = argv[2];
        const std::wstring_view mode = argv[3];
        fs::create_directories(logDir);
        const auto monId = secondary_monitor_id();
        expect(!monId.empty(), "no active secondary display");
        expect(SetEnvironmentVariableW(L"OBS_NR_VFG_QA_MONITOR", monId.c_str()) != FALSE, "set QA monitor");

        const auto vfgRuntime = Controller::find_runtime_dir();
        const auto isolatedLocalApp = logDir / L"localappdata";
        fs::create_directories(isolatedLocalApp);
        expect(SetEnvironmentVariableW(L"LOCALAPPDATA", isolatedLocalApp.c_str()) != FALSE, "isolate LOCALAPPDATA");
        if (!vfgRuntime.empty()) {
            expect(SetEnvironmentVariableW(L"OBS_NR_VFG_RUNTIME", vfgRuntime.c_str()) != FALSE, "preserve VFG runtime");
        }

        auto readEnv = [](const wchar_t* k) -> std::wstring {
            wchar_t buf[4096]{};
            const auto n = GetEnvironmentVariableW(k, buf, ARRAYSIZE(buf));
            return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring{};
        };

        if (mode == L"dedup-noisy" || mode == L"dedup-60to30-adaptive" || mode == L"dedup-60to30-fixed" || mode == L"live-anchor" ||
            mode == L"2-deep-repeat-shot-change" || mode == L"2-deep-discontinuity" || mode == L"2-deep-resize" || mode == L"bank-wait-timeout") {
            if (mode == L"dedup-noisy") run_pair_probe(presenter, logDir / L"dedup-exact.log", monId);
            run_pair_probe(presenter, logDir / (std::wstring(mode) + L".log"), monId);
        } else if (mode == L"shot-change") run_shot_change(presenter, logDir, monId);
        else if (mode == L"hud-lock") run_hud_lock(presenter, logDir, monId);
        else if (mode == L"pacing-repair") run_pacing_repair(presenter, logDir, monId);
        else if (mode == L"adaptive") run_adaptive(presenter, logDir, monId);
        else if (mode == L"pushflow-smoke") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-smoke");
            run_pushflow_smoke(presenter, logDir, monId, rt);
        } else if (mode == L"pushflow-stalled-esc") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-stalled-esc");
            run_pushflow_stalled_esc(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-userstop-stalled") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-userstop-stalled");
            run_pushflow_userstop_stalled(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-midrun-crash") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-midrun-crash");
            run_pushflow_midrun_crash(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-midrun-crash-manual") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-midrun-crash-manual");
            run_pushflow_midrun_crash_manual(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-empty") run_pushflow_empty(presenter, logDir, monId);
        else if (mode == L"stalled-log-esc") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for stalled-log-esc");
            run_stalled_log_esc(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"watchdog-stalled-pipe-log") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for watchdog-stalled-pipe-log");
            run_watchdog_stalled_pipe_log(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-esc-stalled-pipe-log") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-esc-stalled-pipe-log");
            run_pushflow_pipe_close_case(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter", false);
        } else if (mode == L"pushflow-userstop-stalled-pipe-log") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-userstop-stalled-pipe-log");
            run_pushflow_pipe_close_case(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter", true);
        } else if (mode == L"pushflow-overlapping-markers") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-overlapping-markers");
            run_pushflow_overlapping_markers(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-cross-process-helper") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-cross-process-helper");
            run_pushflow_cross_process_helper(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-cross-process-markers") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-cross-process-markers");
            run_pushflow_cross_process_markers(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"anr-ab") {
            const auto dll = readEnv(L"OBS_NR_TEST_ANR_MOCK_DLL");
            expect(!dll.empty(), "OBS_NR_TEST_ANR_MOCK_DLL required for anr-ab");
            run_anr_ab(presenter, logDir, monId, dll);
        } else throw std::runtime_error("unknown mode");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_ab_smoke_runner: %s\n", e.what());
        return 1;
    }
}
