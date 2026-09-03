#include "nr_adapter_api.hpp"
#include "worker_protocol.hpp"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>
#include <cwchar>
#include <memory>
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
};

class PassthroughNrAdapter final : public INrAdapter {
public:
    bool initialize(ID3D11Device*, const D3D11_TEXTURE2D_DESC&) override { return true; }
    bool process(ID3D11DeviceContext*, ID3D11Texture2D*,
                 const TemporalAnalysisPayload&) override { return true; }
    LONG state_code() const noexcept override { return 1; }
    const wchar_t* name() const noexcept override { return L"Passthrough"; }
    const wchar_t* error_message() const noexcept override { return L""; }
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
            !api_->process || !api_->shutdown || !api_->last_error || !api_->display_name) {
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

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) return 2;
    const DWORD parent_id = static_cast<DWORD>(_wtoi(argv[1]));
    const HANDLE shared_texture = reinterpret_cast<HANDLE>(_wcstoui64(argv[2], nullptr, 10));
    const HANDLE shared_metadata = reinterpret_cast<HANDLE>(_wcstoui64(argv[3], nullptr, 10));
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
    HANDLE ready = reinterpret_cast<HANDLE>(_wcstoui64(argv[4], nullptr, 10));
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
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, &feature, &context))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 5;
    }
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device.As(&device1)) ||
        FAILED(device1->OpenSharedResource1(shared_texture, IID_PPV_ARGS(&texture)))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 6;
    }
    ComPtr<IDXGIKeyedMutex> mutex;
    if (FAILED(texture.As(&mutex))) {
        UnmapViewOfFile(temporal); CloseHandle(parent); return 7;
    }

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    LONG adapter_error = 0;
    const bool nr_enabled = InterlockedCompareExchange(&temporal->nr_enabled, 0, 0) != 0;
    std::unique_ptr<INrAdapter> adapter;
    if (nr_enabled)
        adapter = std::make_unique<ExternalNrAdapter>(adapter_path(true), adapter_error);
    else
        adapter = std::make_unique<PassthroughNrAdapter>();
    std::wstring adapter_error_message;
    if (!adapter->initialize(device.Get(), description)) {
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
    while (WaitForSingleObject(parent, 0) == WAIT_TIMEOUT) {
        InterlockedExchange64(&temporal->worker_heartbeat_ms, static_cast<LONG64>(GetTickCount64()));
        const HRESULT acquired = mutex->AcquireSync(1, 100);
        if (SUCCEEDED(acquired)) {
            TemporalAnalysisPayload payload{};
            LONG before{}, after{};
            do {
                before = InterlockedCompareExchange(&temporal->sequence, 0, 0);
                if (before & 1) { Sleep(0); continue; }
                MemoryBarrier();
                std::memcpy(&payload, &temporal->payload, sizeof(payload));
                MemoryBarrier();
                after = InterlockedCompareExchange(&temporal->sequence, 0, 0);
            } while (before != after || (after & 1));
            payload.nr_style = static_cast<uint16_t>(InterlockedCompareExchange(&temporal->nr_style, 0, 0));
            payload.nr_preset = static_cast<uint16_t>(InterlockedCompareExchange(&temporal->nr_preset, 0, 0));
            payload.nr_intensity_percent = static_cast<uint16_t>(InterlockedCompareExchange(&temporal->nr_intensity_percent, 0, 0));
            payload.nr_temporal = InterlockedCompareExchange(&temporal->nr_temporal, 0, 0) != 0;
            payload.nr_automask = 1;
            if ((payload.flags & temporal_valid) != 0 && payload.frame_sequence >= last_sequence) {
                if (adapter->process(context.Get(), texture.Get(), payload)) {
                    consecutive_failures = 0;
                    last_sequence = payload.frame_sequence;
                    InterlockedIncrement64(&temporal->worker_processed_frames);
                    if (!signaled) { SetEvent(ready); signaled = true; }
                } else if (++consecutive_failures >= 3 && adapter->state_code() == 2) {
                    adapter_error_message = adapter->error_message();
                    adapter = std::make_unique<PassthroughNrAdapter>();
                    adapter->initialize(device.Get(), description);
                    InterlockedExchange(&temporal->worker_adapter_state, adapter->state_code());
                    InterlockedExchange(&temporal->worker_adapter_error, 5);
                    wcsncpy_s(temporal->worker_adapter_name, adapter->name(), _TRUNCATE);
                    wcsncpy_s(temporal->worker_adapter_error_message, adapter_error_message.c_str(), _TRUNCATE);
                    consecutive_failures = 0;
                }
            }
            mutex->ReleaseSync(0);
        }
    }
    InterlockedExchange(&temporal->worker_adapter_state, 0);
    UnmapViewOfFile(temporal);
    CloseHandle(parent);
    return 0;
}
