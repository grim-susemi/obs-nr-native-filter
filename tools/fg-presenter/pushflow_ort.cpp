// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors.
// PushFPS §5(a): Based on PushFPS (GPL-3.0-or-later), modified 2026-11-22 for the OBS presenter.
#include "pushflow_ort.h"
#include "vfg_gpu.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace nr::fg::vfg::pushflow {
namespace {
// Both modules and the environment are process lifetime. A resize can replace
// a session, but must neither unload a runtime nor release its global state.
struct ProcessRuntime {
    std::mutex lock;
    OrtEnv* env{};
    const OrtApi* api{};
};
ProcessRuntime runtime;

}

bool OrtState::status(const char* operation, OrtStatus* error) noexcept
{
    if (!error) return true;
    const char* message{};
    if (invoke("GetErrorMessage", [&] { message = api_->GetErrorMessage(error); }))
        std::fprintf(log_, "event=pushflow_ort_error operation=%s detail=%s\n", operation, message);
    invoke("ReleaseStatus", api_->ReleaseStatus, error);
    return false;
}

bool OrtState::load(const fs::path& directory, ID3D12Device* device)
{
    if (!directory.is_absolute()) return false;
    constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    const auto load = [&](const wchar_t* file) {
        HMODULE module{};
        const auto absolute = directory / file;
        if (!invoke("LoadLibraryExW", [&] { module = LoadLibraryExW(absolute.c_str(), nullptr, flags); })) return module;
        if (!module) std::fprintf(log_, "event=pushflow_load_failed dll=%ls error=%lu\n", file, GetLastError());
        return module;
    };
    const auto dmlModule = load(L"DirectML.dll");
    if (!dmlModule) return false;
    const auto ortModule = load(L"onnxruntime.dll");
    if (!ortModule || !device) return false;
    const auto getBase = reinterpret_cast<decltype(&OrtGetApiBase)>(
        reinterpret_cast<void*>(GetProcAddress(ortModule, "OrtGetApiBase")));
    const auto createDml = reinterpret_cast<decltype(&DMLCreateDevice)>(
        reinterpret_cast<void*>(GetProcAddress(dmlModule, "DMLCreateDevice")));
    const OrtApiBase* base{};
    if (!getBase || !createDml || !invoke("OrtGetApiBase", [&] { base = getBase(); }) || !base) return false;
    const char* version{};
    if (!invoke("GetVersionString", [&] { version = base->GetVersionString(); }) ||
        std::strcmp(version, "1.20.1") != 0 ||
        !invoke("GetApi", [&] { api_ = base->GetApi(ORT_API_VERSION); }) || !api_) return false;
    const void* provider{};
    if (!call("GetExecutionProviderApi", api_->GetExecutionProviderApi, "DML", ORT_API_VERSION, &provider) ||
        !provider) return false;
    dml_ = static_cast<const OrtDmlApi*>(provider);
    HRESULT hr{};
    auto** address = device_.GetAddressOf();
    if (!invoke("DMLCreateDevice", [&] { hr = createDml(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(address)); }) ||
        FAILED(hr)) return false;
    {
        std::lock_guard guard(runtime.lock);
        if (!runtime.env) {
            OrtEnv* env{};
            if (!call("CreateEnv", api_->CreateEnv, ORT_LOGGING_LEVEL_WARNING, "obs-pushflow", &env)) return false;
            runtime.env = env;
            runtime.api = api_;
        }
        if (runtime.api != api_) return false;
        env_ = runtime.env;
    }
    return call("CreateMemoryInfo", api_->CreateMemoryInfo, "DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &memory_);
}

bool OrtState::create_session(const fs::path& model, ID3D12CommandQueue* queue,
    const std::array<std::pair<std::string, int64_t>, 4>& overrides)
{
    OrtSessionOptions* options{};
    if (!call("CreateSessionOptions", api_->CreateSessionOptions, &options)) return false;
    bool ok = call("GraphOptimizationLevel", api_->SetSessionGraphOptimizationLevel, options, ORT_ENABLE_ALL) &&
        call("DisableMemPattern", api_->DisableMemPattern, options) &&
        call("ExecutionMode", api_->SetSessionExecutionMode, options, ORT_SEQUENTIAL) &&
        call("IntraOpNumThreads", api_->SetIntraOpNumThreads, options, 1) &&
        call("AllowSpinning", api_->AddSessionConfigEntry, options, "session.intra_op.allow_spinning", "0");
    for (const auto& [symbol, size] : overrides)
        if (ok && !symbol.empty())
            ok = call("FreeDimensionOverride", api_->AddFreeDimensionOverrideByName, options, symbol.c_str(), size);
    ok = ok && call("AppendDML1", dml_->SessionOptionsAppendExecutionProvider_DML1, options, device_.Get(), queue) &&
        call("CreateSession", api_->CreateSession, env_, model.c_str(), options, &session_) &&
        call("HoldPool", api_->CreateAllocator, session_, memory_, &pool_);
    return invoke("ReleaseSessionOptions", api_->ReleaseSessionOptions, options) && ok;
}

bool OrtState::release_session()
{
    // HoldPool is a required invariant even for pass one. If obtaining it
    // failed, do not invoke the 1.20.1 teardown with an unprotected pool.
    if (!session_) return true;
    if (!pool_ || !invoke("ReleaseSession", api_->ReleaseSession, session_)) return false;
    session_ = nullptr;
    if (!invoke("ReleaseAllocator", api_->ReleaseAllocator, pool_)) return false;
    pool_ = nullptr;
    return true;
}

bool OrtState::open(const fs::path& model, const ModelFacts& facts, const Geometry& geometry,
    ID3D12CommandQueue* queue, const Beat& beat)
{
    std::array<std::pair<std::string, int64_t>, 4> overrides{};
    for (unsigned pass = 0; pass < 2; ++pass) {
        beat();
        if (!create_session(model, queue, overrides)) return false;
        beat();
        size_t inputCount{};
        if (!call("SessionGetInputCount", api_->SessionGetInputCount, session_, &inputCount) || inputCount != 1 ||
            !call("SessionGetOutputCount", api_->SessionGetOutputCount, session_, &outputCount) ||
            (outputCount != 1 && outputCount != 4)) return false;
        OrtAllocator* allocator{};
        if (!call("DefaultAllocator", api_->GetAllocatorWithDefaultOptions, &allocator)) return false;
        std::array<size_t, 5> slots{};
        for (size_t i = 0; i <= outputCount; ++i) {
            char* name{};
            const auto getter = i == 0 ? api_->SessionGetInputName : api_->SessionGetOutputName;
            if (!call("TensorName", getter, session_, i ? i - 1 : 0, allocator, &name)) return false;
            const std::string found(name);
            if (!call("AllocatorFree", api_->AllocatorFree, allocator, name)) return false;
            const auto slot = i < 2 ? i : found == "pushflow_grid0" ? 2u :
                found == "pushflow_grid1" ? 3u : found == "pushflow_mask" ? 4u : 0u;
            if ((i >= 2 && !slot) || !names[slot].empty()) return false;
            names[slot] = found;
            slots[i] = slot;
        }
        const auto selected = model_shader(outputCount, !names[2].empty() && !names[3].empty() && !names[4].empty(), facts);
        if (!selected) return false;
        shader = *selected;
        OrtTypeInfo* type{};
        const OrtTensorTypeAndShapeInfo* info{};
        ONNXTensorElementDataType element{};
        size_t rank{};
        Shape shape{};
        std::array<const char*, 4> symbols{};
        if (!call("InputTypeInfo", api_->SessionGetInputTypeInfo, session_, 0, &type)) return false;
        const bool shapeOk = call("TensorTypeInfo", api_->CastTypeInfoToTensorInfo, type, &info) && info &&
            call("TensorElementType", api_->GetTensorElementType, info, &element) &&
            element == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            call("TensorRank", api_->GetDimensionsCount, info, &rank) && rank == shape.size() &&
            call("TensorDimensions", api_->GetDimensions, info, shape.data(), shape.size()) &&
            call("TensorDimensionNames", api_->GetSymbolicDimensions, info, symbols.data(), symbols.size());
        std::array<std::string, 4> dimensionNames{};
        if (shapeOk)
            for (size_t i = 0; i < symbols.size(); ++i) if (symbols[i]) dimensionNames[i] = symbols[i];
        if (!invoke("ReleaseTypeInfo", api_->ReleaseTypeInfo, type) || !shapeOk ||
            (shape[1] != 7 && shape[1] != 11)) return false;
        inputShape = {1, shape[1], geometry.padH, geometry.padW};
        if (shape == inputShape) {
            for (size_t i = 1; i <= outputCount; ++i)
                if (!validate_output(i - 1, slots[i])) return false;
            return true;
        }
        if (pass == 1) return false;
        for (size_t i = 0; i < shape.size(); ++i) {
            if (shape[i] == inputShape[i]) continue;
            if (shape[i] >= 0 || dimensionNames[i].empty()) return false;
            for (size_t j = 0; j < i; ++j)
                if (overrides[j].first == dimensionNames[i] && overrides[j].second != inputShape[i]) return false;
            overrides[i] = {dimensionNames[i], inputShape[i]};
        }
        if (!release_session()) return false;
        names = {};
    }
    return false;
}

bool OrtState::validate_output(size_t index, size_t slot)
{
    OrtTypeInfo* type{};
    if (!call("OutputTypeInfo", api_->SessionGetOutputTypeInfo, session_, index, &type)) return false;
    const OrtTensorTypeAndShapeInfo* info{};
    OutputMetadata metadata{};
    std::array<const char*, 4> symbols{};
    bool accepted = call("OutputTensorInfo", api_->CastTypeInfoToTensorInfo, type, &info) && info &&
        call("OutputElementType", api_->GetTensorElementType, info, &metadata.element) &&
        call("OutputRank", api_->GetDimensionsCount, info, &metadata.rank) &&
        metadata.rank == metadata.shape.size() &&
        call("OutputDimensions", api_->GetDimensions, info, metadata.shape.data(), metadata.shape.size());
    if (accepted && std::ranges::find(metadata.shape, -1) != metadata.shape.end()) {
        accepted = call("OutputDimensionNames", api_->GetSymbolicDimensions, info, symbols.data(), symbols.size());
        for (size_t i = 0; i < symbols.size(); ++i) if (symbols[i]) metadata.symbols[i] = symbols[i];
    }
    accepted = accepted && compatible_output(metadata, output_layout(slot), inputShape);
    if (!accepted)
        std::fprintf(log_, "event=pushflow_output_metadata_rejected index=%zu name=%s type=%d rank=%zu\n",
            index, names[slot].c_str(), static_cast<int>(metadata.element), metadata.rank);
    return invoke("ReleaseOutputTypeInfo", api_->ReleaseTypeInfo, type) && accepted;
}

bool OrtState::bind(ID3D12Device* device)
{
    if (!call("CreateIoBinding", api_->CreateIoBinding, session_, &binding_) ||
        !call("CreateRunOptions", api_->CreateRunOptions, &runOptions_) ||
        !call("QueuedInference", api_->AddRunConfigEntry, runOptions_, "disable_synchronize_execution_providers", "1"))
        return false;
    for (size_t i = 0; i <= outputCount; ++i) {
        auto& tensor = tensors[i];
        const auto shape = i == 0 ? inputShape : output_shape(output_layout(i), inputShape);
        tensor.elements = static_cast<UINT>(shape[1] * shape[2] * shape[3]);
        const auto bytes = size_t{tensor.elements} * sizeof(float);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = (bytes + 255u) & ~size_t{255};
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tensor.buffer)), "PushFlow tensor buffer");
        if (!call("GPUAllocation", dml_->CreateGPUAllocationFromD3DResource, tensor.buffer.Get(), &tensor.allocation) ||
            !call("CreateTensor", api_->CreateTensorWithDataAsOrtValue, memory_, tensor.allocation, bytes,
                shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tensor.value)) return false;
        if (i == 0 ? !call("BindInput", api_->BindInput, binding_, names[i].c_str(), tensor.value) :
            !call("BindOutput", api_->BindOutput, binding_, names[i].c_str(), tensor.value)) return false;
    }
    return true;
}

bool OrtState::run() { return call("RunWithBinding", api_->RunWithBinding, session_, runOptions_, binding_); }
bool OrtState::sync() { return !binding_ || call("SynchronizeBoundOutputs", api_->SynchronizeBoundOutputs, binding_); }

bool OrtState::release()
{
    if (poisoned_) return false;
    // Keep binding and D3D resources live through provider sync and queue drain
    // (the caller's responsibility); do not proceed after any unverifiable release.
    if (binding_ && !invoke("ReleaseIoBinding", api_->ReleaseIoBinding, binding_)) return false;
    for (auto& t : tensors) if (t.value && !invoke("ReleaseValue", api_->ReleaseValue, t.value)) return false;
    for (auto& t : tensors) if (t.allocation && !call("FreeGPUAllocation", dml_->FreeGPUAllocation, t.allocation)) return false;
    if (!release_session()) return false;
    if (runOptions_ && !invoke("ReleaseRunOptions", api_->ReleaseRunOptions, runOptions_)) return false;
    if (memory_ && !invoke("ReleaseMemoryInfo", api_->ReleaseMemoryInfo, memory_)) return false;
    if (auto* dmlDevice = device_.Detach(); dmlDevice && !invoke("ReleaseDMLDevice", [&] { dmlDevice->Release(); }))
        return false;
    return true;
}
} // namespace nr::fg::vfg::pushflow
