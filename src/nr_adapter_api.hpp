#pragma once
#include "worker_protocol.hpp"
#include <d3d11.h>
#include <cstdint>

constexpr uint32_t nr_adapter_abi_version = 1;

using NrAdapterInitialize = bool (__stdcall*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*);
using NrAdapterProcess = bool (__stdcall*)(ID3D11DeviceContext*, ID3D11Texture2D*,
                                            const TemporalAnalysisPayload*);
using NrAdapterShutdown = void (__stdcall*)();

struct NrAdapterApi {
    uint32_t byte_size{};
    uint32_t abi_version{};
    const wchar_t* display_name{};
    NrAdapterInitialize initialize{};
    NrAdapterProcess process{};
    NrAdapterShutdown shutdown{};
};

using NrAdapterGetApi = const NrAdapterApi* (__stdcall*)(uint32_t host_abi_version);
