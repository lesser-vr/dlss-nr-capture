#pragma once
#include "worker_protocol.hpp"
#include <d3d11.h>
#include <cstdint>

constexpr uint32_t nr_adapter_abi_version = 7;

struct NrTimingSnapshot {
    uint64_t total_us{};
    uint64_t input_us{};
    uint64_t setup_us{};
    uint64_t optical_flow_us{};
    uint64_t motion_vector_us{};
    uint64_t gpu_prepare_us{};
    uint64_t gpu_execute_us{};
    uint64_t bridge_output_us{};
    uint64_t correction_output_us{};
    uint32_t flow_mode{}; // 0=inactive, 1=GPU, 2=CPU, 3=CPU after GPU failure.
    uint32_t flow_error{}; // Original sharing HRESULT, retained for diagnostics.
};

using NrAdapterInitialize = bool (__stdcall*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*);
using NrAdapterProcess = bool (__stdcall*)(ID3D11DeviceContext*, ID3D11Texture2D*,
                                            const TemporalAnalysisPayload*);
using NrAdapterShutdown = void (__stdcall*)();
using NrAdapterLastError = const wchar_t* (__stdcall*)();
using NrAdapterGetTimings = void (__stdcall*)(NrTimingSnapshot*);

struct NrAdapterApi {
    uint32_t byte_size{};
    uint32_t abi_version{};
    const wchar_t* display_name{};
    NrAdapterInitialize initialize{};
    NrAdapterProcess process{};
    NrAdapterShutdown shutdown{};
    NrAdapterLastError last_error{};
    NrAdapterGetTimings get_timings{};
};

using NrAdapterGetApi = const NrAdapterApi* (__stdcall*)(uint32_t host_abi_version);
