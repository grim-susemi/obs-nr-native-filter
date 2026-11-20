// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Private to the worker. NVOF uses the same private immediate context as NGX,
// under ngxMutex, so its registered textures cannot escape to OBS graphics.
template<typename Fn, typename... Args>
__declspec(noinline) bool flow_call(NV_OF_STATUS& status, Fn fn, Args... args) noexcept
{
    __try {
        status = fn(args...);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct OpticalFlow {
    Handle file;
    HMODULE module = nullptr;
    NV_OF_D3D11_API_FUNCTION_LIST api{};
    NvOFHandle session = nullptr;
    std::array<NvOFGPUBufferHandle, 4> registered{};
    std::array<ComPtr<ID3D11Texture2D>, 2> inputs;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> targets;
    Image vectors, cost;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11RasterizerState> raster;
    bool uncertain = false;

    template<typename Fn, typename... Args>
    void checked(const char* where, Fn fn, Args... args)
    {
        NV_OF_STATUS status = NV_OF_ERR_GENERIC;
        if (!flow_call(status, fn, args...)) {
            uncertain = true;
            throw std::runtime_error(std::string(where) + " faulted");
        }
        if (status != NV_OF_SUCCESS)
            throw std::runtime_error(std::string(where) + " status=" + std::to_string(static_cast<unsigned>(status)));
    }

    std::array<DXGI_FORMAT, 256> formats(NV_OF_BUFFER_USAGE usage)
    {
        uint32_t count = 0;
        checked("NVOF format count", api.nvOFGetSurfaceFormatCountD3D11, session, usage, NV_OF_MODE_OPTICALFLOW, &count);
        std::array<DXGI_FORMAT, 256> values{};
        if (!count || count > values.size())
            throw std::runtime_error("NVOF malformed or unsupported format list");
        checked("NVOF formats", api.nvOFGetSurfaceFormatD3D11, session, usage, NV_OF_MODE_OPTICALFLOW, values.data());
        return values;
    }

    void open(ID3D11Device* device, ID3D11DeviceContext* commands, UINT width, UINT height)
    {
        std::array<wchar_t, 32768> system{};
        const auto length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (!length || length >= system.size())
            throw std::runtime_error("NVOF System32 unavailable");
        const auto path = std::filesystem::path(system.data()) / L"nvofapi64.dll";
        // Same signer/path admission as nr_guides. Keep the verified file
        // pinned through cleanup and reject exports forwarded to another image.
        admit_runtime(path, file);
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module || !image_matches(module, path))
            throw std::runtime_error("NVOF System32 library image rejected");
        const auto maximum = NgxD3d11::symbol<decltype(&NvOFGetMaxSupportedApiVersion)>(module, "NvOFGetMaxSupportedApiVersion");
        const auto create = NgxD3d11::symbol<decltype(&NvOFAPICreateInstanceD3D11)>(module, "NvOFAPICreateInstanceD3D11");
        uint32_t version = 0;
        checked("NVOF API version", maximum, &version);
        if (version < NV_OF_API_VERSION)
            throw std::runtime_error("NVOF API 5 unsupported");
        checked("NVOF D3D11 API table", create, NV_OF_API_VERSION, &api);
        if (!api.nvCreateOpticalFlowD3D11 || !api.nvOFInit || !api.nvOFGetCaps ||
            !api.nvOFGetSurfaceFormatCountD3D11 || !api.nvOFGetSurfaceFormatD3D11 ||
            !api.nvOFRegisterResourceD3D11 || !api.nvOFUnregisterResourceD3D11 ||
            !api.nvOFExecute || !api.nvOFDestroy)
            throw std::runtime_error("NVOF D3D11 API table incomplete");
        checked("NVOF D3D11 create", api.nvCreateOpticalFlowD3D11, device, commands, &session);
        if (!session)
            throw std::runtime_error("NVOF returned no session");
        uint32_t count = 0;
        constexpr auto cap = NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES;
        checked("NVOF grid count", api.nvOFGetCaps, session, cap, nullptr, &count);
        std::array<uint32_t, 256> grids{};
        if (!count || count > grids.size())
            throw std::runtime_error("NVOF grid capability malformed");
        const auto allocated = count;
        checked("NVOF grids", api.nvOFGetCaps, session, cap, grids.data(), &count);
        const auto gridsEnd = grids.begin() + allocated;
        if (count != allocated || std::find(grids.begin(), gridsEnd, 4u) == gridsEnd)
            throw std::runtime_error("NVOF 4x4 output grid unsupported");

        const auto has = [](const auto& values, DXGI_FORMAT format) {
            return std::find(values.begin(), values.end(), format) != values.end();
        };
        constexpr auto inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        constexpr auto vectorFormat = DXGI_FORMAT_R16G16_SINT;
        if (!has(formats(NV_OF_BUFFER_USAGE_INPUT), inputFormat) ||
            !has(formats(NV_OF_BUFFER_USAGE_OUTPUT), vectorFormat))
            throw std::runtime_error("NVOF BGRA8 input or SHORT2 vectors unsupported");
        const auto costs = formats(NV_OF_BUFFER_USAGE_COST);
        const auto costFormat = has(costs, DXGI_FORMAT_R8_UINT) ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32_UINT;
        if (!has(costs, costFormat))
            throw std::runtime_error("NVOF cost unsupported");

        NV_OF_INIT_PARAMS init{};
        init.width = width;
        init.height = height;
        init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = NV_OF_PERF_LEVEL_FAST;
        init.enableOutputCost = NV_OF_TRUE;
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
        checked("NVOF init 4x4", api.nvOFInit, session, &init);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = inputFormat;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (size_t i = 0; i < inputs.size(); ++i) {
            check(device->CreateTexture2D(&desc, nullptr, &inputs[i]), "NVOF BGRA8 texture");
            check(device->CreateRenderTargetView(inputs[i].Get(), nullptr, &targets[i]), "NVOF BGRA8 target");
            checked("NVOF register input", api.nvOFRegisterResourceD3D11, session, inputs[i].Get(), &registered[i]);
        }
        vectors = make_image(device, (width + 3) / 4, (height + 3) / 4, vectorFormat, true);
        cost = make_image(device, (width + 3) / 4, (height + 3) / 4, costFormat, true);
        checked("NVOF register vectors", api.nvOFRegisterResourceD3D11, session, vectors.texture.Get(), &registered[2]);
        checked("NVOF register cost", api.nvOFRegisterResourceD3D11, session, cost.texture.Get(), &registered[3]);
        if (std::find(registered.begin(), registered.end(), nullptr) != registered.end())
            throw std::runtime_error("NVOF resource handle missing");
        const auto vs = shader_code("flow_vertex", "vs_5_0");
        const auto ps = shader_code("flow_input", "ps_5_0");
        check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "NVOF input VS");
        check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel), "NVOF input PS");
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rs, &raster), "NVOF input rasterizer");
        blog(LOG_INFO, "obs-nr-upscale: NVOF D3D11 API 5 initialized (4x4, FAST, %ux%u, %s)",
             width, height, costFormat == DXGI_FORMAT_R8_UINT ? "R8 cost" : "R32 cost");
    }

    void prepare(ID3D11DeviceContext* commands, size_t slot, const Image& color, UINT w, UINT h) const
    {
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        auto* srv = color.srv.Get();
        auto* rtv = targets[slot].Get();
        commands->OMSetRenderTargets(1, &rtv, nullptr);
        commands->OMSetBlendState(nullptr, nullptr, UINT_MAX);
        commands->RSSetState(raster.Get());
        commands->RSSetViewports(1, &viewport);
        commands->IASetInputLayout(nullptr);
        commands->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->VSSetShader(vertex.Get(), nullptr, 0);
        commands->GSSetShader(nullptr, nullptr, 0);
        commands->PSSetShader(pixel.Get(), nullptr, 0);
        commands->PSSetShaderResources(0, 1, &srv);
        commands->Draw(3, 0);
        srv = nullptr;
        commands->PSSetShaderResources(0, 1, &srv);
        commands->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void execute(size_t slot, bool resetHints)
    {
        NV_OF_EXECUTE_INPUT_PARAMS in{};
        in.inputFrame = registered[slot];
        in.referenceFrame = registered[slot ^ 1u];
        in.disableTemporalHints = resetHints ? NV_OF_TRUE : NV_OF_FALSE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};
        out.outputBuffer = registered[2];
        out.outputCostBuffer = registered[3];
        checked("NVOF current-to-previous execute", api.nvOFExecute, session, &in, &out);
    }

    // Caller has proven the private immediate context idle. Each successful
    // unregister must precede destroy; on uncertainty retain the owner.
    bool close() noexcept
    {
        if (uncertain)
            return false;
        for (const auto buffer : registered) {
            NV_OF_STATUS status = NV_OF_ERR_GENERIC;
            if (buffer && (!flow_call(status, api.nvOFUnregisterResourceD3D11, buffer) || status != NV_OF_SUCCESS))
                return false;
        }
        NV_OF_STATUS status = NV_OF_ERR_GENERIC;
        if (session && (!flow_call(status, api.nvOFDestroy, session) || status != NV_OF_SUCCESS))
            return false;
        return !module || FreeLibrary(module);
    }
};
