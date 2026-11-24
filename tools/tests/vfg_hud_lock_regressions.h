// SPDX-License-Identifier: GPL-3.0-only
#pragma once

void check_held_slots(Fixture& f, const Image& learned)
{
    constexpr uint32_t strokes[]{0xffabcdefu, 0xff6284a6u, 0xff153759u};
    for (unsigned i = 0; i < 3; ++i) {
        auto held = learned;
        held[10 * width + 32] = strokes[i];
        f.upload(f.gpu->input[i].Get(), held);
    }
    // Retain learned confidence; change only the compose inputs so the
    // shader cannot hide a wrong held index behind identical stroke bytes.
    for (unsigned i = 0; i < 3; ++i) {
        const auto composed = f.image(f.display(i, i, (i + 1) % 3, true, false, 0));
        pixel(composed, 32, 10, strokes[i], "compose selected wrong held input");
        pixel(composed, 4, 10, 0xff5234c7u, "held-slot moving background");
    }
    std::puts("held slots=0:ffabcdef,1:ff6284a6,2:ff153759 PASS");
}

class DrawGate final {
    Fixture& f_;
    ComPtr<ID3D12Fence> fence_;

public:
    explicit DrawGate(Fixture& f) : f_(f)
    {
        checked(f_.gpu->d3d_device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "gate fence");
        checked(f_.gpu->drawQueue->Wait(fence_.Get(), 1), "draw queue gate");
    }
    ~DrawGate()
    {
        if (FAILED(fence_->Signal(1))) TerminateProcess(GetCurrentProcess(), 2);
        f_.drain();
    }
    [[nodiscard]] bool blocked() const { return fence_->GetCompletedValue() == 0; }
};

void check_hud_retirement(Fixture& f, const Image& generated)
{
    const auto old = f.gpu->hud_lock();
    {
        DrawGate gate(f);
        require(f.gpu->record_draw([&](ID3D12GraphicsCommandList* cmd) {
            const Gpu::SyntheticFrame frame{f.gpu->syntheticTex[2].Get(), [](ID3D12GraphicsCommandList*) {},
                0, 1, false, 0, true, 2};
            auto* display = f.gpu->record_synthetic_display(cmd, frame);
            require(display == old.lock()->output(), "queued compose did not use the old scratch");
        }) != 0, "compose did not submit");
        f.gpu->sync_hud_lock(false);
        require(gate.blocked() && f.gpu->hud_lock().expired() && !old.expired(), "OFF freed or kept active a queued HUD");
        f.gpu->sync_hud_lock(true);
        f.gpu->update_hud_lock(1, 0, false);
        const auto fresh = f.gpu->hud_lock();
        require(!fresh.expired() && !old.expired() && (fresh.owner_before(old) || old.owner_before(fresh)),
            "re-enable reused an in-flight HUD");
    }
    f.gpu->sync_hud_lock(true);
    require(old.expired(), "completed old HUD was retained after re-enable");
    all_zero(f.state(), "fresh state after rapid toggle");
    equal_image(f.image(f.display(0, 0, 1, true, false, 0)), generated, "fresh scratch passthrough");

    const auto last = f.gpu->hud_lock();
    {
        DrawGate gate(f);
        f.gpu->update_hud_lock(1, 0, true);
        f.gpu->sync_hud_lock(false);
        require(gate.blocked() && !last.expired(), "OFF freed a queued HUD update");
    }
    f.gpu->sync_hud_lock(false);
    require(last.expired() && f.gpu->hud_lock().expired(), "idle OFF did not retire the completed HUD");
    equal_image(f.image(f.display(0, 0, 1, false, false, 0)), generated, "live OFF passthrough");
    std::puts("live OFF: deferred compose/update fences, old released after completion, rapid ON state zero, idle OFF released PASS");
}

void check_original_only_repeat()
{
    Fixture f(1);
    Image original{};
    original.fill(0xff182a3cu);
    f.upload(f.gpu->input[0].Get(), original);
    f.upload(f.gpu->input[1].Get(), original);
    f.gpu->sync_hud_lock(true);
    f.update(0, 0, true);
    auto dest = f.texture();
    f.record([&](ID3D12GraphicsCommandList* cmd) { f.gpu->record_present_copy(cmd, {f.gpu->input[0].Get(), dest.Get()}); });
    equal_image(f.image(dest.Get()), original, "1x original before enabling pill");
    const auto repeated = f.compare(0, 1);
    require(repeated.same, "1x repeated bytes were not classified as a repeat");
    constexpr uint32_t pill = 0xff65cb3du;
    present_content_repeat(false, false, [&] {
        f.gpu->set_hud(1, 0, 1, 1, &pill);
        f.record([&](ID3D12GraphicsCommandList* cmd) { f.gpu->record_present_copy(cmd, {f.gpu->input[1].Get(), dest.Get()}); });
    });
    original[0] = pill;
    equal_image(f.image(dest.Get()), original, "1x repeat must show the new FPS pill");
    all_zero(f.state(), "1x repeat must not learn HUD state");
    std::puts("1x identical repeat: changed FPS pill=ff65cb3d, other pixels exact, HUD state unchanged PASS");
}

void check_abandoned_hud_compare()
{
    Fixture f(1);
    Image a{}, b{};
    a.fill(0xff000000u);
    b.fill(0xffffffffu);
    f.upload(f.gpu->input[0].Get(), a);
    f.upload(f.gpu->input[1].Get(), a);
    f.upload(f.gpu->input[2].Get(), b);
    auto dest = f.texture();
    Handle oldReady;
    oldReady.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(oldReady.h != nullptr, "abandoned comparison event");
    f.gpu->sync_hud_lock(true);
    f.update(0, 0, true);
    {
        DrawGate gate(f);
        require(f.gpu->begin_compare(0, 1, oldReady.h), "HUD-only comparison not submitted");
        require(WaitForSingleObject(oldReady.h, 0) == WAIT_TIMEOUT, "compare was not pending");
        f.gpu->sync_hud_lock(false);
        f.gpu->discard_compare();
        require(f.gpu->record_draw([&](ID3D12GraphicsCommandList* cmd) {
            f.gpu->record_present_copy(cmd, {f.gpu->input[1].Get(), dest.Get()});
        }) != 0 && gate.blocked(), "OFF original did not submit while comparison was pending");
        f.gpu->sync_hud_lock(true);
        require(!f.gpu->begin_compare(1, 2, f.compareEvent.h), "fresh comparison reused an in-flight readback");
    }
    require(WaitForSingleObject(oldReady.h, 30000) == WAIT_OBJECT_0, "abandoned comparison did not complete");
    require(!f.gpu->compare_result(), "completed abandoned comparison leaked its result");
    equal_image(f.image(dest.Get()), a, "OFF original passthrough");
    const auto fresh = f.compare(1, 2);
    require(!fresh.same && fresh.cut && !fresh.flash, "fresh comparison consumed stale equality");
    require(!f.gpu->compare_result(), "fresh comparison result consumed twice");
    f.update(2, 1, fresh.cut);
    f.record([&](ID3D12GraphicsCommandList* cmd) { f.gpu->record_present_copy(cmd, {f.gpu->input[2].Get(), dest.Get()}); });
    equal_image(f.image(dest.Get()), b, "admitted fresh original after re-enable");
    std::puts("1x pending HUD compare: OFF original queued, reuse fenced, abandoned result hidden, ON fresh cut and original exact PASS");
}
