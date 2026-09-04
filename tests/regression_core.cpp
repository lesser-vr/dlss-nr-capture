#include "frame_processor.hpp"
#include "nr_adapter_api.hpp"
#include "nr_notification.hpp"
#include "nr_temporal_policy.hpp"
#include "nr_speed_policy.hpp"
#include "overlay_style.hpp"
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
    const auto info_style = overlay_palette(OverlayMessageStyle::information);
    const auto error_style = overlay_palette(OverlayMessageStyle::error);
    check(info_style.background_rgb == 0 && info_style.text_rgb == 0xFFFF00 &&
          info_style.background_opacity == 1.0f, "general overlay uses black and yellow");
    check(error_style.background_rgb == 0xA81919 && error_style.text_rgb == 0xFFFFFF &&
          error_style.background_opacity == 0.88f, "error overlay preserves TOO SLOW style");
    NrWarmupGate warmup;
    check(!warmup.accept_sample(10000), "first NR result excluded from timing");
    check(!warmup.accept_sample(11999), "startup samples excluded for two seconds");
    check(warmup.accept_sample(12000), "post-warmup samples accepted");
    check(warmup.accept_sample(12001), "warmup completes only once");
    warmup = {};
    check(!warmup.accept_sample(50000), "FPS change restarts warmup");
    check(!warmup.accept_sample(49999), "backward time cannot finish warmup");
    check(warmup.accept_sample(52000), "new FPS warmup completes");
    const auto speed30 = nr_speed_policy(30, 1);
    check(speed30.enable_us == 30000 && speed30.slow_us == 50000 &&
          speed30.fast_samples == 15 && speed30.slow_samples == 30, "30 Hz NR budget");
    const auto speed60 = nr_speed_policy(60, 1);
    check(speed60.enable_us == 15000 && speed60.slow_us == 25000 &&
          speed60.fast_samples == 30 && speed60.slow_samples == 60, "60 Hz preserves tested policy");
    const auto fractional = nr_speed_policy(60000, 1001);
    check(fractional.enable_us == 15015 && fractional.slow_us == 25025 &&
          fractional.fast_samples == 30 && fractional.slow_samples == 60, "59.94 Hz rational budget");
    const auto speed120 = nr_speed_policy(120, 1);
    check(speed120.enable_us == 7500 && speed120.slow_us == 12500 &&
          speed120.fast_samples == 60 && speed120.slow_samples == 120, "120 Hz NR budget");
    check(nr_speed_policy(0, 1).enable_us == 15000 &&
          nr_speed_policy(60, 0).slow_us == 25000 &&
          nr_speed_policy(1, 2).fast_samples == 30 &&
          nr_speed_policy(0xffffffffu, 1).slow_samples == 60, "invalid FPS falls back safely");
    check(nr_speed_policy(0xffffffffu, 0xffffffffu).enable_us == 900000,
          "large rational FPS does not overflow");
    TemporalAnalysisPayload temporal_frame{};
    temporal_frame.nr_temporal = 1;
    check(nr_temporal_policy(temporal_frame).use_motion_vectors, "normal frame keeps temporal enabled");
    temporal_frame.flags = temporal_scene_cut;
    check(nr_temporal_policy(temporal_frame).reset_history, "transition requests history reset");
    check(nr_temporal_policy(temporal_frame).use_motion_vectors, "reset preserves temporal feature mode");
    temporal_frame.nr_temporal = 0;
    check(!nr_temporal_policy(temporal_frame).use_motion_vectors, "reset respects user temporal-off setting");
    temporal_frame.flags = temporal_reject_all;
    check(!nr_temporal_policy(temporal_frame).reset_history, "translation rejection does not reset NR");
    check(nr_notification_opacity(0) == 1.0f, "NR notification starts opaque");
    check(nr_notification_opacity(1000) == 1.0f, "NR notification holds for one second");
    check(nr_notification_opacity(1250) == 0.5f, "NR notification fade midpoint");
    check(nr_notification_opacity(1500) == 0.0f, "NR notification expires after 1.5 seconds");
    check(nr_notification_opacity(5000) == 0.0f, "NR notification stays expired");
    check(nr_worker_protocol_version == 6, "worker protocol version changed unexpectedly");
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
    check((motion->temporal_state().flags & temporal_valid) == 0,
          "history reset clears published metadata");
    for (int restart = 0; restart < 3; ++restart) {
        auto old_frame = solid(64, 48, 100, 9000);
        motion->process(old_frame);
        motion->process(old_frame);
        motion->reset_history();
        auto new_first = solid(64, 48, 100, 0);
        motion->process(new_first);
        check(motion->temporal_state().frame_sequence == 0 &&
              (motion->temporal_state().flags & temporal_valid) == 0,
              "capture restart cannot publish old high sequence during warmup");
        new_first.sequence = 1;
        motion->process(new_first);
        check(motion->temporal_state().frame_sequence == 1 &&
              (motion->temporal_state().flags & temporal_valid) != 0,
              "new session becomes valid without catching up to old sequence");
    }

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
                check(api && api->initialize && api->process && api->shutdown && api->last_error &&
                      api->get_timings, "adapter callbacks");
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
            check(GetProcAddress(bridge, "dlss5nr_create_correction_target") != nullptr,
                  "bridge shared correction target export");
            check(GetProcAddress(bridge, "dlss5nr_shutdown") != nullptr, "bridge shutdown export");
            check(GetProcAddress(bridge, "dlss5nr_get_timings") != nullptr, "bridge timings export");
            FreeLibrary(bridge);
        }
    } else check(false, "runtime bridge path argument missing");

    if (failures) { std::cerr << failures << " regression check(s) failed\n"; return 1; }
    std::cout << "core regression checks passed\n";
    return 0;
}
