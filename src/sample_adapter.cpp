#include "nr_adapter_api.hpp"

namespace {
bool __stdcall initialize(ID3D11Device*, const D3D11_TEXTURE2D_DESC*) { return true; }
bool __stdcall process(ID3D11DeviceContext*, ID3D11Texture2D*,
                       const TemporalAnalysisPayload*) { return true; }
void __stdcall shutdown() {}
const wchar_t* __stdcall last_error() { return L""; }
const NrAdapterApi api{
    sizeof(NrAdapterApi), nr_adapter_abi_version, L"Sample passthrough",
    initialize, process, shutdown, last_error
};
}

extern "C" __declspec(dllexport)
const NrAdapterApi* __stdcall DlssNrAdapterGetApi(uint32_t host_abi_version)
{
    return host_abi_version == nr_adapter_abi_version ? &api : nullptr;
}
