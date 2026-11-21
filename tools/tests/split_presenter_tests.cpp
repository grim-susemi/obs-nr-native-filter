// SPDX-License-Identifier: GPL-3.0-only
#include "fg_controller.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nr::fg;
using namespace nr::fg::vfg;
namespace {
void expect(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
HWND window_from_log(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line)) {
        const auto pos = line.find("hwnd=");
        if (pos != std::string::npos) {
            char* end{};
            const auto value = std::strtoull(line.c_str() + pos + 5, &end, 16);
            if (end != line.c_str() + pos + 5 && value)
                return reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
        }
    }
    return nullptr;
}
COLORREF pixel(HWND window, int percent, int offset = 0)
{
    RECT client{};
    if (!GetClientRect(window, &client)) return CLR_INVALID;
    POINT p{client.right * percent / 100 + offset, client.bottom / 2};
    if (!ClientToScreen(window, &p)) return CLR_INVALID;
    const HDC dc = GetDC(nullptr);
    if (!dc) return CLR_INVALID;
    const auto color = GetPixel(dc, p.x, p.y);
    ReleaseDC(nullptr, dc);
    return color;
}
bool red(COLORREF c) { return c != CLR_INVALID && GetRValue(c) > 180 && GetGValue(c) < 70 && GetBValue(c) < 70; }
bool blue(COLORREF c) { return c != CLR_INVALID && GetBValue(c) > 180 && GetGValue(c) < 70 && GetRValue(c) < 70; }
bool green(COLORREF c) { return c != CLR_INVALID && GetGValue(c) > 180 && GetRValue(c) < 70 && GetBValue(c) < 70; }
bool divider(HWND window, int percent)
{
    // DXGI stretch samples fractional positions on displays wider than the back buffer.
    for (int dx = -2; dx <= 12; ++dx)
        if (green(pixel(window, percent, dx))) return true;
    return false;
}
template<class Predicate>
void await(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return;
        SwitchToThread();
    }
    throw std::runtime_error(message);
}
void run(const wchar_t* presenter, const std::filesystem::path& scratch)
{
    expect(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr,
        "match presenter physical-pixel coordinates");
    std::filesystem::create_directories(scratch);
    const auto log = scratch / L"presenter.log";
    std::filesystem::remove(log);
    expect(SetEnvironmentVariableW(L"OBS_NR_VFG_PRESENTER", presenter) != FALSE, "set presenter");
    expect(SetEnvironmentVariableW(L"OBS_NR_VFG_LOG", log.c_str()) != FALSE, "set log");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context)), "create D3D11 device");
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    expect(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapterDesc)), "adapter");
    ipc::Shape shape{640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    shape.splitCompare = true;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = shape.width; desc.Height = shape.height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = shape.format; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> original(shape.width * shape.height, 0xff0000ffu);
    std::vector<uint32_t> processed(shape.width * shape.height, 0xffff0000u);
    D3D11_SUBRESOURCE_DATA beforeData{original.data(), shape.width * 4, 0};
    D3D11_SUBRESOURCE_DATA afterData{processed.data(), shape.width * 4, 0};
    ComPtr<ID3D11Texture2D> before, after;
    expect(SUCCEEDED(device->CreateTexture2D(&desc, &beforeData, &before)) &&
        SUCCEEDED(device->CreateTexture2D(&desc, &afterData, &after)), "create paired textures");
    const auto key = LR"(Local\nr-split-test-)" + std::to_wstring(GetCurrentProcessId());
    ipc::Producer producer(device.Get(), key, shape);
    LaunchOptions options{};
    options.multiplier = 1;
    Controller controller(options);
    const auto offer = producer.connect(controller.peer());
    expect(controller.offer(offer, shape, adapterDesc.AdapterLuid), "offer");
    expect(WaitForSingleObject(producer.control_event(), 10000) == WAIT_OBJECT_0 &&
        producer.status() == ipc::Status::Ok, "handshake");
    uint64_t tick = 1;
    uint32_t percent = 50;
    bool swap = false;
    const auto publish = [&] {
        const auto now = clock_ns();
        const auto status = producer.publish(after.Get(), now, now, false, tick += 16666667, before.Get(), percent, swap);
        expect(status == ipc::Status::Ok || status == ipc::Status::Dropped, "publish paired frame");
    };
    HWND window{};
    await([&] { publish(); window = window_from_log(log); return window && IsWindowVisible(window); },
        "presenter window not visible");
    expect(SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE,
        "raise presenter for screen readback");
    COLORREF left{}, middle{}, right{};
    try {
        await([&] {
            publish();
            left = pixel(window, 25); middle = pixel(window, 50); right = pixel(window, 75);
            return red(left) && blue(right) && divider(window, 50);
        }, "50 percent presenter pixels");
    } catch (...) {
        std::fprintf(stderr, "screen pixels left=%08lx divider=%08lx right=%08lx\n",
            static_cast<unsigned long>(left), static_cast<unsigned long>(middle), static_cast<unsigned long>(right));
        throw;
    }
    RECT client{};
    expect(GetClientRect(window, &client) != FALSE, "client rectangle");
    const int y = client.bottom / 2;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(client.right / 2, y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(client.right * 3 / 4, y));
    await([&] { publish(); return red(pixel(window, 60)) && blue(pixel(window, 90)) && divider(window, 75); },
        "divider must follow mouse move before the button is released");
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(client.right * 3 / 4, y));
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(client.right / 2, y));
    await([&] { publish(); return red(pixel(window, 60)) && blue(pixel(window, 90)) && divider(window, 75); },
        "released divider must retain its position");
    percent = 25;
    await([&] { publish(); return red(pixel(window, 10)) && blue(pixel(window, 60)) && divider(window, 25); },
        "changed OBS position must override drag");
    swap = true;
    await([&] { publish(); return blue(pixel(window, 10)) && red(pixel(window, 60)) && divider(window, 25); },
        "swapped presenter sides must show processed on left and original on right");
    controller.stop();
    await([&] { return !controller.alive(); }, "presenter exit");
    expect(producer.retire() == ipc::Status::Ok, "retire");
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        expect(argc == 3, "arguments: presenter scratch");
        run(argv[1], argv[2]);
        std::puts("PASS split_presenter");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL split_presenter: %s\n", e.what());
        return 1;
    }
}
