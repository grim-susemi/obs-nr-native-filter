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
inline constexpr uint64_t protocol_version=1, slot_count=4, generation_limit=2;
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
};
struct Frame {
 uint64_t generation=0, serial=0, timestampNs=0, arrivalNs=0;
 Shape shape{};
};
struct CopyTicket {
 size_t slot;
 Frame frame;
};
struct CopyStep {
 Status status;
 std::optional<CopyTicket> current{};
 std::optional<CopyTicket> previous{}; // Present only for a compatible adjacent input pair.
};
struct Offer { std::wstring mapping; uint64_t generation; };
// Fixed x64 wire layout; do not place pointers, STL types or bools in the mapping.
// All mutable shared words, including payload, use Windows Interlocked operations.
enum class Transfer:LONG64 { Initial, Offered, Importing, Ready, Rejected, Closed, Revoked };
struct alignas(8) WireFrame { volatile LONG64 sequence; volatile LONG64 words[16]; };
struct alignas(8) Header {
 uint64_t magic, version, bytes, generation;
 LUID luid;
 DWORD producerPid, consumerPid;
 uint64_t producerCreation, consumerCreation;
 uint64_t handles[14]; // Four (texture, write fence, return fence), frame and control events.
 alignas(8) volatile LONG64 transfer;
 WireFrame frames[4];
};
static_assert(sizeof(wchar_t)==2&&sizeof(void*)==8);
static_assert(sizeof(WireFrame)==136&&offsetof(Header,frames)%8==0);
// Useful to a consumer and CPU-only contract tests. Never retries indefinitely.
Status validate(const Header&,LUID);
std::optional<Frame> snapshot(const WireFrame&);

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
 Status publish(ID3D11Texture2D*,uint64_t timestampNs,uint64_t arrivalNs,bool discontinuity=false);
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
 static RevokeProbeResult test_revoke(std::array<HANDLE,14>&,decltype(&DuplicateHandle));
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
 // Copies into a same-shape COMMON texture or a sufficiently large readback
 // buffer. Keeps destination, allocator and list alive through the return fence.
 // Queue is dedicated to this endpoint; rendering uses the destination only.
 Status copy(size_t slot,ID3D12Resource* destination);
 // Copy the oldest visible unconsumed IPC frame to a private COMMON texture.
 // Keep each destination alive and unmodified through its last reader. Do not
 // alias input allocations. A ticket means queued, not GPU-ready: check both
 // tickets with copy_status before reading off-queue. Keep original output.
 [[nodiscard]] CopyStep copy_next(ID3D12Resource* destination);
 // Poll a ticket issued by this Consumer on its graphics-owner thread.
 [[nodiscard]] Status copy_status(const CopyTicket&)const;
 Status close(); // Pending preserves all imports; destructor uses bounded quarantine.
#ifdef NR_FG_IPC_TEST_PROBE
 // Compiled only into the separate test transport, never fg_ipc. The child
 // pauses exactly after Execute and before Signal until its owned test parent
 // either resumes or kills it. Neither event is borrowed by production builds.
 void test_submit_probe(HANDLE reached,HANDLE resume);
#endif
private:
 Status copy_frame(const CopyTicket&,ID3D12Resource*,bool guardCrossSlotDestination);
};
} // namespace nr::fg::ipc
