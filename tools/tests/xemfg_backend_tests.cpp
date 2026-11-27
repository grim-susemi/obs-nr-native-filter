// SPDX-License-Identifier: GPL-3.0-only
// Contract tier for the opt-in 3x/4x XeMFG transport. The mock provider is a
// real signed-API-shaped DLL: the production validator, SEH wrappers, refusal
// checks, syntheticTex writes, HUD lock compose and teardown all run unchanged.
#include "anr_mock_gpu.h"
#include "anr_mock_addon.h"
#include "xemfg_backend.h"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace anr_test;
using nr::fg::vfg::FgPairTicket;
using nr::fg::vfg::ImportedFlow;
using nr::fg::vfg::XeMfgBackend;
using nr::fg::vfg::XeMfgOpen;

struct Log {
    FILE* file{};
    explicit Log(const fs::path& path)
    {
        file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
        require(file != nullptr, "open backend log");
        setvbuf(file, nullptr, _IONBF, 0);
    }
    ~Log() { if (file) std::fclose(file); }
};

struct XemfgScratch {
    const fs::path dir;
    explicit XemfgScratch(const fs::path& parent)
        : dir(parent / (L"xemfg-backend-" + std::to_wstring(GetCurrentProcessId())))
    {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
        require(fs::create_directory(dir), "create backend scratch");
    }
    ~XemfgScratch()
    {
        // Quarantined images stay mapped on purpose; a later run removes any
        // abandoned scratch directory before reusing its own name.
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
};

std::wstring widen(std::string_view text)
{
    if (text.empty()) return {};
    std::wstring value(text.size(), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        value.data(), static_cast<int>(value.size()));
    require(written == static_cast<int>(text.size()), "widen fixture path");
    return value;
}

std::string read_bytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "write fixture provider");
}

std::string sha256_file(const fs::path& path)
{
    const auto bytes = read_bytes(path);
    std::array<unsigned char, 32> digest{};
    require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()),
        digest.data(), static_cast<ULONG>(digest.size())) >= 0, "hash mock companion");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

fs::path stage(const fs::path& root, const fs::path& from, const wchar_t* name = L"MFGUnlocker.dll")
{
    fs::create_directories(root);
    const auto path = root / name;
    fs::copy_file(from, path, fs::copy_options::overwrite_existing);
    return path;
}

bool make_junction(const fs::path& link, const fs::path& target)
{
    std::wstring command = L"cmd.exe /d /c mklink /j \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            target.parent_path().c_str(), &startup, &child)) return false;
    struct Close { HANDLE process, thread; ~Close() { CloseHandle(thread); CloseHandle(process); } }
        close{child.hProcess, child.hThread};
    if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) return false;
    DWORD code{};
    return GetExitCodeProcess(child.hProcess, &code) && !code;
}

uint32_t mock_color(unsigned index)
{
    const auto c0 = static_cast<uint32_t>(0.25f * static_cast<float>(index) * 255.f + 0.5f);
    return 0xFF000000u | (0xBFu << 16) | (0x80u << 8) | c0;
}

struct Fixture {
    Graphics graphics;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion;
    explicit Fixture(bool warp = true)
        : graphics(warp, false), motion(graphics.texture(DXGI_FORMAT_R16G16_FLOAT))
    {
        const Image zero(extent.width * extent.height, 0);
        graphics.transfer(motion.Get(), &zero);
        const Image edges(extent.width * extent.height, 0xff202020u);
        for (unsigned i = 0; i < 4; ++i) graphics.transfer(graphics.gpu->input[i].Get(), &edges);
    }
    ImportedFlow flow() const
    {
        ImportedFlow value{};
        value.motion = motion.Get();
        value.guideWidth = extent.width;
        value.guideHeight = extent.height;
        value.mvScaleX = value.mvScaleY = 1.f;
        value.owner = nr::fg::ipc::FlowOwner::NrOwned;
        value.provenance = nr::fg::ipc::FlowProvenance::Valid;
        return value;
    }
    FgPairDesc pair(unsigned current, unsigned serial, unsigned multiplier, bool shotCut = false) const
    {
        auto& gpu = *graphics.gpu;
        return {gpu.input[current ^ 1u].Get(), gpu.input[current].Get(), serialBase + serial - 1,
            serialBase + serial, multiplier, false, shotCut, flow()};
    }
};

XeMfgOpen options(Fixture& fixture, const fs::path& dll, const fs::path& xess, unsigned multiplier, FILE* log)
{
    return {fixture.graphics.gpu->d3d_device(), fixture.graphics.gpu->drawQueue.Get(), extent, 1,
        dll.wstring(), xess.wstring(), true, multiplier, log, fixture.graphics.gpu.get(), nullptr, 0};
}

void open_backend(XeMfgBackend& backend, const XeMfgOpen& args, const fs::path& log, const char* why)
{
    if (backend.open(args)) return;
    std::fflush(nullptr);
    std::fprintf(stderr, "%s\n%s\n", why, read_bytes(log).c_str());
    throw std::runtime_error(why);
}

void verify_pair(Fixture& fixture, XeMfgBackend& backend, const FgPairTicket& ticket)
{
    backend.synchronize();
    require(backend.pair_complete(ticket), "XeMfg pair complete");
    for (unsigned sub = 1; sub <= ticket.lastSubmittedSub; ++sub) {
        require(backend.subframe_ready(ticket, sub), "XeMfg subframe ready");
        auto* target = fixture.graphics.gpu->syntheticTex[ticket.slotParity * (nr::fg::vfg::max_multiplier - 1) +
            sub - 1].Get();
        const auto pixels = fixture.graphics.transfer(target);
        require(pixels.front() == mock_color(sub) && pixels.back() == mock_color(sub),
            "mock generated subframe populated the exact syntheticTex slot");
    }
}

void verify_hud(Fixture& fixture, XeMfgBackend& backend, const FgPairTicket& ticket)
{
    const Image black(extent.width * extent.height, 0xff000000u);
    const Image white(extent.width * extent.height, 0xffffffffu);
    fixture.graphics.transfer(fixture.graphics.gpu->input[0].Get(), &black);
    fixture.graphics.transfer(fixture.graphics.gpu->input[1].Get(), &white);
    fixture.graphics.gpu->sync_hud_lock(true);
    fixture.graphics.gpu->update_hud_lock(1, 0, true);
    auto* synthetic = fixture.graphics.gpu->syntheticTex[ticket.slotParity *
        (nr::fg::vfg::max_multiplier - 1)].Get();
    ID3D12Resource* display{};
    require(fixture.graphics.gpu->record_draw([&](auto* cmd) {
        display = fixture.graphics.gpu->record_synthetic_display(cmd, {synthetic,
            [&](auto* resolve) { backend.resolve_subframe(resolve, ticket, synthetic, 1); },
            0, 1, false, 0, true, 0, ticket.slotParity});
    }) != 0, "VfgHudLock compose draw");
    fixture.graphics.drain();
    require(display && display != synthetic, "VfgHudLock compose produced its scratch display");
    std::puts("hud_lock: update=ok compose=ok display=scratch");
}

void happy(Fixture& fixture, const fs::path& scratch, const fs::path& dll, const fs::path& xess)
{
    const auto log = scratch / L"happy.log";
    const auto trace = scratch / L"happy.trace";
    const Environment traceEnv(L"OBS_NR_TEST_XEMFG_TRACE", trace.c_str());
    Log logFile(log);
    XeMfgBackend backend;
    open_backend(backend, options(fixture, dll, xess, 3, logFile.file), log, "open 3x mock provider");
    require(backend.max_in_flight_pairs() == 2, "3x keeps two pairs in flight");
    require(backend.multiplier_limit() == 4, "max_multiplier ceiling preserved");
    const auto ticket = backend.try_submit_pair(fixture.pair(1, 10, 3), 0, 1);
    require(ticket && ticket->activeMultiplier == 3 && ticket->lastSubmittedSub == 2 && ticket->slotParity == 0,
        "3x pair admitted on bank 0");
    verify_pair(fixture, backend, *ticket);
    verify_hud(fixture, backend, *ticket);
    backend.retire_pair(*ticket);
    const auto four = backend.try_submit_pair(fixture.pair(3, 11, 4), 2, 3);
    require(four && four->activeMultiplier == 4 && four->lastSubmittedSub == 3,
        "4x cadence admitted after retirement");
    verify_pair(fixture, backend, *four);
    backend.retire_pair(*four);
    require(backend.close(), "happy close releases features and unloads the provider");
    const auto logText = read_bytes(log);
    require(logText.find("event=xemfg_initialized multiplier=3 max_generated=3 banks=2") != std::string::npos,
        "initialized log names the clamped 3x multiplier and two banks");
    require(logText.find("event=xemfg_validation_rejected") == std::string::npos,
        "happy provider accepted");
    const auto traceText = read_bytes(trace);
    require(traceText.find("release\n") != std::string::npos && traceText.find("shutdown\n") != std::string::npos &&
        traceText.find("detach\n") != std::string::npos, "close released features, shut down and unloaded");
    std::puts("happy: 3x=sub1,sub2 4x=sub1,sub2,sub3 mock_color=exact");
}

void capacity(Fixture& fixture, const fs::path& scratch, const fs::path& dll, const fs::path& xess)
{
    const auto log = scratch / L"capacity.log";
    Log logFile(log);
    XeMfgBackend backend;
    open_backend(backend, options(fixture, dll, xess, 3, logFile.file), log, "open capacity backend");
    const auto first = backend.try_submit_pair(fixture.pair(1, 20, 3), 0, 1);
    const auto second = backend.try_submit_pair(fixture.pair(3, 21, 3), 2, 3);
    require(first && second && first->slotParity == 0 && second->slotParity == 1,
        "two 3x pairs own both banks");
    require(!backend.try_submit_pair(fixture.pair(1, 22, 3), 0, 1), "third pair refused while both banks are busy");
    backend.synchronize();
    verify_pair(fixture, backend, *first);
    verify_pair(fixture, backend, *second);
    backend.retire_pair(*first);
    backend.retire_pair(*second);
    backend.synchronize();
    const auto third = backend.try_submit_pair(fixture.pair(1, 23, 3), 0, 1);
    require(third.has_value(), "retired bank accepted a new pair");
    backend.retire_pair(*third);
    require(backend.close(), "capacity close");
    std::puts("capacity: banks=2 busy_pair_refused=1 reuse=after_retire");
}

void four_x_single_bank(Fixture& fixture, const fs::path& scratch, const fs::path& dll, const fs::path& xess)
{
    const auto log = scratch / L"four.log";
    Log logFile(log);
    XeMfgBackend backend;
    open_backend(backend, options(fixture, dll, xess, 4, logFile.file), log, "open 4x backend");
    require(backend.max_in_flight_pairs() == 1, "4x keeps one pair in flight");
    const auto ticket = backend.try_submit_pair(fixture.pair(1, 30, 4), 0, 1);
    require(ticket && ticket->lastSubmittedSub == 3, "4x pair admitted");
    verify_pair(fixture, backend, *ticket);
    backend.retire_pair(*ticket);
    require(backend.close(), "4x close");
    std::puts("four_x: max_in_flight_pairs=1 subframes=3");
}

void refusal(Fixture& fixture, const fs::path& scratch, const fs::path& dll, const fs::path& xess)
{
    const auto log = scratch / L"refusal.log";
    const auto trace = scratch / L"refusal.trace";
    const Environment traceEnv(L"OBS_NR_TEST_XEMFG_TRACE", trace.c_str());
    const Environment refusalEnv(L"OBS_NR_TEST_XEMFG_REFUSAL", L"cuda_unavailable");
    Log logFile(log);
    XeMfgBackend backend;
    require(!backend.open(options(fixture, dll, xess, 3, logFile.file)), "refused provider falls back");
    require(backend.faulted() && backend.multiplier_limit() == 1, "refused provider is 1x");
    const auto logText = read_bytes(log);
    require(logText.find("event=xemfg_refused reason=cuda_unavailable fallback=1x") != std::string::npos,
        "refusal reason is logged verbatim");
    const auto traceText = read_bytes(trace);
    require(traceText.find("refusal\n") != std::string::npos && traceText.find("init\n") == std::string::npos &&
        traceText.find("create\n") == std::string::npos, "refusal checked before init/feature creation");
}

void capability(Fixture& fixture, const fs::path& scratch, const fs::path& dll, const fs::path& xess)
{
    {
        const Environment none(L"OBS_NR_TEST_XEMFG_MAX_GENERATED", L"0");
        const auto log = scratch / L"none.log";
        Log logFile(log);
        XeMfgBackend backend;
        require(!backend.open(options(fixture, dll, xess, 3, logFile.file)), "zero generated frames refused");
        require(read_bytes(log).find("reason=insufficient_capability") != std::string::npos,
            "zero capability reason");
    }
    {
        const Environment one(L"OBS_NR_TEST_XEMFG_MAX_GENERATED", L"1");
        const auto log = scratch / L"one.log";
        Log logFile(log);
        XeMfgBackend backend;
        open_backend(backend, options(fixture, dll, xess, 4, logFile.file), log, "2x-only provider still opens");
        require(backend.multiplier_limit() == 2 && backend.max_in_flight_pairs() == 2,
            "provider capability clamps 4x to 2x and keeps two banks");
        const auto ticket = backend.try_submit_pair(fixture.pair(1, 40, 4), 0, 1);
        require(ticket && ticket->lastSubmittedSub == 1, "clamped 2x pair generates one subframe");
        verify_pair(fixture, backend, *ticket);
        backend.retire_pair(*ticket);
        require(backend.close(), "clamped close");
    }
    std::puts("capability: max0=refused max1=clamped_2x");
}

void validation(Fixture& fixture, const fs::path& scratch, const fs::path& mock, const fs::path& xess)
{
    const auto root = scratch / L"validation";
    const auto dll = stage(root / L"opt-out", mock);
    {
        Log logFile(root / L"opt-out.log");
        XeMfgBackend backend;
        auto args = options(fixture, dll, xess, 3, logFile.file);
        args.optIn = false;
        require(!backend.open(args), "opt-in=false rejected");
        require(read_bytes(root / L"opt-out.log").find("reason=experimental_xemfg_opt_in_required") != std::string::npos,
            "opt-in reason");
    }
    {
        const auto noExport = stage(root / L"no-export", mock);
        auto bytes = read_bytes(noExport);
        constexpr std::string_view name = "MfgXeMFGProviderGetApi";
        size_t at = 0;
        while ((at = bytes.find(name, at)) != std::string::npos) {
            bytes[at + name.size() - 1] = 'j';
            at += name.size();
        }
        write_bytes(noExport, bytes);
        Log logFile(root / L"no-export.log");
        XeMfgBackend backend;
        require(!backend.open(options(fixture, noExport, xess, 3, logFile.file)), "missing export rejected");
        require(read_bytes(root / L"no-export.log").find("reason=missing_xemfg_export") != std::string::npos,
            "missing export reason");
    }
    {
        const auto lonely = stage(root / L"no-companions", mock);
        Log logFile(root / L"no-companions.log");
        XeMfgBackend backend;
        require(!backend.open(options(fixture, lonely, {}, 3, logFile.file)), "missing companion rejected");
        require(read_bytes(root / L"no-companions.log").find("reason=xemfg_companion_rejected") != std::string::npos,
            "missing companion reason");
    }
    {
        const auto wrong = stage(root / L"wrong-hash", mock);
        const Environment trust(L"OBS_NR_TEST_XESS_TRUST_MOCK", L"1");
        const Environment hash(L"OBS_NR_TEST_XESS_FG_SHA256", std::wstring(64, L'0').c_str());
        Log logFile(root / L"wrong-hash.log");
        XeMfgBackend backend;
        require(!backend.open(options(fixture, wrong, xess, 3, logFile.file)), "wrong companion hash rejected");
        require(read_bytes(root / L"wrong-hash.log").find("reason=xemfg_companion_rejected") != std::string::npos,
            "wrong companion hash reason");
    }
    {
        const auto outside = scratch / L"escape-outside";
        fs::create_directory(outside);
        stage(outside, mock);
        const auto link = scratch / L"escape-link";
        require(make_junction(link, outside), "create backend junction");
        struct Junction { const fs::path& path; ~Junction() { RemoveDirectoryW(path.c_str()); } } cleanup{link};
        Log logFile(scratch / L"escape.log");
        XeMfgBackend backend;
        require(!backend.open(options(fixture, link / L"MFGUnlocker.dll", xess, 3, logFile.file)),
            "junction escape rejected");
        require(read_bytes(scratch / L"escape.log").find("reason=reparse_or_root_escape") != std::string::npos,
            "junction reason");
    }
    std::puts("validation: opt_in=false no_export no_companions wrong_hash junction=rejected_before_load");
}

void fault_and_relaunch(Fixture& fixture, const fs::path& scratch, const fs::path& mock, const fs::path& xess)
{
    const auto trace = scratch / L"fault.trace";
    const auto log = scratch / L"fault.log";
    const auto dll = stage(scratch / L"fault", mock);
    {
        const Environment traceEnv(L"OBS_NR_TEST_XEMFG_TRACE", trace.c_str());
        const Environment fault(L"OBS_NR_TEST_XEMFG_FAULT", L"1");
        Log logFile(log);
        XeMfgBackend backend;
        open_backend(backend, options(fixture, dll, xess, 3, logFile.file), log, "fault backend opened");
        require(!backend.try_submit_pair(fixture.pair(1, 50, 3), 0, 1), "SEH evaluation withholds the pair");
        require(backend.faulted(), "SEH evaluation faults the backend");
        require(!backend.close(), "quarantined close refuses to unload");
        const auto traceText = read_bytes(trace);
        require(traceText.find("eval") != std::string::npos && traceText.find("release\n") == std::string::npos &&
            traceText.find("shutdown\n") == std::string::npos, "quarantine skips release and shutdown");
        require(traceText.find("detach\n") == std::string::npos, "quarantine never FreeLibrary");
        std::puts("fault: SEH=quarantined release=0 shutdown=0 detach=0");
    }
    {
        const auto healthy = stage(scratch / L"relaunch", mock);
        const auto relaunchLog = scratch / L"relaunch.log";
        Log logFile(relaunchLog);
        XeMfgBackend backend;
        open_backend(backend, options(fixture, healthy, xess, 2, logFile.file), relaunchLog,
            "clean 1x relaunch opens a fresh provider");
        const auto ticket = backend.try_submit_pair(fixture.pair(1, 51, 2), 0, 1);
        require(ticket && ticket->lastSubmittedSub == 1, "relaunched provider generates again");
        verify_pair(fixture, backend, *ticket);
        backend.retire_pair(*ticket);
        require(backend.close(), "relaunched provider closes cleanly");
        std::puts("fault: relaunch=fresh_module 2x=sub1 close=clean");
    }
}

void teardown_refusals(Fixture& fixture, const fs::path& scratch, const fs::path& mock, const fs::path& xess)
{
    {
        const auto trace = scratch / L"release-refused.trace";
        const Environment traceEnv(L"OBS_NR_TEST_XEMFG_TRACE", trace.c_str());
        const Environment refused(L"OBS_NR_TEST_XEMFG_RELEASE_RETURN", L"1");
        Log logFile(scratch / L"release-refused.log");
        XeMfgBackend backend;
        open_backend(backend, options(fixture, stage(scratch / L"release", mock), xess, 2, logFile.file),
            scratch / L"release-refused.log", "release-refusal backend opened");
        require(!backend.close(), "failed release quarantines close");
        const auto traceText = read_bytes(trace);
        require(traceText.find("release_refused\n") != std::string::npos &&
            traceText.find("shutdown\n") == std::string::npos && traceText.find("detach\n") == std::string::npos,
            "failed release never reaches shutdown or FreeLibrary");
    }
    {
        const auto trace = scratch / L"shutdown-refused.trace";
        const Environment traceEnv(L"OBS_NR_TEST_XEMFG_TRACE", trace.c_str());
        const Environment refused(L"OBS_NR_TEST_XEMFG_SHUTDOWN_RETURN", L"1");
        Log logFile(scratch / L"shutdown-refused.log");
        XeMfgBackend backend;
        open_backend(backend, options(fixture, stage(scratch / L"shutdown", mock), xess, 2, logFile.file),
            scratch / L"shutdown-refused.log", "shutdown-refusal backend opened");
        require(!backend.close(), "failed shutdown quarantines close");
        const auto traceText = read_bytes(trace);
        require(traceText.find("release\n") != std::string::npos &&
            traceText.find("shutdown_refused\n") != std::string::npos &&
            traceText.find("detach\n") == std::string::npos,
            "failed shutdown never reaches FreeLibrary");
    }
    std::puts("teardown: release_refused=quarantined shutdown_refused=quarantined");
}

void unsupported_size(Fixture& fixture, const fs::path& scratch, const fs::path& mock, const fs::path& xess)
{
    Log logFile(scratch / L"size.log");
    XeMfgBackend backend;
    const auto staged = stage(scratch / L"size", mock);
    const XeMfgOpen tiny{fixture.graphics.gpu->d3d_device(), fixture.graphics.gpu->drawQueue.Get(),
        {320, 180}, 1, staged.wstring(), xess.wstring(), true, 3, logFile.file, fixture.graphics.gpu.get(),
        nullptr, 0};
    require(!backend.open(tiny), "undersized XeMFG extent rejected");
    require(read_bytes(scratch / L"size.log").find("reason=unsupported_size width=320 height=180") != std::string::npos,
        "unsupported size reason");
    std::puts("size: 320x180=rejected");
}

int real(const fs::path& dll)
{
    fs::path provider = dll;
    provider.make_preferred();
    const auto official = environment(L"OBS_NR_TEST_XESS_DIR");
    if (provider.empty()) {
        std::puts("UNVERIFIED_EXTERNAL_BINARY (BLOCKED on user-supplied MFGUnlocker.dll)");
        return 0;
    }
    Fixture fixture(false);
    const auto log = fs::temp_directory_path() / L"xemfg-real.log";
    Log logFile(log);
    XeMfgBackend backend;
    if (!backend.open(options(fixture, provider, official, 4, logFile.file))) {
        std::fprintf(stderr, "real_xemfg_open=FAILED (see %ls)\n", log.c_str());
        return 2;
    }
    require(backend.multiplier_limit() >= 4, "real provider reports three generated frames");
    for (const auto [multiplier, serial] : {std::pair{3u, 60u}, {4u, 61u}}) {
        const auto ticket = backend.try_submit_pair(fixture.pair(1, serial, multiplier), 0, 1);
        require(ticket && ticket->lastSubmittedSub == multiplier - 1, "real 3x/4x pair admitted");
        backend.synchronize();
        bool any = false;
        for (unsigned sub = 1; sub <= ticket->lastSubmittedSub; ++sub) {
            require(backend.subframe_ready(*ticket, sub), "real subframe ready");
            auto* target = fixture.graphics.gpu->syntheticTex[ticket->slotParity *
                (nr::fg::vfg::max_multiplier - 1) + sub - 1].Get();
            const auto pixels = fixture.graphics.transfer(target);
            any = any || std::any_of(pixels.begin(), pixels.end(), [](uint32_t p) { return p != 0; });
        }
        require(any, "real provider wrote non-zero subframes");
        backend.retire_pair(*ticket);
        if (multiplier == 3) backend.synchronize();
    }
    require(backend.close(), "real provider closed");
    bool logChecked = false;
    for (const fs::path& dir : std::array<fs::path, 3>{provider.parent_path(), fs::path(official),
             provider.parent_path() / L"RTXMFG-XeSS"}) {
        std::error_code ignored;
        if (!fs::is_directory(dir, ignored)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ignored)) {
            if (!entry.path().filename().wstring().starts_with(L"RTXMFG-")) continue;
            const auto text = read_bytes(entry.path());
            const auto at = text.find("d3d12_provider_batch");
            if (at == std::string::npos) continue;
            logChecked = true;
            const auto launches = text.find("\"cuda_launches\":", at);
            const auto fallbacks = text.find("\"cuda_fallbacks\":", at);
            require(launches != std::string::npos && fallbacks != std::string::npos, "RTXMFG batch counters");
            const auto value = [&](size_t pos) { return std::stoull(text.substr(pos + 16)); };
            require(value(launches) > 0 && value(fallbacks) == 0, "RTXMFG cuda launches without fallbacks");
        }
    }
    std::printf("real_xemfg=3x,4x non_zero_subframes=1 rtxmfg_batch_log=%s\n",
        logChecked ? "cuda_launches>0 cuda_fallbacks=0" : "absent");
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--real-xemfg-dll") {
            require(argc <= 3, "usage: --real-xemfg-dll [<path>]");
            const auto path = argc == 3 ? fs::path(argv[2]) :
                fs::path(environment(L"OBS_NR_TEST_XEMFG_DLL"));
            return real(path);
        }
        const auto live = environment(L"OBS_NR_TEST_XEMFG_DLL");
        if (argc == 1 && !live.empty()) return real(fs::path(live));
        std::wstring exe(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        require(length && length < exe.size(), "locate test executable");
        exe.resize(length);
        const auto directory = fs::path(exe).parent_path();
        fs::path mock = argc == 2 ? fs::path(argv[1]) : directory / L"MFGUnlocker.dll";
        mock.make_preferred();
        require(argc == 1 || argc == 2, "usage: xemfg_backend_tests [<mock_provider.dll>]");
        require(fs::exists(mock), "mock provider path exists");
        require(fs::exists(directory / L"xess-fg" / L"libxess_fg.dll"), "staged mock companion runtime");
        const fs::path xess = directory / L"xess-fg";
        ComPtr<ID3D12Debug> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12 debug layer required");
        debug->EnableDebugLayer();
        const XemfgScratch scratch(fs::temp_directory_path());
        const Environment trust(L"OBS_NR_TEST_XESS_TRUST_MOCK", L"1");
        const Environment fgHash(L"OBS_NR_TEST_XESS_FG_SHA256", widen(sha256_file(xess / L"libxess_fg.dll")).c_str());
        const Environment xellHash(L"OBS_NR_TEST_XESS_XELL_SHA256", widen(sha256_file(xess / L"libxell.dll")).c_str());
        Fixture fixture;
        happy(fixture, scratch.dir, mock, xess);
        capacity(fixture, scratch.dir, mock, xess);
        four_x_single_bank(fixture, scratch.dir, mock, xess);
        refusal(fixture, scratch.dir, mock, xess);
        capability(fixture, scratch.dir, mock, xess);
        validation(fixture, scratch.dir, mock, xess);
        fault_and_relaunch(fixture, scratch.dir, mock, xess);
        teardown_refusals(fixture, scratch.dir, mock, xess);
        unsupported_size(fixture, scratch.dir, mock, xess);
        fixture.graphics.verify_debug();
        std::puts("PASS xemfg_backend");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL xemfg_backend: %s\n", error.what());
        return 1;
    }
}
