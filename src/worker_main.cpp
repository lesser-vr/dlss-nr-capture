#include "nr_adapter_api.hpp"
#include "worker_output_policy.hpp"
#include "worker_protocol.hpp"
#include "shared_copy_completion.hpp"
#include "nr_composition.hpp"
#include <chrono>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstring>
#include <cwchar>
#include <memory>
#include <algorithm>
#include <string>
using Microsoft::WRL::ComPtr;

class INrAdapter {
public:
    virtual ~INrAdapter() = default;
    virtual bool initialize(ID3D11Device*, const D3D11_TEXTURE2D_DESC&) = 0;
    virtual bool process(ID3D11DeviceContext*, ID3D11Texture2D*,
                         const TemporalAnalysisPayload&) = 0;
    virtual LONG state_code() const noexcept = 0;
    virtual const wchar_t* name() const noexcept = 0;
    virtual const wchar_t* error_message() const noexcept = 0;
    virtual NrTimingSnapshot timings() const noexcept = 0;
};

class PassthroughNrAdapter final : public INrAdapter {
public:
    bool initialize(ID3D11Device*, const D3D11_TEXTURE2D_DESC&) override { return true; }
    bool process(ID3D11DeviceContext*, ID3D11Texture2D*,
                 const TemporalAnalysisPayload&) override { return true; }
    LONG state_code() const noexcept override { return 1; }
    const wchar_t* name() const noexcept override { return L"Passthrough"; }
    const wchar_t* error_message() const noexcept override { return L""; }
    NrTimingSnapshot timings() const noexcept override { return {}; }
};

class ExternalNrAdapter final : public INrAdapter {
public:
    ExternalNrAdapter(std::wstring path, LONG& error) : path_(std::move(path)), error_(error) {}
    ~ExternalNrAdapter() override {
        if (api_ && api_->shutdown) api_->shutdown();
        if (module_) FreeLibrary(module_);
    }
    bool initialize(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& description) override {
        if (GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) { error_ = 0; error_message_ = L"NR adapter DLL was not found"; return false; }
        module_ = LoadLibraryExW(path_.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) { error_ = 1; error_message_ = L"NR adapter DLL could not be loaded"; return false; }
        auto get_api = reinterpret_cast<NrAdapterGetApi>(GetProcAddress(module_, "DlssNrAdapterGetApi"));
        if (!get_api) { error_ = 2; error_message_ = L"NR adapter API export is missing"; return false; }
        api_ = get_api(nr_adapter_abi_version);
        if (!api_ || api_->byte_size < sizeof(NrAdapterApi) ||
            api_->abi_version != nr_adapter_abi_version || !api_->initialize ||
            !api_->process || !api_->shutdown || !api_->last_error || !api_->get_timings ||
            !api_->display_name) {
            error_ = 3; error_message_ = L"NR adapter ABI is incompatible"; return false;
        }
        if (!api_->initialize(device, &description)) {
            error_ = 4; error_message_ = api_->last_error(); return false;
        }
        initialized_ = true;
        error_ = 0;
        return true;
    }
    bool process(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                 const TemporalAnalysisPayload& payload) override {
        if (!initialized_ || !api_->process(context, texture, &payload)) {
            error_message_ = api_ && api_->last_error ? api_->last_error() : L"NR processing failed";
            return false;
        }
        error_message_.clear();
        return true;
    }
    LONG state_code() const noexcept override { return 2; }
    const wchar_t* name() const noexcept override {
        return api_ && api_->display_name ? api_->display_name : L"External";
    }
    const wchar_t* error_message() const noexcept override { return error_message_.c_str(); }
    NrTimingSnapshot timings() const noexcept override {
        NrTimingSnapshot result{};
        if (api_ && api_->get_timings) api_->get_timings(&result);
        return result;
    }
private:
    std::wstring path_;
    LONG& error_;
    HMODULE module_{};
    const NrAdapterApi* api_{};
    bool initialized_{};
    std::wstring error_message_;
};

std::wstring adapter_path(bool nr_enabled)
{
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring path = module;
    const size_t separator = path.find_last_of(L"\\/");
    path.resize(separator == std::wstring::npos ? 0 : separator + 1);
    path += nr_enabled ? L"dlss-nr-adapter-bridge.dll" : L"dlss-nr-adapter.dll";
    return path;
}

int run_worker(int argc, wchar_t** argv)
{
    const bool warp_test = (argc == 7 || argc == 8) && wcscmp(argv[6], L"--warp-test") == 0;
    if (argc != 6 && !warp_test) return 2;
    const DWORD parent_id = static_cast<DWORD>(_wtoi(argv[1]));
    const HANDLE shared_input = reinterpret_cast<HANDLE>(_wcstoui64(argv[2], nullptr, 10));
    const HANDLE shared_output = reinterpret_cast<HANDLE>(_wcstoui64(argv[3], nullptr, 10));
    const HANDLE shared_metadata = reinterpret_cast<HANDLE>(_wcstoui64(argv[4], nullptr, 10));
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
    HANDLE ready = reinterpret_cast<HANDLE>(_wcstoui64(argv[5], nullptr, 10));
    if (!parent || !ready) { if (parent) CloseHandle(parent); return 3; }

    auto* temporal = static_cast<WorkerTemporalState*>(
        MapViewOfFile(shared_metadata, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WorkerTemporalState)));
    if (!temporal || temporal->magic != nr_worker_protocol_magic ||
        temporal->version != nr_worker_protocol_version ||
        temporal->byte_size != sizeof(WorkerTemporalState)) {
        if (temporal) UnmapViewOfFile(temporal);
        CloseHandle(parent); return 4;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature{};
    if (FAILED(D3D11CreateDevice(nullptr, warp_test ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, &feature, &context))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 5;
    }
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> input_texture, output_texture;
    if (FAILED(device.As(&device1)) ||
        FAILED(device1->OpenSharedResource1(shared_input, IID_PPV_ARGS(&input_texture))) ||
        FAILED(device1->OpenSharedResource1(shared_output, IID_PPV_ARGS(&output_texture)))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 6;
    }
    ComPtr<IDXGIKeyedMutex> input_mutex, output_mutex;
    if (FAILED(input_texture.As(&input_mutex)) || FAILED(output_texture.As(&output_mutex))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 7;
    }

    D3D11_TEXTURE2D_DESC description{};
    output_texture->GetDesc(&description);
    auto processing_desc = description;
    processing_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> processing_texture;
    if (FAILED(device->CreateTexture2D(&processing_desc, nullptr, &processing_texture))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 8;
    }
    LONG adapter_error = 0;
    const bool nr_enabled = InterlockedCompareExchange(&temporal->nr_enabled, 0, 0) != 0;
    NrComposition composition;
    const auto model_description=composition.initialize(device.Get(),description,
        nr_enabled?static_cast<UINT>(InterlockedCompareExchange(&temporal->nr_scale_percent,0,0)):100,
        nr_enabled?static_cast<UINT>(std::clamp<LONG>(InterlockedCompareExchange(&temporal->nr_color_preserve,0,0),0,100)):0,
        nr_enabled && InterlockedCompareExchange(&temporal->nr_highlight_guard,0,0)!=0);
    std::unique_ptr<INrAdapter> adapter;
    if (nr_enabled)
        adapter = std::make_unique<ExternalNrAdapter>(warp_test && argc == 8 ? argv[7] : adapter_path(true), adapter_error);
    else
        adapter = std::make_unique<PassthroughNrAdapter>();
    std::wstring adapter_error_message;
    if (!adapter->initialize(device.Get(), model_description)) {
        adapter_error_message = adapter->error_message();
        adapter = std::make_unique<PassthroughNrAdapter>();
        if (!adapter->initialize(device.Get(), description)) {
            UnmapViewOfFile(temporal); CloseHandle(parent); return 8;
        }
    }
    InterlockedExchange(&temporal->worker_adapter_state, adapter->state_code());
    InterlockedExchange(&temporal->worker_adapter_error, adapter_error);
    wcsncpy_s(temporal->worker_adapter_name, adapter->name(), _TRUNCATE);
    wcsncpy_s(temporal->worker_adapter_error_message, adapter_error_message.c_str(), _TRUNCATE);
    InterlockedExchange64(&temporal->worker_heartbeat_ms, static_cast<LONG64>(GetTickCount64()));

    bool signaled = false;
    uint64_t last_sequence = 0;
    int consecutive_failures = 0;
    SharedCopyCompletion copy_completion;
    ComPtr<IDXGIDevice> memory_device;
    ComPtr<IDXGIAdapter> memory_base;
    ComPtr<IDXGIAdapter3> memory_adapter;
    if (SUCCEEDED(device.As(&memory_device)) &&
        SUCCEEDED(memory_device->GetAdapter(&memory_base))) memory_base.As(&memory_adapter);
    uint64_t last_memory_sample = 0;
    while (WaitForSingleObject(parent, 0) == WAIT_TIMEOUT) {
        const auto memory_now = GetTickCount64();
        if (memory_now - last_memory_sample >= 1000) {
            last_memory_sample = memory_now;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (memory_adapter && SUCCEEDED(memory_adapter->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                InterlockedExchange64(&temporal->gpu_memory_sample_ms, 0);
                InterlockedExchange64(&temporal->gpu_memory_usage, static_cast<LONG64>(info.CurrentUsage));
                InterlockedExchange64(&temporal->gpu_memory_budget, static_cast<LONG64>(info.Budget));
                InterlockedExchange64(&temporal->gpu_memory_sample_ms, static_cast<LONG64>(memory_now));
            } else InterlockedExchange64(&temporal->gpu_memory_sample_ms, 0);
        }
        InterlockedExchange64(&temporal->worker_heartbeat_ms, static_cast<LONG64>(GetTickCount64()));
        const HRESULT acquired = input_mutex->AcquireSync(1, 100);
        if (acquired == S_OK) {
            context->CopyResource(processing_texture.Get(), input_texture.Get());
            TemporalAnalysisPayload payload = temporal->payload;
            MemoryBarrier();
            if (FAILED(copy_completion.wait(context.Get()))) { UnmapViewOfFile(temporal); CloseHandle(parent); return 9; }
            input_mutex->ReleaseSync(0);
            bool output_completed = false;
            payload.nr_style = static_cast<uint16_t>(InterlockedCompareExchange(&temporal->nr_style, 0, 0));
            payload.nr_preset = static_cast<uint16_t>(InterlockedCompareExchange(&temporal->nr_preset, 0, 0));
            payload.nr_intensity_percent = static_cast<uint16_t>(InterlockedCompareExchange(&temporal->nr_intensity_percent, 0, 0));
            payload.nr_temporal = InterlockedCompareExchange(&temporal->nr_temporal, 0, 0) != 0;
            payload.nr_tone_percent=static_cast<uint16_t>(std::clamp<LONG>(InterlockedCompareExchange(&temporal->nr_tone_percent,0,0),0,100));
            payload.nr_structure_percent=static_cast<uint16_t>(std::clamp<LONG>(InterlockedCompareExchange(&temporal->nr_structure_percent,0,0),0,100));
            payload.nr_automask = 1;
            const LONG requested_passes=InterlockedCompareExchange(&temporal->nr_passes,0,0);
            payload.nr_passes=requested_passes>=1 && requested_passes<=3?requested_passes:1;
            if (worker_frame_eligible(payload, last_sequence)) {
                const auto processing_start=std::chrono::steady_clock::now();
                auto* model_input=adapter->state_code()==2?composition.prepare(context.Get(),processing_texture.Get()):processing_texture.Get();
                if (adapter->process(context.Get(), model_input, payload)) {
                    if(adapter->state_code()==2)composition.compose(context.Get(),processing_texture.Get());
                    if(FAILED(copy_completion.wait(context.Get()))){UnmapViewOfFile(temporal);CloseHandle(parent);return 9;}
                    consecutive_failures = 0;
                    last_sequence = payload.frame_sequence;
                    NrTimingSnapshot timing = adapter->timings();
                    timing.total_us=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-processing_start).count());
                    InterlockedExchange(&temporal->flow_mode,static_cast<LONG>(timing.flow_mode));
                    InterlockedExchange(&temporal->flow_error,static_cast<LONG>(timing.flow_error));
                    InterlockedExchange64(&temporal->nr_total_us, static_cast<LONG64>(timing.total_us));
                    InterlockedExchange64(&temporal->nr_input_us, static_cast<LONG64>(timing.input_us));
                    InterlockedExchange64(&temporal->nr_setup_us, static_cast<LONG64>(timing.setup_us));
                    InterlockedExchange64(&temporal->nr_optical_flow_us, static_cast<LONG64>(timing.optical_flow_us));
                    InterlockedExchange64(&temporal->nr_motion_vector_us, static_cast<LONG64>(timing.motion_vector_us));
                    InterlockedExchange64(&temporal->nr_gpu_prepare_us, static_cast<LONG64>(timing.gpu_prepare_us));
                    InterlockedExchange64(&temporal->nr_gpu_execute_us, static_cast<LONG64>(timing.gpu_execute_us));
                    InterlockedExchange64(&temporal->nr_bridge_output_us, static_cast<LONG64>(timing.bridge_output_us));
                    InterlockedExchange64(&temporal->nr_correction_output_us, static_cast<LONG64>(timing.correction_output_us));
                    InterlockedIncrement64(&temporal->worker_processed_frames);
                    output_completed = true;
                    if (!signaled) { SetEvent(ready); signaled = true; }
                } else if (++consecutive_failures >= 3 && adapter->state_code() == 2) {
                    adapter_error_message = adapter->error_message();
                    adapter = std::make_unique<PassthroughNrAdapter>();
                    adapter->initialize(device.Get(), description);
                    InterlockedExchange(&temporal->worker_adapter_state, adapter->state_code());
                    InterlockedExchange(&temporal->worker_adapter_error, 5);
                    InterlockedExchange(&temporal->flow_mode, 0);
                    wcsncpy_s(temporal->worker_adapter_name, adapter->name(), _TRUNCATE);
                    wcsncpy_s(temporal->worker_adapter_error_message, adapter_error_message.c_str(), _TRUNCATE);
                    consecutive_failures = 0;
                }
            }
            // Never wait for presentation: keep one completed output and drop
            // a newer result when the consumer is busy, without growing a queue.
            if (output_completed && output_mutex->AcquireSync(0, 0) == S_OK) {
                context->CopyResource(output_texture.Get(), processing_texture.Get());
                temporal->output_frame_sequence = payload.frame_sequence;
                temporal->output_processing_us = static_cast<uint64_t>(InterlockedCompareExchange64(&temporal->nr_total_us,0,0));
                temporal->output_completed_ms = GetTickCount64();
                InterlockedIncrement64(&temporal->worker_published_frames);
                MemoryBarrier();
                if (FAILED(copy_completion.wait(context.Get()))) { UnmapViewOfFile(temporal); CloseHandle(parent); return 9; }
                output_mutex->ReleaseSync(1);
            } else if (output_completed) InterlockedIncrement64(&temporal->worker_dropped_outputs);
        }
    }
    InterlockedExchange(&temporal->worker_adapter_state, 0);
    UnmapViewOfFile(temporal);
    CloseHandle(parent);
    return 0;
}

int wmain(int argc,wchar_t** argv) {
    try {return run_worker(argc,argv);}
    catch(const std::exception&){return 10;}
}
