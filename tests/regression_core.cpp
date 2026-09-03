#include "frame_processor.hpp"
#include "nr_adapter_api.hpp"
#include <windows.h>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
VideoFrame solid(uint32_t width, uint32_t height, uint8_t value, uint64_t sequence) {
    VideoFrame frame{}; frame.width = width; frame.height = height; frame.sequence = sequence;
    frame.bgra.resize(static_cast<size_t>(width) * height * 4, value);
    for (size_t i = 3; i < frame.bgra.size(); i += 4) frame.bgra[i] = 255;
    return frame;
}
}

int wmain(int argc, wchar_t** argv) {
    check(nr_worker_protocol_version == 5, "worker protocol version changed unexpectedly");
    WorkerTemporalState state{};
    check(state.magic == nr_worker_protocol_magic, "protocol magic default");
    check(state.byte_size == sizeof(WorkerTemporalState), "protocol byte size default");
    check(state.nr_style == 1 && state.nr_preset == 3 && state.nr_intensity_percent == 100,
          "NR defaults");

    auto pass = create_passthrough_processor();
    auto first = solid(64, 48, 32, 7);
    check(pass->process(first), "passthrough accepts frame");
    const auto pass_payload = pass->temporal_state();
    check(pass_payload.frame_sequence == 7, "passthrough sequence");
    check((pass_payload.flags & (temporal_valid | temporal_reject_all)) ==
          (temporal_valid | temporal_reject_all), "passthrough rejects temporal history");

    auto motion = create_motion_analysis_processor();
    auto dark1 = solid(64, 48, 0, 1);
    auto dark2 = solid(64, 48, 0, 2);
    auto bright = solid(64, 48, 255, 3);
    check(motion->process(dark1) && motion->process(dark2), "motion warmup/stable frames");
    auto stable = motion->temporal_state();
    check((stable.flags & temporal_valid) != 0, "motion payload becomes valid");
    check(stable.frame_sequence == 2, "motion sequence");
    check(motion->process(bright), "motion scene-cut frame");
    auto cut = motion->temporal_state();
    check((cut.flags & temporal_scene_cut) != 0, "scene cut detected");
    check((cut.flags & temporal_reject_all) != 0, "scene cut rejects history");
    motion->reset_history();
    check(motion->diagnostics() == "warming up", "history reset");

    if (argc >= 2) {
        HMODULE module = LoadLibraryExW(argv[1], nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "sample adapter loads");
        if (module) {
            auto get_api = reinterpret_cast<NrAdapterGetApi>(GetProcAddress(module, "DlssNrAdapterGetApi"));
            check(get_api != nullptr, "adapter API export");
            if (get_api) {
                check(get_api(nr_adapter_abi_version + 1) == nullptr, "adapter rejects wrong ABI");
                const NrAdapterApi* api = get_api(nr_adapter_abi_version);
                check(api && api->byte_size >= sizeof(NrAdapterApi), "adapter API size");
                check(api && api->initialize && api->process && api->shutdown && api->last_error, "adapter callbacks");
                if (api && api->last_error) check(std::wstring(api->last_error()).empty(), "adapter error default");
            }
            FreeLibrary(module);
        }
    } else check(false, "sample adapter path argument missing");

    if (argc >= 3) {
        HMODULE bridge = LoadLibraryExW(argv[2], nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(bridge != nullptr, "runtime bridge loads without proprietary runtime");
        if (bridge) {
            check(GetProcAddress(bridge, "dlss5nr_init") != nullptr, "bridge init export");
            check(GetProcAddress(bridge, "dlss5nr_process") != nullptr, "bridge process export");
            check(GetProcAddress(bridge, "dlss5nr_shutdown") != nullptr, "bridge shutdown export");
            FreeLibrary(bridge);
        }
    } else check(false, "runtime bridge path argument missing");

    if (failures) { std::cerr << failures << " regression check(s) failed\n"; return 1; }
    std::cout << "core regression checks passed\n";
    return 0;
}
