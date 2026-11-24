// SPDX-License-Identifier: GPL-3.0-only
#include "anr_mock_hardware.h"
#include "anr_mock_addon.h"
#include "vfg_generation_scope.h"

namespace {
using namespace anr_test;
enum class Case { Clean, BeforeProducer, AfterProducer, GuideSubmit, GuideClose, Release, Shutdown };
Case scenario{};
bool reachedConsumerSeam{};

struct Device {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    Device()
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "headless DXGI factory");
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "headless WARP adapter");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "headless WARP device");
        const D3D12_COMMAND_QUEUE_DESC desc{};
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "headless WARP queue");
    }
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format) const
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        ComPtr<ID3D12Resource> texture;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, common, nullptr,
            IID_PPV_ARGS(&texture)), "headless fixture texture");
        return texture;
    }
};

class Guides final : public IAnrGuideProvider {
    ComPtr<ID3D12Resource> motion_, depth_;
    std::optional<FlowResult> output_;
public:
    explicit Guides(const Device& d) : motion_(d.texture(DXGI_FORMAT_R16G16_FLOAT)),
        depth_(d.texture(DXGI_FORMAT_R32_FLOAT)) {}
    FlowProblem submit(const FlowInput& in) override
    {
        if (scenario == Case::GuideSubmit && !in.reset) return FlowProblem::ApiFailure;
        output_.emplace(FlowResult{in.current.key, motion_.Get(), depth_.Get(), nullptr, nullptr, in.current.ready,
            in.reset, nullptr, nullptr, nullptr, {}, {}, {}, {}, {}, {}, nullptr, nullptr, {}});
        return FlowProblem::None;
    }
    const FlowResult* result() const override { return output_ ? &*output_ : nullptr; }
    bool collect(const JobKey& key, NV_OF_FENCE_POINT consumer) override
    {
        if (!output_ || key != output_->key || !consumer.fence ||
            consumer.fence->GetCompletedValue() < consumer.value) return false;
        output_.reset();
        return true;
    }
    bool close() override { return scenario != Case::GuideClose && !output_; }
};

void trace_outcome(const fs::path& trace)
{
    const auto events = contents(trace);
    switch (scenario) {
    case Case::Release: require(events.ends_with("release_refused\n"), "release refusal called shutdown"); break;
    case Case::Shutdown:
        require(events.ends_with("release\nshutdown_refused\n"), "shutdown refusal did not follow release");
        break;
    case Case::GuideSubmit:
    case Case::GuideClose:
    case Case::Clean:
    case Case::BeforeProducer:
    case Case::AfterProducer:
        break;
    }
}

bool exercise(const fs::path& path)
{
    const Device d;
    const auto a = d.texture(DXGI_FORMAT_R8G8B8A8_UNORM), b = d.texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    const Environment before(L"OBS_NR_TEST_ANR_EVAL_RETURN", scenario == Case::BeforeProducer ? L"1" : nullptr);
    const Environment releaseFailure(L"OBS_NR_TEST_ANR_RELEASE_RETURN", scenario == Case::Release ? L"1" : nullptr);
    const Environment shutdownFailure(L"OBS_NR_TEST_ANR_SHUTDOWN_RETURN", scenario == Case::Shutdown ? L"1" : nullptr);
    auto backend = std::make_unique<AnrFgBackend>();
    require(backend->open({d.device.Get(), d.queue.Get(), extent, 1, path.native(), true, stderr},
        std::make_unique<Guides>(d)), "open local ownership fixture");
    bool originalAdmitted{}, generationReturned{}, requestedFallback{}, submitted{};
    try {
        run_generation([&] {
            const auto ticket = backend->try_submit_pair({a.Get(), b.Get(), serialBase + 1, serialBase + 2, 2, false}, 0, 1);
            submitted = ticket.has_value();
            requestedFallback = !ticket && backend->faulted() && backend->multiplier_limit() == 1;
            originalAdmitted = true;
        }, [&](bool) {
            check(backend->close() ? S_OK : E_FAIL, "ANR teardown unconfirmed");
            backend.reset();
            return false;
        });
        generationReturned = true;
    } catch (const nr::fg::ipc::Error& error) {
        std::printf("generation_fault=%s\n", error.what());
    }
    backend.reset();
    const auto loaded = GetModuleHandleW(path.c_str()) != nullptr;
    std::printf("ownership case=%u consumer_fixture_reached=%d original_admitted=%d generation_returned=%d runtime_loaded=%d\n",
        static_cast<unsigned>(scenario), reachedConsumerSeam, originalAdmitted, generationReturned, loaded);
    switch (scenario) {
    case Case::Clean: return submitted && originalAdmitted && generationReturned && !loaded;
    case Case::BeforeProducer: return requestedFallback && !reachedConsumerSeam && generationReturned && !loaded;
    case Case::AfterProducer: return reachedConsumerSeam && !originalAdmitted && !generationReturned;
    case Case::GuideSubmit:
    case Case::GuideClose:
        return !generationReturned && loaded;
    case Case::Release:
    case Case::Shutdown: {
        AnrFgBackend replacement;
        const bool reused = replacement.open({d.device.Get(), d.queue.Get(), extent, 2, path.native(), true, stderr},
            std::make_unique<Guides>(d));
        std::printf("quarantined_runtime_reused=%d\n", reused);
        return !generationReturned && loaded && !reused;
    }
    }
    return false;
}

DWORD run_child(const fs::path& executable, const fs::path& addon, unsigned index)
{
    auto command = L"\"" + executable.native() + L"\" --ownership-regression " + std::to_wstring(index) + L" \"" +
        addon.native() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    std::fflush(stdout);
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
        &startup, &child), "start owned headless case");
    Handle process, thread;
    process.h = child.hProcess;
    thread.h = child.hThread;
    if (WaitForSingleObject(process.h, 12000) != WAIT_OBJECT_0) {
        TerminateProcess(process.h, ERROR_TIMEOUT);
        (void)WaitForSingleObject(process.h, 3000);
        throw std::runtime_error("headless test child timed out");
    }
    DWORD result{};
    require(GetExitCodeProcess(process.h, &result), "read headless child exit");
    return result;
}
} // namespace

namespace nr::fg::vfg {
HRESULT anr_mock_consumer_reset(const AnrEvaluation& dispatch)
{
    if (scenario != Case::AfterProducer || dispatch.reset) return S_OK;
    reachedConsumerSeam = true;
    return E_FAIL;
}
} // namespace nr::fg::vfg

int anr_mock_ownership(int argc, wchar_t** argv)
{
    if (argc == 4) {
        const auto n = std::wstring_view(argv[2]);
        require(n.size() == 1 && n[0] >= L'0' && n[0] <= L'6', "invalid owned case");
        scenario = static_cast<Case>(n[0] - L'0');
        const fs::path addon(argv[3]);
        const auto trace = addon.parent_path() / ("trace-" + std::to_string(static_cast<unsigned>(scenario)));
        const Environment marker(L"OBS_NR_TEST_ANR_TRACE", trace.c_str());
        const auto passed = exercise(addon);
        trace_outcome(trace);
        return passed ? 0 : 1;
    }
    require(argc == 2, "invalid ownership fixture invocation");
    std::wstring exe(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    require(length && length < exe.size(), "locate ownership test executable");
    exe.resize(length);
    const auto dir = fs::path(exe).parent_path();
    const Scratch scratch(dir);
    const auto addon = stage(scratch.dir / "headless", dir / "anr_mock_dll.dll", true);
    unsigned failures{};
    for (unsigned index = 0; index < 7; ++index) {
        const auto exit = run_child(exe, addon, index);
        std::printf("ownership_case=%u exit=%lu\n", index, exit);
        if (exit) ++failures;
    }
    return failures ? 1 : 0;
}
