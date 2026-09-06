#include "nr_adapter_api.hpp"
namespace {
bool __stdcall initialize(ID3D11Device*, const D3D11_TEXTURE2D_DESC*) { return true; }
bool __stdcall process(ID3D11DeviceContext*, ID3D11Texture2D*, const TemporalAnalysisPayload* p) {
    // Only fail if the two-pass request actually arrived through the worker.
    return !p || p->nr_passes != 2;
}
void __stdcall shutdown() {}
const wchar_t* __stdcall error() { return L"Injected two-pass adapter failure"; }
void __stdcall timings(NrTimingSnapshot* p) { if(p) *p={}; }
const NrAdapterApi api{sizeof(NrAdapterApi),nr_adapter_abi_version,L"Failing test adapter",
    initialize,process,shutdown,error,timings};
}
extern "C" __declspec(dllexport) const NrAdapterApi* __stdcall DlssNrAdapterGetApi(uint32_t v) {
    return v==nr_adapter_abi_version?&api:nullptr;
}
