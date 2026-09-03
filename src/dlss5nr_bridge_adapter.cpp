#include "nr_adapter_api.hpp"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {
using BridgeInit = int (__cdecl*)(int, const wchar_t*, char*, int);
using BridgeProcess = int (__cdecl*)(const float*, float*, int, int, int, int,
                                     float, float, float, float, int, int, int, char*, int);
using BridgeShutdown = void (__cdecl*)();

HMODULE bridge_module{};
BridgeInit bridge_init{};
BridgeProcess bridge_process{};
BridgeShutdown bridge_shutdown{};
ID3D11Device* device{};
ID3D11Texture2D* staging{};
D3D11_TEXTURE2D_DESC input_description{};
std::vector<float> rgb_input;
std::vector<float> rgb_output;
std::wstring last_error_message;

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
    bridge_shutdown = reinterpret_cast<BridgeShutdown>(GetProcAddress(bridge_module, "dlss5nr_shutdown"));
    if (!bridge_init || !bridge_process || !bridge_shutdown) {
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
    const size_t values = static_cast<size_t>(description->Width) * description->Height * 3;
    rgb_input.resize(values);
    rgb_output.resize(values);
    return true;
}

bool __stdcall process(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                       const TemporalAnalysisPayload* temporal)
{
    if (!context || !texture || !temporal || !staging || !bridge_process) return false;
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint32_t width = input_description.Width, height = input_description.Height;
    for (uint32_t y = 0; y < height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        float* target = rgb_input.data() + static_cast<size_t>(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            target[x * 3] = row[x * 4 + 2] / 255.0f;
            target[x * 3 + 1] = row[x * 4 + 1] / 255.0f;
            target[x * 3 + 2] = row[x * 4] / 255.0f;
        }
    }
    context->Unmap(staging, 0);

    const bool reject_history = (temporal->flags & (temporal_scene_cut | temporal_reject_all)) != 0;
    char error[1024]{};
    if (!bridge_process(rgb_input.data(), rgb_output.data(), static_cast<int>(width),
            static_cast<int>(height), temporal->nr_style, temporal->nr_preset,
            temporal->nr_intensity_percent / 100.0f, 1.0f, 1.0f, -1.0f,
            temporal->nr_automask ? 1 : 0, reject_history ? 1 : 0,
            (!reject_history && temporal->nr_temporal) ? 1 : 0,
            error, static_cast<int>(sizeof(error)))) {
        set_last_error(error);
        return false;
    }
    last_error_message.clear();

    double direct_error = 0.0, swapped_error = 0.0;
    for (size_t i = 0; i < rgb_output.size(); i += 192) {
        direct_error += std::abs(rgb_output[i] - rgb_input[i]) +
                        std::abs(rgb_output[i + 2] - rgb_input[i + 2]);
        swapped_error += std::abs(rgb_output[i + 2] - rgb_input[i]) +
                         std::abs(rgb_output[i] - rgb_input[i + 2]);
    }
    const bool swap_channels = swapped_error < direct_error;

    if (FAILED(context->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped))) return false;
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        const float* source = rgb_output.data() + static_cast<size_t>(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            const float r = source[x * 3 + (swap_channels ? 2 : 0)];
            const float g = source[x * 3 + 1];
            const float b = source[x * 3 + (swap_channels ? 0 : 2)];
            row[x * 4] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[x * 4 + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[x * 4 + 2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            row[x * 4 + 3] = 255;
        }
    }
    context->Unmap(staging, 0);
    context->CopyResource(texture, staging);
    return true;
}

void __stdcall shutdown()
{
    if (bridge_shutdown) bridge_shutdown();
    if (staging) { staging->Release(); staging = nullptr; }
    if (device) { device->Release(); device = nullptr; }
    if (bridge_module) { FreeLibrary(bridge_module); bridge_module = nullptr; }
    bridge_init = nullptr; bridge_process = nullptr; bridge_shutdown = nullptr;
    rgb_input.clear(); rgb_output.clear();
}

const NrAdapterApi api{
    sizeof(NrAdapterApi), nr_adapter_abi_version, L"DLSS 5 NR bridge",
    initialize, process, shutdown, last_error
};
}

extern "C" __declspec(dllexport)
const NrAdapterApi* __stdcall DlssNrAdapterGetApi(uint32_t host_abi_version)
{
    return host_abi_version == nr_adapter_abi_version ? &api : nullptr;
}
