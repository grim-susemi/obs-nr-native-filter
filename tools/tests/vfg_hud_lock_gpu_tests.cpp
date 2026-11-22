// SPDX-License-Identifier: GPL-3.0-only
#include "../fg-presenter/vfg_gpu.h"
#include <dxgi1_4.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace nr::fg::vfg;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT width = 64, height = 36, rgbaPitch = width * 4, statePitch = width * 8;
using Image = std::array<uint32_t, width * height>;
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void checked(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(what); }
uint32_t gray(uint32_t n) { return 0xff000000u | n | (n << 8) | (n << 16); }
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER v{};
    v.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return v;
}
struct Window {
    HWND value = CreateWindowExW(0, L"STATIC", L"HUD WARP fixture", WS_OVERLAPPED,
        0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() { require(value != nullptr, "hidden window creation failed"); }
    ~Window() { DestroyWindow(value); }
};
struct Fixture {
    Window window;
    Session session{};
    std::unique_ptr<Gpu> gpu;
    ComPtr<ID3D12Fence> fence;
    Handle fenceEvent, compareEvent;
    uint64_t value{};
    explicit Fixture(unsigned multiplier = 4)
    {
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter"); checked(adapter->GetDesc(&desc), "WARP adapter description");
        session.width = width; session.height = height; session.gpuShotChange = 1; session.adapter = desc.AdapterLuid;
        gpu = std::make_unique<Gpu>(session, window.value, multiplier);
        checked(gpu->d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        fenceEvent.h = CreateEventW(nullptr, FALSE, FALSE, nullptr); compareEvent.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(fenceEvent.h && compareEvent.h, "event creation");
    }
    void drain()
    {
        const uint64_t next = ++value;
        checked(fence->SetEventOnCompletion(next, fenceEvent.h), "fence event");
        checked(gpu->drawQueue->Signal(fence.Get(), next), "queue signal");
        if (WaitForSingleObject(fenceEvent.h, 30000) != WAIT_OBJECT_0) {
            std::fputs("WARP fence timeout; preserving in-flight resources\n", stderr);
            TerminateProcess(GetCurrentProcess(), 2);
        }
        checked(gpu->d3d_device()->GetDeviceRemovedReason(), "WARP device removed");
    }
    template<typename F> void record(F&& action)
    {
        require(gpu->record_draw(action) != 0, "draw ring unavailable");
        drain();
    }
    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        checked(gpu->d3d_device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&result)), "buffer allocation");
        return result;
    }
    ComPtr<ID3D12Resource> texture()
    {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = gpu->input[0]->GetDesc();
        ComPtr<ID3D12Resource> result;
        checked(gpu->d3d_device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)), "destination texture");
        return result;
    }
    void upload(ID3D12Resource* dst, const Image& image)
    {
        auto staging = buffer(sizeof(image), D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{}; const D3D12_RANGE empty{0, 0};
        checked(staging->Map(0, &empty, &mapped), "upload map");
        std::memcpy(mapped, image.data(), sizeof(image)); staging->Unmap(0, nullptr);
        record([&](ID3D12GraphicsCommandList* cmd) {
            auto b = transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = staging.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, rgbaPitch};
            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            b = transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            cmd->ResourceBarrier(1, &b);
        });
    }
    std::vector<uint8_t> read(ID3D12Resource* src, DXGI_FORMAT format, UINT pitch)
    {
        auto staging = buffer(UINT64{pitch} * height, D3D12_HEAP_TYPE_READBACK);
        record([&](ID3D12GraphicsCommandList* cmd) {
            auto b = transition(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = staging.Get(); d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            d.PlacedFootprint.Footprint = {format, width, height, 1, pitch};
            cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            b = transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            cmd->ResourceBarrier(1, &b);
        });
        void* mapped{}; const D3D12_RANGE range{0, size_t{pitch} * height}, empty{0, 0};
        checked(staging->Map(0, &range, &mapped), "readback map");
        const auto* bytes = static_cast<const uint8_t*>(mapped);
        std::vector<uint8_t> result(bytes, bytes + range.End);
        staging->Unmap(0, &empty);
        return result;
    }
    std::vector<uint8_t> image(ID3D12Resource* r) { return read(r, DXGI_FORMAT_R8G8B8A8_UNORM, rgbaPitch); }
    std::vector<uint8_t> state() { return read(gpu->hud_lock().lock()->state(), DXGI_FORMAT_R16G16B16A16_FLOAT, statePitch); }
    void update(unsigned current, unsigned previous, bool reset) { gpu->update_hud_lock(current, previous, reset); drain(); }
    Gpu::CompareResult compare(unsigned a, unsigned b)
    {
        require(gpu->begin_compare(a, b, compareEvent.h), "compare not submitted");
        if (WaitForSingleObject(compareEvent.h, 30000) != WAIT_OBJECT_0) {
            std::fputs("comparison timed out; preserving in-flight resources\n", stderr);
            TerminateProcess(GetCurrentProcess(), 2);
        }
        auto result = gpu->compare_result(); require(result.has_value(), "comparison result unavailable");
        drain(); return *result;
    }
    ID3D12Resource* display(unsigned subframe, unsigned a, unsigned b, bool enabled, bool compared, unsigned parity)
    {
        ID3D12Resource* out{};
        record([&](ID3D12GraphicsCommandList* cmd) {
            Gpu::SyntheticFrame frame{gpu->syntheticTex[subframe].Get(), [](ID3D12GraphicsCommandList*) {},
                a, b, compared, parity, enabled, subframe};
            out = gpu->record_synthetic_display(cmd, frame);
        });
        require(out != nullptr, "display recorder returned null");
        return out;
    }
};
void equal_image(const std::vector<uint8_t>& actual, const Image& expected, const char* name)
{
    const auto* want = reinterpret_cast<const uint8_t*>(expected.data());
    for (size_t i = 0; i < actual.size(); ++i) if (actual[i] != want[i]) {
        std::fprintf(stderr, "%s byte %zu expected %02x got %02x\n", name, i, want[i], actual[i]);
        throw std::runtime_error(name);
    }
}
void pixel(const std::vector<uint8_t>& image, unsigned x, unsigned y, uint32_t expected, const char* name)
{
    uint32_t actual{}; std::memcpy(&actual, image.data() + (y * width + x) * 4, 4);
    if (actual != expected) {
        std::fprintf(stderr, "%s (%u,%u) expected %08x got %08x\n", name, x, y, expected, actual);
        throw std::runtime_error(name);
    }
}
void all_zero(const std::vector<uint8_t>& data, const char* name)
{
    for (size_t i = 0; i < data.size(); ++i) if (data[i] != 0) {
        std::fprintf(stderr, "%s byte %zu = %02x\n", name, i, data[i]); throw std::runtime_error(name);
    }
}
#include "vfg_hud_lock_regressions.h"
}
int main()
{
    try {
        Fixture f;
        Image generated{}; generated.fill(0xff5234c7u);
        for (auto& tex : f.gpu->syntheticTex) f.upload(tex.Get(), generated);
        require(f.gpu->hud_lock().expired(), "HUD allocated while off");
        auto* off = f.display(0, 0, 1, false, false, 0);
        require(off == f.gpu->syntheticTex[0].Get() && f.gpu->hud_lock().expired(), "off path allocated HUD");
        equal_image(f.image(off), generated, "off passthrough");
        Image frames[2]{};
        for (unsigned phase = 0; phase < 2; ++phase) {
            frames[phase].fill(gray(phase ? 40 : 32));
            for (unsigned y = 0; y < height; ++y) frames[phase][y * width + 32] = gray(y % 2 ? 0 : 255);
        }
        f.upload(f.gpu->input[0].Get(), frames[0]);
        f.update(0, 0, true);
        require(!f.gpu->hud_lock().expired(), "HUD not allocated on enable");
        all_zero(f.state(), "first reset state");
        unsigned prev = 0;
        for (unsigned i = 1; i <= 72; ++i) {
            const unsigned current = i % 3;
            f.upload(f.gpu->input[current].Get(), frames[i % 2]);
            f.update(current, prev, false);
            prev = current;
        }
        const unsigned current = 1;
        f.upload(f.gpu->input[current].Get(), frames[1]);
        equal_image(f.image(f.gpu->input[0].Get()), frames[0], "original slot 0");
        equal_image(f.image(f.gpu->input[1].Get()), frames[1], "original slot 1");
        equal_image(f.image(f.gpu->input[2].Get()), frames[1], "original slot 2");
        auto noncut = f.compare(prev, current);
        require(!noncut.cut, "non-cut comparison incorrectly cut");
        for (unsigned slot = 0; slot < 3; ++slot) {
            auto* out = f.display(slot, prev, current, true, true, noncut.parity);
            require(out == f.gpu->hud_lock().lock()->output(), "HUD scratch not displayed");
            const auto pixels = f.image(out);
            pixel(pixels, 32, 10, 0xffffffffu, "locked white stroke");
            pixel(pixels, 32, 11, 0xff000000u, "locked black stroke");
            pixel(pixels, 4, 10, 0xff5234c7u, "moving background");
        }
        auto* scratch = f.gpu->hud_lock().lock()->output();
        const auto cleanScratch = f.image(scratch), cleanState = f.state();
        const auto clean0 = f.image(f.gpu->input[0].Get()), clean1 = f.image(f.gpu->input[1].Get());
        const auto clean2 = f.image(f.gpu->input[2].Get()), cleanSynthetic = f.image(f.gpu->syntheticTex[2].Get());
        auto dest = f.texture();
        constexpr uint32_t overlay[]{0xff00a5f0u, 0xff00a5f0u, 0xff00a5f0u, 0xff00a5f0u};
        f.gpu->set_hud(1, 0, 2, 2, overlay);
        f.record([&](ID3D12GraphicsCommandList* cmd) { f.gpu->record_present_copy(cmd, {scratch, dest.Get()}); });
        auto presented = f.image(dest.Get());
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const size_t p = (y * width + x) * 4;
            if (x < 2 && y < 2) pixel(presented, x, y, overlay[0], "overlay destination");
            else require(std::memcmp(presented.data() + p, cleanScratch.data() + p, 4) == 0, "overlay changed unrelated pixel");
        }
        require(f.image(scratch) == cleanScratch && f.state() == cleanState &&
            f.image(f.gpu->input[0].Get()) == clean0 && f.image(f.gpu->input[1].Get()) == clean1 &&
            f.image(f.gpu->input[2].Get()) == clean2 && f.image(f.gpu->syntheticTex[2].Get()) == cleanSynthetic,
            "overlay modified clean resource");
        check_held_slots(f, frames[0]);
        Image moved = frames[1]; moved[10 * width + 32] = gray(0);
        f.upload(f.gpu->input[2].Get(), moved);
        f.update(2, 1, false);
        auto state = f.state();
        const size_t confidence = 10 * statePitch + 32 * 8;
        require(state[confidence] == 0 && state[confidence + 1] == 0, "motion did not clear confidence");
        pixel(f.image(f.display(0, 1, 2, true, false, 0)), 32, 10, 0xff5234c7u, "motion passthrough");
        f.update(2, 2, true);
        all_zero(f.state(), "reset state");
        equal_image(f.image(f.display(1, 1, 2, true, false, 0)), generated, "reset passthrough");
        Image black{}, white{}; black.fill(gray(0)); white.fill(gray(255)); f.upload(f.gpu->input[0].Get(), black); f.upload(f.gpu->input[1].Get(), white);
        auto cut = f.compare(0, 1);
        require(cut.cut && !cut.flash, "black-white comparison was not a real cut");
        auto* finalCut = f.display(2, 0, 1, true, true, cut.parity);
        require(finalCut == f.gpu->hud_lock().lock()->output(), "cut did not target scratch");
        equal_image(f.image(finalCut), white, "cut final scratch");
        check_hud_retirement(f, generated);
        check_original_only_repeat();
        check_abandoned_hud_compare();
        std::printf("PASS WARP: OFF HUD textures=0 (absent owner), updates=72, slots=3, stroke=ffffffff/ff000000, background=ff5234c7, overlay pixels=4, motion confidence=0000, reset zero bytes=%u, cut B bytes=%zu\n",
            statePitch * height, sizeof(white));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
