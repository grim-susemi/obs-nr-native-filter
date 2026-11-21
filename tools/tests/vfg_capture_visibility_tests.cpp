// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;
using namespace nr::fg;
using namespace nr::fg::vfg;

namespace {
void expect(bool ok, const char* why)
{
    if (!ok) throw std::runtime_error(why);
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("failed to open file: " + path.string());
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

void check_contracts(const std::filesystem::path& repoRoot)
{
    const LaunchOptions defaults{};
    expect(defaults.hideFromCapture == 0, "LaunchOptions default must allow capture (WDA_NONE)");
    expect(display_affinity(defaults.hideFromCapture) == WDA_NONE, "default display_affinity must be WDA_NONE");
    expect(display_affinity(0) == WDA_NONE, "display_affinity(0) must be WDA_NONE");
    expect(display_affinity(1) == WDA_EXCLUDEFROMCAPTURE, "display_affinity(1) must be 0x11");
    expect(display_affinity(42) == WDA_EXCLUDEFROMCAPTURE, "display_affinity(nonzero) must be 0x11");

    expect(!should_hide_source_canvas(false, false), "canvas visible when option off & VFG off");
    expect(!should_hide_source_canvas(false, true), "canvas visible when option off & VFG on");
    expect(!should_hide_source_canvas(true, false), "canvas visible when option on & VFG off");
    expect(should_hide_source_canvas(true, true), "canvas hidden when option on & VFG active");

    for (const auto& rel : {"data/locale/en-US.ini", "data/locale/ko-KR.ini"}) {
        const auto text = read_file(repoRoot / rel);
        for (const auto* key : {
                "VFG.HideFromCapture=",
                "VFG.Hint.HideFromCapture=",
                "VFG.HideSourceCanvas=",
                "VFG.Hint.HideSourceCanvas="}) {
            if (text.find(key) == std::string::npos)
                throw std::runtime_error(std::string("missing locale key ") + key + " in " + rel);
        }
    }
}

HWND parse_hwnd(const std::filesystem::path& logPath)
{
    const auto text = read_file(logPath);
    const auto pos = text.find("hwnd=");
    if (pos == std::string::npos) return nullptr;
    const char* start = text.c_str() + pos + 5;
    char* end = nullptr;
    const unsigned long long addr = std::strtoull(start, &end, 16);
    if (end == start || !addr) return nullptr;
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(addr));
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        SwitchToThread();
        Sleep(1);
    }
    return pred();
}

void verify_presenter_affinity(const std::wstring& presenterPath, const std::filesystem::path& scratchDir,
    uint32_t initialHide, bool testLiveToggle)
{
    const auto logPath = (scratchDir / (initialHide ? L"affinity-hidden.log" : L"affinity-default.log")).wstring();
    std::error_code ec;
    std::filesystem::remove(logPath, ec);
    SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenterPath.c_str());
    SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", logPath.c_str());

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &dev, &fl, &ctx)), "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    expect(SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)), "adapter");

    const ipc::Shape shape{640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
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

    const auto key = LR"(Local\nr-vfg-affinity-)" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(initialHide);
    ipc::Producer producer(dev.Get(), key, shape);
    LaunchOptions opts{};
    opts.multiplier = 1;
    opts.hideFromCapture = initialHide;
    Controller controller(opts);

    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, shape, desc.AdapterLuid), "controller.offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0 &&
        producer.status() == ipc::Status::Ok, "IPC handshake ready");

    const auto publish_one = [&](uint64_t tick) {
        const auto now = clock_ns();
        const auto st = producer.publish(tex.Get(), now, now, false, tick);
        expect(st == ipc::Status::Ok || st == ipc::Status::Dropped, "publish");
    };

    HWND hwnd = nullptr;
    uint64_t tick = 1000000;
    expect(wait_until([&] {
        publish_one(tick += 16666667);
        hwnd = parse_hwnd(logPath);
        return hwnd != nullptr && IsWindow(hwnd);
    }, std::chrono::milliseconds(5000)), "presenter window handle logged");

    const DWORD expectedInitial = initialHide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    DWORD actual = 0xFFFFFFFFu;
    expect(wait_until([&] {
        publish_one(tick += 16666667);
        return GetWindowDisplayAffinity(hwnd, &actual) && actual == expectedInitial;
    }, std::chrono::milliseconds(3000)), "initial GetWindowDisplayAffinity mismatch");

    if (testLiveToggle) {
        controller.set_hide_from_capture(1);
        expect(wait_until([&] {
            publish_one(tick += 16666667);
            return GetWindowDisplayAffinity(hwnd, &actual) && actual == WDA_EXCLUDEFROMCAPTURE;
        }, std::chrono::milliseconds(3000)), "live toggle to WDA_EXCLUDEFROMCAPTURE (0x11) failed");

        controller.set_hide_from_capture(0);
        expect(wait_until([&] {
            publish_one(tick += 16666667);
            return GetWindowDisplayAffinity(hwnd, &actual) && actual == WDA_NONE;
        }, std::chrono::milliseconds(3000)), "live toggle back to WDA_NONE (0) failed");
    }

    controller.stop();
    expect(wait_until([&] { return !controller.alive(); }, std::chrono::milliseconds(5000)), "presenter exit");
    expect(producer.retire() == ipc::Status::Ok, "producer.retire");
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 4, "usage: vfg_capture_visibility_tests <presenter.exe> <repo_root> <scratch_dir>");
        const std::wstring presenter = argv[1];
        const std::filesystem::path repoRoot = argv[2];
        const std::filesystem::path scratch = argv[3];
        std::filesystem::create_directories(scratch);
        check_contracts(repoRoot);
        verify_presenter_affinity(presenter, scratch, 0, true);
        verify_presenter_affinity(presenter, scratch, 1, false);
        std::puts("PASS vfg_capture_visibility");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL vfg_capture_visibility: %s\n", e.what());
        return 1;
    }
}
