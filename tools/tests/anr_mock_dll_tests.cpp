// SPDX-License-Identifier: GPL-3.0-only
#include "anr_mock_hardware.h"
#include "anr_mock_addon.h"
#include "fg/anr_dll_validator.h"
#include <sstream>

int anr_mock_ownership(int argc, wchar_t** argv);

namespace {
using namespace anr_test;
namespace validator = nr::fg::anr;
constexpr size_t area = size_t{extent.width} * extent.height;

struct Log {
    FILE* file{};
    explicit Log(const fs::path& path)
    {
        require(_wfopen_s(&file, path.c_str(), L"wb") == 0, "open fixture log");
    }
    ~Log() { std::fclose(file); }
};

void verify_preload(Graphics& g, const Scratch& s, const fs::path& dll, const fs::path& trace)
{
    const auto absent = stage(s.dir / "standalone", dll, false);
    const auto partial = stage(s.dir / "partial", dll, true);
    require(fs::remove(partial.parent_path() / missingPack), "remove exactly one required pack");
    const auto invalid = stage(s.dir / "missing-export", dll, true);
    const auto linked = stage(s.dir / "junction", dll, true);
    const auto subdir = (linked.parent_path() / missingPack).parent_path();
    fs::rename(subdir, s.dir / "outside");
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, ARRAYSIZE(system));
    require(length && length < ARRAYSIZE(system), "resolve system junction tool");
    auto utility = fs::path(system) / L"cmd.exe";
    std::wstring command = LR"(cmd.exe /d /v:off /c mklink /j "junction\programs\fg-lite-any-dp4a-srgb8" "outside")";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(utility.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, s.dir.c_str(), &startup, &child), "create owned junction");
    Handle process, thread;
    process.h = child.hProcess;
    thread.h = child.hThread;
    if (WaitForSingleObject(process.h, 4000) != WAIT_OBJECT_0) {
        TerminateProcess(process.h, ERROR_TIMEOUT);
        throw std::runtime_error("fixture junction creation timed out");
    }
    DWORD exitCode{};
    require(GetExitCodeProcess(process.h, &exitCode) && !exitCode &&
        (GetFileAttributesW(subdir.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), "fixture directory junction");
    struct Junction { const fs::path& path; ~Junction() { RemoveDirectoryW(path.c_str()); } } junction{subdir};
    {
        auto bytes = contents(invalid);
        const auto at = bytes.find("NVSDK_NGX_D3D12_Shutdown1");
        require(at != bytes.npos, "owned DLL export spelling");
        bytes[at + std::string_view("NVSDK_NGX_D3D12_Shutdown").size()] = '9';
        std::ofstream out(invalid, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(out), "write inert missing-export case");
    }
    const struct Case { fs::path path; Extent size; bool optIn; const char* why; } cases[]{
        {absent, extent, true, "incomplete_anr_addon_missing_companions missing=nr_fg.ini"},
        {partial, extent, true, "incomplete_anr_addon_missing_companions missing=programs/fg-lite-any-dp4a-srgb8/pack.nrx"},
        {invalid, extent, true, "missing_anr_export"},
        {linked, extent, true, "reparse_or_root_escape missing=programs/fg-lite-any-dp4a-srgb8/pack.nrx"},
        {absent, extent, false, "opt_in_required"},
        {absent, {639, 360}, true, "unsupported_size width=639 height=360"},
        {absent, {7681, 4320}, true, "unsupported_size width=7681 height=4320"},
        {absent, {320, 240}, true, "unsupported_size width=320 height=240"},
    };
    const auto path = s.dir / "backend.log";
    {
        Log log(path);
        for (const auto& item : cases) {
            AnrFgBackend backend;
            require(!backend.open({g.gpu->d3d_device(), g.gpu->drawQueue.Get(), item.size, 1,
                item.path.native(), item.optIn, log.file}) && backend.faulted() && backend.multiplier_limit() == 1,
                "pre-load rejection must select original-only fallback");
            require(!fs::exists(trace), "pre-load refusal ran DllMain");
        }
    }
    const auto log = contents(path);
    for (const auto& item : cases) require(log.find(item.why) != log.npos, item.why);
    std::printf("%sloaded_before_validation=0\n", log.c_str());
}

void verify_display(Graphics& g, AnrFgBackend& backend, const FgPairTicket& ticket, const Image& white)
{
    const Image black(area, 0xff000000u);
    auto* a = g.gpu->input[0].Get();
    auto* b = g.gpu->input[1].Get();
    g.transfer(a, &black);
    g.transfer(b, &white);
    Handle event;
    event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event.h && g.gpu->begin_compare(0, 1, event.h) && WaitForSingleObject(event.h, 4000) == WAIT_OBJECT_0,
        "ANR composition compare");
    const auto compared = g.gpu->compare_result();
    require(compared && compared->cut && !compared->flash, "ANR composition classified actual cut");
    g.gpu->sync_hud_lock(true);
    g.gpu->update_hud_lock(1, 0, true);
    auto* synthetic = g.gpu->syntheticTex[1].Get();
    ID3D12Resource* display{};
    require(g.gpu->record_draw([&](auto* cmd) {
        display = g.gpu->record_synthetic_display(cmd, {synthetic,
            [&](auto* resolve) { backend.resolve_subframe(resolve, ticket, synthetic, 2); },
            0, 1, true, compared->parity, true, 1});
    }) != 0, "ANR compose shared HUD/shot draw");
    g.drain();
    require(display != synthetic && g.transfer(display) == white &&
        g.transfer(synthetic) == Image(area, 0xffbf4040u), "ANR cut must replace the composed display scratch with B");
    std::puts("shot_hud_composition=PASS cut_display_is_B=1 synthetic_preserved=1");
}

void verify_submission_order(const fs::path& path)
{
    std::istringstream in(contents(path));
    const auto read = [&](const char* expected, unsigned long long& frame, unsigned& sub, unsigned long long& fence) {
        std::string line;
        if (!std::getline(in, line)) return false;
        char phase[16]{};
        require(sscanf_s(line.c_str(), "%15s frame=%llu sub=%u fence=%llu", phase, 16u, &frame, &sub, &fence) == 4 &&
            std::string_view(phase) == expected, "ANR submission trace phase");
        return true;
    };
    unsigned groups{};
    unsigned long long last{};
    for (;;) {
        unsigned long long a{}, b{}, producer{}, consumer{};
        unsigned p{}, c{};
        if (!read("producer", a, p, producer)) break;
        require(read("consumer", b, c, consumer) && a == b && p == c && last < producer && producer < consumer,
            "producer submission must precede each separate consumer submission");
        last = consumer;
        ++groups;
    }
    require(groups == 12, "expected exact count of split submissions including withheld resets");
    std::printf("execute_producer_before_consumer=PASS pairs=%u\n", groups);
}

void verify_single_bank(Graphics& g, const fs::path& dll, const Scratch& s)
{
    const auto fixture = stage(s.dir / "single-bank", dll, true);
    const auto log = s.dir / "single-bank.log";
    Log fixtureLog(log);
    AnrFgBackend backend;
    require(backend.max_in_flight_pairs() == 1, "ANR keeps a single pair in flight");
    require(backend.open({g.gpu->d3d_device(), g.gpu->drawQueue.Get(), extent, 1, fixture.native(), true,
        fixtureLog.file, g.gpu.get()}, std::make_unique<FixtureGuides>(g)), "open single-bank mock addon");
    const auto first = backend.try_submit_pair(pair(g, 1, 60, 2), 0, 1);
    require(first && first->slotParity == 0, "first ANR pair owns bank 0");
    std::puts("ANR pair=1 bank=0 accepted=1");
    require(!backend.try_submit_pair(pair(g, 0, 61, 2), 1, 0), "second ANR pair refused while bank 0 is unretired");
    std::puts("ANR pair=2 refused=1 reason=bank0_active_unretired");
    backend.synchronize();
    ComPtr<ID3D12Fence> consume;
    check(g.gpu->d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consume)), "ANR consumer fence");
    backend.retire_pair(*first, consume.Get(), 5);
    require(consume->GetCompletedValue() < 5, "consumer fence starts below its retire value");
    require(!backend.try_submit_pair(pair(g, 0, 62, 2), 1, 0),
        "retired bank 0 refused while consumer GPU draws are pending");
    std::puts("ANR pair=3 refused=1 reason=consumer_pending completed=0/5");
    Handle done;
    done.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(done.h && SUCCEEDED(g.gpu->drawQueue->Signal(consume.Get(), 5)) &&
        SUCCEEDED(consume->SetEventOnCompletion(5, done.h)) && WaitForSingleObject(done.h, 4000) == WAIT_OBJECT_0 &&
        consume->GetCompletedValue() == 5, "ANR consumer fence completion");
    const auto released = backend.try_submit_pair(pair(g, 1, 63, 2), 0, 1);
    require(released && released->slotParity == 0, "consumer completion releases ANR bank 0");
    std::puts("ANR pair=4 bank=0 accepted=1 completed=5/5");
    backend.synchronize();
    backend.retire_pair(*released);
    require(backend.close(), "close single-bank mock addon");
    std::puts("ANR single_bank=PASS max_in_flight_pairs=1");
}

void verify_warp(const fs::path& dll, const Scratch& s)
{
    Graphics g(true);
    const auto fixture = stage(s.dir / "complete", dll, true);
    const auto trace = s.dir / "mock.trace";
    const Environment attach(L"OBS_NR_TEST_ANR_TRACE", trace.c_str());
    verify_preload(g, s, dll, trace);
    const Image asymmetric(area, 0xc8734c19u);
    const Image white(area, 0xffffffffu);
    auto* a = g.gpu->input[0].Get();
    auto* b = g.gpu->input[1].Get();
    g.transfer(a, &asymmetric);
    g.transfer(b, &white);
    {
        AnrBgra conversion(g.gpu->d3d_device(), extent);
        g.record([&](auto* cmd) { conversion.record(cmd, a, 0); });
        const auto converted = g.transfer(conversion.get(0));
        require(converted == Image(area, 0xc8194c73u), "BGRA RTV double-swizzled asymmetric input");
        std::puts("bgra_pixel=0xc8194c73 input_rgba_pixel=0xc8734c19");
    }
    const auto ordering = s.dir / "submission.log";
    {
        Log submissions(ordering);
        AnrFgBackend backend;
        require(backend.open({g.gpu->d3d_device(), g.gpu->drawQueue.Get(), extent,
                1, fixture.native(), true, stderr, g.gpu.get(), submissions.file},
            std::make_unique<FixtureGuides>(g)), "open complete mock addon");
        for (const auto [multiplier, serial] : {std::pair{4u, 14u}, {2u, 15u}, {3u, 16u}}) {
            const unsigned current = serial & 1u;
            const auto ticket = backend.try_submit_pair(pair(g, current, serial, multiplier), current ^ 1u, current);
            require(ticket && ticket->activeMultiplier == multiplier && ticket->lastSubmittedSub == multiplier - 1,
                "mock lost a dynamic group");
            backend.synchronize();
            constexpr uint32_t color[]{0xffbf4020u, 0xffbf4040u, 0xffbf4060u};
            for (unsigned sub = 1; sub < multiplier; ++sub) {
                require(backend.pair_complete(*ticket) && backend.subframe_ready(*ticket, sub), "mock output ready");
                auto* target = g.gpu->syntheticTex[sub - 1].Get();
                g.record([&](auto* cmd) { backend.resolve_subframe(cmd, *ticket, target, sub); });
                require(g.transfer(target) == Image(area, color[sub - 1]), "mock GPU color transfer");
            }
            backend.retire_pair(*ticket);
        }
        {
            const Environment disable(L"OBS_NR_TEST_ANR_DISABLE", L"1");
            const auto ticket = backend.try_submit_pair(pair(g, 1, 30, 4), 0, 1);
            require(ticket.has_value(), "disable case admission");
            backend.synchronize();
            require(backend.pair_complete(*ticket) &&
                backend.output(*ticket, 1) == AnrFgBackend::Output::Suppressed &&
                backend.output(*ticket, 2) == AnrFgBackend::Output::Ready &&
                backend.output(*ticket, 3) == AnrFgBackend::Output::Ready, "pair sentinel must initialize only on sub 1");
            std::puts("disable_bytes=1,0,0 display_decisions=suppressed,ready,ready");
            verify_display(g, backend, *ticket, white);
            backend.retire_pair(*ticket);
        }
        {
            const Environment fault(L"OBS_NR_TEST_ANR_FAULT", L"1");
            require(!backend.try_submit_pair(pair(g, 1, 40, 2), 0, 1) && backend.faulted() &&
                backend.multiplier_limit() == 1, "synthetic SEH must degrade to 1x");
        }
    }
    verify_submission_order(ordering);
    const auto events = contents(trace);
    require(events.starts_with("attach\ninit\ncreate flags=07/B8 size=640x360\n"), "complete addon dispatch");
    auto previous = size_t{};
    for (const auto [mult, serial] : {std::pair{4u, 14u}, {2u, 15u}, {3u, 16u}}) {
        for (unsigned sub = 1; sub < mult; ++sub) {
            const auto token = "eval frame=" + std::to_string(serialBase + serial) + " count=" +
                std::to_string(mult - 1) + " sub=" + std::to_string(sub) + " reset=0 scale=1,1 resources=5\n";
            previous = events.find(token, previous);
            require(previous != events.npos, "dynamic group or uint64 wire ID telemetry");
            previous += token.size();
        }
    }
    require(events.ends_with("release\nshutdown\n"), "feature release before device shutdown");
    std::printf("%s", events.c_str());
    {
        const Environment cap(L"OBS_NR_TEST_ANR_CAPABILITY", L"1");
        AnrFgBackend backend;
        require(!open(backend, g, fixture, std::make_unique<FixtureGuides>(g)) && backend.faulted() &&
            backend.multiplier_limit() == 1, "insufficient provider capability fallback");
    }
    require(contents(trace).substr(events.size()) == "attach\ninit\nshutdown\n", "capability refusal skips create");
    verify_single_bank(g, dll, s);
    g.verify_debug();
    std::puts("warp_mock=PASS groups=4,2,3 bgra=PASS release_before_shutdown=PASS");
    hardware(fixture, false);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc > 1 && std::wstring_view(argv[1]) == L"--ownership-regression") return anr_mock_ownership(argc, argv);
        smokeWithoutDebugLayer = argc == 2 && std::wstring_view(argv[1]) == L"--smoke-without-debug-layer";
        require(argc == 1 || smokeWithoutDebugLayer || (argc == 3 && std::wstring_view(argv[1]) == L"--real-b15-dll"),
            "usage: anr_mock_dll_tests [--real-b15-dll <explicit path> | --smoke-without-debug-layer]");
        verify_oracle();
        const auto explicitPath = argc == 3 ? std::wstring(argv[2]) :
            smokeWithoutDebugLayer ? std::wstring{} : environment(L"OBS_NR_TEST_ANR_B15_DLL");
        if (argc == 3 || !explicitPath.empty()) {
            const auto validated = validator::validate_addon(explicitPath, true);
            if (validated.status != validator::ValidationStatus::Accepted) {
                std::fprintf(stderr, "real_b15_preload_rejection reason=%s path=%.*s loader_calls=0 fallback=1x\n",
                    validator::status_name(validated.status), static_cast<int>(validated.relativePath.size()),
                    validated.relativePath.empty() ? "" : validated.relativePath.data());
                return 2;
            }
        }
        if (!smokeWithoutDebugLayer) {
            ComPtr<ID3D12Debug> debug;
            const auto hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
            if (FAILED(hr)) std::fprintf(stderr, "d3d12_debug_interface_hresult=0x%08lX\n", static_cast<unsigned long>(hr));
            check(hr, "D3D12 debug layer required");
            debug->EnableDebugLayer();
        }
        if (argc == 1 || smokeWithoutDebugLayer) {
            std::wstring exe(32768, L'\0');
            const auto len = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            require(len && len < exe.size(), "locate test executable");
            exe.resize(len);
            const auto directory = fs::path(exe).parent_path();
            const Scratch scratch(directory);
            verify_warp(directory / "anr_mock_dll.dll", scratch);
        }
        if (explicitPath.empty())
            std::puts("real_b15_interpolation=UNVERIFIED (complete user-supplied B15 addon not provided)");
        else
            hardware(explicitPath, true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL anr_mock_dll: %s\n", error.what());
        return 1;
    }
}
