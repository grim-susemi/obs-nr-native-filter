// SPDX-License-Identifier: GPL-3.0-only
#pragma once
void compare_readback_test(Fixture& fixture)
{
    using namespace nr::fg::vfg;
    HWND window = CreateWindowExW(0, L"STATIC", L"VFG WARP fixture", WS_OVERLAPPED,
        0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "hidden window creation failed");
    {
        Session session{};
        session.width = width; session.height = height; session.gpuShotChange = 1;
        session.adapter = fixture.device->GetAdapterLuid();
        Gpu gpu(session, window, 2);
        auto* device = gpu.d3d_device();
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> cmd;
        ComPtr<ID3D12Fence> gate;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&cmd)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = imageBytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        void* mapped{};
        D3D12_RANGE empty{0, 0};
        check(upload->Map(0, &empty, &mapped));
        std::memset(mapped, 0x63, imageBytes);
        upload->Unmap(0, nullptr);
        for (UINT i = 0; i < 2; ++i) {
            auto barrier = transition(gpu.input[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, pitch};
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = gpu.input[i].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            barrier = transition(gpu.input[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            cmd->ResourceBarrier(1, &barrier);
        }
        Handle ready;
        ready.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(ready.h != nullptr, "comparison event creation failed");
        Handle drained;
        drained.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(drained.h != nullptr, "drain event creation failed");
        struct QueueDrain {
            ID3D12CommandQueue* queue;
            ID3D12Fence* gate;
            HANDLE event;
            ~QueueDrain() {
                if (FAILED(gate->SetEventOnCompletion(2, event)) || FAILED(gate->Signal(1)) ||
                    FAILED(queue->Signal(gate, 2)) || WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) {
                    std::fputs("GPU drain failed; retaining resources until process exit\n", stderr);
                    TerminateProcess(GetCurrentProcess(), 2);
                }
            }
        } drain{gpu.drawQueue.Get(), gate.Get(), drained.h};
        check(cmd->Close());
        ID3D12CommandList* lists[]{cmd.Get()};
        gpu.drawQueue->ExecuteCommandLists(1, lists);
        check(gpu.drawQueue->Wait(gate.Get(), 1));
        const bool emptyBefore = !gpu.compare_result().has_value();
        const bool submitted = gpu.begin_compare(0, 1, ready.h);
        const bool pending = !gpu.compare_result().has_value();
        check(gate->Signal(1));
        require(submitted, "comparison not submitted");
        require(WaitForSingleObject(ready.h, 30000) == WAIT_OBJECT_0, "comparison fence timed out");
        const auto result = gpu.compare_result();
        require(emptyBefore && pending, "stale comparison was returned");
        require(result && result->same && !result->cut && !result->flash,
            "identical compare included diagnostic words");
        require(!gpu.compare_result().has_value(), "comparison result reused");
        std::puts("compare-readback pending=empty same=1 cut=0 flash=0 consumed=empty PASS");
    }
    DestroyWindow(window);
}
