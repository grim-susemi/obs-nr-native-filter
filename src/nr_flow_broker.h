// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "nr_contract.h"
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace nr {
// Single-owner policy for GPU motion analysis. Values match nr::fg::ipc::FlowOwner
// on the v3 wire, so the FG bridge forwards them without remapping.
enum class FlowOwnerPolicy : uint32_t { Unavailable = 0, NrOwned = 1, ConsumerFallback = 2 };
// Whether this displayed frame's vectors are the exact analysed pair. Values match
// nr::fg::ipc::FlowProvenance. Only NrOwned + Valid is motion a consumer may use.
enum class FlowProvenance : uint32_t {
    Unknown = 0, Valid = 1, DedupReused = 2, HeldPrevious = 3, Reset = 4, Discontinuity = 5
};
// One displayed colour frame. canonicalTick is the OBS canonical video tick; the
// raw OBS nanosecond timestamp is not an identity.
struct FlowColorKey {
    uint64_t sourceInstanceId = 0, canonicalTick = 0, historyEpoch = 0, contentEpoch = 0;
    uint64_t submissionSerial = 0, resourceGeneration = 0;
    bool valid() const;
    bool operator==(const FlowColorKey&) const = default;
};
// The two colour frames one analysis pair belongs to. Both endpoints must match
// the displayed colour identities; a single matching end is never enough.
struct FramePairKey {
    FlowColorKey prevColorKey{}, currColorKey{};
    bool valid() const;
    bool operator==(const FramePairKey&) const = default;
};
// Metadata for one exported pair. The ring texture is already in current->previous
// analysis pixels (never re-divided by the S10.5 grid); mvScale is carried for
// consumers whose extent differs (SR upscale, FG render size).
struct SharedFlowSlot {
    uint32_t guideWidth = 0, guideHeight = 0;
    uint32_t activeX = 0, activeY = 0, activeWidth = 0, activeHeight = 0;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    bool shotCut = false, reset = false;
    bool valid(Extent analysis) const;
    bool operator==(const SharedFlowSlot&) const = default;
};
enum class FlowConsumer : uint32_t { Sr = 0, Fg = 1 };
// NR worker input for one ring copy. The list must already be ordered after
// FlowResult::complete on this broker's queue.
struct FlowFrameRef {
    JobKey key;
    uint64_t canonicalTick = 0;
};
struct FlowExport {
    FlowFrameRef current;
    std::optional<FlowFrameRef> previous; // absent on reset; a reset pair is never exported
    ID3D12Resource* motion = nullptr;
    SharedFlowSlot guide;
};
// Immutable snapshot handed to a consumer. Handles are broker-owned process
// handles: open them with OpenSharedHandle and never CloseHandle them.
struct ExportedFlow {
    FramePairKey pair;
    SharedFlowSlot guide;
    HANDLE motion = nullptr, readyFence = nullptr, returnFence = nullptr;
    uint64_t readyValue = 0, returnValue = 0;
    uint32_t slot = 0;
};
struct DisplayedFlow {
    FlowOwnerPolicy owner = FlowOwnerPolicy::Unavailable;
    FlowProvenance provenance = FlowProvenance::Unknown;
    std::optional<FramePairKey> pair;
    bool operator==(const DisplayedFlow&) const = default;
};
// Pure classification for every actually displayed frame. A DelayedNr is Valid
// only when the caller resolved the exact exported pair; a dedup copy is never
// promoted, and raw/priming frames never carry motion.
struct DisplayInput {
    OutputKind kind = OutputKind::CurrentSource;
    Status status = Status::Disabled;
    bool settingsEnabled = false, resetTemporal = false, heldPrevious = false, dedupReused = false;
    std::optional<FramePairKey> pair;
};
DisplayedFlow classify_display(const DisplayInput&);

inline constexpr uint32_t flow_ring_slots = 4, flow_ring_generation_limit = 2;
// The device/queue that runs the ring copy, the analysis extent the ring is
// allocated at, and the source instance the broker answers for.
struct FlowBrokerTarget {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    Extent analysis{0, 0};
    uint64_t sourceInstanceId = 0;
};

// Bounded fence-owned D3D12 shared MV ring. The private D3D12Guides::motion
// texture is never exposed: each export copies it through the sanitize shader
// into a D3D12_HEAP_FLAG_SHARED slot. A slot is reused only when its write fence
// and every actually leased consumer's return fence have completed; process
// exit or a timeout is never treated as GPU completion.
class FlowBroker {
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit FlowBroker(std::shared_ptr<Impl> impl);
    static std::array<std::shared_ptr<Impl>, flow_ring_generation_limit>& parked_rings();
    static std::mutex& parked_lock();
    static void collect_parked(); // caller holds parked_lock()
public:
    // Returns null and fills diagnostic when the device cannot host a ring or
    // the two-generation retirement budget is full. Never throws.
    static std::shared_ptr<FlowBroker> create(const FlowBrokerTarget&, std::string* diagnostic = nullptr);
    ~FlowBroker();
    FlowBroker(const FlowBroker&) = delete;
    FlowBroker& operator=(const FlowBroker&) = delete;

    uint64_t source_instance_id() const;
    uint64_t resource_generation() const;
    // Append the sanitize/ring copy to a list submitted on this broker's queue.
    // Non-blocking: false means every slot is still fenced (droppedExport++).
    bool record_export(ID3D12GraphicsCommandList*, const FlowExport&);
    // Called after that list was submitted on this broker's queue and succeeded.
    void commit_export(ID3D12CommandQueue*);
    // Called when the list was not submitted: the slot is fenced to its current
    // queue position before it can be reused and is never published.
    void abandon_export();

    // Display provenance, written by the graphics owner for the displayed tick.
    std::optional<FramePairKey> pair_for_job(uint64_t generation, uint64_t displayTick,
        uint64_t submissionSerial) const;
    void record_display(uint64_t generation, uint64_t displayTick, const DisplayedFlow&);
    std::optional<DisplayedFlow> displayed(uint64_t generation, uint64_t displayTick) const;

    // Consumer lease; returnValue is the value that consumer signals on
    // returnFence after its GPU reads complete.
    std::optional<ExportedFlow> find_pair(const FramePairKey&, FlowConsumer);
    std::optional<ExportedFlow> find_displayed(uint64_t generation, uint64_t displayTick, FlowConsumer);
    std::optional<ExportedFlow> lease(uint32_t slot, FlowConsumer);

    struct Stats {
        uint64_t exported = 0, droppedExport = 0, reusedSlots = 0, displayRecords = 0;
    };
    Stats stats() const;
    uint32_t slot_count() const;
    bool close(); // true only when every published slot completed; unresolved ownership is kept

    // Contract view used by tests and by the SR/FG consumers: the exact display
    // tick a job key serves (displayed tick T -> analysed pair ending at T-1).
    static bool display_tick_for(const FramePairKey&, uint64_t displayTick);
};

// Process-wide seam: the model registers its broker, the graphics owner records
// display provenance, and the SR/FG consumers look the broker up by source and
// resource generation.
void register_flow_broker(const std::shared_ptr<FlowBroker>&);
void unregister_flow_broker(const FlowBroker*);
std::shared_ptr<FlowBroker> find_flow_broker(uint64_t sourceInstanceId, uint64_t resourceGeneration);
} // namespace nr
