// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Small NVSDK_NGX_Parameter implementation shared by the ANR and XeMFG
// transports. Providers only read parameters synchronously inside one call, so
// a type-erased map is enough and no NGX helper library is needed.
#include <nvsdk_ngx_params.h>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace nr::fg::vfg {
class NgxParameters final : public NVSDK_NGX_Parameter {
    using Value = std::variant<unsigned long long, unsigned, int, float, double, void*>;
    std::map<std::string, Value, std::less<>> values_;
    template<class T> NVSDK_NGX_Result get(const char* name, T* out) const
    {
        const auto found = values_.find(name);
        if (!out || found == values_.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        if constexpr (std::is_pointer_v<T>) {
            const auto* value = std::get_if<void*>(&found->second);
            if (!value) return NVSDK_NGX_Result_FAIL_InvalidParameter;
            *out = static_cast<T>(*value);
        } else if (const auto* exact = std::get_if<T>(&found->second)) {
            *out = *exact;
        } else {
            const auto& item = found->second;
            long double n{};
            if (const auto* wide = std::get_if<unsigned long long>(&item)) n = static_cast<long double>(*wide);
            else if (const auto* word = std::get_if<unsigned>(&item)) n = *word;
            else if (const auto* signedWord = std::get_if<int>(&item)) n = *signedWord;
            else if (const auto* single = std::get_if<float>(&item)) n = *single;
            else if (const auto* real = std::get_if<double>(&item)) n = *real;
            else return NVSDK_NGX_Result_FAIL_InvalidParameter;
            if constexpr (std::is_integral_v<T>) {
                if (!(n >= static_cast<long double>(std::numeric_limits<T>::lowest()) &&
                    n < static_cast<long double>(std::numeric_limits<T>::max()) + 1.L))
                    return NVSDK_NGX_Result_FAIL_InvalidParameter;
            }
            *out = static_cast<T>(n);
        }
        return NVSDK_NGX_Result_Success;
    }
public:
    void Set(const char* n, unsigned long long v) override { values_[n] = v; }
    void Set(const char* n, unsigned v) override { values_[n] = v; }
    void Set(const char* n, int v) override { values_[n] = v; }
    void Set(const char* n, float v) override { values_[n] = v; }
    void Set(const char* n, double v) override { values_[n] = v; }
    void Set(const char* n, ID3D11Resource* v) override { values_[n] = static_cast<void*>(v); }
    void Set(const char* n, ID3D12Resource* v) override { values_[n] = static_cast<void*>(v); }
    void Set(const char* n, void* v) override { values_[n] = v; }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, unsigned* v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, int* v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, float* v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, double* v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** v) const override { return get(n, v); }
    NVSDK_NGX_Result Get(const char* n, void** v) const override { return get(n, v); }
    void Reset() override { values_.clear(); }
};
} // namespace nr::fg::vfg
