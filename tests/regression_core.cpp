#include "frame_processor.hpp"
#include "av_sync.hpp"
#include "capture_color.hpp"
#include "capture_power.hpp"
#include "audio_output_checks.hpp"
#include "audio_health.hpp"
#include "device_refresh.hpp"
#include "event_log.hpp"
#include "quality_capture.hpp"
#include <filesystem>
#include "diagnostics_report.hpp"
#include "frame_rate_meter.hpp"
#include "benchmark_stats.hpp"
#include "worker_health.hpp"
#include "capture_health.hpp"
#include "quality_metrics.hpp"
#include "worker_job.hpp"
#include "nr_adapter_api.hpp"
#include "nr_notification.hpp"
#include "nr_temporal_policy.hpp"
#include "nr_speed_policy.hpp"
#include "overlay_style.hpp"
#include "worker_output_policy.hpp"
#include "gpu_memory_policy.hpp"
#include <windows.h>
#include <iostream>
#include <string>

namespace {
int failures = 0;
EXECUTION_STATE power_flags{};
int power_calls{};
bool power_fail{};
EXECUTION_STATE WINAPI fake_power(EXECUTION_STATE flags) {
    ++power_calls; power_flags = flags; return power_fail ? 0 : ES_CONTINUOUS;
}
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
    check(!gpu_memory_pressure(100,0,1000,1000), "unknown budget is not pressure");
    check(!gpu_memory_pressure(89,100,1000,1000), "below memory threshold");
    check(gpu_memory_pressure(90,100,1000,1000), "90 percent memory boundary");
    check(gpu_memory_pressure(110,100,1000,4000), "over budget and last fresh sample");
    check(!gpu_memory_pressure(110,100,1000,4001), "stale memory cannot trigger warning");
    check(!gpu_memory_pressure(110,100,0,1000), "failed memory query cannot trigger warning");
    check(!gpu_memory_pressure(110,100,1000,999), "memory clock reversal guarded");
    {
        auto c=CaptureColor::from(2,1,0,0);check(c.bt601 && c.full && !c.assumed,"601 full metadata");
        check(CaptureColor::from(0,0,0,0).assumed,"missing color metadata explicit default");
        check(CaptureColor::from(1,2,15,0).unsupported,"PQ rejected without tone mapping");
        check(CaptureColor::from(1,2,16,0).hdr,"HLG identified");
        check(CaptureColor::from(4,2,0,9).unsupported,"2020 not silently treated as 709");
        AvSyncClock clock;
        check(clock.delay(10000000)==0,"AV no video clock fallback");
        for(int i=0;i<100;i++)clock.observe(i*166666,10000000+i*166666,10200000+i*166666);
        check(clock.delay(10000000+99*166666)>=17 && clock.delay(10000000+99*166666)<=23,"AV converges to app video latency");
        check(clock.delay(40000000)==0,"AV stale clock expires");
        clock.reset();check(clock.delay(40000000)==0,"AV mode change resets clock");
        clock.observe(0,40000000,40000000);check(clock.delay(40000000)==0,"AV new timestamp epoch");
        // Deterministic scheduling tests, not physical audiovisual measurements.
        int64_t stream=0; uint64_t arrival=50000000;
        for(int fps : {30,60,30}) for(int latency_ms : {0,20,50,0}) {
            clock.reset();
            for(int i=0;i<120;++i){
                stream+=10000000/fps;arrival+=10000000/fps;
                clock.observe(stream,arrival,arrival+latency_ms*10000);
            }
            check(std::abs(int(clock.delay(arrival))-latency_ms)<=3,"AV FPS and NR latency transition settles within 3 ms");
        }
        clock.reset();
        for(int i=0;i<120;++i){arrival+=166666;clock.observe(i*166666,arrival,arrival+4000000);}
        check(clock.delay(arrival)<=200 && clock.delay(arrival)>=197,"AV excessive delay bounded at 200 ms");
        clock.reset();clock.observe(0,arrival,arrival-1);
        check(clock.delay(arrival)==0,"AV invalid presentation timestamp ignored");
    }
    {
        CapturePowerRequest request(fake_power);
        check(request.update(true) && request.active(), "power request activates");
        check(power_flags == (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED), "power request flags");
        request.update(true);
        check(power_calls == 1, "unchanged power request does not reset idle timers");
        power_fail = true;
        check(!request.update(false) && request.active(), "failed release remains tracked");
        power_fail = false;
        check(request.update(false) && !request.active() && power_flags == ES_CONTINUOUS, "power request releases");
        request.update(true);
    }
    check(power_flags == ES_CONTINUOUS, "power request destructor releases");
    check(capture_requires_awake(true,true,false,1000,999), "live capture prevents sleep");
    check(!capture_requires_awake(false,true,false,1000,999), "disabled power preference");
    check(!capture_requires_awake(true,false,false,1000,999), "stopped capture allows sleep");
    check(!capture_requires_awake(true,true,true,1000,999), "failed capture allows sleep");
    check(!capture_requires_awake(true,true,false,3000,1000), "stalled capture releases");
    check(!capture_requires_awake(true,true,false,1000,0), "no first frame allows sleep");
    check_audio_output(check);
    {
        const uint8_t pixels[] = {1,2,3,255, 4,5,6,255, 7,8,9,255, 10,11,12,255};
        const auto roi = quality_proxy(pixels, 2, 2, 8, 1, 1, 1, 1, 1, 1);
        check(roi == std::vector<uint8_t>({12,11,10}), "quality ROI uses exact source location and RGB order");
    }
    {
        wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
        const auto folder = std::filesystem::path(temp) / (L"dlss-log-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(folder);
        EventLog log; log.set_path((folder / L"events.log").wstring(), 100);
        check(log.append(L"First recovery event"), "event log writes");
        check(log.append(L"Second recovery event that rotates the log"), "event log rotates");
        check(std::filesystem::exists(folder / L"events.log.previous"), "event log retains prior segment");
        std::filesystem::remove_all(folder);
        log.set_path((folder / L"missing" / L"events.log").wstring());
        check(!log.append(L"error"), "event log failure is nonfatal");
    }
    check(!audio_retry_due(4999, 0, true), "audio recovery backoff");
    check(audio_retry_due(5000, 0, true), "audio failure triggers recovery");
    check(!audio_retry_due(10000, 0, false), "healthy audio does not reconnect");
    check(!audio_retry_due(100, 200, true), "audio recovery guards clock reversal");
    check(audio_endpoint_matches(L"endpoint-A", L"endpoint-A"), "audio exact endpoint recovery");
    check(!audio_endpoint_matches(L"endpoint-A", L"endpoint-B"), "audio refuses other endpoints");
    check(!audio_endpoint_matches(L"", L""), "audio requires a nonempty identity");
    struct SavedAudioTestDevice { std::wstring id, name; };
    const std::vector<SavedAudioTestDevice> audio_devices = {
        {L"A", L"Capture"}, {L"B", L"Capture"}, {L"C", L"Unique"}
    };
    check(audio_restore_index(audio_devices, L"B", L"Capture") == 1, "saved ID disambiguates audio names");
    check(audio_restore_index(audio_devices, L"B", L"Old name") == 1, "saved audio ID survives rename");
    check(audio_restore_index(audio_devices, L"Missing", L"Unique") == 3, "saved audio ID never falls back to name");
    check(audio_restore_index(audio_devices, L"", L"Unique") == 2, "unique legacy audio name migrates");
    check(audio_restore_index(audio_devices, L"", L"Capture") == 3, "ambiguous legacy audio waits for selection");
    check(audio_restore_index(audio_devices, L"", L"") == 3, "audio Off does not choose an endpoint");
    const auto audio_key = [](const auto& device) { return device.id; };
    std::vector<SavedAudioTestDevice> refreshed = {audio_devices[2], audio_devices[1], audio_devices[0]};
    check(remap_selected_device(audio_devices, refreshed, 0, audio_key) == 2,
          "device refresh preserves selected identity when enumeration order changes");
    refreshed = {audio_devices[2]};
    check(remap_selected_device(audio_devices, refreshed, 0, audio_key) == 1 &&
          refreshed[1].id == L"A", "missing selected video retained for live session and reconnect");
    check(!remap_selected_device(audio_devices, refreshed, std::nullopt, audio_key),
          "device refresh never automatically selects an input");
    check(!capture_retry_due(4999, 0, 0, 0, true), "capture recovery backoff");
    check(capture_retry_due(5000, 0, 0, 0, true), "capture error retries without waiting for frames");
    check(!capture_retry_due(7999, 0, 0, 0, false), "capture startup grace");
    check(capture_retry_due(8000, 0, 0, 0, false), "capture no-frame timeout");
    check(!capture_retry_due(20000, 0, 19900, 0, false), "no-signal image with arriving frames is healthy");
    check(capture_retry_due(20000, 0, 10000, 0, false), "capture stalled frames trigger recovery");
    check(!capture_retry_due(100, 0, 0, 200, true), "capture retry guards clock reversal");
    check(!capture_input_interrupted(7999, 0, 0, false), "input banner honors startup grace");
    check(capture_input_interrupted(8000, 0, 0, false), "input banner after startup timeout");
    check(!capture_input_interrupted(2999, 0, 1000, false), "brief input gap has no banner");
    check(capture_input_interrupted(3000, 0, 1000, false), "stalled input gets banner");
    check(!capture_input_interrupted(3000, 0, 2999, false), "fresh frames clear input banner");
    check(capture_input_interrupted(1000, 0, 999, true), "capture error immediately gets banner");
    check(!capture_input_interrupted(1000, 0, 2000, false), "input banner guards clock reversal");
    check(!worker_needs_restart(4999, 0, false, false, 0), "watchdog restart backoff");
    check(worker_needs_restart(5000, 0, false, false, 0), "watchdog retries failed process creation");
    check(worker_needs_restart(5000, 0, true, true, 0), "watchdog recovers exited worker without frames");
    check(!worker_needs_restart(29999, 0, true, false, 0), "watchdog allows cold initialization");
    check(worker_needs_restart(30000, 0, true, false, 0), "watchdog detects missing initial heartbeat");
    check(!worker_needs_restart(10000, 0, true, false, 8000), "watchdog heartbeat boundary");
    check(worker_needs_restart(10001, 0, true, false, 8000), "watchdog detects stale heartbeat");
    check(!worker_needs_restart(10000, 0, true, false, 9900), "idle healthy worker is not restarted");
    check(!worker_needs_restart(100, 200, true, true, 0), "watchdog guards clock reversal");
    check(quality_mae({0, 20, 255}, {0, 20, 255}) == 0, "identical quality proxies have zero difference");
    check(quality_mae({0, 0, 0}, {30, 30, 30}) == 30, "quality MAE scale is 0-255");
    check(quality_residual_change({50}, {30}, {40}, {20}) == 0, "constant enhancement tracks source without residual flicker");
    check(quality_residual_change({70}, {30}, {40}, {20}) == 20, "enhancement jump changes temporal residual");
    const auto empty_stats = benchmark_stats({});
    check(empty_stats.mean == 0 && empty_stats.p99 == 0, "empty benchmark statistics");
    std::vector<uint64_t> ordered_samples;
    for (uint64_t i = 100; i > 0; --i) ordered_samples.push_back(i);
    const auto stats = benchmark_stats(ordered_samples);
    check(stats.mean == 50.5 && stats.p95 == 95 && stats.p99 == 99 && stats.maximum == 100,
          "benchmark statistics use nearest-rank percentiles");
    if (argc == 2 && std::wstring(argv[1]) == L"--job-probe") {
        Sleep(30000); // Bounded fallback if the lifetime guard fails.
        return 0;
    }
    {
        WorkerJob job;
        job.create();
        DWORD flags{};
        check(GetHandleInformation(job.get(), &flags) && !(flags & HANDLE_FLAG_INHERIT),
              "worker job handle cannot be inherited by child");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        check(QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
              &limits, sizeof(limits), nullptr) &&
              (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
              "worker job kills children on last handle close");
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --job-probe";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        const bool created = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &child) != FALSE;
        check(created, "create lifetime test child suspended");
        if (created) {
            try {
                job.assign(child.hProcess);
                BOOL assigned{};
                check(IsProcessInJob(child.hProcess, job.get(), &assigned) && assigned,
                      "child is assigned before execution");
                check(ResumeThread(child.hThread) != static_cast<DWORD>(-1), "resume lifetime test child");
                job.reset();
                check(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
                      "closing worker job terminates live child");
            } catch (...) { check(false, "worker job lifecycle threw"); }
            // Failure cleanup is restricted to the child created by this test.
            TerminateProcess(child.hProcess, 0);
            WaitForSingleObject(child.hProcess, 5000);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
    }
    FrameRateMeter rate;
    check(!rate.observe(0, 0), "FPS first sample seeds baseline");
    check(!rate.observe(999, 59), "FPS waits for one second");
    check(rate.observe(1000, 60) && rate.fps() == 60.0, "FPS measures counter delta");
    check(rate.observe(3000, 120) && rate.fps() == 30.0, "FPS uses actual elapsed time");
    check(rate.observe(4000, 120) && rate.fps() == 0.0, "FPS zero progress is zero");
    check(!rate.observe(4100, 0) && rate.fps() == 0.0, "FPS counter restart resets baseline");
    check(rate.observe(5100, 30) && rate.fps() == 30.0, "FPS recovers after restart");
    check(!rate.observe(100, 31) && rate.fps() == 0.0, "FPS clock reversal resets baseline");
    DiagnosticsReport report;
    report.add(L"Device", L"캡쳐 장치");
    report.add(L"NR total us", uint64_t{1234567890123});
    report.add(L"Worker", std::wstring(2048, L'x'));
    check(report.text().find(L"Device: 캡쳐 장치\r\n") != std::wstring::npos,
          "diagnostics preserves Unicode and Windows newlines");
    check(report.text().find(L"NR total us: 1234567890123\r\n") != std::wstring::npos,
          "diagnostics preserves full precision counters");
    check(report.text().find(std::wstring(2048, L'x')) != std::wstring::npos,
          "diagnostics does not truncate worker status");
    for (const uint32_t fps : {30u, 60u, 120u}) {
        NrSpeedMonitor monitor;
        monitor.reset(fps, 1);
        const auto limits = nr_speed_policy(fps, 1);
        const uint64_t quick = limits.enable_us / 2;
        uint64_t clock_ms = 2100;
        monitor.observe(100, 5000000);
        monitor.observe(2099, 5000000);
        check(monitor.preparing() && monitor.average_us() == 0,
              "startup cost never enters speed average");
        for (uint32_t i = 1; i < limits.fast_samples; ++i)
            monitor.observe(clock_ms++, quick);
        check(monitor.preparing(), "activation waits for qualifying result count");
        monitor.observe(clock_ms++, quick);
        check(monitor.fast() && !monitor.slow(), "stable output activates NR");
        monitor.observe(clock_ms++, limits.slow_us * 3);
        check(monitor.fast() && !monitor.slow(), "isolated latency spike cannot disable NR");
        for (uint32_t i = 0; i < limits.slow_samples * 2 + 100; ++i)
            monitor.observe(clock_ms++, limits.slow_us * 2);
        check(monitor.slow() && !monitor.fast(), "sustained slowness still raises warning");
        for (uint32_t i = 0; i < limits.fast_samples * 2 + 100; ++i)
            monitor.observe(clock_ms++, quick);
        check(monitor.fast() && !monitor.slow(), "fast recovery clears slow warning");
        monitor.reset(fps == 30 ? 60 : 30, 1);
        check(monitor.preparing() && monitor.average_us() == 0,
              "FPS change clears prior speed state");
        monitor.observe(clock_ms++, 0);
        check(monitor.average_us() == 0 && monitor.preparing(),
              "missing timing cannot qualify output");
        monitor.observe(clock_ms++, 5000000);
        check(monitor.average_us() == 0, "restarted worker receives a new warmup");
    }
    NrSpeedMonitor cold_slow;
    cold_slow.reset(30, 1);
    cold_slow.observe(0, 100000);
    for (uint64_t i = 0; i < 30; ++i) cold_slow.observe(2000 + i, 100000);
    check(cold_slow.slow() && !cold_slow.preparing(),
          "genuinely slow startup eventually exits preparing with warning");
    TemporalAnalysisPayload input{};
    input.frame_sequence = 100;
    check(!worker_frame_eligible(input, 0), "invalid analysis cannot be processed");
    input.flags = temporal_valid;
    check(!worker_frame_eligible(input, 101), "older sequence is skipped");
    check(worker_frame_eligible(input, 100), "equal sequence preserves existing worker behavior");
    check(worker_frame_eligible(input, 99), "new sequence can be processed");
    check(worker_output_release_key(false) == 0, "skipped or failed processing does not publish output");
    check(worker_output_release_key(true) == 1, "successful processing publishes output");
    const auto info_style = overlay_palette(OverlayMessageStyle::information);
    const auto error_style = overlay_palette(OverlayMessageStyle::error);
    check(info_style.background_rgb == 0 && info_style.text_rgb == 0x76B900 &&
          info_style.background_opacity == 1.0f, "general overlay uses black and NVIDIA green");
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
    check(nr_worker_protocol_version == 11, "worker protocol version changed unexpectedly");
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

    auto resized = solid(80, 60, 100, 0);
    motion->process(resized);
    check(motion->temporal_state().frame_sequence == 0 &&
          !worker_frame_eligible(motion->temporal_state(), 0),
          "resolution warmup cannot expose previous analysis");
    resized.sequence = 1;
    motion->process(resized);
    check(worker_frame_eligible(motion->temporal_state(), 0),
          "resized input becomes processable after warmup");

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
            check(GetProcAddress(bridge, "dlss5nr_process_v2") != nullptr, "versioned multipass process export");
            check(TemporalAnalysisPayload{}.nr_passes==1 && WorkerTemporalState{}.nr_passes==1,
                  "NR defaults to one pass");
            check(GetProcAddress(bridge, "dlss5nr_create_correction_target") != nullptr,
                  "bridge shared correction target export");
            check(GetProcAddress(bridge, "dlss5nr_shutdown") != nullptr, "bridge shutdown export");
            check(GetProcAddress(bridge, "dlss5nr_get_timings_v2") != nullptr, "versioned bridge timings export");
            check(GetProcAddress(bridge, "dlss5nr_get_timings") == nullptr, "reject unsafe legacy timing struct");
            check(GetProcAddress(bridge, "dlss5nr_test_flow_failure") != nullptr, "isolated flow failure probe");
            FreeLibrary(bridge);
        }
    } else check(false, "runtime bridge path argument missing");

    if (failures) { std::cerr << failures << " regression check(s) failed\n"; return 1; }
    std::cout << "core regression checks passed\n";
    return 0;
}
