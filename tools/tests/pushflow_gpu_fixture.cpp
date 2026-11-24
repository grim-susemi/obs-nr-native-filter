// SPDX-License-Identifier: GPL-3.0-only
#include "pushflow_fixture.h"
#include "../fg-presenter/pushflow_compute.h"
#include "../fg-presenter/vfg_generation_scope.h"
#include <chrono>

namespace nr::fg::qa {
namespace {
using namespace nr::fg::vfg;
using namespace nr::fg::vfg::pushflow;

struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"PushFlow fixture", WS_POPUP,
        0, 0, fixtureWidth, fixtureHeight, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};

struct Transfer {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    Transfer(ID3D12Resource* texture, Gpu& gpu, D3D12_HEAP_TYPE type)
    {
        const auto tex = texture->GetDesc();
        auto* device = gpu.d3d_device();
        device->GetCopyableFootprints(&tex, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.MipLevels = desc.DepthOrArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&buffer)), "fixture transfer buffer");
    }

    void copy(ID3D12GraphicsCommandList* cmd, ID3D12Resource* texture, bool upload) const
    {
        D3D12_TEXTURE_COPY_LOCATION memory{}, image{};
        memory.pResource = buffer.Get();
        memory.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        memory.PlacedFootprint = footprint;
        image.pResource = texture;
        image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const auto state = upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        const auto enter = transition(texture, D3D12_RESOURCE_STATE_COMMON, state);
        cmd->ResourceBarrier(1, &enter);
        cmd->CopyTextureRegion(upload ? &image : &memory, 0, 0, 0, upload ? &memory : &image, nullptr);
        const auto leave = transition(texture, state, D3D12_RESOURCE_STATE_COMMON);
        cmd->ResourceBarrier(1, &leave);
    }
};

void finish(const Command& command, Gpu& gpu)
{
    ComPtr<ID3D12Fence> fence;
    Handle signal;
    check(gpu.d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fixture fence");
    signal.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!signal.h) throw std::runtime_error("fixture fence event");
    command.submit(gpu.drawQueue.Get());
    const auto signalled = gpu.drawQueue->Signal(fence.Get(), 1);
    const auto armed = SUCCEEDED(signalled) ? fence->SetEventOnCompletion(1, signal.h) : signalled;
    const auto wait = SUCCEEDED(armed) ? WaitForSingleObject(signal.h, 5000) : WAIT_FAILED;
    if (wait != WAIT_OBJECT_0 || fence->GetCompletedValue() != 1) {
        std::fprintf(stderr, "fixture queue drain failed wait=%lu signal=0x%08lX arm=0x%08lX removed=0x%08lX\n", wait,
            static_cast<unsigned long>(signalled), static_cast<unsigned long>(armed),
            static_cast<unsigned long>(gpu.d3d_device()->GetDeviceRemovedReason()));
        std::fflush(stderr);
        // Process isolation owns the device after a failed drain.
        std::_Exit(wait == WAIT_TIMEOUT ? 124 : 3);
    }
}

void close_fixture_backend(PushFlowBackend& backend, bool fault) noexcept
{
    if (fault) backend.generation_fault();
    try {
        if (backend.close()) return;
    } catch (...) {
        // Keep the fixture's queue and allocators owned until process exit.
    }
    std::fputs("fixture PushFlow shutdown could not verify release\n", stderr);
    std::fflush(stderr);
    std::_Exit(3);
}

void upload_endpoints(Gpu& gpu)
{
    Command command;
    command.prepare(gpu.d3d_device());
    auto* cmd = command.begin();
    std::array<Transfer, 2> uploads{Transfer(gpu.input[0].Get(), gpu, D3D12_HEAP_TYPE_UPLOAD),
        Transfer(gpu.input[1].Get(), gpu, D3D12_HEAP_TYPE_UPLOAD)};
    for (unsigned i = 0; i < uploads.size(); ++i) {
        auto& transfer = uploads[i];
        const auto square = i ? endpointB : endpointA;
        void* pixels{};
        const D3D12_RANGE noRead{0, 0};
        check(transfer.buffer->Map(0, &noRead, &pixels), "fixture map upload");
        const auto fp = transfer.footprint.Footprint;
        auto* data = static_cast<unsigned char*>(pixels);
        for (UINT y = 0; y < fp.Height; ++y) {
            for (UINT x = 0; x < fp.Width; ++x) {
                const bool white = int(x) >= square.x && int(x) < square.x + square.width &&
                    int(y) >= square.y && int(y) < square.y + square.width;
                const auto offset = size_t{y} * fp.RowPitch + size_t{x} * 4;
                data[offset] = data[offset + 1] = data[offset + 2] = white ? 255 : 0;
                data[offset + 3] = 255;
            }
        }
        transfer.buffer->Unmap(0, nullptr);
        transfer.copy(cmd, gpu.input[i].Get(), true);
    }
    finish(command, gpu);
}

std::vector<float> readback(Gpu& gpu, PushFlowBackend& backend, const FgPairTicket& ticket, unsigned sub)
{
    auto* texture = gpu.syntheticTex[sub - 1].Get();
    Transfer transfer(texture, gpu, D3D12_HEAP_TYPE_READBACK);
    Command command;
    command.prepare(gpu.d3d_device());
    auto* cmd = command.begin();
    backend.resolve_subframe(cmd, ticket, texture, sub);
    transfer.copy(cmd, texture, false);
    finish(command, gpu);
    void* pixels{};
    const D3D12_RANGE read{0, static_cast<SIZE_T>(transfer.bytes)};
    check(transfer.buffer->Map(0, &read, &pixels), "fixture map readback");
    const auto fp = transfer.footprint.Footprint;
    const auto* data = static_cast<const unsigned char*>(pixels);
    std::vector<float> frame(size_t{fp.Width} * fp.Height);
    for (UINT y = 0; y < fp.Height; ++y) {
        for (UINT x = 0; x < fp.Width; ++x) {
            const auto offset = size_t{y} * fp.RowPitch + size_t{x} * 4;
            frame[size_t{y} * fp.Width + x] = std::min(1.0f, (0.2126f * data[offset] + 0.7152f * data[offset + 1] +
                0.0722f * data[offset + 2]) / 255.0f);
        }
    }
    const D3D12_RANGE noWrite{0, 0};
    transfer.buffer->Unmap(0, &noWrite);
    return frame;
}

bool exercise_pair(Gpu& gpu, PushFlowBackend& backend, const Session& session, unsigned mult)
{
    const FgPairDesc pair{gpu.input[0].Get(), gpu.input[1].Get(), 1, 2, mult, false};
    const auto ticket = backend.submit_pair(pair, 0, 1);
    if (!ticket || ticket->lastSubmittedSub != mult - 1) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!backend.subframe_ready(*ticket, mult - 1)) {
        if (backend.faulted()) return false;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "fixture timed out waiting for %ux subframe fence\n", mult);
            return false;
        }
        Sleep(1);
    }
    std::array<std::vector<float>, max_multiplier - 1> frames;
    std::array<InterpolatedFrame, max_multiplier - 1> samples{};
    for (unsigned sub = 1; sub < mult; ++sub) {
        auto& pixels = frames[sub - 1];
        pixels = readback(gpu, backend, *ticket, sub);
        samples[sub - 1] = {{static_cast<int>(session.width), static_cast<int>(session.height), pixels}, double(sub) / mult};
    }
    return verify_interpolated_sequence(std::span(samples).first(mult - 1), endpointA, endpointB);
}
}

void real_pushflow(TestRun& test, const std::filesystem::path& runtime, unsigned expected)
{
    Window window;
    if (!window.handle) throw std::runtime_error("fixture window creation");
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "fixture DXGI factory");
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
        "fixture GPU adapter");
    DXGI_ADAPTER_DESC1 info{};
    check(adapter->GetDesc1(&info), "fixture adapter description");
    if (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) throw std::runtime_error("fixture requires a hardware adapter");
    Session session{};
    session.backend = Backend::PushFlow;
    session.pushflowTier = PushFlowTier::Quality;
    session.multiplier = max_multiplier;
    session.width = fixtureWidth;
    session.height = fixtureHeight;
    session.adapter = info.AdapterLuid;
    const auto path = std::filesystem::absolute(runtime);
    if (path.native().size() >= ARRAYSIZE(session.pushflowRuntimeDir))
        throw std::runtime_error("fixture runtime path too long");
    wcscpy_s(session.pushflowRuntimeDir, path.c_str());
    for (unsigned resize = 0; resize < 2; ++resize) {
        if (resize) { session.width += 64; session.height += 64; }
        Gpu gpu(session, window.handle, session.multiplier);
        if (resize == 0 && expected) {
            PushFlowBackend rejected;
            run_generation([&] {
                const auto opposite = expected == 1 ? 4u : 1u;
                const bool opened = rejected.open({session, stdout, [] {}, [] { return false; }, opposite}, &gpu);
                test.expect(!opened && rejected.faulted() && !rejected.runtime_fault() &&
                    rejected.output_count() == expected, "real model rejects the opposite CLI output count before inference");
            }, [&](bool fault) { close_fixture_backend(rejected, fault); return false; });
            if (test.failures) return;
        }
        PushFlowBackend backend;
        run_generation([&] {
        const bool opened = backend.open({session, stdout, [] {}, [] { return false; }, expected}, &gpu);
        std::printf("MEASURE resize=%u dimensions=%ux%u expected_outputs=%u actual_outputs=%zu shader=%s\n",
            resize, session.width, session.height, expected, backend.output_count(),
            backend.shader_path() == ShaderPath::Unpack ? "unpack" : "reconstruct");
        test.expect(opened, "runtime opened with requested output count and shader path");
        if (!opened) return;
        upload_endpoints(gpu);
        for (const unsigned mult : {2u, 3u, 4u}) {
            std::printf("GPU resize=%u multiplier=%u\n", resize, mult);
            test.expect(exercise_pair(gpu, backend, session, mult), "real DirectML generates ordered intermediate squares");
            if (test.failures) return;
        }
        }, [&](bool fault) { close_fixture_backend(backend, fault); return false; });
        if (test.failures) return;
        test.expect(!backend.faulted(), "DirectML tears down with the pool held");
    }
}
} // namespace nr::fg::qa
