#include "nr_adapter_api.hpp"
#include "nr_temporal_policy.hpp"
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace {
using BridgeInit = int (__cdecl*)(int, const wchar_t*, char*, int);
using BridgeCreateCorrectionTarget = int (__cdecl*)(int, int, HANDLE*, char*, int);
using BridgeProcess = int (__cdecl*)(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*,
                                     const uint8_t*, int, int, int, int,
                                     float, float, float, float, int, int, int,
                                     const uint8_t*, int, int, char*, int);
using BridgeShutdown = void (__cdecl*)();
using BridgeGetTimings = void (__cdecl*)(NrTimingSnapshot*);

HMODULE bridge_module{};
BridgeInit bridge_init{};
BridgeCreateCorrectionTarget bridge_create_correction_target{};
BridgeProcess bridge_process{};
BridgeShutdown bridge_shutdown{};
BridgeGetTimings bridge_get_timings{};
ID3D11Device* device{};
ID3D11Texture2D* staging{};
ID3D11Texture2D* gpu_correction{};
ID3D11Query* correction_copy_query{};
HANDLE gpu_correction_handle{};
D3D11_TEXTURE2D_DESC input_description{};
std::vector<uint8_t> bgra_input;
std::wstring last_error_message;
NrTimingSnapshot last_timings{};
bool needs_channel_reference{true};

using TimingClock = std::chrono::steady_clock;
uint64_t elapsed_us(TimingClock::time_point start, TimingClock::time_point end)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

void set_last_error(const char* message)
{
    if (!message || !*message) { last_error_message.clear(); return; }
    const int length = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
    if (length <= 1) { last_error_message = L"Unknown NR adapter error"; return; }
    last_error_message.resize(static_cast<size_t>(length));
    MultiByteToWideChar(CP_UTF8, 0, message, -1, last_error_message.data(), length);
    last_error_message.resize(static_cast<size_t>(length - 1));
}

const wchar_t* __stdcall last_error() { return last_error_message.c_str(); }
void __stdcall get_timings(NrTimingSnapshot* result) { if (result) *result = last_timings; }

std::wstring module_directory()
{
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring directory = path;
    const size_t separator = directory.find_last_of(L"\\/");
    directory.resize(separator == std::wstring::npos ? 0 : separator + 1);
    return directory;
}

bool __stdcall initialize(ID3D11Device* supplied_device, const D3D11_TEXTURE2D_DESC* description)
{
    needs_channel_reference = true;
    last_error_message.clear();
    if (!supplied_device || !description || description->Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        last_error_message = L"Unsupported D3D11 device or texture format";
        return false;
    }
    const std::wstring base = module_directory();
    const std::wstring runtime = base + L"nr-runtime";
    const std::wstring bridge = runtime + L"\\dlss5nr_bridge.dll";
    bridge_module = LoadLibraryExW(bridge.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!bridge_module) { last_error_message = L"Cannot load nr-runtime\\dlss5nr_bridge.dll"; return false; }
    bridge_init = reinterpret_cast<BridgeInit>(GetProcAddress(bridge_module, "dlss5nr_init"));
    bridge_process = reinterpret_cast<BridgeProcess>(GetProcAddress(bridge_module, "dlss5nr_process"));
    bridge_create_correction_target = reinterpret_cast<BridgeCreateCorrectionTarget>(
        GetProcAddress(bridge_module, "dlss5nr_create_correction_target"));
    bridge_shutdown = reinterpret_cast<BridgeShutdown>(GetProcAddress(bridge_module, "dlss5nr_shutdown"));
    bridge_get_timings = reinterpret_cast<BridgeGetTimings>(GetProcAddress(bridge_module, "dlss5nr_get_timings_v2"));
    if (!bridge_init || !bridge_create_correction_target || !bridge_process ||
        !bridge_shutdown || !bridge_get_timings) {
        last_error_message = L"Required dlss5nr_bridge exports are missing";
        return false;
    }

    char error[1024]{};
    if (!bridge_init(0, runtime.c_str(), error, static_cast<int>(sizeof(error)))) {
        set_last_error(error);
        return false;
    }

    device = supplied_device;
    device->AddRef();
    input_description = *description;
    D3D11_TEXTURE2D_DESC staging_description = *description;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    staging_description.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&staging_description, nullptr, &staging))) {
        last_error_message = L"Cannot create the NR staging texture";
        return false;
    }
    if (!bridge_create_correction_target(static_cast<int>(description->Width),
            static_cast<int>(description->Height), &gpu_correction_handle,
            error, static_cast<int>(sizeof(error)))) {
        set_last_error(error);
        return false;
    }
    ID3D11Device1* device1{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
        FAILED(device1->OpenSharedResource1(gpu_correction_handle,
                                            IID_PPV_ARGS(&gpu_correction)))) {
        if (device1) device1->Release();
        last_error_message = L"Cannot open the D3D12 correction target in D3D11";
        return false;
    }
    device1->Release();
    D3D11_QUERY_DESC query_description{};
    query_description.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateQuery(&query_description, &correction_copy_query))) {
        last_error_message = L"Cannot create the GPU correction copy fence";
        return false;
    }
    bgra_input.resize(static_cast<size_t>(description->Width) * description->Height * 4);
    return true;
}

bool __stdcall process(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                       const TemporalAnalysisPayload* temporal)
{
    if (!context || !texture || !temporal || !bridge_process) return false;
    const auto frame_start = TimingClock::now();
    last_timings = {};
    const uint32_t width = input_description.Width, height = input_description.Height;
    // CPU pixels are needed only for the first output channel-order calibration.
    if (needs_channel_reference) {
        context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
        for (uint32_t y = 0; y < height; ++y) {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            memcpy(bgra_input.data() + static_cast<size_t>(y) * width * 4, row,
                   static_cast<size_t>(width) * 4);
        }
        context->Unmap(staging, 0);
    }
    const auto input_end = TimingClock::now();
    last_timings.input_us = elapsed_us(frame_start, input_end);

    // NVOF supplies the NR temporal motion field. The low-resolution CPU
    // analyzer models translation only, so its reject-all/tile mask can
    // oscillate during camera rotation and must not gate the NR output.
    const auto policy = nr_temporal_policy(*temporal);
    char error[1024]{};
    if (!bridge_process(device, context, texture, needs_channel_reference ? bgra_input.data() : nullptr, static_cast<int>(width),
            static_cast<int>(height), temporal->nr_style, temporal->nr_preset,
            temporal->nr_intensity_percent / 100.0f, temporal->nr_tone_percent / 100.0f,
            temporal->nr_structure_percent / 100.0f, -1.0f,
            temporal->nr_automask ? 1 : 0, policy.reset_history ? 1 : 0,
            policy.use_motion_vectors ? 1 : 0,
            nullptr, 0, 0,
            error, static_cast<int>(sizeof(error)))) {
        set_last_error(error);
        return false;
    }
    const auto bridge_end = TimingClock::now();
    needs_channel_reference = false;
    if (staging) { staging->Release(); staging = nullptr; }
    std::vector<uint8_t>().swap(bgra_input);
    bridge_get_timings(&last_timings);
    last_timings.input_us += elapsed_us(frame_start, input_end);
    last_error_message.clear();

    context->CopyResource(texture, gpu_correction);
    context->End(correction_copy_query);
    context->Flush();
    const uint64_t copy_deadline = GetTickCount64() + 1000;
    for (;;) {
        const HRESULT copy_status = context->GetData(correction_copy_query, nullptr, 0, 0);
        if (copy_status == S_OK) break;
        if (copy_status != S_FALSE) {
            last_error_message = L"GPU correction copy query failed";
            return false;
        }
        if (GetTickCount64() >= copy_deadline) {
            last_error_message = L"Timed out waiting for the GPU correction copy";
            return false;
        }
        Sleep(0);
    }
    const auto frame_end = TimingClock::now();
    last_timings.correction_output_us = elapsed_us(bridge_end, frame_end);
    last_timings.total_us = elapsed_us(frame_start, frame_end);
    return true;
}

void __stdcall shutdown()
{
    if (bridge_shutdown) bridge_shutdown();
    if (staging) { staging->Release(); staging = nullptr; }
    if (gpu_correction) { gpu_correction->Release(); gpu_correction = nullptr; }
    if (correction_copy_query) { correction_copy_query->Release(); correction_copy_query = nullptr; }
    if (gpu_correction_handle) { CloseHandle(gpu_correction_handle); gpu_correction_handle = nullptr; }
    if (device) { device->Release(); device = nullptr; }
    if (bridge_module) { FreeLibrary(bridge_module); bridge_module = nullptr; }
    bridge_init = nullptr; bridge_create_correction_target = nullptr;
    bridge_process = nullptr; bridge_shutdown = nullptr;
    bridge_get_timings = nullptr;
    bgra_input.clear();
}

const NrAdapterApi api{
    sizeof(NrAdapterApi), nr_adapter_abi_version, L"DLSS 5 NR bridge",
    initialize, process, shutdown, last_error, get_timings
};
}

extern "C" __declspec(dllexport)
const NrAdapterApi* __stdcall DlssNrAdapterGetApi(uint32_t host_abi_version)
{
    return host_abi_version == nr_adapter_abi_version ? &api : nullptr;
}
