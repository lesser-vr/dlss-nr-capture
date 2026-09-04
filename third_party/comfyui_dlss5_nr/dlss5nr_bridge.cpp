// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nvof_flow.h"
#include "nr_adapter_api.hpp"

using Microsoft::WRL::ComPtr;
using NGXResult = int;
static constexpr NGXResult NGX_SUCCESS = 1;
static constexpr int NR_FEATURE_ID = 18;
static constexpr unsigned long long APP_ID = 141959980ULL;
static constexpr const char* PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";

struct NGXHandle { unsigned int Id; };

// Minimal ABI-compatible interface used by the NVIDIA NGX parameter object.
struct NGXParameter {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;
    virtual NGXResult Get(const char*, float*) const = 0;
    virtual NGXResult Get(const char*, double*) const = 0;
    virtual NGXResult Get(const char*, unsigned int*) const = 0;
    virtual NGXResult Get(const char*, int*) const = 0;
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NGXResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

struct NGXPathListInfo {
    wchar_t const* const* Path;
    unsigned int Length;
};
enum NGXLoggingLevel { NGX_LOG_OFF = 0, NGX_LOG_ON = 1, NGX_LOG_VERBOSE = 2 };
using NGXLogCallback = void(__cdecl*)(const char*, NGXLoggingLevel, int);
struct NGXLoggingInfo {
    NGXLoggingLevel LoggingLevel;
    NGXLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
};
struct NGXFeatureCommonInfoInternal;
struct NGXFeatureCommonInfo {
    NGXPathListInfo PathListInfo;
    NGXFeatureCommonInfoInternal* InternalData;
    NGXLoggingInfo LoggingInfo;
};

using InitExtFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using InitProjectIdFn = NGXResult(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using CreateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvaluateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFeatureFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

using ShimInitFn = NGXResult(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using ShimCreateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using ShimEvaluateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShimReleaseFn = NGXResult(__cdecl*)(void*, NGXHandle*);

static std::mutex g_mutex;
static std::string g_last_error;
static std::wstring g_runtime_dir;
static int g_gpu_index = 0;
static std::string g_gpu_name = "unknown";
static bool g_initialized = false;
static NrTimingSnapshot g_timings{};
using TimingClock = std::chrono::steady_clock;
static uint64_t ElapsedUs(TimingClock::time_point start, TimingClock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

static HMODULE g_core_mod = nullptr;
static HMODULE g_nr_mod = nullptr;
static HMODULE g_shim_mod = nullptr;
static InitExtFn g_core_init_ext = nullptr;
static InitProjectIdFn g_core_init_project = nullptr;
static AllocParamsFn g_alloc_params = nullptr;
static CreateFeatureFn g_core_create = nullptr;
static EvaluateFeatureFn g_core_eval = nullptr;
static ReleaseFeatureFn g_core_release = nullptr;
static ShutdownFn g_core_shutdown = nullptr;
static SnippetInitFn g_nr_init = nullptr;
static CreateFeatureFn g_nr_create = nullptr;
static EvaluateFeatureFn g_nr_eval = nullptr;
static ReleaseFeatureFn g_nr_release = nullptr;
static ShimInitFn g_shim_init = nullptr;
static ShimCreateFn g_shim_create = nullptr;
static ShimEvaluateFn g_shim_eval = nullptr;
static ShimReleaseFn g_shim_release = nullptr;

static ComPtr<IDXGIAdapter1> g_adapter;
static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_cmd_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static UINT64 g_fence_value = 0;

static NGXParameter* g_params = nullptr;
static NGXHandle* g_feature = nullptr;
static ComPtr<ID3D12Resource> g_color;
static ComPtr<ID3D12Resource> g_output;
static ComPtr<ID3D12Resource> g_upload;
static ComPtr<ID3D12Resource> g_readback;
static ComPtr<ID3D12Resource> g_correction_target;
static ComPtr<ID3D12Resource> g_rejection_mask_upload;
static ComPtr<ID3D12DescriptorHeap> g_correction_descriptors;
static ComPtr<ID3D12DescriptorHeap> g_correction_rtv_heap;
static ComPtr<ID3D12RootSignature> g_correction_root_signature;
static ComPtr<ID3D12PipelineState> g_correction_pipeline;
static ComPtr<ID3D12DescriptorHeap> g_input_descriptors;
static ComPtr<ID3D12RootSignature> g_input_root_signature;
static ComPtr<ID3D12PipelineState> g_input_pipeline;
// v0.3.0: temporal mode always supplies a full-resolution R16G16_FLOAT
// motion-vector texture. Frame 0 uses zeros; later frames are generated by NVOFA.
static ComPtr<ID3D12Resource> g_mvec;
static ComPtr<ID3D12Resource> g_mvec_upload;
static ComPtr<ID3D12DescriptorHeap> g_mvec_descriptors;
static ComPtr<ID3D12RootSignature> g_mvec_root_signature;
static ComPtr<ID3D12PipelineState> g_mvec_pipeline;
static UINT g_width = 0, g_height = 0, g_row_pitch = 0;
static UINT64 g_total_bytes = 0;
static UINT64 g_mvec_total_bytes = 0;
static UINT g_flow_capacity_width = 0;
static UINT g_flow_capacity_height = 0;
static int g_feature_style = -999;
static int g_feature_preset = -999;
static int g_feature_motion = -1;
static bool g_channel_order_known = false;
static bool g_swap_output_channels = false;

static void SetError(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_last_error = buf;
}

static void CopyError(char* dst, int cap) {
    if (!dst || cap <= 0) return;
    const size_t n = std::min<size_t>(g_last_error.size(), static_cast<size_t>(cap - 1));
    memcpy(dst, g_last_error.data(), n);
    dst[n] = '\0';
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t c = a.back();
    if (c == L'\\' || c == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long FileTimeKey(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static HMODULE LoadCoreNGX(const std::wstring& runtime) {
    // 1) Explicit local override. This is also the quickest workaround if
    // DriverStore auto-discovery ever misses a vendor-specific INF name.
    const std::wstring local = Join(runtime, L"_nvngx.dll");
    if (FileExists(local)) {
        if (HMODULE m = LoadLibraryW(local.c_str())) return m;
    }

    // 2) Normal loader search (works on systems where NVIDIA exposes it).
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) return m;

    // 3) NVIDIA ships NGX core inside the active display-driver package in
    // DriverStore. The INF prefix is NOT always nv_dispi: depending on OEM,
    // notebook/desktop package and driver generation it can be nvddi, nvaci,
    // nvhmui, etc. Scan every NVIDIA-looking *.inf_* package instead.
    wchar_t windows_dir[MAX_PATH] = {};
    UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (windows_len == 0 || windows_len >= MAX_PATH) return nullptr;
    const std::wstring repo = std::wstring(windows_dir) + L"\\System32\\DriverStore\\FileRepository";
    const std::wstring pat = repo + L"\\nv*.inf_*";

    struct Candidate {
        std::wstring path;
        unsigned long long stamp;
    };
    std::vector<Candidate> candidates;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                candidates.push_back({candidate, FileTimeKey(fad.ftLastWriteTime)});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Prefer the newest package. DriverStore often retains older drivers after
    // updates, and loading a stale NGX core is worse than not finding one.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.stamp > b.stamp; });

    for (const Candidate& c : candidates) {
        if (HMODULE m = LoadLibraryW(c.path.c_str())) return m;
    }

    return nullptr;
}

static ComPtr<ID3D12Device> CreateDevice(int nvidia_index) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    int seen = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
        if (seen++ != nvidia_index) continue;
        char gpu_utf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu_utf8, static_cast<int>(sizeof(gpu_utf8)), nullptr, nullptr);
        if (gpu_utf8[0]) g_gpu_name = gpu_utf8;
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d)))) {
            g_adapter = adapter;
            return d;
        }
        return nullptr;
    }
    return nullptr;
}

static bool SetupD3D12() {
    g_device = CreateDevice(g_gpu_index);
    if (!g_device) { SetError("Could not create a D3D12 device for NVIDIA GPU index %d", g_gpu_index); return false; }

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue)))) { SetError("CreateCommandQueue failed"); return false; }
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc)))) { SetError("CreateCommandAllocator failed"); return false; }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd)))) { SetError("CreateCommandList failed"); return false; }
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { SetError("CreateFence failed"); return false; }
    return true;
}

static bool ExecuteAndWait() {
    HRESULT hr = g_cmd->Close();
    if (FAILED(hr)) { SetError("CommandList::Close failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
    ID3D12CommandList* lists[] = { g_cmd.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    ++g_fence_value;
    hr = g_queue->Signal(g_fence.Get(), g_fence_value);
    if (FAILED(hr)) { SetError("Queue::Signal failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
    if (g_fence->GetCompletedValue() < g_fence_value) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev) { SetError("CreateEvent failed"); return false; }
        g_fence->SetEventOnCompletion(g_fence_value, ev);
        DWORD w = WaitForSingleObject(ev, 30000);
        CloseHandle(ev);
        if (w != WAIT_OBJECT_0) { SetError("Timed out waiting for DLSS5 NR GPU work"); return false; }
    }
    g_cmd_alloc->Reset();
    g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
    return true;
}

static void WaitQueueIdle() {
    if (!g_queue || !g_fence) return;
    ++g_fence_value;
    if (SUCCEEDED(g_queue->Signal(g_fence.Get(), g_fence_value)) && g_fence->GetCompletedValue() < g_fence_value) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            g_fence->SetEventOnCompletion(g_fence_value, ev);
            WaitForSingleObject(ev, 30000);
            CloseHandle(ev);
        }
    }
}

static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

static ComPtr<ID3D12Resource> CreateTexture(
    UINT w, UINT h, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static ComPtr<ID3D12Resource> CreateLinearBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, sizeof(x));
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(s);
        m = (m | 0x800000u) >> (1 - e);
        return static_cast<uint16_t>(s | (m >> 13));
    }
    if (e >= 31) return static_cast<uint16_t>(s | 0x7c00u);
    return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

static float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (m == 0) x = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; --e; }
            m &= 0x3ff;
            x = (s << 31) | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 0x1f) x = (s << 31) | 0x7f800000u | (m << 13);
    else x = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, sizeof(f)); return f;
}

static bool EnsureMotionVectorPipeline() {
    if (g_mvec_pipeline && g_mvec_root_signature) return true;
    static constexpr char shader_source[] = R"(
StructuredBuffer<uint> coarse_flow : register(t0);
RWTexture2D<float2> motion_vectors : register(u0);
cbuffer Dimensions : register(b0) { uint width; uint height; uint flow_width; uint flow_height; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint cx = min((id.x * flow_width) / width, flow_width - 1);
    uint cy = min((id.y * flow_height) / height, flow_height - 1);
    uint packed = coarse_flow[cy * flow_width + cx];
    int dx = (int)(packed << 16) >> 16;
    int dy = (int)packed >> 16;
    motion_vectors[id.xy] = float2((float)dx / 32.0 / width,
                                   (float)dy / 32.0 / height);
})";
    ComPtr<ID3DBlob> shader, errors;
    HRESULT hr = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr,
                            "main", "cs_5_0", 0, 0, &shader, &errors);
    if (FAILED(hr)) { SetError("Motion-vector compute shader compilation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 3;
    root_desc.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &serialized, &errors);
    if (FAILED(hr)) { SetError("Motion-vector root signature serialization failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    hr = g_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&g_mvec_root_signature));
    if (FAILED(hr)) { SetError("Motion-vector root signature creation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = g_mvec_root_signature.Get();
    pipeline_desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&g_mvec_pipeline));
    if (FAILED(hr)) { SetError("Motion-vector compute pipeline creation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    return true;
}

static bool CreateMotionVectorDescriptors() {
    if (!EnsureMotionVectorPipeline()) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_mvec_descriptors)))) {
        SetError("Motion-vector descriptor heap creation failed"); return false;
    }
    const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_mvec_descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = g_flow_capacity_width * g_flow_capacity_height;
    srv.Buffer.StructureByteStride = sizeof(uint32_t);
    g_device->CreateShaderResourceView(g_mvec_upload.Get(), &srv, handle);
    handle.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_mvec.Get(), nullptr, &uav, handle);
    return true;
}

static bool EnsureInputPipeline() {
    if (g_input_pipeline && g_input_root_signature) return true;
    static constexpr char shader_source[] = R"(
StructuredBuffer<uint> capture_frame : register(t0);
RWTexture2D<float4> dlss_color : register(u0);
cbuffer Dimensions : register(b0) { uint width; uint height; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint packed = capture_frame[id.y * width + id.x];
    float b = (packed & 255) / 255.0;
    float g = ((packed >> 8) & 255) / 255.0;
    float r = ((packed >> 16) & 255) / 255.0;
    dlss_color[id.xy] = float4(r, g, b, 1.0);
})";
    ComPtr<ID3DBlob> shader, errors;
    HRESULT hr = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr,
                            "main", "cs_5_0", 0, 0, &shader, &errors);
    if (FAILED(hr)) { SetError("Input conversion shader compilation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.Num32BitValues = 2;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 3;
    root_desc.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &serialized, &errors);
    if (FAILED(hr)) { SetError("Input conversion root signature serialization failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    hr = g_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&g_input_root_signature));
    if (FAILED(hr)) { SetError("Input conversion root signature creation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = g_input_root_signature.Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g_input_pipeline));
    if (FAILED(hr)) { SetError("Input conversion pipeline creation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    return true;
}

static bool CreateInputDescriptors() {
    if (!g_upload || !g_color || !EnsureInputPipeline()) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_input_descriptors)))) {
        SetError("Input conversion descriptor heap creation failed"); return false;
    }
    const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_input_descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = g_width * g_height;
    srv.Buffer.StructureByteStride = sizeof(uint32_t);
    g_device->CreateShaderResourceView(g_upload.Get(), &srv, handle);
    handle.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_color.Get(), nullptr, &uav, handle);
    return true;
}

static bool RecordInputConversion() {
    if (!g_input_descriptors) { SetError("Shared GPU input is not configured"); return false; }
    auto to_write = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cmd->ResourceBarrier(1, &to_write);
    ID3D12DescriptorHeap* heaps[] = {g_input_descriptors.Get()};
    g_cmd->SetDescriptorHeaps(1, heaps);
    g_cmd->SetComputeRootSignature(g_input_root_signature.Get());
    g_cmd->SetPipelineState(g_input_pipeline.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_input_descriptors->GetGPUDescriptorHandleForHeapStart();
    g_cmd->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_cmd->SetComputeRootDescriptorTable(1, gpu);
    const UINT dimensions[] = {g_width, g_height};
    g_cmd->SetComputeRoot32BitConstants(2, 2, dimensions, 0);
    g_cmd->Dispatch((g_width + 7) / 8, (g_height + 7) / 8, 1);
    auto restore = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cmd->ResourceBarrier(1, &restore);
    return true;
}

static bool EnsureCorrectionPipeline() {
    if (g_correction_pipeline && g_correction_root_signature) return true;
    static constexpr char shader_source[] = R"(
Texture2D<float4> denoised : register(t0);
Texture2D<float4> source_color : register(t1);
StructuredBuffer<uint> rejection_mask : register(t2);
cbuffer Settings : register(b0) {
    uint width; uint height; uint mask_columns; uint mask_rows; uint swap_channels; uint padding;
};
float4 vs_main(uint id : SV_VertexID) : SV_POSITION {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_main(float4 position : SV_POSITION) : SV_TARGET {
    uint2 p = uint2(position.xy);
    float3 raw = denoised.Load(int3(p, 0)).rgb;
    float3 result = swap_channels != 0 ? raw.bgr : raw.rgb;
    float3 base = source_color.Load(int3(p, 0)).rgb;
    bool reject = false;
    if (mask_columns != 0 && mask_rows != 0) {
        uint column = min(mask_columns - 1, p.x * mask_columns / width);
        uint row = min(mask_rows - 1, p.y * mask_rows / height);
        reject = rejection_mask[row * mask_columns + column] != 0;
    }
    return float4(reject ? base : saturate(result), 1.0);
})";
    ComPtr<ID3DBlob> vertex, pixel, errors;
    HRESULT hr = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr,
                            "vs_main", "vs_5_0", 0, 0, &vertex, &errors);
    if (FAILED(hr)) { SetError("Correction vertex shader compilation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    errors.Reset();
    hr = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr,
                    "ps_main", "ps_5_0", 0, 0, &pixel, &errors);
    if (FAILED(hr)) { SetError("Correction pixel shader compilation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 6;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &serialized, &errors);
    if (FAILED(hr)) { SetError("Correction root signature serialization failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    hr = g_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&g_correction_root_signature));
    if (FAILED(hr)) { SetError("Correction root signature creation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = g_correction_root_signature.Get();
    pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pipeline.SampleDesc.Count = 1;
    hr = g_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&g_correction_pipeline));
    if (FAILED(hr)) { SetError("Correction graphics pipeline creation failed: 0x%08X", static_cast<unsigned>(hr)); return false; }
    return true;
}

static bool CreateCorrectionDescriptors() {
    if (!g_correction_target || !g_output || !g_color || !g_rejection_mask_upload ||
        !EnsureCorrectionPipeline()) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 3;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_correction_descriptors)))) {
        SetError("Correction descriptor heap creation failed"); return false;
    }
    const UINT stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_correction_descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC texture_srv{};
    texture_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texture_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture_srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_output.Get(), &texture_srv, handle);
    handle.ptr += stride;
    g_device->CreateShaderResourceView(g_color.Get(), &texture_srv, handle);
    handle.ptr += stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC mask_srv{};
    mask_srv.Format = DXGI_FORMAT_UNKNOWN;
    mask_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    mask_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    mask_srv.Buffer.NumElements = nr_worker_max_mask_tiles;
    mask_srv.Buffer.StructureByteStride = sizeof(uint32_t);
    g_device->CreateShaderResourceView(g_rejection_mask_upload.Get(), &mask_srv, handle);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{};
    rtv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap.NumDescriptors = 1;
    if (FAILED(g_device->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&g_correction_rtv_heap)))) {
        SetError("Correction RTV heap creation failed"); return false;
    }
    g_device->CreateRenderTargetView(g_correction_target.Get(), nullptr,
                                      g_correction_rtv_heap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

static bool RecordCorrectionOutput(const uint8_t* rejection_mask, int mask_columns, int mask_rows) {
    if (!g_correction_descriptors || !g_correction_rtv_heap) {
        SetError("Correction GPU target is not configured"); return false;
    }
    void* mapped{};
    if (FAILED(g_rejection_mask_upload->Map(0, nullptr, &mapped)) || !mapped) {
        SetError("Correction rejection-mask upload failed"); return false;
    }
    auto* values = static_cast<uint32_t*>(mapped);
    memset(values, 0, nr_worker_max_mask_tiles * sizeof(uint32_t));
    const int mask_count = mask_columns * mask_rows;
    for (int i = 0; rejection_mask && i < mask_count; ++i) values[i] = rejection_mask[i];
    g_rejection_mask_upload->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER barriers[] = {
        Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Barrier(g_correction_target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    g_cmd->ResourceBarrier(3, barriers);
    ID3D12DescriptorHeap* heaps[] = {g_correction_descriptors.Get()};
    g_cmd->SetDescriptorHeaps(1, heaps);
    g_cmd->SetGraphicsRootSignature(g_correction_root_signature.Get());
    g_cmd->SetPipelineState(g_correction_pipeline.Get());
    g_cmd->SetGraphicsRootDescriptorTable(0, g_correction_descriptors->GetGPUDescriptorHandleForHeapStart());
    const UINT settings[] = {g_width, g_height, static_cast<UINT>(mask_columns),
        static_cast<UINT>(mask_rows), g_swap_output_channels ? 1u : 0u, 0u};
    g_cmd->SetGraphicsRoot32BitConstants(1, 6, settings, 0);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_correction_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
    g_cmd->RSSetViewports(1, &viewport);
    g_cmd->RSSetScissorRects(1, &scissor);
    g_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmd->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER restore[] = {
        Barrier(g_output.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_correction_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON)};
    g_cmd->ResourceBarrier(3, restore);
    return true;
}

static bool UploadMotionVectorTexture(const NvofFlowFrame* flow, bool execute_now) {
    if (!g_mvec || !g_mvec_upload || !g_mvec_descriptors ||
        g_width == 0 || g_height == 0) {
        SetError("Motion-vector resources are not allocated");
        return false;
    }

    void* mapped = nullptr;
    HRESULT hr = g_mvec_upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        SetError("Motion-vector upload Map failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    UINT flow_width = g_flow_capacity_width;
    UINT flow_height = g_flow_capacity_height;
    if (flow && flow->has_flow) {
        if (flow->width == 0 || flow->height == 0 ||
            flow->width > g_flow_capacity_width || flow->height > g_flow_capacity_height ||
            flow->xy.size() < static_cast<size_t>(flow->width) * flow->height * 2) {
            g_mvec_upload->Unmap(0, nullptr);
            SetError("NVIDIA Optical Flow returned an invalid flow field");
            return false;
        }
        flow_width = flow->width;
        flow_height = flow->height;
        memcpy(mapped, flow->xy.data(), static_cast<size_t>(flow_width) * flow_height * sizeof(uint32_t));
    } else
        memset(mapped, 0, static_cast<size_t>(g_mvec_total_bytes));
    g_mvec_upload->Unmap(0, nullptr);

    auto to_write = Barrier(
        g_mvec.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cmd->ResourceBarrier(1, &to_write);
    ID3D12DescriptorHeap* heaps[] = {g_mvec_descriptors.Get()};
    g_cmd->SetDescriptorHeaps(1, heaps);
    g_cmd->SetComputeRootSignature(g_mvec_root_signature.Get());
    g_cmd->SetPipelineState(g_mvec_pipeline.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_mvec_descriptors->GetGPUDescriptorHandleForHeapStart();
    g_cmd->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_cmd->SetComputeRootDescriptorTable(1, gpu);
    const UINT dimensions[] = {g_width, g_height, flow_width, flow_height};
    g_cmd->SetComputeRoot32BitConstants(2, 4, dimensions, 0);
    g_cmd->Dispatch((g_width + 7) / 8, (g_height + 7) / 8, 1);

    auto to_read = Barrier(
        g_mvec.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cmd->ResourceBarrier(1, &to_read);

    return !execute_now || ExecuteAndWait();
}

static void ReleaseFeatureAndResources() {
    WaitQueueIdle();
    if (g_feature) {
        if (g_nr_release && g_shim_release) g_shim_release(reinterpret_cast<void*>(g_nr_release), g_feature);
        else if (g_core_release) g_core_release(g_feature);
        g_feature = nullptr;
    }

    // NGX parameter objects do not necessarily AddRef resources stored in them.
    if (g_params) {
        g_params->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
    }

    g_input_descriptors.Reset();
    g_correction_descriptors.Reset(); g_correction_rtv_heap.Reset();
    g_color.Reset(); g_output.Reset(); g_upload.Reset(); g_readback.Reset();
    g_mvec.Reset(); g_mvec_upload.Reset(); g_mvec_descriptors.Reset();
    g_width = g_height = g_row_pitch = 0;
    g_total_bytes = 0;
    g_mvec_total_bytes = 0;
    g_flow_capacity_width = g_flow_capacity_height = 0;
    g_feature_style = -999; g_feature_preset = -999; g_feature_motion = -1;
}

static bool AllocateFrameResources(UINT w, UINT h, bool use_motion_vectors) {
    g_color = CreateTexture(
        w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_output = CreateTexture(
        w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!g_color || !g_output) { SetError("Failed to create RGBA16F D3D12 textures"); return false; }

    g_row_pitch = (w * 8u + 255u) & ~255u;
    g_total_bytes = static_cast<UINT64>(g_row_pitch) * h;
    g_upload = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_readback = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_upload || !g_readback) { SetError("Failed to create D3D12 upload/readback buffers"); return false; }

    g_width = w; g_height = h;

    if (use_motion_vectors) {
        g_mvec = CreateTexture(
            w, h, DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!g_mvec) { SetError("Failed to create R16G16_FLOAT motion-vector texture"); return false; }

        g_flow_capacity_width = (w + 1) / 2;
        g_flow_capacity_height = (h + 1) / 2;
        g_mvec_total_bytes = static_cast<UINT64>(g_flow_capacity_width) *
            g_flow_capacity_height * sizeof(uint32_t);
        g_mvec_upload = CreateLinearBuffer(
            g_mvec_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_mvec_upload) { SetError("Failed to create motion-vector upload buffer"); return false; }
        if (!CreateMotionVectorDescriptors()) return false;

        // Feature creation always starts with a defined zero-MV resource. The
        // first temporal frame also uses this; NVOF has no previous frame yet.
        if (!UploadMotionVectorTexture(nullptr, true)) return false;
    }

    if (g_correction_target) {
        if (!g_rejection_mask_upload) {
            g_rejection_mask_upload = CreateLinearBuffer(
                nr_worker_max_mask_tiles * sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!g_rejection_mask_upload) { SetError("Failed to create rejection-mask upload buffer"); return false; }
        }
        if (!CreateCorrectionDescriptors()) return false;
    }
    if (!CreateInputDescriptors()) return false;

    return true;
}

static void SetCommonParams(
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, bool use_motion_vectors) {
    g_params->Set("DLSSNR.Width", g_width);
    g_params->Set("DLSSNR.Height", g_height);
    g_params->Set("DLSSNR.Enabled", 1);
    g_params->Set("DLSSNR.Reset", reset);
    g_params->Set("DLSSNR.Style", style);
    g_params->Set("DLSSNR.Hint.Render.Preset", preset);
    g_params->Set("DLSSNR.Intensity", intensity);
    g_params->Set("DLSSNR.LocalToneStrength", tone);
    g_params->Set("DLSSNR.LocalStructureStrength", structure);
    g_params->Set("DLSSNR.SkinStructureStrength", skin);
    g_params->Set("DLSSNR.UseAutoMask", automask);
    g_params->Set("DLSSNR.UICorrection", 0);
    g_params->Set("DLSSNR.DepthInverted", 1);
    g_params->Set("DLSSNR.ScalingRatio", 1.0f);
    g_params->Set("DLSSNR.Color", g_color.Get());
    g_params->Set("DLSSNR.Output", g_output.Get());
    g_params->Set("DLSSNR.Backbuffer", g_output.Get());
    g_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    g_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    g_params->Set("DLSSNR.ColorSubrectWidth", g_width);
    g_params->Set("DLSSNR.ColorSubrectHeight", g_height);
    g_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    g_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    g_params->Set("DLSSNR.OutputSubrectWidth", g_width);
    g_params->Set("DLSSNR.OutputSubrectHeight", g_height);

    if (use_motion_vectors && g_mvec) {
        g_params->Set("DLSSNR.MVec", g_mvec.Get());
        // g_mvec stores normalized UV. Multiplying by dimensions reconstructs
        // the original pixel displacement from NVOF exactly.
        g_params->Set("DLSSNR.MVecScaleX", static_cast<float>(g_width));
        g_params->Set("DLSSNR.MVecScaleY", static_cast<float>(g_height));
        g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
        g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
        g_params->Set("DLSSNR.MVecSubrectWidth", g_width);
        g_params->Set("DLSSNR.MVecSubrectHeight", g_height);
    } else {
        g_params->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
        g_params->Set("DLSSNR.MVecScaleX", 1.0f);
        g_params->Set("DLSSNR.MVecScaleY", 1.0f);
        g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
        g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
        g_params->Set("DLSSNR.MVecSubrectWidth", 0);
        g_params->Set("DLSSNR.MVecSubrectHeight", 0);
    }
}

static bool EnsureFeature(
    UINT w, UINT h, int style, int preset, float intensity, float tone, float structure,
    float skin, int automask, bool use_motion_vectors) {
    const int motion_key = use_motion_vectors ? 1 : 0;
    const bool rebuild = !g_feature || w != g_width || h != g_height ||
        style != g_feature_style || preset != g_feature_preset ||
        motion_key != g_feature_motion;
    if (!rebuild) {
        SetCommonParams(style, preset, intensity, tone, structure, skin, automask, 0, use_motion_vectors);
        return true;
    }

    ReleaseFeatureAndResources();
    if (!AllocateFrameResources(w, h, use_motion_vectors)) return false;
    SetCommonParams(style, preset, intensity, tone, structure, skin, automask, 1, use_motion_vectors);

    NGXResult r;
    if (g_nr_create && g_shim_create)
        r = g_shim_create(reinterpret_cast<void*>(g_nr_create), g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    else
        r = g_core_create(g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    if (r != NGX_SUCCESS || !g_feature) {
        SetError("CreateFeature(18) failed: 0x%08X. Check GPU support, driver, nvngx_dlssnr.dll, and caller shim.", static_cast<unsigned>(r));
        return false;
    }
    g_feature_style = style;
    g_feature_preset = preset;
    g_feature_motion = motion_key;
    return true;
}

static bool LoadNGX() {
    g_core_mod = LoadCoreNGX(g_runtime_dir);
    if (!g_core_mod) {
        SetError("Could not load NVIDIA NGX core _nvngx.dll. Tried runtime\\_nvngx.dll, normal DLL search, and NVIDIA DriverStore packages matching nv*.inf_*. You can copy the _nvngx.dll from your active NVIDIA DriverStore folder into runtime\\_nvngx.dll as an explicit override.");
        return false;
    }

    const std::wstring nr_path = Join(g_runtime_dir, L"nvngx_dlssnr.dll");
    if (!FileExists(nr_path)) { SetError("nvngx_dlssnr.dll not found in runtime folder"); return false; }
    g_nr_mod = LoadLibraryW(nr_path.c_str());
    if (!g_nr_mod) { SetError("LoadLibrary(nvngx_dlssnr.dll) failed: Win32 %lu", GetLastError()); return false; }

    std::wstring shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll_comfy.dll");
    if (!FileExists(shim_path)) {
        // Backward-compatible fallback for older builds.
        shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll");
    }
    if (!FileExists(shim_path)) { SetError("caller shim not found (expected caller\\nvngx.dll_comfy.dll)"); return false; }
    g_shim_mod = LoadLibraryW(shim_path.c_str());
    if (!g_shim_mod) { SetError("LoadLibrary(caller shim) failed: Win32 %lu", GetLastError()); return false; }

    g_core_init_ext = reinterpret_cast<InitExtFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_core_init_project = reinterpret_cast<InitProjectIdFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_ProjectID"));
    g_alloc_params = reinterpret_cast<AllocParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_AllocateParameters"));
    g_core_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_core_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_core_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
    g_core_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Shutdown"));

    g_nr_init = reinterpret_cast<SnippetInitFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_nr_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_nr_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_nr_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));

    g_shim_init = reinterpret_cast<ShimInitFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallInit"));
    g_shim_create = reinterpret_cast<ShimCreateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallCreate"));
    g_shim_eval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallEvaluate"));
    g_shim_release = reinterpret_cast<ShimReleaseFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallRelease"));

    if (!g_core_init_ext || !g_alloc_params || !g_core_create || !g_core_eval || !g_core_release || !g_core_shutdown) {
        SetError("Required NGX core exports are missing"); return false;
    }
    if (!g_nr_init || !g_nr_create || !g_nr_eval || !g_nr_release) {
        SetError("Required DLSSNR exports are missing from nvngx_dlssnr.dll"); return false;
    }
    if (!g_shim_init || !g_shim_create || !g_shim_eval || !g_shim_release) {
        SetError("Required caller shim exports are missing"); return false;
    }
    return true;
}

static bool InitNGXSession() {
    const wchar_t* paths[1] = { g_runtime_dir.c_str() };
    NGXPathListInfo pli{ paths, 1 };
    NGXFeatureCommonInfo fci{};
    fci.PathListInfo = pli;
    fci.LoggingInfo.LoggingLevel = NGX_LOG_OFF;

    bool core_ok = false;
    if (g_core_init_project) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_project(PROJECT_ID, 0, "0.3.0", g_runtime_dir.c_str(), g_device.Get(), ver, nullptr);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_ext(APP_ID, g_runtime_dir.c_str(), g_device.Get(), ver, &fci);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) { SetError("NGX core initialization failed for API versions 0x13..0x20"); return false; }

    NGXResult sr = g_shim_init(reinterpret_cast<void*>(g_nr_init), APP_ID, g_runtime_dir.c_str(), g_device.Get(), 0x15, &fci);
    if (sr != NGX_SUCCESS) {
        wchar_t shim_self[MAX_PATH] = L"<unknown>";
        GetModuleFileNameW(g_shim_mod, shim_self, MAX_PATH);
        char shim_utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, shim_self, -1, shim_utf8, static_cast<int>(sizeof(shim_utf8)), nullptr, nullptr);
        SetError("DLSSNR snippet Init_Ext via caller shim failed: 0x%08X; loaded shim=%s", static_cast<unsigned>(sr), shim_utf8);
        return false;
    }

    NGXResult ar = g_alloc_params(&g_params);
    if (ar != NGX_SUCCESS || !g_params) {
        SetError("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", static_cast<unsigned>(ar));
        return false;
    }
    return true;
}

static void ShutdownUnlocked() {
    ReleaseFeatureAndResources();
    NvofShutdown();
    if (g_core_shutdown) g_core_shutdown();
    g_params = nullptr;
    g_input_descriptors.Reset(); g_input_pipeline.Reset(); g_input_root_signature.Reset();
    g_correction_descriptors.Reset(); g_correction_rtv_heap.Reset();
    g_correction_target.Reset(); g_rejection_mask_upload.Reset();
    g_correction_pipeline.Reset(); g_correction_root_signature.Reset();
    g_device.Reset(); g_adapter.Reset(); g_queue.Reset(); g_cmd_alloc.Reset(); g_cmd.Reset(); g_fence.Reset();
    if (g_shim_mod) FreeLibrary(g_shim_mod);
    if (g_nr_mod) FreeLibrary(g_nr_mod);
    if (g_core_mod) FreeLibrary(g_core_mod);
    g_shim_mod = g_nr_mod = g_core_mod = nullptr;

    g_core_init_ext = nullptr;
    g_core_init_project = nullptr;
    g_alloc_params = nullptr;
    g_core_create = nullptr;
    g_core_eval = nullptr;
    g_core_release = nullptr;
    g_core_shutdown = nullptr;
    g_nr_init = nullptr;
    g_nr_create = nullptr;
    g_nr_eval = nullptr;
    g_nr_release = nullptr;
    g_shim_init = nullptr;
    g_shim_create = nullptr;
    g_shim_eval = nullptr;
    g_shim_release = nullptr;

    g_initialized = false;
}

extern "C" {

__declspec(dllexport) const char* __cdecl dlss5nr_version() {
    return "0.8.0-direct-nvof-input";
}

__declspec(dllexport) const char* __cdecl dlss5nr_gpu_name() {
    return g_gpu_name.c_str();
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_available() {
    return NvofDriverApiAvailable() ? 1 : 0;
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_grid() {
    return static_cast<int>(NvofGridSize());
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_perf() {
    return static_cast<int>(NvofPerfLevel());
}

__declspec(dllexport) int __cdecl dlss5nr_init(int gpu_index, const wchar_t* runtime_dir, char* err, int err_cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (g_initialized) { CopyError(err, err_cap); return 1; }
    if (!runtime_dir || !*runtime_dir) { SetError("runtime_dir is empty"); CopyError(err, err_cap); return 0; }

    g_gpu_index = gpu_index;
    g_runtime_dir = runtime_dir;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)co; // RPC_E_CHANGED_MODE is harmless for this use.

    if (!SetupD3D12() || !LoadNGX() || !InitNGXSession()) {
        ShutdownUnlocked();
        CopyError(err, err_cap);
        return 0;
    }
    g_initialized = true;
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) int __cdecl dlss5nr_create_correction_target(
    int width, int height, HANDLE* correction_handle, char* err, int err_cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (!g_initialized || !correction_handle || width <= 0 || height <= 0) {
        SetError("Invalid shared correction target"); CopyError(err, err_cap); return 0;
    }
    *correction_handle = nullptr;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> target;
    HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target));
    if (FAILED(hr)) {
        SetError("Create shared D3D12 correction target failed: 0x%08X", static_cast<unsigned>(hr));
        CopyError(err, err_cap); return 0;
    }
    hr = g_device->CreateSharedHandle(target.Get(), nullptr, GENERIC_ALL, nullptr, correction_handle);
    if (FAILED(hr)) {
        SetError("CreateSharedHandle for D3D12 correction target failed: 0x%08X", static_cast<unsigned>(hr));
        CopyError(err, err_cap); return 0;
    }
    g_correction_target = target;
    g_channel_order_known = false;
    g_swap_output_channels = false;
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) int __cdecl dlss5nr_process(
    ID3D11Device* input_device, ID3D11DeviceContext* input_context,
    ID3D11Texture2D* input_texture, const uint8_t* bgra_in, int width, int height,
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, int temporal,
    const uint8_t* rejection_mask, int mask_columns, int mask_rows,
    char* err, int err_cap) {

    std::lock_guard<std::mutex> guard(g_mutex);
    const auto frame_start = TimingClock::now();
    g_timings = {};
    g_last_error.clear();
    if (!g_initialized) { SetError("DLSS5 NR bridge is not initialized"); CopyError(err, err_cap); return 0; }
    if (!input_device || !input_context || !input_texture || !bgra_in ||
        !g_correction_target || width <= 0 || height <= 0) {
        SetError("Invalid D3D11 input, image buffer, dimensions, or correction target");
        CopyError(err, err_cap); return 0;
    }
    if (mask_columns < 0 || mask_rows < 0 ||
        static_cast<uint64_t>(mask_columns) * mask_rows > nr_worker_max_mask_tiles) {
        SetError("Invalid rejection mask dimensions"); CopyError(err, err_cap); return 0;
    }
    if (width > 16384 || height > 16384) { SetError("Image dimensions are unreasonably large"); CopyError(err, err_cap); return 0; }

    const bool use_motion_vectors = temporal != 0;
    const auto setup_start = TimingClock::now();
    if (!use_motion_vectors) {
        // Still-image mode owns no temporal history or OFA resources. This also
        // releases OFA VRAM when a workflow switches from temporal back to still.
        NvofReleaseSession();
    }

    if (!EnsureFeature(static_cast<UINT>(width), static_cast<UINT>(height), style, preset, intensity, tone, structure, skin, automask, use_motion_vectors)) {
        CopyError(err, err_cap); return 0;
    }
    const auto setup_end = TimingClock::now();
    g_timings.setup_us = ElapsedUs(setup_start, setup_end);

    const auto flow_start = TimingClock::now();
    if (use_motion_vectors) {
        NvofFlowFrame flow;
        std::string of_error;
        if (!NvofPrepareFrame(g_adapter.Get(), input_device, input_context,
                input_texture, static_cast<UINT>(width), static_cast<UINT>(height),
                reset != 0, flow, of_error)) {
            SetError("%s", of_error.c_str());
            CopyError(err, err_cap);
            return 0;
        }
        const auto flow_end = TimingClock::now();
        g_timings.optical_flow_us = ElapsedUs(flow_start, flow_end);
        const auto motion_start = flow_end;
        // First frame: no previous image exists, so this deliberately writes zero
        // MVs. Later frames upload NVOFA current->previous optical flow.
        if (!UploadMotionVectorTexture(flow.has_flow ? &flow : nullptr, false)) {
            CopyError(err, err_cap);
            return 0;
        }
        g_timings.motion_vector_us = ElapsedUs(motion_start, TimingClock::now());
    }

    const auto prepare_start = TimingClock::now();

    SetCommonParams(style, preset, intensity, tone, structure, skin, automask, reset ? 1 : 0, use_motion_vectors);

    void* mapped = nullptr;
    HRESULT upload_result = g_upload->Map(0, nullptr, &mapped);
    if (FAILED(upload_result) || !mapped) {
        SetError("BGRA input upload Map failed: 0x%08X", static_cast<unsigned>(upload_result));
        CopyError(err, err_cap); return 0;
    }
    memcpy(mapped, bgra_in, static_cast<size_t>(width) * height * 4);
    g_upload->Unmap(0, nullptr);
    if (!RecordInputConversion()) { CopyError(err, err_cap); return 0; }

    NGXResult er = g_shim_eval(reinterpret_cast<void*>(g_nr_eval), g_cmd.Get(), g_feature, g_params, nullptr);
    if (er != NGX_SUCCESS) {
        SetError("DLSSNR EvaluateFeature failed: 0x%08X", static_cast<unsigned>(er));
        // Reset command list to a clean state before returning.
        ExecuteAndWait();
        CopyError(err, err_cap); return 0;
    }

    const auto execute_start = TimingClock::now();
    g_timings.gpu_prepare_us = ElapsedUs(prepare_start, execute_start);
    if (!g_channel_order_known) {
        auto to_copy = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_cmd->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION rd{};
        rd.pResource = g_readback.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.PlacedFootprint.Footprint.Width = width; rd.PlacedFootprint.Footprint.Height = height;
        rd.PlacedFootprint.Footprint.Depth = 1; rd.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
        D3D12_TEXTURE_COPY_LOCATION rs{};
        rs.pResource = g_output.Get(); rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        auto restore_output = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_cmd->ResourceBarrier(1, &restore_output);
        if (!ExecuteAndWait()) { CopyError(err, err_cap); return 0; }
        const auto detect_start = TimingClock::now();
        void* rmap = nullptr;
        HRESULT hr = g_readback->Map(0, nullptr, &rmap);
        if (FAILED(hr) || !rmap) { SetError("Readback Map failed: 0x%08X", static_cast<unsigned>(hr)); CopyError(err, err_cap); return 0; }
        const auto* base = static_cast<const uint8_t*>(rmap);
        double direct_error = 0.0, swapped_error = 0.0;
        for (int y = 0; y < height; y += 8) {
            const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * g_row_pitch);
            for (int x = 0; x < width; x += 8) {
                const size_t pixel = static_cast<size_t>(y) * width + x;
                const float first = HalfToFloat(row[x * 4]);
                const float third = HalfToFloat(row[x * 4 + 2]);
                const float expected_r = bgra_in[pixel * 4 + 2] / 255.0f;
                const float expected_b = bgra_in[pixel * 4] / 255.0f;
                direct_error += std::abs(first - expected_r) + std::abs(third - expected_b);
                swapped_error += std::abs(third - expected_r) + std::abs(first - expected_b);
            }
        }
        g_readback->Unmap(0, nullptr);
        g_swap_output_channels = swapped_error < direct_error;
        g_channel_order_known = true;
        g_timings.bridge_output_us = ElapsedUs(detect_start, TimingClock::now());
    }
    if (!RecordCorrectionOutput(rejection_mask, mask_columns, mask_rows) ||
        !ExecuteAndWait()) { CopyError(err, err_cap); return 0; }
    const auto frame_end = TimingClock::now();
    g_timings.gpu_execute_us = ElapsedUs(execute_start, frame_end) - g_timings.bridge_output_us;
    g_timings.total_us = ElapsedUs(frame_start, frame_end);
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) void __cdecl dlss5nr_get_timings(NrTimingSnapshot* result) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (result) *result = g_timings;
}

__declspec(dllexport) void __cdecl dlss5nr_shutdown() {
    std::lock_guard<std::mutex> guard(g_mutex);
    ShutdownUnlocked();
}

}
