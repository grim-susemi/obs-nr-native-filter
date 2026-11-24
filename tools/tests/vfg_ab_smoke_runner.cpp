// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    while (clock_ns() < targetNs) {
        const auto rem = targetNs - clock_ns();
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
        expect(!controller.can_recover(), "UserClosed must disable relaunch");
        expect(producer.retire() == ipc::Status::Ok, "retire");
        const auto c1 = read_text(log);
        expect(c1.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
            "expected UserClosed force terminate");
        expect(count_tokens(c1, "event=controller_auto_fallback_1x_relaunch") == 0, "expected 0 relaunches");
        expect(!fs::exists(marksDir / L"pushflow-starting.txt"), "pushflow-starting.txt must be cleaned on Esc");
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
        expect(producer.retire() == ipc::Status::Ok, "retire userstop");
        const auto cs = read_text(logStop);
        expect(cs.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
            "Controller::stop(true) must preserve UserClosed on force terminate");
        expect(!fs::exists(marksDir / L"pushflow-starting.txt"), "pushflow-starting.txt must be cleaned on user stop");
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
        expect(!fs::exists(marksDir / L"pushflow-starting.txt"),
            "pushflow-starting.txt must already be cleared after 120 steady inferences");

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
    expect(producer.retire() == ipc::Status::Ok, "retire log lock");

    const auto c = read_text(log);
    expect(c.find("event=shutdown_force_terminate watchdog=0 exit_reason=1") != std::string::npos,
        "expected unshared shutdown_force_terminate diagnostic");
    expect(!fs::exists(marksDir / L"pushflow-starting.txt"), "pushflow-starting.txt must be cleaned even when worker holds log lock");
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_LOG_LOCK_MS", nullptr);
    std::printf("PASS ab_stalled_log_esc exit_ms=%llu\n", static_cast<unsigned long long>(escElapsedMs));
}

void run_watchdog_stalled_unshared_log(const std::wstring& presenter, const fs::path& logDir, const std::wstring& monId,
    const std::wstring& pfRuntime, const fs::path& marksDir)
{
    const auto log = logDir / L"ab-watchdog-stalled-unshared-log.log";
    fs::remove(log);
    fs::remove_all(marksDir);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_WORKER_MS", L"7000");
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_UNSHARED_LOG_MS", L"10000");

    GpuFixture gfx(640, 360);
    auto producer = std::make_unique<ipc::Producer>(gfx.dev.Get(), unique_mapping(L"pfwdunshared"), gfx.shape);
    LaunchOptions opts{};
    opts.backend = Backend::PushFlow;
    opts.pushflowTier = PushFlowTier::Fast;
    opts.pushflowRuntimeDir = pfRuntime;
    opts.multiplier = 2;
    opts.monitorId = monId;
    Controller controller(opts);
    const auto offer = producer->connect(controller.peer());
    expect(controller.offer(offer, gfx.shape, gfx.luid), "offer watchdog unshared");
    expect(WaitForSingleObject(producer->control_event(), 10000) == WAIT_OBJECT_0, "handshake watchdog unshared");
    wait_log_token(log, "event=test_worker_stalled", controller);

    expect(fs::exists(marksDir / L"pushflow-starting.txt"), "pushflow-starting.txt must be armed before watchdog");
    const auto stallStart = clock_ns();
    const auto deadline = stallStart + 4'500'000'000ULL;
    while (controller.alive() && clock_ns() < deadline) Sleep(10);
    const auto watchdogElapsedMs = (clock_ns() - stallStart) / 1'000'000ULL;
    expect(!controller.alive(), "no-input watchdog must terminate despite 10s unshared log stall");
    expect(controller.exit_reason() == ExitReason::Watchdog, "watchdog must preserve ExitReason::Watchdog");
    expect(fs::exists(marksDir / L"pushflow-starting.txt"), "watchdog must NOT clear starting marker");
    expect(producer->retire() == ipc::Status::Ok, "retire watchdog unshared");
    producer.reset();

    expect(controller.can_recover() && controller.begin_async_fallback_relaunch(),
        "watchdog must remain eligible for 1x fallback recovery");
    const auto recDeadline = clock_ns() + 8'000'000'000ULL;
    while (clock_ns() < recDeadline) {
        if (controller.poll_async_fallback_relaunch() == RelaunchState::Ready) break;
        Sleep(10);
    }
    expect(fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
        "watchdog recovery must persist crash quarantine");
    wait_controller_exit(controller);

    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_WORKER_MS", nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_UNSHARED_LOG_MS", nullptr);
    std::printf("PASS ab_watchdog_stalled_unshared_log exit_ms=%llu\n",
        static_cast<unsigned long long>(watchdogElapsedMs));
}

void run_pushflow_userstop_stalled_unshared_log(const std::wstring& presenter, const fs::path& logDir,
    const std::wstring& monId, const std::wstring& pfRuntime, const fs::path& marksDir)
{
    const auto log = logDir / L"ab-pushflow-userstop-stalled-unshared-log.log";
    fs::remove(log);
    fs::remove_all(marksDir);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str());
    SetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", pfRuntime.c_str());
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_LOG_LOCK_MS", L"5000");
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_UNSHARED_LOG_MS", L"10000");

    GpuFixture gfx(640, 360);
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfuserunshared"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer userstop unshared");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0, "handshake userstop unshared");
        wait_log_token(log, "event=test_log_locked", controller);

        expect(fs::exists(marksDir / L"pushflow-starting.txt"), "pushflow-starting.txt must exist before user close");
        const auto offerLine = find_line(read_text(log), "event=offer ");
        const auto hwndVal = std::stoull(field_value(offerLine, "hwnd="), nullptr, 16);
        const auto escSentNs = clock_ns();
        expect(PostMessageW(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwndVal)), WM_KEYDOWN, VK_ESCAPE, 0) != FALSE,
            "PostMessage VK_ESCAPE during stalled unshared log");
        const auto exitDeadline = escSentNs + 2'200'000'000ULL;
        while (controller.alive() && clock_ns() < exitDeadline) Sleep(10);
        const auto escElapsedMs = (clock_ns() - escSentNs) / 1'000'000ULL;
        expect(!controller.alive(), "user close must exit within 2.2s despite stalled unshared log");
        expect(controller.exit_reason() == ExitReason::UserClosed, "user close must preserve UserClosed");
        expect(!fs::exists(marksDir / L"pushflow-starting.txt"),
            "pushflow-starting.txt must be cleaned independently of stalled unshared log");
        expect(!fs::exists(marksDir / L"pushflow-crash-quarantine.txt"),
            "user close must not write crash quarantine");
        expect(producer.retire() == ipc::Status::Ok, "retire userstop unshared");
        std::printf("PASS ab_pushflow_userstop_stalled_unshared_log exit_ms=%llu\n",
            static_cast<unsigned long long>(escElapsedMs));
    }

    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_LOG_LOCK_MS", nullptr);
    SetEnvironmentVariableW(L"OBS_NR_TEST_STALL_UNSHARED_LOG_MS", nullptr);
    const auto logReopen = logDir / L"ab-pushflow-userstop-stalled-unshared-log-reopen.log";
    fs::remove(logReopen);
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logReopen.c_str());
    {
        ipc::Producer producer(gfx.dev.Get(), unique_mapping(L"pfuserunshared-re"), gfx.shape);
        LaunchOptions opts{};
        opts.backend = Backend::PushFlow;
        opts.pushflowTier = PushFlowTier::Fast;
        opts.pushflowRuntimeDir = pfRuntime;
        opts.multiplier = 2;
        opts.monitorId = monId;
        Controller controller(opts);
        const auto offer = producer.connect(controller.peer());
        expect(controller.offer(offer, gfx.shape, gfx.luid), "offer userstop unshared reopen");
        expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0,
            "handshake userstop unshared reopen");
        wait_log_token(logReopen, "event=initialized backend=pushflow", controller);

        const auto start = clock_ns();
        for (uint32_t f = 0; f < 35; ++f) {
            const auto ts = start + uint64_t{f} * 1'000'000'000ULL / 30;
            pace_to(ts);
            gfx.fill_frame(0xFF101010u, 0xFFFFFFFFu, (f * 20) % 480);
            publish_ok_or_dropped(producer, gfx.tex.Get(), ts);
        }
        Sleep(300);
        wait_controller_exit(controller);
        expect(producer.retire() == ipc::Status::Ok, "retire userstop unshared reopen");

        const auto cr = read_text(logReopen);
        expect(cr.find("event=pushflow_previous_crash_detected") == std::string::npos,
            "reopen after stalled-log user close must NOT trigger false crash quarantine");
        expect(count_tokens(cr, "event=present_vfg_subframe ") >= 5,
            "reopen after stalled-log user close must resume PushFlow synthesis");
        expect(count_tokens(cr, "event=error") == 0, "unexpected error on reopen");
        std::printf("PASS ab_pushflow_userstop_stalled_unshared_log_reopen subframes=%zu\n",
            count_tokens(cr, "event=present_vfg_subframe "));
    }
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

        if (mode == L"shot-change") run_shot_change(presenter, logDir, monId);
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
        } else if (mode == L"watchdog-stalled-unshared-log") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for watchdog-stalled-unshared-log");
            run_watchdog_stalled_unshared_log(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
        } else if (mode == L"pushflow-userstop-stalled-unshared-log") {
            const auto rt = readEnv(L"OBS_NR_PUSHFLOW_RUNTIME");
            expect(!rt.empty(), "OBS_NR_PUSHFLOW_RUNTIME required for pushflow-userstop-stalled-unshared-log");
            run_pushflow_userstop_stalled_unshared_log(presenter, logDir, monId, rt, isolatedLocalApp / L"obs-nr-native-filter");
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
