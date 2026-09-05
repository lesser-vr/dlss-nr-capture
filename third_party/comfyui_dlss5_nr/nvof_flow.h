#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <string>
#include <vector>

// Coarse S10.5 optical-flow field produced by NVIDIA Optical Flow (NVOFA).
// Each cell stores signed X,Y displacement in units of 1/32 pixel.
struct NvofFlowFrame {
    bool has_flow = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t grid = 0;
    std::vector<int16_t> xy;
    // Borrowed until the next prepare/release; consumer must finish its copy.
    ID3D11Texture2D* gpu_texture = nullptr;
};

// Prepare flow from a BGRA texture on the caller''s D3D11 device.
// - reset=true starts a new sequence; this frame only primes the previous-frame slot.
// - on the first frame out.has_flow is false and the caller should supply zero MVs.
// - subsequent frames return current->previous flow, matching the DLSS reprojection direction.
bool NvofPrepareFrame(
    IDXGIAdapter1* adapter,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    uint32_t width,
    uint32_t height,
    bool reset,
    NvofFlowFrame& out,
    std::string& error,
    bool gpu_only = false);

// Releases the active OFA session and its D3D11 textures, but keeps no contract state.
void NvofReleaseSession();
// Read the already computed field without advancing the input pair a second time.
bool NvofReadCurrentFlow(NvofFlowFrame& out, std::string& error);

// Full teardown including nvofapi64.dll.
void NvofShutdown();

// Lightweight driver check used by Runtime Info. Does not create an OFA session.
bool NvofDriverApiAvailable();

// Fixed settings used by this release.
uint32_t NvofGridSize();
uint32_t NvofPerfLevel();
