// SPDX-License-Identifier: GPL-3.0-only
#include "pushflow_fixture.h"
#include "pushflow_review_contracts.h"
#include "../fg-presenter/vfg_gpu.h"
#include <array>

namespace {
using namespace nr::fg;
using namespace nr::fg::vfg;
using namespace nr::fg::qa;
using namespace nr::fg::vfg::pushflow;

void oracle_controls(TestRun& test)
{
    const auto allocate = [] { return std::vector<float>(fixtureWidth * fixtureHeight); };
    const auto frame = [](std::span<const float> p) { return LuminanceFrame{fixtureWidth, fixtureHeight, p}; };
    auto a = allocate(), b = allocate();
    const auto black = allocate();
    paint(a, fixtureWidth, endpointA, 1);
    paint(b, fixtureWidth, endpointB, 1);
    constexpr double times[]{0.25, 1.0 / 3, 0.5, 2.0 / 3, 0.75};
    std::array<std::vector<float>, 5> exact{};
    for (size_t i = 0; i < exact.size(); ++i) {
        const auto t = times[i];
        exact[i] = allocate();
        auto blend = allocate();
        // Independent positions from 64 + round(128*t).
        constexpr int positions[]{96, 107, 128, 149, 160};
        paint(exact[i], fixtureWidth, {positions[i], 56, 32}, 1);
        paint(blend, fixtureWidth, endpointA, static_cast<float>(1 - t));
        paint(blend, fixtureWidth, endpointB, static_cast<float>(t));
        std::printf("CONTROL t=%.6f x=%d\n", t, positions[i]);
        test.expect(verify_interpolated_square(frame(exact[i]), endpointA, endpointB, t).has_value(),
            "ground truth square passes");
        test.expect(!verify_interpolated_square(frame(blend), endpointA, endpointB, t), "linear alpha blend rejected");
        test.expect(!verify_interpolated_square(frame(a), endpointA, endpointB, t) &&
            !verify_interpolated_square(frame(b), endpointA, endpointB, t) &&
            !verify_interpolated_square(frame(black), endpointA, endpointB, t), "both endpoints and black rejected");
    }
    const InterpolatedFrame three[]{{frame(exact[1]), times[1]}, {frame(exact[3]), times[3]}};
    const InterpolatedFrame four[]{{frame(exact[0]), times[0]}, {frame(exact[2]), times[2]}, {frame(exact[4]), times[4]}};
    const InterpolatedFrame same[]{{frame(exact[1]), times[1]}, {frame(exact[1]), times[3]}};
    const InterpolatedFrame backwards[]{three[1], three[0]};
    test.expect(verify_interpolated_sequence(three, endpointA, endpointB), "3x distinct ordered pair");
    test.expect(verify_interpolated_sequence(four, endpointA, endpointB), "4x ordered triplet");
    test.expect(!verify_interpolated_sequence(same, endpointA, endpointB), "duplicated 3x pair rejected");
    test.expect(!verify_interpolated_sequence(backwards, endpointA, endpointB), "reversed 3x pair rejected");
    auto ghost = exact[2];
    paint(ghost, fixtureWidth, endpointA, 0.30f);
    test.expect(!verify_interpolated_square(frame(ghost), endpointA, endpointB, .5),
        "endpoint ghost rejected despite full intermediate occupancy");
    auto nearA = allocate(), nearB = allocate();
    paint(nearA, fixtureWidth, {107, 56, 32}, .62f);
    paint(nearA, fixtureWidth, {149, 56, 32}, .43f);
    paint(nearB, fixtureWidth, {107, 56, 32}, .43f);
    paint(nearB, fixtureWidth, {149, 56, 32}, .62f);
    const InterpolatedFrame nearDuplicate[]{{frame(nearA), times[1]}, {frame(nearB), times[3]}};
    test.expect(verify_interpolated_square(frame(nearA), endpointA, endpointB, times[1]) &&
        verify_interpolated_square(frame(nearB), endpointA, endpointB, times[3]) &&
        !verify_interpolated_sequence(nearDuplicate, endpointA, endpointB),
        "3x pair MAD rejects two individually valid ordered squares");
}

struct Scratch {
    const fs::path path = fs::temp_directory_path() / (L"obs-nr-t9-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    Scratch()
    {
        if (!fs::create_directory(path)) throw std::runtime_error("fixture scratch already exists");
    }
    ~Scratch() { std::error_code ec; fs::remove_all(path, ec); }
};

void lifecycle(TestRun& test, const fs::path& marks)
{
    const auto starting = marks / L"pushflow-starting.txt", quarantine = marks / L"pushflow-crash-quarantine.txt";
    StartingMarker armed(marks);
    test.expect(armed.begin() == StartingMarker::Open::Armed && fs::exists(starting), "startup marker armed");
    for (unsigned i = 1; i < 120; ++i) armed.steady();
    test.expect(fs::exists(starting), "startup marker survives through 119 inferences");
    test.expect(armed.steady() && !fs::exists(starting), "startup marker clears at inference 120");
    test.expect(armed.steady() && armed.begin_teardown(false) && fs::exists(starting) &&
        armed.finish_teardown(false, true) && !fs::exists(starting), "resize re-arms then clears after clean drain");
    test.expect(armed.begin_teardown(true) && !fs::exists(starting), "user close does not re-arm after startup");
    test.expect(armed.begin_teardown(false) && armed.finish_teardown(false, false) &&
        fs::exists(starting), "unverified teardown keeps startup marker");
    std::ofstream(quarantine) << "controller fixture\n";
    test.expect(StartingMarker(marks).clear() && !fs::exists(starting) && fs::exists(quarantine),
        "user force close cleans only starting marker");
    const auto retry = [&] {
        StartingMarker manual(marks);
        const bool once = manual.begin() == StartingMarker::Open::PreviousCrash;
        return once && !fs::exists(starting) && !fs::exists(quarantine) &&
            manual.begin() == StartingMarker::Open::Armed && manual.finish_teardown(true, true);
    };
    test.expect(retry(), "quarantine-only degrades exactly one manual open");
    std::ofstream(starting) << "stale fixture\n";
    std::ofstream(quarantine) << "controller fixture\n";
    test.expect(retry(), "both previous crash records are consumed and retry arms");
}

void model_facts(TestRun& test, const fs::path& dir)
{
    const auto model = dir / L"fixture.onnx", sidecar = dir / L"fixture.onnx.txt";
    const auto raw = read_model_facts(model);
    test.expect(model_shader(1, false, raw) == ShaderPath::Unpack && !model_shader(4, true, raw),
        "raw RGB requires no metadata; motion requires verified metadata");
    std::ofstream(sidecar) << "align_corners=1\npad=128\n";
    test.expect(!model_shader(4, true, read_model_facts(model)), "missing polarity rejects reconstruct");
    std::ofstream(sidecar) << "align_corners=1\nmask_for_first=0\npad=128\n";
    const auto known = read_model_facts(model);
    test.expect(known.alignCorners == true && known.maskForFirst == false &&
        model_shader(4, true, known) == ShaderPath::Reconstruct && !model_shader(4, false, known),
        "explicit false polarity accepted only with all named outputs");
    std::ofstream(sidecar, std::ios::app) << "mask_for_first=1\n";
    test.expect(!model_shader(4, true, read_model_facts(model)), "conflicting polarity rejects reconstruct");
    test.expect(!matches_outputs(4, 1, ShaderPath::Unpack) && !matches_outputs(1, 4, ShaderPath::Reconstruct) &&
        !matches_outputs(1, 1, ShaderPath::Reconstruct) && matches_outputs(1, 1, ShaderPath::Unpack) &&
        matches_outputs(4, 4, ShaderPath::Reconstruct), "count and active shader must both match");
    test.expect(known.pad == 128 && raw.pad == 64, "sidecar parses explicit 128 pad and defaults to 64");
}

bool fixture_open(PushFlowBackend& backend, const fs::path& runtime, unsigned multiplier, PreviewMultiplier& preview)
{
    Session session{};
    session.backend = Backend::PushFlow;
    session.multiplier = multiplier;
    wcscpy_s(session.pushflowRuntimeDir, runtime.c_str());
    return backend.open({session, stdout, [] {}, [] { return false; }, 0, &preview}, nullptr);
}

int corrupt_ort_child(const fs::path& dir)
{
    const auto runtime = dir / L"runtime";
    PushFlowBackend backend;
    PreviewMultiplier preview;
    const bool opened = fixture_open(backend, runtime, 2, preview);
    const bool graceful = !opened && GetModuleHandleW((runtime / L"DirectML.dll").c_str()) &&
        backend.faulted() && !backend.runtime_fault() && !fs::exists(marker_directory() / L"pushflow-starting.txt");
    return graceful ? 0 : 1;
}

bool run_corrupt_ort_child(const fs::path& dir)
{
    wchar_t executable[4096]{};
    const auto size = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!size || size >= ARRAYSIZE(executable)) throw std::runtime_error("fixture executable path");
    std::wstring args = L"\"" + std::wstring(executable) + L"\" --internal-corrupt-ort \"" + dir.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    std::fflush(stdout);
    if (!CreateProcessW(executable, args.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child))
        throw std::runtime_error("launch owned corrupt-ORT fixture");
    Handle process, thread;
    process.h = child.hProcess;
    thread.h = child.hThread;
    const auto wait = WaitForSingleObject(process.h, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.h, 124);
        const auto reaped = WaitForSingleObject(process.h, 2000);
        std::printf("fixture=corrupt_ort pid=%lu wait=%lu reap=%lu timed_out=%d\n",
            child.dwProcessId, wait, reaped, wait == WAIT_TIMEOUT);
        return false;
    }
    DWORD exit{};
    if (!GetExitCodeProcess(process.h, &exit)) return false;
    std::printf("fixture=corrupt_ort pid=%lu wait=%lu exit=0x%08lX timed_out=0\n", child.dwProcessId, wait, exit);
    return exit == 0;
}

void loader_failures(TestRun& test, const fs::path& dir)
{
    const auto marks = marker_directory();
    const auto starting = marks / L"pushflow-starting.txt", quarantine = marks / L"pushflow-crash-quarantine.txt";
    const auto runtime = dir / L"runtime";
    fs::create_directory(runtime);
    PreviewMultiplier preview;
    {
        PushFlowBackend backend;
        test.expect(!fixture_open(backend, runtime, 2, preview) && backend.faulted() && !backend.runtime_fault() && !fs::exists(starting),
            "empty runtime falls back to 1x without crashing or poisoning the next open");
    }
    wchar_t executable[4096]{};
    const auto size = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!size || size >= ARRAYSIZE(executable)) throw std::runtime_error("fixture executable path");
    // An owned PE with no entry point reaches the real corrupt ORT loader failure.
    fs::copy_file(fs::path(executable).parent_path() / L"pushflow_loader_fixture.dll", runtime / L"DirectML.dll");
    std::ofstream(runtime / L"onnxruntime.dll", std::ios::binary).put('x');
    // The DLL stays mapped in the child; the parent owns fixture cleanup after reap.
    test.expect(run_corrupt_ort_child(dir), "one-byte ORT DLL falls back to 1x without native exception");
    std::ofstream(starting) << "old startup\n";
    std::ofstream(quarantine) << "controller quarantine\n";
    {
        PushFlowBackend backend;
        test.expect(!fixture_open(backend, runtime, 1, preview) && !backend.faulted() && fs::exists(starting) && fs::exists(quarantine),
            "auto fallback 1x does not consume either crash record");
    }
    {
        PushFlowBackend backend;
        test.expect(!fixture_open(backend, runtime, 2, preview) && backend.faulted() && !backend.runtime_fault() &&
            preview.effective(2) == 1 && !fs::exists(starting) && !fs::exists(quarantine),
            "active stale-marker open consumes records and falls back");
    }
    std::ofstream(quarantine) << "later controller record\n";
    PushFlowBackend resized;
    test.expect(!fixture_open(resized, runtime, 4, preview) && !resized.faulted() &&
        preview.effective(4) == 1 && fs::exists(quarantine), "quarantine-consumed preview stays 1x at resize");
    {
        PushFlowBackend reopened;
        PreviewMultiplier manual;
        test.expect(manual.effective(4) == 4 &&
            !fixture_open(reopened, runtime, 4, manual) && reopened.faulted() &&
            manual.effective(4) == 1 && !fs::exists(quarantine), "new manual preview evaluates marker policy again");
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wstring_view(argv[1]) == L"--internal-corrupt-ort") {
        const fs::path scratch(argv[2]);
        if (scratch / L"obs-nr-native-filter" != marker_directory() ||
            !scratch.filename().wstring().starts_with(L"obs-nr-t9-")) return 2;
        return corrupt_ort_child(scratch);
    }
    bool required{};
    unsigned expected{};
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--require-runtime" && !required) required = true;
        else if (arg == L"--expect-outputs" && !expected && i + 1 < argc) {
            const std::wstring_view value(argv[++i]);
            if (value == L"1") expected = 1;
            else if (value == L"4") expected = 4;
            else { std::fprintf(stderr, "--expect-outputs requires 1 or 4\n"); return 2; }
        } else { std::fprintf(stderr, "invalid PushFlow fixture arguments\n"); return 2; }
    }
    try {
        wchar_t runtime[4096]{};
        const auto size = GetEnvironmentVariableW(L"OBS_NR_PUSHFLOW_RUNTIME", runtime, ARRAYSIZE(runtime));
        TestRun test;
        Scratch scratch;
        if (!SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.path.c_str())) throw std::runtime_error("fixture LOCALAPPDATA");
        pair_dispatch_controls(test);
        oracle_controls(test);
        output_metadata_controls(test);
        generation_shutdown_controls(test);
        lifecycle(test, scratch.path / L"markers");
        ownership_and_timeout_controls(test, scratch.path / L"ring");
        model_facts(test, scratch.path);
        if (size && size < ARRAYSIZE(runtime) && !test.failures) real_pushflow(test, runtime, expected);
        else if (required) test.expect(false, "required OBS_NR_PUSHFLOW_RUNTIME was not exercised");
        else if (expected) test.expect(false, "cannot verify --expect-outputs without OBS_NR_PUSHFLOW_RUNTIME");
        else std::puts("SKIP optional runtime: OBS_NR_PUSHFLOW_RUNTIME not configured");
        loader_failures(test, scratch.path);
        std::printf("%s pushflow_backend (%u failures)\n", test.failures ? "FAIL" : "PASS", test.failures);
        return test.failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fflush(stdout);
        std::fprintf(stderr, "fixture failure: %s\n", error.what());
        return 1;
    }
}
