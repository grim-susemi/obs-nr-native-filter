// SPDX-License-Identifier: GPL-3.0-only
// Official XeSS 3.0.2 XeFG presenter contract tests.
//
// In-process: constants, neutral depth readback, runtime admission (unsigned
// refusal, pinned mock acceptance, wrong-hash refusal), swapchain-less Gpu, and
// the XeFG/XeLL teardown order.
//
// Child process (the real presenter loop with mock libxess_fg/libxell):
// 10 frames with a shot cut, three back-to-back resize generations, a watchdog
// stall with the 1x recovery relaunch, and clean 1x refusal of unsigned and
// wrong-hash runtimes. The mock DLLs append every ABI call to a file so the
// assertions are made from observed state, not from the presenter's claims.
#include "fg_controller.h"
#include "xefg_presenter.h"
#include "xefg_runtime.h"
#include <bcrypt.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nr::fg;
using namespace nr::fg::vfg;
namespace fs = std::filesystem;

namespace {
void expect(bool ok, const char* why)
{
    if (!ok) throw std::runtime_error(why);
}

std::string read_text(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

size_t count_occurrences(std::string_view text, std::string_view needle)
{
    size_t count = 0, position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        if (end > start) lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

struct EnvScope {
    struct Saved {
        std::wstring name;
        std::optional<std::wstring> value;
    };
    std::vector<Saved> saved;
    void set(const wchar_t* name, const std::wstring& value)
    {
        const auto known = std::find_if(saved.begin(), saved.end(),
            [&](const Saved& entry) { return entry.name == name; });
        if (known == saved.end()) {
            wchar_t buffer[32768]{};
            const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
            Saved entry{name, std::nullopt};
            if (length && length < ARRAYSIZE(buffer)) entry.value = std::wstring(buffer, length);
            saved.push_back(std::move(entry));
        }
        SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
    }
    void clear(const wchar_t* name) { set(name, L""); }
    ~EnvScope()
    {
        for (const auto& entry : saved)
            SetEnvironmentVariableW(entry.name.c_str(), entry.value ? entry.value->c_str() : nullptr);
    }
};

std::wstring widen(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string sha256_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    const std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(stream), {});
    expect(!bytes.empty(), "hash fixture is readable");
    BCRYPT_ALG_HANDLE algorithm{};
    expect(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "bcrypt open");
    std::array<unsigned char, 32> digest{};
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, const_cast<unsigned char*>(bytes.data()),
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    expect(status >= 0, "bcrypt sha256");
    const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

void trust_mock(EnvScope& env, const fs::path& runtimeDir, const std::wstring& callLog)
{
    env.set(L"OBS_NR_TEST_XESS_TRUST_MOCK", L"1");
    env.set(L"OBS_NR_TEST_XESS_FG_SHA256", widen(sha256_file(runtimeDir / L"libxess_fg.dll")));
    env.set(L"OBS_NR_TEST_XESS_XELL_SHA256", widen(sha256_file(runtimeDir / L"libxell.dll")));
    env.set(L"OBS_NR_TEST_XESS_CALL_LOG", callLog);
}

void wait_for(const std::function<bool()>& predicate, DWORD timeoutMs, const char* why,
    const Controller* controller = nullptr)
{
    const auto deadline = clock_ns() + uint64_t{timeoutMs} * 1'000'000;
    while (clock_ns() < deadline) {
        if (predicate()) return;
        if (controller && !controller->alive())
            throw std::runtime_error(std::string("presenter exited while waiting: ") + why);
        Sleep(10);
    }
    throw std::runtime_error(std::string("timeout: ") + why);
}

void wait_for_log(const fs::path& log, std::string_view needle, size_t count, DWORD timeoutMs,
    const char* why, const Controller* controller = nullptr)
{
    wait_for([&] { return count_occurrences(read_text(log), needle) >= count; }, timeoutMs, why, controller);
}

void wait_initialized(const fs::path& log, const Controller& controller, const char* backend, size_t count)
{
    wait_for_log(log, std::string("event=initialized backend=") + backend, count, 20000,
        "presenter initialization", &controller);
}

struct TestWindow {
    HWND hwnd{};
    TestWindow()
    {
        hwnd = CreateWindowExW(0, L"STATIC", L"xefg-presenter-test", WS_POPUP, 0, 0, 1280, 720,
            nullptr, nullptr, nullptr, nullptr);
        expect(hwnd != nullptr, "hidden test window");
    }
    ~TestWindow() { if (hwnd) DestroyWindow(hwnd); }
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
};

ComPtr<ID3D12Device> make_d3d12_device()
{
    ComPtr<ID3D12Device> device;
    expect(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
        "D3D12 device");
    return device;
}

Session make_session(ID3D12Device* device, const fs::path& runtimeDir)
{
    Session session{};
    session.backend = Backend::XeFg;
    session.width = 1280;
    session.height = 720;
    session.adapter = device->GetAdapterLuid();
    session.multiplier = 2;
    session.generation = 1;
    session.launchEpoch = 1;
    const auto dir = runtimeDir.wstring();
    expect(dir.size() < ARRAYSIZE(session.xessRuntimeDir), "runtime dir fits session");
    std::copy(dir.begin(), dir.end(), session.xessRuntimeDir);
    session.xessRuntimeDir[dir.size()] = L'\0';
    return session;
}

void constants_contract()
{
    XeFgFrameInput input{};
    input.width = 2560;
    input.height = 1440;
    auto constants = xefg_frame_constants(input);
    expect(constants.resetHistory == 0, "no reset without a cut");
    expect(constants.viewMatrix[0] == 1.0f && constants.viewMatrix[5] == 1.0f &&
        constants.viewMatrix[10] == 1.0f && constants.viewMatrix[15] == 1.0f, "identity view matrix");
    unsigned zeros = 0;
    for (unsigned i = 0; i < 16; ++i) if (i % 5) zeros += constants.viewMatrix[i] == 0.0f ? 1u : 0u;
    expect(zeros == 12, "identity view has exactly twelve zero entries");
    const float f = 1.0f / std::tan(60.0f * 3.14159265358979323846f / 360.0f);
    expect(std::abs(constants.projectionMatrix[5] - f) < 1e-5f, "fixed 60 degree vertical projection");
    expect(std::abs(constants.projectionMatrix[0] - f * 1440.0f / 2560.0f) < 1e-5f,
        "projection respects the render aspect");
    expect(constants.projectionMatrix[11] == -1.0f, "D3D projection w = -z");
    expect(constants.jitterOffsetX == 0.0f && constants.jitterOffsetY == 0.0f, "zero jitter");
    for (const auto cut : {true, false}) {
        XeFgFrameInput shot{};
        shot.shotCut = cut;
        expect((xefg_frame_constants(shot).resetHistory == 1) == cut, "shotCut maps to resetHistory");
    }
    XeFgFrameInput discontinuity{};
    discontinuity.discontinuity = true;
    expect(xefg_frame_constants(discontinuity).resetHistory == 1, "discontinuity maps to resetHistory");
    XeFgFrameInput flowReset{};
    flowReset.flowReset = true;
    expect(xefg_frame_constants(flowReset).resetHistory == 1, "flow reset maps to resetHistory");
    XeFgFrameInput scaled{};
    scaled.mvScaleX = 10.0f / 7.0f;
    scaled.mvScaleY = 0.75f;
    scaled.frameRenderTimeMs = 500.0f;
    auto result = xefg_frame_constants(scaled);
    expect(result.motionVectorScaleX == 10.0f / 7.0f && result.motionVectorScaleY == 0.75f,
        "motion scale passes through");
    expect(result.frameRenderTime == 100.0f, "render time is clamped");
    std::printf("PASS xefg_presenter: constants 2d_identity_view fixed_60deg zero_jitter "
        "shotCut/discontinuity/flowReset -> resetHistory=1 mv_scale passthrough\n");
}

void depth_contract(const fs::path& runtimeDir, const fs::path& scratch)
{
    EnvScope env;
    env.set(L"OBS_NR_TEST_XESS_CALL_LOG", (scratch / L"depth-calls.log").wstring());
    auto device = make_d3d12_device();
    Session session = make_session(device.Get(), runtimeDir);
    TestWindow window;
    Gpu gpu(session, window.hwnd, 2, false);
    expect(gpu.swapchain_less() && gpu.swapchain() == nullptr, "Gpu constructed without a swapchain");
    expect(gpu.dxgi_factory() != nullptr && gpu.d3d_device() != nullptr && gpu.drawQueue != nullptr,
        "swapchain-less Gpu still owns device, queues, and factory");
    XeFgGuides guides;
    expect(guides.ensure(gpu, 896, 504), "guide extent 896x504 allocated");
    expect(guides.width() == 896 && guides.height() == 504, "guide extent recorded");
    expect(guides.verify_neutral(gpu.d3d_device(), gpu.drawQueue.Get()), "neutral depth is R32_FLOAT 1.0");
    expect(guides.ensure(gpu, 960, 540), "guide extent changes cleanly");
    expect(guides.verify_neutral(gpu.d3d_device(), gpu.drawQueue.Get()), "resized depth is still 1.0");
    std::printf("PASS xefg_presenter: gpu swapchain_less=true depth R32_FLOAT=1.0 extent=896x504 "
        "then 960x540\n");
}

void loader_contract(const fs::path& runtimeDir, const fs::path& scratch)
{
    const auto log = scratch / L"loader.log";
    FILE* file = nullptr;
    _wfopen_s(&file, log.c_str(), L"wb");
    struct File { FILE*& f; ~File() { if (f) fclose(f); } } close{file};
    EnvScope env;
    env.clear(L"OBS_NR_TEST_XESS_TRUST_MOCK");
    env.clear(L"OBS_NR_TEST_XESS_FG_SHA256");
    env.clear(L"OBS_NR_TEST_XESS_XELL_SHA256");
    XeFgRuntime runtime;
    expect(runtime.open(runtimeDir.wstring(), file) == XeFgLoadStatus::SignatureRejected,
        "unsigned fixtures are refused before admission");
    expect(!runtime.loaded(), "refused runtime has no exports");
    env.set(L"OBS_NR_TEST_XESS_TRUST_MOCK", L"1");
    env.set(L"OBS_NR_TEST_XESS_FG_SHA256", widen(sha256_file(runtimeDir / L"libxess_fg.dll")));
    env.set(L"OBS_NR_TEST_XESS_XELL_SHA256", widen(sha256_file(runtimeDir / L"libxell.dll")));
    expect(runtime.open(runtimeDir.wstring(), file) == XeFgLoadStatus::Ok, "pinned mock runtime admitted");
    expect(runtime.loaded() && runtime.test_fixture(), "all required exports resolved");
    const auto version = runtime.swapchain_version();
    expect(version.major == 3 && version.minor == 0 && version.patch == 2, "mock reports 3.0.2");
    runtime.close();
    env.set(L"OBS_NR_TEST_XESS_FG_SHA256", std::wstring(64, L'0'));
    expect(runtime.open(runtimeDir.wstring(), file) == XeFgLoadStatus::HashMismatch,
        "wrong pinned hash refused");
    env.clear(L"OBS_NR_TEST_XESS_TRUST_MOCK");
    expect(runtime.open((runtimeDir / L"missing").wstring(), file) == XeFgLoadStatus::MissingLibrary,
        "missing runtime directory refused");
    expect(runtime.open(L"relative-xess", file) == XeFgLoadStatus::UntrustedPath,
        "relative runtime directory refused");
    std::printf("PASS xefg_presenter: loader unsigned=%s pinned=ok wrong_hash=%s missing=%s "
        "relative=%s exports_resolved=1\n",
        xefg_load_status_name(XeFgLoadStatus::SignatureRejected),
        xefg_load_status_name(XeFgLoadStatus::HashMismatch),
        xefg_load_status_name(XeFgLoadStatus::MissingLibrary),
        xefg_load_status_name(XeFgLoadStatus::UntrustedPath));
}

void presenter_lifecycle(const fs::path& runtimeDir, const fs::path& scratch)
{
    const auto callLog = scratch / L"lifecycle-calls.log";
    std::error_code ignored;
    fs::remove(callLog, ignored);
    EnvScope env;
    trust_mock(env, runtimeDir, callLog.wstring());
    auto device = make_d3d12_device();
    Session session = make_session(device.Get(), runtimeDir);
    TestWindow window;
    Gpu gpu(session, window.hwnd, 2, false);
    auto runtime = std::make_unique<XeFgRuntime>();
    FILE* file = nullptr;
    _wfopen_s(&file, (scratch / L"lifecycle.log").c_str(), L"wb");
    struct File { FILE*& f; ~File() { if (f) fclose(f); } } close{file};
    expect(runtime->open(runtimeDir.wstring(), file) == XeFgLoadStatus::Ok, "lifecycle runtime admitted");
    XeFgPresenter presenter(gpu, session, file);
    expect(presenter.open(std::move(runtime)), "XeFG presenter initializes on the mock ABI");
    expect(presenter.owns_presentation(), "XeFG owns presentation");
    expect(presenter.max_in_flight_pairs() == 1, "one original pair in flight");
    expect(presenter.close(), "XeFG teardown succeeds");
    const auto calls = read_text(callLog);
    const auto destroy = calls.find("xefg_destroy");
    const auto xellDestroy = calls.find("xell_destroy");
    expect(calls.find("xefg_init ") != std::string::npos, "proxy swapchain initialized");
    expect(destroy != std::string::npos && xellDestroy != std::string::npos && destroy < xellDestroy,
        "xefgSwapChainDestroy precedes xellDestroyContext");
    expect(count_occurrences(calls, "xefg_destroy") == 1 && count_occurrences(calls, "xell_destroy") == 1,
        "exactly one teardown pair");
    std::printf("PASS xefg_presenter: lifecycle init -> destroy -> xell_destroy order "
        "swapchain_less=1 owns_presentation=1\n");
}

void latency_fallback(const fs::path& runtimeDir, const fs::path& scratch)
{
    const auto callLog = scratch / L"latency-calls.log";
    const auto presenterLog = scratch / L"latency.log";
    std::error_code ignored;
    fs::remove(callLog, ignored);
    fs::remove(presenterLog, ignored);
    EnvScope env;
    trust_mock(env, runtimeDir, callLog.wstring());
    env.set(L"OBS_NR_TEST_XESS_FORCE_LATENCY_UNSUPPORTED", L"1");
    auto device = make_d3d12_device();
    Session session = make_session(device.Get(), runtimeDir);
    TestWindow window;
    Gpu gpu(session, window.hwnd, 2, false);
    FILE* file = nullptr;
    _wfopen_s(&file, presenterLog.c_str(), L"wb");
    struct File { FILE*& f; ~File() { if (f) fclose(f); } } close{file};
    auto runtime = std::make_unique<XeFgRuntime>();
    expect(runtime->open(runtimeDir.wstring(), file) == XeFgLoadStatus::Ok, "latency runtime admitted");
    XeFgPresenter presenter(gpu, session, file);
    expect(presenter.open(std::move(runtime)),
        "XeFG initializes after dropping unsupported XeLL latency reduction");
    expect(presenter.close(), "latency fallback teardown");
    if (file) {
        std::fclose(file);
        file = nullptr;
    }
    const auto calls = read_text(callLog);
    expect(calls.find("xefg_enabled latency_unsupported") != std::string::npos,
        "driver rejected XeLL latency reduction");
    expect(count_occurrences(calls, "xefg_create_context") == 2,
        "context is recreated without the XeLL link");
    expect(count_occurrences(calls, "xefg_latency_reduction context=") == 1,
        "latency reduction is linked only on the first context");
    expect(count_occurrences(calls, "xefg_enabled enabled=1") == 1,
        "2x enabled on the retry without latency reduction");
    const auto log = read_text(presenterLog);
    expect(log.find("event=xefg_latency_reduction_unsupported fallback=disabled") != std::string::npos,
        "fallback is logged");
    expect(log.find("latency_reduction=0") != std::string::npos,
        "initialized without XeLL latency reduction");
    std::printf("PASS xefg_presenter: latency_fallback set_enabled=-15 -> xell link disabled -> 2x enabled\n");
}

struct ProducerFixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> color, motion;
    std::unique_ptr<ipc::Producer> producer;
    ipc::Shape shape;
    LUID luid{};
    std::vector<uint32_t> pixels;
    ProducerFixture(const std::wstring& mapping, uint32_t width, uint32_t height, uint32_t guideWidth,
        uint32_t guideHeight)
        : shape{width, height, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
              {}, false, false, {guideWidth, guideHeight}}
    {
        D3D_FEATURE_LEVEL level{};
        expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, &level, &context)), "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        expect(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&desc)), "producer adapter");
        luid = desc.AdapterLuid;
        color = make_texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
        motion = make_texture(guideWidth, guideHeight, DXGI_FORMAT_R16G16_FLOAT);
        std::vector<uint8_t> zeros(size_t{guideWidth} * guideHeight * 4, 0);
        context->UpdateSubresource(motion.Get(), 0, nullptr, zeros.data(), guideWidth * 4, 0);
        pixels.assign(size_t{width} * height, 0xFF102030u);
        producer = std::make_unique<ipc::Producer>(device.Get(), mapping, shape);
    }
    ComPtr<ID3D11Texture2D> make_texture(uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "producer texture");
        return texture;
    }
    ipc::FrameKey key(uint64_t frame, uint64_t tick) const
    {
        return ipc::FrameKey{0x51u, static_cast<uint32_t>(producer->generation()), frame, tick, 1, 2};
    }
    void publish(uint64_t frame, uint64_t tick, bool shotCut)
    {
        const uint32_t bar = static_cast<uint32_t>((frame * 40) % (shape.width - 64));
        std::fill(pixels.begin(), pixels.end(), 0xFF102030u);
        for (uint32_t y = 100; y < 164; ++y)
            for (uint32_t x = bar; x < bar + 64; ++x)
                pixels[size_t{y} * shape.width + x] = 0xFFE8E8E8u;
        context->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(), shape.width * 4, 0);
        ipc::FlowSample flow{};
        flow.owner = ipc::FlowOwner::NrOwned;
        flow.provenance = ipc::FlowProvenance::Valid;
        flow.mvScaleX = static_cast<float>(shape.width) / static_cast<float>(shape.guides.width);
        flow.mvScaleY = static_cast<float>(shape.height) / static_cast<float>(shape.guides.height);
        flow.pairPrevTick = frame ? frame - 1 : 0;
        flow.pairCurrTick = frame;
        flow.shotCut = shotCut;
        flow.hasSharedMv = true;
        flow.currColor = key(frame, tick);
        flow.prevColor = frame > 1 ? key(frame - 1, tick - 16'666'667) :
            key(0x1000, 0x1000ull * 16'666'667ull);
        const ipc::FlowBundle bundle{motion.Get(), flow.currColor, flow};
        const auto status = producer->publish(color.Get(), tick, clock_ns(), false, tick, nullptr, 50,
            false, bundle);
        if (status != ipc::Status::Ok && status != ipc::Status::Dropped) {
            D3D11_TEXTURE2D_DESC colorDesc{}, motionDesc{};
            color->GetDesc(&colorDesc);
            motion->GetDesc(&motionDesc);
            char message[512]{};
            std::snprintf(message, sizeof(message),
                "producer publish status=%d frame=%llu gen=%llu shape=%ux%u guide=%ux%u color=%ux%u f=%u mips=%u arr=%u "
                "motion=%ux%u f=%u mips=%u arr=%u prev(g=%u s=%llu t=%llu) curr(g=%u s=%llu t=%llu) "
                "mv=%d owner=%d prov=%d scale=%.4f,%.4f ticks=%llu,%llu",
                static_cast<int>(status), static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(producer->generation()), shape.width, shape.height,
                shape.guides.width, shape.guides.height, colorDesc.Width, colorDesc.Height,
                static_cast<unsigned>(colorDesc.Format), colorDesc.MipLevels, colorDesc.ArraySize,
                motionDesc.Width, motionDesc.Height, static_cast<unsigned>(motionDesc.Format),
                motionDesc.MipLevels, motionDesc.ArraySize, flow.prevColor.resourceGeneration,
                static_cast<unsigned long long>(flow.prevColor.submissionSerial),
                static_cast<unsigned long long>(flow.prevColor.canonicalTick),
                flow.currColor.resourceGeneration,
                static_cast<unsigned long long>(flow.currColor.submissionSerial),
                static_cast<unsigned long long>(flow.currColor.canonicalTick),
                flow.hasSharedMv ? 1 : 0, static_cast<int>(flow.owner), static_cast<int>(flow.provenance),
                flow.mvScaleX, flow.mvScaleY, static_cast<unsigned long long>(flow.pairPrevTick),
                static_cast<unsigned long long>(flow.pairCurrTick));
            throw std::runtime_error(message);
        }
    }
};

std::wstring mapping_name(const wchar_t* tag, uint32_t serial)
{
    return std::wstring(L"Local\\nr-xefg-test-") + tag + L"-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(serial);
}

void happy_child(const fs::path& runtimeDir, const fs::path& presenterExe, const fs::path& scratch)
{
    const auto presenterLog = scratch / L"happy-presenter.log";
    const auto callLog = scratch / L"happy-calls.log";
    std::error_code ignored;
    fs::remove(presenterLog, ignored);
    fs::remove(callLog, ignored);
    EnvScope env;
    trust_mock(env, runtimeDir, callLog.wstring());
    env.set(L"OBS_NR_VFG_PRESENTER", presenterExe.wstring());
    env.set(L"OBS_NR_VFG_LOG", presenterLog.wstring());
    env.clear(L"OBS_NR_TEST_STALL_WORKER_MS");
    ProducerFixture fixture(mapping_name(L"happy", 1), 1280, 720, 896, 504);
    LaunchOptions options{};
    options.backend = Backend::XeFg;
    options.multiplier = 2;
    options.xessRuntimeDir = runtimeDir.wstring();
    options.fpsOverlay = 1;
    options.lowLatency = 1;
    Controller controller(options);
    auto offer = fixture.producer->connect(controller.peer());
    expect(controller.offer(offer, fixture.shape, fixture.luid), "happy offer");
    expect(WaitForSingleObject(fixture.producer->control_event(), 10000) == WAIT_OBJECT_0 &&
        fixture.producer->status() == ipc::Status::Ok, "happy IPC handshake");
    wait_initialized(presenterLog, controller, "xefg", 1);
    const auto start = clock_ns();
    for (uint64_t frame = 1; frame <= 10; ++frame) {
        const auto tick = start + frame * 16'666'667ull;
        while (clock_ns() < tick) Sleep(1);
        fixture.publish(frame, tick, frame == 5);
    }
    wait_for_log(presenterLog, "event=xefg_frame ", 8, 15000, "eight XeFG originals presented", &controller);
    for (int resize = 0; resize < 3; ++resize) {
        expect(controller.request_resize(), "happy request resize");
        wait_for([&] { return controller.resize_drained(); }, 10000, "resize drained", &controller);
        offer = fixture.producer->connect(controller.peer());
        expect(controller.offer(offer, fixture.shape, fixture.luid), "happy re-offer");
        expect(WaitForSingleObject(fixture.producer->control_event(), 10000) == WAIT_OBJECT_0,
            "happy re-offer handshake");
        wait_initialized(presenterLog, controller, "xefg", static_cast<size_t>(resize) + 2);
    }
    controller.stop();
    wait_for([&] { return !controller.alive(); }, 10000, "happy presenter exit");
    expect(fixture.producer->retire() == ipc::Status::Ok, "happy producer retire");

    const auto log = read_text(presenterLog);
    const auto calls = read_text(callLog);
    expect(count_occurrences(log, "event=offer mode=2x") == 4, "four 2x offers");
    expect(count_occurrences(log, "event=initialized backend=xefg") == 4, "four XeFG generations");
    expect(count_occurrences(log, "event=resize_drained") == 3, "three resize drains");
    expect(count_occurrences(log, "event=xefg_teardown") == 4, "four XeFG teardowns");
    expect(count_occurrences(log, "event=error") == 0 &&
        log.find("DXGI_ERROR_INVALID_CALL") == std::string::npos &&
        log.find("0x887A0001") == std::string::npos, "zero presenter errors / invalid calls");
    expect(count_occurrences(log, "event=session_finished exit_reason=0") == 1, "clean session exit");
    expect(count_occurrences(calls, "xefg_destroy") == 4, "four swapchain destroys");
    expect(count_occurrences(calls, "xell_destroy") == 4, "four XeLL destroys");
    expect(count_occurrences(calls, "xefg_tag_constants") >= 8, "every original tagged");
    expect(count_occurrences(calls, "xefg_tag_resource") >= 24, "depth, motion, and backbuffer tagged");
    expect(calls.find(" type=1 validity=1 state=0 w=896 h=504 format=41 ") != std::string::npos,
        "neutral R32_FLOAT depth tagged at the shared MV guide extent");
    expect(calls.find(" type=2 validity=1 state=0 w=896 h=504 format=34 ") != std::string::npos,
        "R16G16_FLOAT shared MV proxy tagged at the guide extent");
    expect(calls.find("xefg_init hwnd=") != std::string::npos &&
        calls.find("max_interpolations=1 ui_mode=1") != std::string::npos,
        "official 2x init with UI_MODE_NONE");
    expect(calls.find("view0=1.000000") != std::string::npos &&
        calls.find("proj5=1.732051") != std::string::npos &&
        calls.find("proj11=-1.000000") != std::string::npos && calls.find("jitter=0.000000,0.000000")
            != std::string::npos, "identity view, fixed 60 degree projection, zero jitter");
    expect(calls.find("mv_scale=1.428571,1.428571") != std::string::npos,
        "motion scales from the shared analysis extent");
    expect(calls.find("xell_sleep frame_id=") != std::string::npos, "low latency calls xellSleep");
    expect(count_occurrences(calls, "xell_marker") >= 5 * 8, "XeLL markers around every present");
    const auto lines = split_lines(log);
    bool cutPresented = false;
    std::optional<unsigned> cutId;
    for (const auto& line : lines) {
        if (line.find("event=xefg_frame ") == std::string::npos || line.find(" shot_cut=1 ") == std::string::npos)
            continue;
        cutPresented = true;
        expect(line.find(" reset=1 ") != std::string::npos, "shot cut frame resets XeFG history");
        const auto at = line.find("present_id=");
        expect(at != std::string::npos, "shot cut frame has a present id");
        cutId = static_cast<unsigned>(std::stoul(line.substr(at + 11)));
    }
    expect(cutPresented && cutId.has_value(), "frame 5 shot cut was presented");
    expect(calls.find("xefg_tag_constants present_id=" + std::to_string(*cutId) + " reset=1 ") !=
        std::string::npos, "mock observed resetHistory=1 for the shot cut present id");
    expect(calls.find("xefg_tag_constants present_id=" + std::to_string(*cutId) + " reset=0 ") ==
        std::string::npos, "shot cut present id is not tagged without reset");
    auto from = size_t{0};
    for (int index = 0; index < 3; ++index) {
        const auto drained = log.find("event=resize_drained", from);
        expect(drained != std::string::npos, "resize drain line exists");
        const auto teardown = log.rfind("event=xefg_teardown", drained);
        expect(teardown != std::string::npos && teardown < drained,
            "teardown precedes every resize_drained");
        from = drained + 1;
    }
    const auto callLines = split_lines(calls);
    for (size_t index = 0; index < callLines.size(); ++index) {
        if (callLines[index] != "xefg_destroy") continue;
        expect(index + 1 < callLines.size() && callLines[index + 1] == "xell_destroy",
            "every xefgSwapChainDestroy is immediately followed by xellDestroyContext");
    }
    const auto finished = log.rfind("event=session_finished");
    const auto finalTeardown = log.rfind("event=xefg_teardown");
    expect(finished != std::string::npos && finalTeardown != std::string::npos && finalTeardown < finished,
        "final XeFG teardown precedes session_finished");
    std::printf("PASS xefg_presenter: happy init -> 10 frames shot_cut=5 resetHistory=1 -> 3x resize "
        "destroy<xellDestroy<state3 -> clean exit ofa=0 invalid_calls=0\n");
}

void watchdog_child(const fs::path& runtimeDir, const fs::path& presenterExe, const fs::path& scratch)
{
    const auto presenterLog = scratch / L"watchdog-presenter.log";
    const auto callLog = scratch / L"watchdog-calls.log";
    std::error_code ignored;
    fs::remove(presenterLog, ignored);
    fs::remove(callLog, ignored);
    EnvScope env;
    trust_mock(env, runtimeDir, callLog.wstring());
    env.set(L"OBS_NR_VFG_PRESENTER", presenterExe.wstring());
    env.set(L"OBS_NR_VFG_LOG", presenterLog.wstring());
    env.set(L"OBS_NR_TEST_STALL_WORKER_MS", L"3000");
    ProducerFixture fixture(mapping_name(L"watchdog", 2), 1280, 720, 896, 504);
    LaunchOptions options{};
    options.backend = Backend::XeFg;
    options.multiplier = 2;
    options.xessRuntimeDir = runtimeDir.wstring();
    Controller controller(options);
    auto offer = fixture.producer->connect(controller.peer());
    expect(controller.offer(offer, fixture.shape, fixture.luid), "watchdog offer");
    expect(WaitForSingleObject(fixture.producer->control_event(), 10000) == WAIT_OBJECT_0,
        "watchdog handshake");
    wait_initialized(presenterLog, controller, "xefg", 1);
    wait_for_log(presenterLog, "event=test_worker_stalled delay_ms=3000", 1, 5000,
        "worker stall fixture entered", &controller);
    wait_for([&] { return controller.exit_reason() == ExitReason::Watchdog; }, 10000,
        "watchdog exit reason");
    expect(controller.begin_async_fallback_relaunch(), "watchdog fallback begins");
    RelaunchState state = RelaunchState::Idle;
    wait_for([&] {
        state = controller.poll_async_fallback_relaunch();
        return state == RelaunchState::Ready;
    }, 20000, "watchdog relaunch ready");
    expect(state == RelaunchState::Ready, "watchdog relaunch reached Ready");
    offer = fixture.producer->connect(controller.peer());
    expect(controller.offer(offer, fixture.shape, fixture.luid), "watchdog recovery offer");
    expect(WaitForSingleObject(fixture.producer->control_event(), 10000) == WAIT_OBJECT_0,
        "watchdog recovery handshake");
    wait_for_log(presenterLog, "event=initialized backend=original", 1, 15000,
        "1x recovery preview initialized", &controller);
    const auto log = read_text(presenterLog);
    expect(log.find("event=watchdog_timeout") != std::string::npos, "watchdog stamped");
    expect(log.find("event=controller_auto_fallback_1x_relaunch") != std::string::npos,
        "controller relaunched at 1x");
    expect(log.find("event=offer mode=1x") != std::string::npos, "recovery preview is 1x");
    expect(count_occurrences(log, "event=test_worker_stalled") == 1,
        "recovery child ignores the first-launch stall fixture");
    expect(log.find("event=xefg_runtime_loaded") != std::string::npos &&
        count_occurrences(log, "event=xefg_initialized") == 1, "XeFG ran only in the first session");
    controller.stop();
    wait_for([&] { return !controller.alive(); }, 10000, "watchdog presenter exit");
    expect(fixture.producer->retire() == ipc::Status::Ok, "watchdog producer retire");
    std::printf("PASS xefg_presenter: watchdog stall_ms=3000 -> exit_reason=Watchdog -> "
        "controller_auto_fallback_1x_relaunch -> mode=1x backend=original\n");
}

void refusal_child(const fs::path& runtimeDir, const fs::path& presenterExe, const fs::path& scratch,
    bool wrongHash)
{
    const auto tag = wrongHash ? L"wronghash" : L"unsigned";
    const auto presenterLog = scratch / (std::wstring(tag) + L"-presenter.log");
    const auto callLog = scratch / (std::wstring(tag) + L"-calls.log");
    std::error_code ignored;
    fs::remove(presenterLog, ignored);
    fs::remove(callLog, ignored);
    EnvScope env;
    if (wrongHash) {
        trust_mock(env, runtimeDir, callLog.wstring());
        env.set(L"OBS_NR_TEST_XESS_FG_SHA256", std::wstring(64, L'0'));
    } else {
        env.clear(L"OBS_NR_TEST_XESS_TRUST_MOCK");
        env.clear(L"OBS_NR_TEST_XESS_FG_SHA256");
        env.clear(L"OBS_NR_TEST_XESS_XELL_SHA256");
        env.set(L"OBS_NR_TEST_XESS_CALL_LOG", callLog.wstring());
    }
    env.set(L"OBS_NR_VFG_PRESENTER", presenterExe.wstring());
    env.set(L"OBS_NR_VFG_LOG", presenterLog.wstring());
    env.clear(L"OBS_NR_TEST_STALL_WORKER_MS");
    ProducerFixture fixture(mapping_name(tag, wrongHash ? 4 : 3), 1280, 720, 896, 504);
    LaunchOptions options{};
    options.backend = Backend::XeFg;
    options.multiplier = 2;
    options.xessRuntimeDir = runtimeDir.wstring();
    Controller controller(options);
    auto offer = fixture.producer->connect(controller.peer());
    expect(controller.offer(offer, fixture.shape, fixture.luid), "refusal offer");
    expect(WaitForSingleObject(fixture.producer->control_event(), 10000) == WAIT_OBJECT_0,
        "refusal handshake");
    wait_initialized(presenterLog, controller, "original", 1);
    const auto start = clock_ns();
    for (uint64_t frame = 1; frame <= 3; ++frame) {
        const auto tick = start + frame * 16'666'667ull;
        while (clock_ns() < tick) Sleep(1);
        fixture.publish(frame, tick, false);
    }
    wait_for_log(presenterLog, "event=present_original", 1, 10000, "1x original preview", &controller);
    controller.stop();
    wait_for([&] { return !controller.alive(); }, 10000, "refusal presenter exit");
    expect(fixture.producer->retire() == ipc::Status::Ok, "refusal producer retire");
    const auto log = read_text(presenterLog);
    const auto calls = read_text(callLog);
    expect(log.find(wrongHash ? "reason=hash_mismatch" : "reason=signature_rejected") !=
        std::string::npos, wrongHash ? "wrong hash reported" : "unsigned runtime reported");
    expect(log.find("gate=libxess_fg") != std::string::npos, "refusal names the refused library");
    expect(log.find("event=xefg_unavailable_fallback_1x") != std::string::npos,
        "refusal degrades to the 1x preview");
    expect(log.find("event=offer mode=1x") != std::string::npos, "refused session offers 1x");
    expect(log.find("event=xefg_initialized") == std::string::npos, "no XeFG context was created");
    expect(calls.empty(), "refused runtime never reaches an XeSS entry point");
    std::printf("PASS xefg_presenter: refusal %s -> xefg_runtime_rejected -> mode=1x "
        "backend=original no_xess_calls\n", wrongHash ? "wrong_hash" : "unsigned");
}

void real_runtime_probe(const fs::path& scratch)
{
    wchar_t directory[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"OBS_NR_TEST_REAL_XESS_DIR", directory,
        ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) {
        std::printf("SKIP xefg_presenter: real XeSS admission (OBS_NR_TEST_REAL_XESS_DIR unset)\n");
        return;
    }
    FILE* file = nullptr;
    _wfopen_s(&file, (scratch / L"real-runtime.log").c_str(), L"wb");
    struct File { FILE*& f; ~File() { if (f) fclose(f); } } close{file};
    XeFgRuntime runtime;
    expect(runtime.open(directory, file) == XeFgLoadStatus::Ok,
        "official signed XeSS runtime admitted by Authenticode and the pinned hash");
    const auto fg = runtime.swapchain_version();
    const auto xell = runtime.xell_version();
    auto device = make_d3d12_device();
    xefg_swapchain_handle_t context{};
    const auto created = runtime.api().d3d12_create_context(device.Get(), &context);
    if (created == XEFG_SWAPCHAIN_RESULT_SUCCESS) (void)runtime.api().destroy(context);
    std::printf("INFO xefg_presenter: real_xess fg=%u.%u.%u xell=%u.%u.%u context_result=%d\n",
        fg.major, fg.minor, fg.patch, xell.major, xell.minor, xell.patch, static_cast<int>(created));
    runtime.close();
    std::printf("PASS xefg_presenter: real_runtime signature=intel hash=pinned_sdk_3.0.2 "
        "context_result=%d (device-dependent, not part of mock acceptance)\n", static_cast<int>(created));
}

void real_live_present(const fs::path& directory, const fs::path& scratch)
{
    const auto logPath = scratch / L"real-live.log";
    FILE* file = nullptr;
    _wfopen_s(&file, logPath.c_str(), L"wb");
    struct File { FILE*& f; ~File() { if (f) fclose(f); } } close{file};
    auto device = make_d3d12_device();
    TestWindow window;
    Session session = make_session(device.Get(), directory);
    Gpu gpu(session, window.hwnd, 2, false);
    auto runtime = std::make_unique<XeFgRuntime>();
    if (runtime->open(directory.wstring(), file) != XeFgLoadStatus::Ok) {
        std::printf("INFO xefg_presenter: real_live admission=%s\n", runtime->detail());
        return;
    }
    XeFgPresenter presenter(gpu, session, file);
    if (!presenter.open(std::move(runtime))) {
        std::printf("INFO xefg_presenter: real_live init_refused (see %s)\n", logPath.string().c_str());
        return;
    }
    FgOriginalDesc original{};
    original.color = gpu.input[0].Get();
    original.index = 0;
    original.multiplier = 2;
    original.discontinuity = true;
    bool presented = false;
    std::string error;
    try {
        presented = presenter.present_original(original).has_value();
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    const auto teardown = presenter.close();
    std::printf("INFO xefg_presenter: real_live init=1 present=%d teardown=%d error=%s\n",
        presented ? 1 : 0, teardown ? 1 : 0, error.empty() ? "none" : error.c_str());
}

void run_all(const fs::path& runtimeDir, const fs::path& presenterExe, const fs::path& scratch)
{
    expect(fs::exists(runtimeDir / L"libxess_fg.dll") && fs::exists(runtimeDir / L"libxell.dll"),
        "mock runtime fixtures exist");
    expect(fs::exists(presenterExe), "mock presenter exists");
    constants_contract();
    depth_contract(runtimeDir, scratch);
    loader_contract(runtimeDir, scratch);
    real_runtime_probe(scratch);
    presenter_lifecycle(runtimeDir, scratch);
    latency_fallback(runtimeDir, scratch);
    happy_child(runtimeDir, presenterExe, scratch);
    watchdog_child(runtimeDir, presenterExe, scratch);
    refusal_child(runtimeDir, presenterExe, scratch, false);
    refusal_child(runtimeDir, presenterExe, scratch, true);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: xefg_presenter_tests <mock-runtime-dir> <mock-presenter-exe>\n");
        return 2;
    }
    const fs::path runtimeDir = fs::absolute(argv[1]);
    const fs::path presenterExe = fs::absolute(argv[2]);
    const auto scratch = fs::temp_directory_path() /
        ("xefg-presenter-tests-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    fs::create_directories(scratch, error);
    if (error) {
        std::fprintf(stderr, "FAIL xefg_presenter: scratch directory: %s\n", error.message().c_str());
        return 1;
    }
    const auto finalize = [&] {
        std::error_code ignored;
        fs::remove_all(scratch, ignored);
    };
    if (argc >= 4 && std::string(argv[3]) == "--real-live") {
        try {
            real_live_present(runtimeDir, scratch);
            return 0;
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "FAIL xefg_presenter real_live: %s (artifacts: %s)\n", failure.what(),
                scratch.string().c_str());
            return 1;
        }
    }
    try {
        run_all(runtimeDir, presenterExe, scratch);
        finalize();
        std::printf("PASS xefg_presenter: all contracts\n");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "FAIL xefg_presenter: %s (artifacts: %s)\n", failure.what(),
            scratch.string().c_str());
        return 1;
    }
}
