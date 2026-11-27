// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace nr::fg::ipc {
inline constexpr uint64_t protocol_version=3, slot_count=4, generation_limit=2;
inline constexpr size_t handle_count=18, mv_handle_first=14, wire_word_count=39;
inline constexpr size_t key_word=24, prev_key_word=29, curr_key_word=34;
// Shared NVOF provenance carried with a frame pair. The owner states who performed
// the motion analysis; the provenance states whether this frame's vectors are the
// exact pair the analysis produced. Only NrOwned+Valid may be consumed as motion.
enum class FlowOwner : uint32_t { Unavailable=0, NrOwned=1, ConsumerFallback=2 };
enum class FlowProvenance : uint32_t { Unknown=0, Valid=1, DedupReused=2, HeldPrevious=3, Reset=4, Discontinuity=5 };
// A compact displayed-colour identity. The raw OBS nanosecond tick is not an
// identity: it cannot distinguish a repeated image, a reset history, or a new
// content epoch at the same timestamp. The producer supplies one key per frame
// and the shared guide pair carries both of its endpoint keys.
struct FrameKey {
 uint32_t sourceInstanceId=0, resourceGeneration=0;
 uint64_t submissionSerial=0, canonicalTick=0, historyEpoch=0, contentEpoch=0;
 friend bool operator==(const FrameKey&,const FrameKey&)=default;
};
struct GuideShape {
 uint32_t width=0,height=0;
 friend bool operator==(const GuideShape&,const GuideShape&)=default;
};
enum class Status {
 Ok, Pending, Dropped, PeerDead, VersionMismatch, AdapterMismatch, WrongPeer,
 Rejected, PeerBusy, RetirementCapacity, DeviceLost, InvalidFrame,
 SequenceExhausted, Closed, ProtocolError
};
class Error final:public std::runtime_error {
 Status status_; HRESULT code_;
public:
 Error(Status,HRESULT,const char*);
 Status status()const noexcept{return status_;}
 HRESULT code()const noexcept{return code_;}
};
// A borrowed process handle supplied by its controller. Verification duplicates this
// exact object; PID is never used to reopen it. Required rights: SYNCHRONIZE,
// PROCESS_QUERY_LIMITED_INFORMATION and PROCESS_DUP_HANDLE.
struct Peer { HANDLE process; DWORD pid; uint64_t creation; };
Peer identity(HANDLE);
struct Shape {
 uint32_t width, height;
 DXGI_FORMAT format;
 DXGI_COLOR_SPACE_TYPE colorSpace;
 std::array<wchar_t,32> sourceMonitor{};
 bool displayCapture=false;
 bool splitCompare=false;
 // Fixed for one generation: the shared motion-vector ring is allocated at this
 // analysis size, independent of the colour shape (a 70% work scale for example).
 GuideShape guides{};
};
// Per-frame flow metadata. The guide extent itself lives in Shape::guides so the
// ring allocation and every published record cannot disagree.
struct FlowSample {
 FlowOwner owner=FlowOwner::Unavailable;
 FlowProvenance provenance=FlowProvenance::Unknown;
 float mvScaleX=1.0f,mvScaleY=1.0f;
 uint64_t pairPrevTick=0,pairCurrTick=0;
 bool shotCut=false,reset=false,hasSharedMv=false;
 // The displayed colour identities the two guide endpoints were analysed for.
 // A pair is exposed only when prevColor equals the previous colour slot's key
 // and currColor equals this colour slot's key.
 FrameKey prevColor{},currColor{};
};
struct Frame {
 uint64_t generation=0, serial=0, timestampNs=0, arrivalNs=0;
 Shape shape{};
 uint64_t obsTick=0; // OBS output tick in ns; zero when the source cannot supply it.
 uint32_t splitPercent=50;
 bool splitSwap=false;
 FrameKey key{}; // Displayed colour identity of this slot.
 FlowSample flow{};
};
struct CopyTicket {
 size_t slot;
 Frame frame;
};
struct CopyStep {
 Status status;
 std::optional<CopyTicket> current{};
 std::optional<CopyTicket> previous{}; // Present only for a compatible adjacent input pair.
 // True only when both tickets carry the same NrOwned+Valid flow with adjacent
 // ticks and identical guide extents: a caller must not interpolate a mixed pair.
 bool hasValidSharedMv=false;
};
struct Offer { std::wstring mapping; uint64_t generation; };
// Fixed x64 wire layout; do not place pointers, STL types or bools in the mapping.
// All mutable shared words, including payload, use Windows Interlocked operations.
enum class Transfer:LONG64 { Initial, Offered, Importing, Ready, Rejected, Closed, Revoked };
// words: 0 generation, 1 serial, 2 timestamp, 3 arrival, 4 colour extent, 5 format/space,
// 6 flags, 7..14 source monitor, 15 OBS tick, 16 flow owner, 17 flow provenance,
// 18 guide extent, 19 motion scales, 20 pair previous tick, 21 pair current tick,
// 22 pair flags (shotCut, reset, hasSharedMv), 23 reserved zero,
// 24..28 colour slot key, 29..33 guide previous endpoint key, 34..38 guide current
// endpoint key (each key: sourceInstanceId, resourceGeneration, submissionSerial,
// canonicalTick, historyEpoch, contentEpoch).
struct alignas(8) WireFrame { volatile LONG64 sequence; volatile LONG64 words[39]; };
struct alignas(8) Header {
 uint64_t magic, version, bytes, generation;
 LUID luid;
 DWORD producerPid, consumerPid;
 uint64_t producerCreation, consumerCreation;
 // 0..11 four colour (texture, write fence, return fence); 12 frame event; 13 control
 // event; 14..17 optional per-slot R16G16_FLOAT motion textures. A null-shared-flow
 // generation writes zero to 14..17; MV readiness and reuse follow the colour slot's
 // own write/return fences because publish copies both under one submission.
 uint64_t handles[18];
 alignas(8) volatile LONG64 transfer;
 WireFrame frames[4];
};
static_assert(sizeof(wchar_t)==2&&sizeof(void*)==8);
static_assert(sizeof(WireFrame)==320&&offsetof(Header,frames)%8==0);
// Useful to a consumer and CPU-only contract tests. Never retries indefinitely.
Status validate(const Header&,LUID);
std::optional<Frame> snapshot(const WireFrame&);

// One publish payload for an optional shared motion ring: the source texture,
// the displayed colour identity of this slot, and the guide pair (with both of
// its endpoint identities). motion == nullptr must agree with !pair.hasSharedMv.
struct FlowBundle {
 ID3D11Texture2D* motion=nullptr;
 FrameKey color{};
 FlowSample pair{};
};

// One graphics-owner thread per endpoint. All producer calls are nonblocking;
// process/fence/event handles can be subscribed by the external controller.
// A process-wide two-entry vault preserves uncertain GPU ownership even if an
// endpoint is destroyed/recreated. There is no per-reconnect allocation loophole.
class Producer final {
 struct Impl;
 std::shared_ptr<Impl> current_;
 struct Config;
 std::unique_ptr<Config> config_;
public:
 Producer(ID3D11Device*,std::wstring,const Shape&);
 ~Producer();
 Producer(const Producer&)=delete;
 Producer& operator=(const Producer&)=delete;
 Offer connect(const Peer&); // Throws Error on rejection/capacity; old generation is retained.
 Status status()const;
 // A discontinuity starts a new frame pair without retiring the transport.
 // In split mode before is required and splitPercent must be in [0,100].
 // A generation with Shape::guides copies the bundle's motion texture (R16G16_FLOAT,
 // guide-sized) into the selected slot under the same write fence. The bundle also
 // carries the slot's displayed colour key and the guide pair's endpoint keys; a
 // null-flow sample rejects a motion texture and vice versa.
 Status publish(ID3D11Texture2D*,uint64_t timestampNs,uint64_t arrivalNs,bool discontinuity=false,uint64_t obsTick=0,
  ID3D11Texture2D* before=nullptr,uint32_t splitPercent=50,bool splitSwap=false,
  const FlowBundle& shared={});
 Status retire(); // Alive importer must close first; dead is NOT a completion.
 HANDLE peer_process()const;
 HANDLE control_event()const;
 uint64_t generation()const;
 static size_t retained_generations();
 // Nonblocking proof for a generation already relinquished by its owner.
 static bool generation_retained(uint64_t generation);
#ifdef NR_FG_IPC_TEST_PROBE
 struct RevokeProbeResult { bool first,second;DWORD error; };
 // CPU-only fixture runs the real revocation twice; only its close-source call
 // is injected. Synthetic source handles never reach a Windows close operation.
 static RevokeProbeResult test_revoke(std::array<HANDLE,handle_count>&,decltype(&DuplicateHandle));
#endif
};
class Consumer final {
 struct Impl;
 std::shared_ptr<Impl> impl_;
public:
 Consumer(ID3D12CommandQueue*,const Offer&,const Peer&);
 ~Consumer();
 Consumer(const Consumer&)=delete;
 Consumer& operator=(const Consumer&)=delete;
 HANDLE frame_event()const;
 HANDLE producer_process()const;
 Status status()const;
 std::optional<Frame> frame(size_t slot)const;
 // Subscribe before copy. The caller owns an unsignaled event and keeps it live
 // through completion. No helper CPU wait is inserted by either method.
 Status arm_return(size_t slot,HANDLE);
 Status arm_return(const CopyTicket&,HANDLE);
 // Copies into a same-shape COMMON texture or a sufficiently large readback
 // buffer. Keeps destination, allocator and list alive through the return fence.
 // Queue is dedicated to this endpoint; rendering uses the destination only.
 // This legacy entry point rejects split frames.
 Status copy(size_t slot,ID3D12Resource* destination);
 // Copy the oldest visible unconsumed IPC frame to a private COMMON texture.
 // Keep each destination alive and unmodified through its last reader. Do not
 // alias input allocations. A ticket means queued, not GPU-ready: check both
 // tickets with copy_status before reading off-queue. In split mode before is
 // required and receives the original under the same ticket and return fence.
 // mv receives this frame's guide-sized R16G16_FLOAT motion when the frame carries
 // shared flow (2D texture or a sufficiently large readback buffer). A frame without
 // shared flow rejects a non-null mv destination instead of copying stale pixels.
 [[nodiscard]] CopyStep copy_next(ID3D12Resource* destination,ID3D12Resource* before=nullptr,ID3D12Resource* mv=nullptr);
 // Poll a ticket issued by this Consumer on its graphics-owner thread.
 [[nodiscard]] Status copy_status(const CopyTicket&)const;
 // Add the exact copy completion dependency to a different consumer queue.
 [[nodiscard]] Status order_copy(const CopyTicket&,ID3D12CommandQueue*)const;
 Status close(); // Pending preserves all imports; destructor uses bounded quarantine.
#ifdef NR_FG_IPC_TEST_PROBE
 // Compiled only into the separate test transport, never fg_ipc. The child
 // pauses exactly after Execute and before Signal until its owned test parent
 // either resumes or kills it. Neither event is borrowed by production builds.
 void test_submit_probe(HANDLE reached,HANDLE resume);
#endif
private:
 struct CopyTargets { ID3D12Resource* destination;ID3D12Resource* before=nullptr;ID3D12Resource* mv=nullptr;
  bool guardCrossSlotDestination=false; };
 Status copy_frame(const CopyTicket&,const CopyTargets&);
};
} // namespace nr::fg::ipc
