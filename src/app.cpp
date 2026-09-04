#include "app.hpp"
#include "diagnostics_report.hpp"
#include "worker_health.hpp"
#include "capture_health.hpp"
#include "audio_health.hpp"
#include "device_refresh.hpp"
#include <cstring>
#include <iomanip>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <windowsx.h>

namespace {
constexpr wchar_t window_class[] = L"DlssNrCaptureWindow";
constexpr UINT frame_ready_message = WM_APP + 1;
constexpr UINT status_changed_message = WM_APP + 2;
constexpr UINT capture_failed_message = WM_APP + 3;
constexpr UINT audio_failed_message = WM_APP + 4;
constexpr UINT test_capture_failure_message = WM_APP + 42;
constexpr UINT test_audio_failure_message = WM_APP + 43;
constexpr UINT_PTR health_timer_id = 1;
constexpr UINT device_command_base = 41000;
constexpr UINT mode_command_base = 42000;
constexpr UINT mode_combo_id = 43000;
constexpr UINT flip_vertical_command = 44000;
constexpr UINT format_command_base = 45000;
constexpr UINT resolution_command_base = 46000;
constexpr UINT frame_rate_command_base = 47000;
constexpr UINT audio_off_command = 48000;
constexpr UINT audio_command_base = 48100;
constexpr UINT fullscreen_command = 49000;
constexpr UINT always_on_top_command = 49001;
constexpr UINT auto_size_command = 49002;
constexpr UINT diagnostics_command = 49003;
constexpr UINT performance_command = 49004;
constexpr UINT refresh_devices_command = 49005;
constexpr UINT passthrough_command = 50000;
constexpr UINT motion_analysis_command = 50001;
constexpr UINT history_overlay_command = 50002;
constexpr UINT nr_enable_command = 51000;
constexpr UINT nr_temporal_command = 51001;
constexpr UINT nr_style_base = 51100;
constexpr UINT nr_preset_base = 51200;
constexpr UINT nr_intensity_base = 51300;
constexpr UINT nr_latency_base = 51400;
const std::wstring& settings_key_path()
{
    static const std::wstring path = [] {
        wchar_t value[512]{};
        const DWORD length = GetEnvironmentVariableW(L"DLSS_NR_TEST_SETTINGS_KEY", value, 512);
        const std::wstring candidate = length > 0 && length < 512 ? std::wstring(value, length) : std::wstring{};
        constexpr wchar_t test_prefix[] = L"Software\\DlssNrCapture\\Tests\\";
        if (candidate.rfind(test_prefix, 0) == 0) return candidate;
        return std::wstring(L"Software\\DlssNrCapture");
    }();
    return path;
}
constexpr UINT cb_set_min_visible = 0x1701;

std::wstring lowercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

int device_preference(const std::wstring& name)
{
    const std::wstring lower = lowercase(name);
    if (lower.find(L"virtual") != std::wstring::npos || lower.find(L"animaze") != std::wstring::npos ||
        lower.find(L"obs") != std::wstring::npos)
        return -1000;
    if (lower.find(L"webcam") != std::wstring::npos || lower.find(L"camera") != std::wstring::npos)
        return -500;
    int score = 0;
    for (const wchar_t* token : {L"capture", L"elgato", L"avermedia", L"hdmi",
                                  L"game capture", L"gc", L"4k", L"usb video"})
        if (lower.find(token) != std::wstring::npos) score += 100;
    return score;
}

int64_t mode_preference(const CaptureMode& mode)
{
    if (!mode.supported) return std::numeric_limits<int64_t>::min();
    const double fps = static_cast<double>(mode.frame_rate_numerator) / mode.frame_rate_denominator;
    const int64_t resolution_distance = std::llabs(static_cast<int64_t>(mode.width) * mode.height - 1920LL * 1080);
    const int64_t fps_distance = static_cast<int64_t>(std::abs(fps - 60.0) * 1000.0);
    int64_t format_bonus = 0;
    if (mode.format_name == L"RGB32" || mode.format_name == L"ARGB32") format_bonus = 3000000000LL;
    else if (mode.format_name == L"NV12") format_bonus = 2000000000LL;
    else if (mode.format_name == L"YUY2") format_bonus = 1000000000LL;
    return format_bonus - resolution_distance - fps_distance * 1000;
}
}

int App::run(HINSTANCE instance, int show_command)
{
    throw_if_failed(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    processor_ = create_motion_analysis_processor();
    capture_ = std::make_unique<CaptureEngine>();
    audio_capture_ = std::make_unique<AudioCapture>();

    WNDCLASSEXW info{sizeof(info)};
    info.style = CS_HREDRAW | CS_VREDRAW;
    info.lpfnWndProc = window_proc;
    info.hInstance = instance;
    info.hCursor = LoadCursor(nullptr, IDC_ARROW);
    info.lpszClassName = window_class;
    RegisterClassExW(&info);

    window_ = CreateWindowExW(0, window_class, L"DLSS NR Capture", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                              nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("CreateWindowExW failed");

    mode_combo_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS |
        CBS_NOINTEGRALHEIGHT | CBS_DISABLENOSCROLL | WS_VSCROLL,
        92, 8, 700, 520, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(mode_combo_id)), instance, nullptr);
    if (!mode_combo_) throw std::runtime_error("Create video-mode combo box failed");
    mode_label_ = CreateWindowExW(0, L"STATIC", L"Video mode:", WS_CHILD | WS_VISIBLE,
                                  8, 12, 80, 24, window_, nullptr, instance, nullptr);
    SendMessageW(mode_combo_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(mode_combo_, cb_set_min_visible, 20, 0);
    SendMessageW(mode_combo_, CB_SETDROPPEDWIDTH, 760, 0);
    SetWindowLongPtrW(mode_combo_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    original_combo_proc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(mode_combo_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(combo_proc)));

    renderer_.initialize(window_);
    renderer_.clear();
    load_settings();
    // Only isolated regression settings may enable frame suppression.
    if (settings_key_path().rfind(L"Software\\DlssNrCapture\\Tests\\", 0) == 0) {
        isolated_test_settings_ = true;
        wchar_t value[2]{};
        suppress_test_frames_ = GetEnvironmentVariableW(L"DLSS_NR_TEST_SUPPRESS_FRAMES", value, 2) == 1 && value[0] == L'1';
    }
    discover_capture_devices();
    if (!SetTimer(window_, health_timer_id, 500, nullptr)) throw std::runtime_error("Start worker health timer failed");
    ShowWindow(window_, show_command);
    resize_window_to_capture();
    set_always_on_top(always_on_top_);
    UpdateWindow(window_);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Handle the application shortcut before dispatch so combo-box focus
        // and the default Windows F10 menu behavior cannot swallow it.
        const bool nr_shortcut = message.wParam == VK_F10 &&
            (message.hwnd == window_ || IsChild(window_, message.hwnd)) &&
            (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
            (GetKeyState(VK_MENU) & 0x8000) == 0 &&
            (GetKeyState(VK_SHIFT) & 0x8000) == 0;
        if (nr_shortcut && (message.message == WM_KEYDOWN ||
                            message.message == WM_SYSKEYDOWN)) {
            if ((message.lParam & (1LL << 30)) == 0)
                set_nr_enabled(!nr_enabled_);
            continue;
        }
        if (nr_shortcut && (message.message == WM_KEYUP ||
                            message.message == WM_SYSKEYUP))
            continue;
        if (message.message == WM_MOUSEWHEEL && handle_mode_wheel(message.wParam))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    capture_->stop();
    audio_capture_->stop();
    stop_nr_worker();
    capture_.reset();
    audio_capture_.reset();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

void App::load_settings()
{
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, settings_key_path().c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    auto read_string = [key](const wchar_t* name) {
        wchar_t value[1024]{};
        DWORD bytes = sizeof(value);
        if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
            return std::wstring{};
        return std::wstring(value);
    };
    auto read_dword = [key](const wchar_t* name, uint32_t& value) {
        DWORD bytes = sizeof(value);
        return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
    };
    saved_device_name_ = read_string(L"VideoDevice");
    saved_format_ = read_string(L"VideoFormat");
    saved_audio_name_ = read_string(L"AudioDevice");
    reconnect_audio_id_ = read_string(L"AudioDeviceId");
    reconnect_audio_name_ = saved_audio_name_;
    audio_expected_ = !reconnect_audio_id_.empty() || !reconnect_audio_name_.empty();
    read_dword(L"Width", saved_width_);
    read_dword(L"Height", saved_height_);
    read_dword(L"FpsNumerator", saved_fps_numerator_);
    read_dword(L"FpsDenominator", saved_fps_denominator_);
    uint32_t flip{};
    has_saved_flip_ = read_dword(L"FlipVertical", flip);
    saved_flip_ = flip != 0;
    uint32_t value{};
    if (read_dword(L"NrEnabled", value)) nr_enabled_ = value != 0;
    if (read_dword(L"NrTemporal", value)) nr_temporal_enabled_ = value != 0;
    if (read_dword(L"NrStyle", value)) nr_style_ = std::min(value, 3u);
    if (read_dword(L"NrPreset", value)) nr_preset_ = std::clamp(value, 1u, 4u);
    if (read_dword(L"NrIntensity", value)) nr_intensity_percent_ = std::clamp(value, 25u, 100u);
    if (read_dword(L"NrWaitMs", value)) nr_wait_ms_ = value == 16 || value == 33 ? value : 2;
    if (read_dword(L"AlwaysOnTop", value)) always_on_top_ = value != 0;
    if (read_dword(L"AutoSizeToResolution", value)) auto_size_to_resolution_ = value != 0;
    if (read_dword(L"PerformanceOverlay", value)) performance_overlay_ = value != 0;
    RegCloseKey(key);
}

void App::save_settings()
{
    if (restoring_settings_) return;
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settings_key_path().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto write_string = [key](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    auto write_dword = [key](const wchar_t* name, uint32_t value) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };
    write_string(L"VideoDevice", active_device_ ? devices_[*active_device_].name : L"");
    write_string(L"AudioDevice", audio_expected_ ? reconnect_audio_name_ : L"");
    write_string(L"AudioDeviceId", audio_expected_ ? reconnect_audio_id_ : L"");
    if (active_mode_) {
        const auto& mode = modes_[*active_mode_];
        write_string(L"VideoFormat", mode.format_name);
        write_dword(L"Width", mode.width);
        write_dword(L"Height", mode.height);
        write_dword(L"FpsNumerator", mode.frame_rate_numerator);
        write_dword(L"FpsDenominator", mode.frame_rate_denominator);
    }
    write_dword(L"FlipVertical", flip_vertical_.load(std::memory_order_relaxed) ? 1u : 0u);
    write_dword(L"NrEnabled", nr_enabled_ ? 1u : 0u);
    write_dword(L"NrTemporal", nr_temporal_enabled_ ? 1u : 0u);
    write_dword(L"NrStyle", nr_style_);
    write_dword(L"NrPreset", nr_preset_);
    write_dword(L"NrIntensity", nr_intensity_percent_);
    write_dword(L"NrWaitMs", nr_wait_ms_);
    write_dword(L"AlwaysOnTop", always_on_top_ ? 1u : 0u);
    write_dword(L"AutoSizeToResolution", auto_size_to_resolution_ ? 1u : 0u);
    write_dword(L"PerformanceOverlay", performance_overlay_ ? 1u : 0u);
    RegCloseKey(key);
}

void App::discover_capture_devices()
{
    devices_ = CaptureEngine::enumerate_devices();
    if (isolated_test_settings_) {
        wchar_t value[2]{};
        if (GetEnvironmentVariableW(L"DLSS_NR_TEST_NO_VIDEO", value, 2) == 1 && value[0] == L'1')
            devices_.clear();
    }
    audio_devices_ = AudioCapture::enumerate_devices();
    menu_bar_ = CreateMenu();
    device_menu_ = CreatePopupMenu();
    mode_menu_ = CreatePopupMenu();
    format_menu_ = CreatePopupMenu();
    resolution_menu_ = CreatePopupMenu();
    frame_rate_menu_ = CreatePopupMenu();
    image_menu_ = CreatePopupMenu();
    view_menu_ = CreatePopupMenu();
    processing_menu_ = CreatePopupMenu();
    nr_menu_ = CreatePopupMenu();
    nr_style_menu_ = CreatePopupMenu();
    nr_preset_menu_ = CreatePopupMenu();
    nr_intensity_menu_ = CreatePopupMenu();
    nr_latency_menu_ = CreatePopupMenu();
    AppendMenuW(nr_menu_, MF_STRING | (nr_enabled_ ? MF_CHECKED : 0), nr_enable_command, L"Toggle DLSS Neural Rendering\tF10");
    AppendMenuW(nr_menu_, MF_STRING | (nr_temporal_enabled_ ? MF_CHECKED : 0), nr_temporal_command, L"Temporal accumulation");
    AppendMenuW(nr_menu_, MF_SEPARATOR, 0, nullptr);
    for (UINT i = 0; i < 4; ++i) {
        const std::wstring label = L"Style " + std::to_wstring(i);
        AppendMenuW(nr_style_menu_, MF_STRING | (nr_style_ == i ? MF_CHECKED : 0), nr_style_base + i, label.c_str());
    }
    for (UINT i = 1; i <= 4; ++i) {
        const std::wstring label = L"Preset " + std::to_wstring(i);
        AppendMenuW(nr_preset_menu_, MF_STRING | (nr_preset_ == i ? MF_CHECKED : 0), nr_preset_base + i, label.c_str());
    }
    const UINT intensities[] = {50, 75, 100};
    for (UINT i = 0; i < 3; ++i) {
        const std::wstring label = std::to_wstring(intensities[i]) + L"%";
        AppendMenuW(nr_intensity_menu_, MF_STRING | (nr_intensity_percent_ == intensities[i] ? MF_CHECKED : 0), nr_intensity_base + i, label.c_str());
    }
    const UINT waits[] = {2, 16, 33};
    const wchar_t* wait_labels[] = {L"2 ms (lowest latency)", L"16 ms (balanced)", L"33 ms (quality priority)"};
    for (UINT i = 0; i < 3; ++i)
        AppendMenuW(nr_latency_menu_, MF_STRING | (nr_wait_ms_ == waits[i] ? MF_CHECKED : 0), nr_latency_base + i, wait_labels[i]);
    AppendMenuW(nr_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(nr_style_menu_), L"Style");
    AppendMenuW(nr_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(nr_preset_menu_), L"Preset");
    AppendMenuW(nr_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(nr_intensity_menu_), L"Intensity");
    AppendMenuW(nr_menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(nr_latency_menu_), L"Maximum output wait");
    AppendMenuW(processing_menu_, MF_STRING, passthrough_command, L"Passthrough");
    AppendMenuW(processing_menu_, MF_STRING | MF_CHECKED, motion_analysis_command, L"Motion analysis");
    AppendMenuW(processing_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(processing_menu_, MF_STRING, history_overlay_command, L"Show rejected history");
    AppendMenuW(view_menu_, MF_STRING, fullscreen_command, L"Full screen (F11)");
    AppendMenuW(view_menu_, MF_STRING | (always_on_top_ ? MF_CHECKED : 0), always_on_top_command, L"Always on top");
    AppendMenuW(view_menu_, MF_STRING | (auto_size_to_resolution_ ? MF_CHECKED : 0), auto_size_command, L"Size window to capture resolution");
    AppendMenuW(view_menu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu_, MF_STRING, diagnostics_command, L"Copy diagnostics to clipboard");
    AppendMenuW(view_menu_, MF_STRING | (performance_overlay_ ? MF_CHECKED : 0),
                performance_command, L"Performance overlay");
    AppendMenuW(view_menu_, MF_STRING, refresh_devices_command, L"Refresh devices");
    audio_menu_ = CreatePopupMenu();
    AppendMenuW(audio_menu_, MF_STRING | MF_CHECKED, audio_off_command, L"Off");
    for (size_t index = 0; index < audio_devices_.size(); ++index)
        AppendMenuW(audio_menu_, MF_STRING, audio_command_base + static_cast<UINT>(index), audio_devices_[index].name.c_str());
    AppendMenuW(image_menu_, MF_STRING, flip_vertical_command, L"Flip vertically");
    for (size_t index = 0; index < devices_.size(); ++index)
        AppendMenuW(device_menu_, MF_STRING, device_command_base + static_cast<UINT>(index),
                    devices_[index].name.c_str());
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(device_menu_), L"Capture device");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(format_menu_), L"Video format");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(resolution_menu_), L"Resolution");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(frame_rate_menu_), L"Frame rate");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(audio_menu_), L"Audio capture");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(image_menu_), L"Image");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(processing_menu_), L"Processing");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(nr_menu_), L"Neural Rendering");
    AppendMenuW(menu_bar_, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu_), L"View");
    SetMenu(window_, menu_bar_);
    renderer_.set_worker_wait_ms(nr_wait_ms_);
    // Restore before video selection can save settings, including audio-only startup.
    if (audio_expected_) {
        const size_t index = audio_restore_index(audio_devices_, reconnect_audio_id_, reconnect_audio_name_);
        if (index < audio_devices_.size()) start_audio_capture(index, true);
        else {
            audio_failed_ = true;
            last_audio_retry_ms_ = GetTickCount64();
            audio_recovery_error_ = L"Saved audio input is missing or ambiguous; waiting for matching device";
            rebuild_audio_menu();
        }
    }
    restoring_settings_ = false;

    if (devices_.empty()) {
        status_ = L"No capture device found";
        update_title();
        return;
    }
    size_t preferred = 0;
    int best = device_preference(devices_[0].name);
    for (size_t index = 0; index < devices_.size(); ++index) {
        if (!saved_device_name_.empty() && devices_[index].name == saved_device_name_) { preferred = index; break; }
        const int score = device_preference(devices_[index].name);
        if (score > best) { best = score; preferred = index; }
    }
    start_capture_device(preferred);
    if (has_saved_flip_) set_vertical_flip(saved_flip_);
}

void App::rebuild_audio_menu()
{
    while (GetMenuItemCount(audio_menu_) > 0) DeleteMenu(audio_menu_, 0, MF_BYPOSITION);
    AppendMenuW(audio_menu_, MF_STRING | (!audio_expected_ ? MF_CHECKED : 0), audio_off_command, L"Off");
    for (size_t index = 0; index < audio_devices_.size(); ++index)
        AppendMenuW(audio_menu_, MF_STRING | (active_audio_device_ && *active_audio_device_ == index ? MF_CHECKED : 0),
                    audio_command_base + static_cast<UINT>(index), audio_devices_[index].name.c_str());
    if (audio_expected_ && !active_audio_device_)
        AppendMenuW(audio_menu_, MF_STRING | MF_GRAYED, 0, (L"Waiting: " + reconnect_audio_name_).c_str());
    DrawMenuBar(window_);
}

void App::refresh_capture_devices()
{
    try {
        // Enumerate both lists before committing either; never restart a live
        // capture or worker simply because the user refreshed the menus.
        auto video = CaptureEngine::enumerate_devices();
        auto audio = AudioCapture::enumerate_devices();
        const auto video_selected = remap_selected_device(devices_, video, active_device_,
            [](const auto& device) { return device.symbolic_link; });
        std::optional<size_t> audio_selected;
        if (active_audio_device_) {
            const size_t index = audio_restore_index(audio, reconnect_audio_id_, reconnect_audio_name_);
            if (index < audio.size()) audio_selected = index;
        }
        const bool audio_missing = active_audio_device_.has_value() && !audio_selected;
        devices_ = std::move(video);
        audio_devices_ = std::move(audio);
        active_device_ = video_selected;
        active_audio_device_ = audio_selected;
        if (audio_missing && audio_expected_) {
            audio_failed_ = true;
            audio_recovery_error_ = L"Selected audio input is not detected";
        }
        while (GetMenuItemCount(device_menu_) > 0) DeleteMenu(device_menu_, 0, MF_BYPOSITION);
        for (size_t i = 0; i < devices_.size(); ++i)
            AppendMenuW(device_menu_, MF_STRING | (active_device_ && *active_device_ == i ? MF_CHECKED : 0),
                        device_command_base + static_cast<UINT>(i), devices_[i].name.c_str());
        rebuild_audio_menu();
        if (!active_device_) status_ = devices_.empty() ? L"No capture device found" : L"Select a capture device";
        renderer_.show_notification(L"DEVICE LIST UPDATED");
        update_title();
    } catch (const std::exception& error) {
        show_error(L"Cannot refresh devices: " + widen(error.what()));
    }
}

void App::start_audio_capture(size_t index, bool reconnecting)
{
    if (!reconnecting) {
        audio_expected_ = false;
        reconnect_audio_id_.clear();
        reconnect_audio_name_.clear();
        audio_recovery_error_.clear();
    }
    const uint64_t generation = ++audio_generation_;
    audio_capture_->stop();
    if (active_audio_device_) CheckMenuItem(audio_menu_, audio_command_base + static_cast<UINT>(*active_audio_device_), MF_BYCOMMAND | MF_UNCHECKED);
    active_audio_device_.reset();
    CheckMenuItem(audio_menu_, audio_off_command, MF_BYCOMMAND | (index >= audio_devices_.size() ? MF_CHECKED : MF_UNCHECKED));
    audio_failed_ = false;
    if (index >= audio_devices_.size()) { rebuild_audio_menu(); save_settings(); return; }
    audio_expected_ = true;
    reconnect_audio_id_ = audio_devices_[index].id;
    reconnect_audio_name_ = audio_devices_[index].name;
    last_audio_retry_ms_ = GetTickCount64();
    try {
    audio_capture_->start(audio_devices_[index], [this, generation](std::wstring error) {
        auto* text = new std::wstring(std::move(error));
        if (!PostMessageW(window_, audio_failed_message, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(text))) delete text;
    });
    audio_recovery_error_.clear();
    } catch (const std::exception& error) {
        audio_failed_ = true;
        audio_recovery_error_ = widen(error.what());
    }
    active_audio_device_ = index;
    rebuild_audio_menu();
    save_settings();
}

void App::ensure_audio_health()
{
    if (!audio_expected_) return;
    const uint64_t now = GetTickCount64();
    if (!audio_retry_due(now, last_audio_retry_ms_, audio_failed_)) return;
    last_audio_retry_ms_ = now;
    ++audio_recovery_attempts_;
    ++audio_generation_;
    audio_capture_->stop();
    try {
        auto discovered = AudioCapture::enumerate_devices();
        const size_t index = audio_restore_index(discovered, reconnect_audio_id_, reconnect_audio_name_);
        audio_devices_ = std::move(discovered);
        active_audio_device_.reset();
        rebuild_audio_menu();
        if (index == audio_devices_.size())
            throw std::runtime_error("Selected audio device is missing or ambiguous");
        start_audio_capture(index, true);
    } catch (const std::exception& error) {
        audio_failed_ = true;
        audio_recovery_error_ = widen(error.what());
    }
}

void App::rebuild_mode_menu()
{
    SendMessageW(mode_combo_, CB_RESETCONTENT, 0, 0);
    formats_.clear();
    resolutions_.clear();
    frame_rates_.clear();
    while (GetMenuItemCount(frame_rate_menu_) > 0) DeleteMenu(frame_rate_menu_, 0, MF_BYPOSITION);
    while (GetMenuItemCount(resolution_menu_) > 0) DeleteMenu(resolution_menu_, 0, MF_BYPOSITION);
    while (GetMenuItemCount(format_menu_) > 0) DeleteMenu(format_menu_, 0, MF_BYPOSITION);
    while (GetMenuItemCount(mode_menu_) > 0)
        DeleteMenu(mode_menu_, 0, MF_BYPOSITION);
    for (size_t index = 0; index < modes_.size(); ++index) {
        const std::wstring label = modes_[index].display_name();
        AppendMenuW(mode_menu_, MF_STRING, mode_command_base + static_cast<UINT>(index), label.c_str());
        SendMessageW(mode_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (std::find(formats_.begin(), formats_.end(), modes_[index].format_name) == formats_.end()) formats_.push_back(modes_[index].format_name);
        const auto resolution = std::make_pair(modes_[index].width, modes_[index].height);
        if (std::find(resolutions_.begin(), resolutions_.end(), resolution) == resolutions_.end()) resolutions_.push_back(resolution);
        const auto frame_rate = std::make_pair(modes_[index].frame_rate_numerator, modes_[index].frame_rate_denominator);
        const bool known_rate = std::any_of(frame_rates_.begin(), frame_rates_.end(), [&](const auto& rate) {
            return static_cast<uint64_t>(rate.first) * frame_rate.second == static_cast<uint64_t>(frame_rate.first) * rate.second;
        });
        if (!known_rate) frame_rates_.push_back(frame_rate);
    }
    if (modes_.empty())
        AppendMenuW(mode_menu_, MF_STRING | MF_GRAYED, mode_command_base, L"No supported mode");
    for (size_t index = 0; index < formats_.size(); ++index)
        AppendMenuW(format_menu_, MF_STRING, format_command_base + static_cast<UINT>(index), formats_[index].c_str());
    for (size_t index = 0; index < resolutions_.size(); ++index) {
        const std::wstring label = std::to_wstring(resolutions_[index].first) + L"x" + std::to_wstring(resolutions_[index].second);
        AppendMenuW(resolution_menu_, MF_STRING, resolution_command_base + static_cast<UINT>(index), label.c_str());
    }
    for (size_t index = 0; index < frame_rates_.size(); ++index) {
        wchar_t label[32]{};
        const double fps = static_cast<double>(frame_rates_[index].first) / frame_rates_[index].second;
        swprintf_s(label, L"%.2f fps", fps);
        AppendMenuW(frame_rate_menu_, MF_STRING, frame_rate_command_base + static_cast<UINT>(index), label);
    }    DrawMenuBar(window_);
}

bool App::handle_mode_wheel(WPARAM wparam)
{
    if (!mode_combo_ || modes_.empty()) return false;
    RECT rect{}; POINT cursor{};
    GetWindowRect(mode_combo_, &rect);
    GetCursorPos(&cursor);
    const bool dropped = SendMessageW(mode_combo_, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
    if (!dropped && !PtInRect(&rect, cursor) && GetFocus() != mode_combo_) return false;
    int selected = static_cast<int>(SendMessageW(mode_combo_, CB_GETCURSEL, 0, 0));
    if (selected == CB_ERR) selected = 0;
    selected += GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
    selected = std::clamp(selected, 0, static_cast<int>(modes_.size()) - 1);
    if (!active_mode_ || static_cast<size_t>(selected) != *active_mode_) {
        SendMessageW(mode_combo_, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        start_capture_mode(static_cast<size_t>(selected));
    }
    return true;
}

void App::set_vertical_flip(bool enabled)
{
    flip_vertical_.store(enabled, std::memory_order_relaxed);
    CheckMenuItem(image_menu_, flip_vertical_command,
                  MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    save_settings();
}
void App::resize_window_to_capture(UINT dpi, const POINT* position)
{
    if (!auto_size_to_resolution_ || fullscreen_ || !active_mode_) return;
    const auto& mode = modes_[*active_mode_];
    RECT desired{0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE));
    using GetWindowDpi = UINT (WINAPI*)(HWND);
    using AdjustForDpi = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!dpi && user32) {
        if (auto get_dpi = reinterpret_cast<GetWindowDpi>(GetProcAddress(user32, "GetDpiForWindow")))
            dpi = get_dpi(window_);
    }
    if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;
    bool adjusted = false;
    if (user32) {
        if (auto adjust = reinterpret_cast<AdjustForDpi>(GetProcAddress(user32, "AdjustWindowRectExForDpi")))
            adjusted = adjust(&desired, style, GetMenu(window_) != nullptr, ex_style, dpi) != FALSE;
    }
    if (!adjusted && !AdjustWindowRectEx(&desired, style, GetMenu(window_) != nullptr, ex_style)) return;
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (position ? 0u : SWP_NOMOVE);
    SetWindowPos(window_, nullptr, position ? position->x : 0, position ? position->y : 0,
                 width, height, flags);
}

void App::set_auto_size_to_resolution(bool enabled)
{
    auto_size_to_resolution_ = enabled;
    if (view_menu_)
        CheckMenuItem(view_menu_, auto_size_command,
                      MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    save_settings();
    if (enabled) resize_window_to_capture();
}
void App::set_always_on_top(bool enabled)
{
    always_on_top_ = enabled;
    SetWindowPos(window_, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (view_menu_)
        CheckMenuItem(view_menu_, always_on_top_command,
                      MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    save_settings();
}
void App::toggle_fullscreen()
{
    const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
    if (!fullscreen_) {
        windowed_placement_.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(window_, &windowed_placement_);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        SetMenu(window_, nullptr);
        ShowWindow(mode_combo_, SW_HIDE);
        ShowWindow(mode_label_, SW_HIDE);
        SetWindowLongPtrW(window_, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
        CheckMenuItem(view_menu_, fullscreen_command, MF_BYCOMMAND | MF_CHECKED);
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetMenu(window_, menu_bar_);
    renderer_.set_worker_wait_ms(nr_wait_ms_);
        SetWindowPlacement(window_, &windowed_placement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        ShowWindow(mode_label_, SW_SHOW);
        ShowWindow(mode_combo_, SW_SHOW);
        fullscreen_ = false;
        CheckMenuItem(view_menu_, fullscreen_command, MF_BYCOMMAND | MF_UNCHECKED);
        resize_window_to_capture();
    }
}

void App::start_capture_device(size_t index)
{
    if (index >= devices_.size()) return;
    capture_expected_ = false;
    worker_expected_ = false;
    ++capture_generation_;
    capture_->stop();
    stop_nr_worker();
    active_mode_.reset();
    modes_.clear();
    status_ = L"Reading modes: " + devices_[index].name;
    update_title();
    try {
        modes_ = CaptureEngine::enumerate_modes(devices_[index]);
        rebuild_mode_menu();
        if (modes_.empty())
            throw std::runtime_error("No RGB24/RGB32, NV12/P010, YUY2 or UYVY mode exposed");
        if (active_device_)
            CheckMenuItem(device_menu_, device_command_base + static_cast<UINT>(*active_device_),
                          MF_BYCOMMAND | MF_UNCHECKED);
        active_device_ = index;
        CheckMenuItem(device_menu_, device_command_base + static_cast<UINT>(index),
                      MF_BYCOMMAND | MF_CHECKED);
        size_t preferred = 0;
        int64_t best = mode_preference(modes_[0]);
        for (size_t i = 0; i < modes_.size(); ++i) {
            const auto& mode = modes_[i];
            if (devices_[index].name == saved_device_name_ && mode.format_name == saved_format_ &&
                mode.width == saved_width_ && mode.height == saved_height_ &&
                static_cast<uint64_t>(mode.frame_rate_numerator) * saved_fps_denominator_ ==
                static_cast<uint64_t>(saved_fps_numerator_) * mode.frame_rate_denominator) {
                preferred = i; break;
            }
            const int64_t score = mode_preference(mode);
            if (score > best) { best = score; preferred = i; }
        }
        start_capture_mode(preferred);
    } catch (const std::exception& error) {
        show_error(L"Cannot enumerate " + devices_[index].name + L": " + widen(error.what()));
    }
}

void App::start_capture_format(size_t index)
{
    if (index >= formats_.size() || modes_.empty()) return;
    size_t best_index = modes_.size();
    double best_distance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < modes_.size(); ++i) {
        if (modes_[i].format_name != formats_[index]) continue;
        double distance = 0.0;
        if (active_mode_) {
            const auto& current = modes_[*active_mode_];
            distance += std::abs(static_cast<double>(modes_[i].width) - current.width) * 10000.0;
            distance += std::abs(static_cast<double>(modes_[i].height) - current.height) * 10000.0;
            const double fps = static_cast<double>(modes_[i].frame_rate_numerator) / modes_[i].frame_rate_denominator;
            const double current_fps = static_cast<double>(current.frame_rate_numerator) / current.frame_rate_denominator;
            distance += std::abs(fps - current_fps) * 100.0;
        }
        if (distance < best_distance) { best_distance = distance; best_index = i; }
    }
    if (best_index < modes_.size()) start_capture_mode(best_index);
}
void App::start_capture_resolution(size_t index)
{
    if (index >= resolutions_.size() || modes_.empty()) return;
    size_t best_index = modes_.size();
    double best_distance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < modes_.size(); ++i) {
        if (modes_[i].width != resolutions_[index].first || modes_[i].height != resolutions_[index].second) continue;
        double distance = 0.0;
        if (active_mode_) {
            const auto& current = modes_[*active_mode_];
            if (modes_[i].format_name != current.format_name) distance += 1000000.0;
            const double fps = static_cast<double>(modes_[i].frame_rate_numerator) / modes_[i].frame_rate_denominator;
            const double current_fps = static_cast<double>(current.frame_rate_numerator) / current.frame_rate_denominator;
            distance += std::abs(fps - current_fps) * 100.0;
        }
        if (!modes_[i].supported) distance += 1000000000.0;
        if (distance < best_distance) { best_distance = distance; best_index = i; }
    }
    if (best_index < modes_.size()) start_capture_mode(best_index);
}
void App::start_capture_frame_rate(size_t index)
{
    if (index >= frame_rates_.size() || modes_.empty()) return;
    size_t best_index = modes_.size();
    int best_rank = std::numeric_limits<int>::min();
    for (size_t i = 0; i < modes_.size(); ++i) {
        if (static_cast<uint64_t>(modes_[i].frame_rate_numerator) * frame_rates_[index].second !=
            static_cast<uint64_t>(frame_rates_[index].first) * modes_[i].frame_rate_denominator) continue;
        int rank = modes_[i].supported ? 100 : 0;
        if (active_mode_) {
            const auto& current = modes_[*active_mode_];
            if (modes_[i].format_name == current.format_name) rank += 20;
            if (modes_[i].width == current.width && modes_[i].height == current.height) rank += 10;
        }
        if (rank > best_rank) { best_rank = rank; best_index = i; }
    }
    if (best_index < modes_.size()) start_capture_mode(best_index);
}void App::start_capture_mode(size_t index)
{
    if (!active_device_ || index >= modes_.size()) return;
    const uint64_t generation = ++capture_generation_;
    if (!reconnecting_capture_) capture_expected_ = false;
    capture_failed_ = false;
    worker_expected_ = false;
    capture_->stop();
    stop_nr_worker();
    {
        std::scoped_lock lock(frame_mutex_);
        pending_frame_.reset();
    }
    received_frames_.store(0, std::memory_order_relaxed);
    dropped_frames_.store(0, std::memory_order_relaxed);
    displayed_frames_ = 0;
    capture_rate_ = {}; present_rate_ = {}; worker_rate_ = {};
    renderer_.set_performance_text(performance_overlay_ ? L"Measuring performance..." : L"");
    last_present_latency_ms_ = 0;
    processor_->reset_history();
    if (!reconnecting_capture_) set_vertical_flip(modes_[index].format_name == L"RGB24");
    status_ = L"Opening " + modes_[index].display_name();
    update_title();
    try {
        capture_started_ms_ = last_capture_retry_ms_ = GetTickCount64();
        last_capture_frame_ms_.store(0);
        capture_->start(devices_[*active_device_], modes_[index],
            [this, generation](VideoFrame&& frame) {
                if (generation == capture_generation_.load()) enqueue_frame(std::move(frame));
            },
            [this, generation](std::wstring error) {
                auto* message = new std::wstring(std::move(error));
                if (!PostMessageW(window_, capture_failed_message, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(message)))
                    delete message;
            });
        capture_expected_ = true;
        reconnect_mode_ = modes_[index];
        reconnect_device_link_ = devices_[*active_device_].symbolic_link;
        capture_recovery_error_.clear();
        if (active_mode_)
            CheckMenuItem(mode_menu_, mode_command_base + static_cast<UINT>(*active_mode_),
                          MF_BYCOMMAND | MF_UNCHECKED);
        active_mode_ = index;
        resize_window_to_capture();
        restart_nr_worker(modes_[index].width, modes_[index].height);
        SendMessageW(mode_combo_, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        CheckMenuItem(mode_menu_, mode_command_base + static_cast<UINT>(index),
                      MF_BYCOMMAND | MF_CHECKED);
        for (size_t format_index = 0; format_index < formats_.size(); ++format_index)
            CheckMenuItem(format_menu_, format_command_base + static_cast<UINT>(format_index),
                          MF_BYCOMMAND | (formats_[format_index] == modes_[index].format_name ? MF_CHECKED : MF_UNCHECKED));
        for (size_t resolution_index = 0; resolution_index < resolutions_.size(); ++resolution_index)
            CheckMenuItem(resolution_menu_, resolution_command_base + static_cast<UINT>(resolution_index),
                          MF_BYCOMMAND | (resolutions_[resolution_index].first == modes_[index].width && resolutions_[resolution_index].second == modes_[index].height ? MF_CHECKED : MF_UNCHECKED));
        for (size_t rate_index = 0; rate_index < frame_rates_.size(); ++rate_index)
            CheckMenuItem(frame_rate_menu_, frame_rate_command_base + static_cast<UINT>(rate_index),
                          MF_BYCOMMAND | (static_cast<uint64_t>(frame_rates_[rate_index].first) * modes_[index].frame_rate_denominator ==
                          static_cast<uint64_t>(modes_[index].frame_rate_numerator) * frame_rates_[rate_index].second ? MF_CHECKED : MF_UNCHECKED));        status_ = L"Live: " + devices_[*active_device_].name + L" — " + modes_[index].display_name();
        save_settings();
    } catch (const std::exception& error) {
        if (reconnecting_capture_) {
            capture_failed_ = true;
            capture_recovery_error_ = widen(error.what());
            status_ = L"Capture reconnect pending: " + capture_recovery_error_;
        } else show_error(L"Cannot open mode: " + widen(error.what()));
    }
    update_title();
}

void App::set_motion_analysis(bool enabled)
{
    if (motion_analysis_enabled_ == enabled) return;
    const std::optional<size_t> mode = active_mode_;
    capture_->stop();
    processor_ = enabled ? create_motion_analysis_processor() : create_passthrough_processor();
    motion_analysis_enabled_ = enabled;
    processor_->set_debug_overlay(history_overlay_enabled_);
    CheckMenuItem(processing_menu_, motion_analysis_command, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(processing_menu_, passthrough_command, MF_BYCOMMAND | (enabled ? MF_UNCHECKED : MF_CHECKED));
    if (mode) start_capture_mode(*mode);
    else update_title();
}

std::wstring App::nr_worker_status() const
{
    if (renderer_.worker_connected() && temporal_state_) {
        const uint64_t heartbeat = static_cast<uint64_t>(
            InterlockedCompareExchange64(&temporal_state_->worker_heartbeat_ms, 0, 0));
        const uint64_t frames = static_cast<uint64_t>(
            InterlockedCompareExchange64(&temporal_state_->worker_processed_frames, 0, 0));
        const LONG adapter = InterlockedCompareExchange(&temporal_state_->worker_adapter_state, 0, 0);
        const LONG adapter_error = InterlockedCompareExchange(&temporal_state_->worker_adapter_error, 0, 0);
        if (heartbeat && GetTickCount64() - heartbeat > 1500) return L"GPU worker timed out — passthrough";
        std::wstring adapter_name = temporal_state_->worker_adapter_name;
        if (adapter_name.empty()) adapter_name = adapter == 1 ? L"Passthrough" : L"External";
        const std::wstring adapter_message = temporal_state_->worker_adapter_error_message;
        if (!adapter_message.empty()) adapter_name += L", " + adapter_message;
        else if (adapter_error) adapter_name += L", adapter error " + std::to_wstring(adapter_error);
        const wchar_t* correction_state = renderer_.worker_correction_active()
            ? L"correction active"
            : (renderer_.worker_correction_too_slow()
                ? L"NR too slow - original only"
                : (renderer_.worker_preparing() ? L"NR preparing" : L"original only"));
        std::wstring timing;
        if (adapter == 2 && frames) {
            const auto milliseconds = [](volatile LONG64* value) {
                const uint64_t us = static_cast<uint64_t>(InterlockedCompareExchange64(value, 0, 0));
                return std::to_wstring((us + 500) / 1000);
            };
            const uint64_t output_us = static_cast<uint64_t>(InterlockedCompareExchange64(
                &temporal_state_->nr_bridge_output_us, 0, 0)) +
                static_cast<uint64_t>(InterlockedCompareExchange64(
                    &temporal_state_->nr_correction_output_us, 0, 0));
            timing = L", NR " + milliseconds(&temporal_state_->nr_total_us) +
                L" ms [in " + milliseconds(&temporal_state_->nr_input_us) +
                L", setup " + milliseconds(&temporal_state_->nr_setup_us) +
                L", flow " + milliseconds(&temporal_state_->nr_optical_flow_us) +
                L", MV " + milliseconds(&temporal_state_->nr_motion_vector_us) +
                L", prep " + milliseconds(&temporal_state_->nr_gpu_prepare_us) +
                L", GPU " + milliseconds(&temporal_state_->nr_gpu_execute_us) +
                L", out " + std::to_wstring((output_us + 500) / 1000) + L"]";
        }
        return L"GPU worker connected [" + adapter_name + L", " +
               std::to_wstring(frames) + L" processed, " +
               std::to_wstring(renderer_.worker_output_frames()) + L" corrections, " +
               std::to_wstring(renderer_.worker_enhanced_frames()) + L" enhanced, " +
               std::to_wstring(renderer_.worker_fallback_frames()) + L" original, " +
               correction_state + timing + L"]";
    }
    if (worker_process_.hProcess) {
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(worker_process_.hProcess, &code) && code != STILL_ACTIVE)
            return L"GPU worker failed (" + std::to_wstring(code) + L")";
    }
    return L"GPU worker starting";
}

void App::ensure_capture_health()
{
    if (!capture_expected_ || !active_device_ || !reconnect_mode_) return;
    const uint64_t now = GetTickCount64();
    if (!capture_retry_due(now, capture_started_ms_, last_capture_frame_ms_.load(),
                           last_capture_retry_ms_, capture_failed_)) return;
    last_capture_retry_ms_ = now;
    ++capture_recovery_attempts_;
    ++capture_generation_; // Ignore queued errors/frames belonging to the old session.
    capture_->stop();
    worker_expected_ = false;
    stop_nr_worker();
    status_ = L"Waiting to reconnect capture device";
    try {
        if (reconnect_device_link_.empty()) throw std::runtime_error("Device has no stable identity; select it manually");
        const auto discovered = CaptureEngine::enumerate_devices();
        const auto device = std::find_if(discovered.begin(), discovered.end(), [&](const auto& value) {
            return value.symbolic_link == reconnect_device_link_;
        });
        if (device == discovered.end()) throw std::runtime_error("Selected device is not connected");
        auto available = CaptureEngine::enumerate_modes(*device);
        const auto wanted = *reconnect_mode_;
        const auto mode = std::find_if(available.begin(), available.end(), [&](const auto& value) {
            return value.supported && value.subtype == wanted.subtype && value.width == wanted.width &&
                value.height == wanted.height && value.frame_rate_denominator && wanted.frame_rate_denominator &&
                static_cast<uint64_t>(value.frame_rate_numerator) * wanted.frame_rate_denominator ==
                static_cast<uint64_t>(wanted.frame_rate_numerator) * value.frame_rate_denominator;
        });
        const size_t index = static_cast<size_t>(mode - available.begin());
        devices_[*active_device_] = *device;
        modes_ = std::move(available);
        active_mode_.reset();
        rebuild_mode_menu();
        if (index == modes_.size()) throw std::runtime_error("Previous capture mode is unavailable; select a mode manually");
        reconnecting_capture_ = true;
        start_capture_mode(index);
        reconnecting_capture_ = false;
    } catch (const std::exception& error) {
        reconnecting_capture_ = false;
        capture_failed_ = true;
        capture_recovery_error_ = widen(error.what());
        status_ = L"Capture reconnect pending: " + capture_recovery_error_;
    }
}

void App::ensure_nr_worker_health()
{
    if (!worker_expected_ || !active_mode_) return;
    const uint64_t now = GetTickCount64();
    DWORD exit_code = STILL_ACTIVE;
    const bool exited = worker_process_.hProcess && GetExitCodeProcess(worker_process_.hProcess, &exit_code) && exit_code != STILL_ACTIVE;
    uint64_t heartbeat = 0;
    if (temporal_state_) heartbeat = static_cast<uint64_t>(
        InterlockedCompareExchange64(&temporal_state_->worker_heartbeat_ms, 0, 0));
    if (worker_needs_restart(now, last_worker_restart_ms_, worker_process_.hProcess != nullptr, exited, heartbeat)) {
        last_worker_restart_ms_ = now;
        ++worker_recovery_attempts_;
        try {
            const auto& mode = modes_[*active_mode_];
            restart_nr_worker(mode.width, mode.height);
            worker_restart_error_.clear();
        } catch (const std::exception& error) {
            worker_restart_error_ = widen(error.what());
            stop_nr_worker();
        }
    }
}

void App::stop_nr_worker()
{
    // Closing the last job handle also covers abrupt parent termination.
    worker_job_.reset();
    if (worker_process_.hProcess) {
        TerminateProcess(worker_process_.hProcess, 0);
        WaitForSingleObject(worker_process_.hProcess, 1000);
        CloseHandle(worker_process_.hProcess);
        CloseHandle(worker_process_.hThread);
        worker_process_ = {};
    }
    std::scoped_lock temporal_lock(temporal_mutex_);
    if (temporal_state_) { UnmapViewOfFile(temporal_state_); temporal_state_ = nullptr; }
    if (temporal_mapping_) { CloseHandle(temporal_mapping_); temporal_mapping_ = nullptr; }
}

void App::restart_nr_worker(uint32_t width, uint32_t height)
{
    stop_nr_worker();
    worker_expected_ = true;
    last_worker_restart_ms_ = GetTickCount64();
    const uint32_t fps_numerator = active_mode_ ? modes_[*active_mode_].frame_rate_numerator : 60;
    const uint32_t fps_denominator = active_mode_ ? modes_[*active_mode_].frame_rate_denominator : 1;
    renderer_.configure_shared_output(width, height, fps_numerator, fps_denominator);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    {
    std::scoped_lock temporal_lock(temporal_mutex_);
    temporal_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                           sizeof(WorkerTemporalState), nullptr);
    if (!temporal_mapping_) throw std::runtime_error("Create temporal metadata mapping failed");
    temporal_state_ = static_cast<WorkerTemporalState*>(
        MapViewOfFile(temporal_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WorkerTemporalState)));
    if (!temporal_state_) throw std::runtime_error("Map temporal metadata failed");
    *temporal_state_ = WorkerTemporalState{};
    InterlockedExchange(&temporal_state_->nr_enabled, nr_enabled_ ? 1 : 0);
    InterlockedExchange(&temporal_state_->nr_style, static_cast<LONG>(nr_style_));
    InterlockedExchange(&temporal_state_->nr_preset, static_cast<LONG>(nr_preset_));
    InterlockedExchange(&temporal_state_->nr_intensity_percent, static_cast<LONG>(nr_intensity_percent_));
    InterlockedExchange(&temporal_state_->nr_temporal, nr_temporal_enabled_ ? 1 : 0);
    }
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring worker_path = module;
    const size_t separator = worker_path.find_last_of(L"\\/");
    worker_path.resize(separator == std::wstring::npos ? 0 : separator + 1);
    worker_path += L"dlss-nr-worker.exe";
    std::wstring command = L"\"" + worker_path + L"\" " + std::to_wstring(GetCurrentProcessId()) +
        L" " + std::to_wstring(reinterpret_cast<uintptr_t>(renderer_.shared_input_handle())) +
        L" " + std::to_wstring(reinterpret_cast<uintptr_t>(renderer_.shared_output_handle())) +
        L" " + std::to_wstring(reinterpret_cast<uintptr_t>(temporal_mapping_)) +
        L" " + std::to_wstring(reinterpret_cast<uintptr_t>(renderer_.worker_event_handle()));
    STARTUPINFOW startup{sizeof(startup)};
    try {
        worker_job_.create();
        // Do not let the child run (or spawn descendants) before job assignment.
        if (!CreateProcessW(worker_path.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &worker_process_))
            throw std::runtime_error("Start GPU worker failed");
        worker_job_.assign(worker_process_.hProcess);
        if (ResumeThread(worker_process_.hThread) == static_cast<DWORD>(-1))
            throw std::runtime_error("Resume GPU worker failed");
        worker_restart_error_.clear();
    } catch (...) {
        stop_nr_worker();
        throw;
    }
}

void App::apply_nr_settings(bool restart_worker)
{
    renderer_.set_worker_wait_ms(nr_wait_ms_);
    if (temporal_state_) {
        InterlockedExchange(&temporal_state_->nr_style, static_cast<LONG>(nr_style_));
        InterlockedExchange(&temporal_state_->nr_preset, static_cast<LONG>(nr_preset_));
        InterlockedExchange(&temporal_state_->nr_intensity_percent, static_cast<LONG>(nr_intensity_percent_));
        InterlockedExchange(&temporal_state_->nr_temporal, nr_temporal_enabled_ ? 1 : 0);
    }
    save_settings();
    if (restart_worker && active_mode_) {
        const auto& mode = modes_[*active_mode_];
        restart_nr_worker(mode.width, mode.height);
    }
    update_title();
}

void App::set_nr_enabled(bool enabled)
{
    if (enabled) {
        wchar_t module[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::wstring runtime = module;
        const size_t separator = runtime.find_last_of(L"\\/");
        runtime.resize(separator == std::wstring::npos ? 0 : separator + 1);
        runtime += L"nr-runtime\\nvngx_dlssnr.dll";
        if (GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES) {
            show_error(L"DLSS Neural Rendering runtime was not found.\n\nPlace a legally obtained nvngx_dlssnr.dll in the nr-runtime folder, then enable Neural Rendering again.");
            return;
        }
    }
    nr_enabled_ = enabled;
    CheckMenuItem(nr_menu_, nr_enable_command, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    apply_nr_settings(true);
    renderer_.show_nr_toggle(enabled);
}
void App::set_history_overlay(bool enabled)
{
    history_overlay_enabled_ = enabled;
    processor_->set_debug_overlay(enabled);
    CheckMenuItem(processing_menu_, history_overlay_command,
                  MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
}

void App::enqueue_frame(VideoFrame&& frame)
{
    last_capture_frame_ms_.store(GetTickCount64());
    if (suppress_test_frames_) return;
    if (flip_vertical_.load(std::memory_order_relaxed) && frame.height > 1) {
        const size_t row_bytes = static_cast<size_t>(frame.width) * 4;
        for (uint32_t top = 0, bottom = frame.height - 1; top < bottom; ++top, --bottom)
            std::swap_ranges(frame.bgra.begin() + static_cast<size_t>(top) * row_bytes,
                             frame.bgra.begin() + static_cast<size_t>(top + 1) * row_bytes,
                             frame.bgra.begin() + static_cast<size_t>(bottom) * row_bytes);
    }
    if (!processor_->process(frame)) return;
    {
    std::scoped_lock temporal_lock(temporal_mutex_);
    if (temporal_state_) {
        const TemporalAnalysisPayload payload = processor_->temporal_state();
        InterlockedIncrement(&temporal_state_->sequence);
        temporal_state_->payload = payload;
        MemoryBarrier();
        InterlockedIncrement(&temporal_state_->sequence);
    }
    }
    received_frames_.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(frame_mutex_);
        if (pending_frame_) dropped_frames_.fetch_add(1, std::memory_order_relaxed);
        pending_frame_ = std::move(frame);
    }
    PostMessageW(window_, frame_ready_message, 0, 0);
}

void App::update_title()
{
    std::wstring title = L"DLSS NR Capture [" + widen(std::string(processor_->name())) + L"] — " + status_;
    const std::string details = processor_->diagnostics();
    if (!details.empty()) title += L" — " + widen(details);
    title += L" — " + nr_worker_status();
    if (capture_recovery_attempts_) title += L" | capture reconnects " + std::to_wstring(capture_recovery_attempts_);
    if (audio_expected_ && audio_failed_) title += L" | Audio reconnect pending: " + audio_recovery_error_;
    if (audio_recovery_attempts_) title += L" | audio reconnects " + std::to_wstring(audio_recovery_attempts_);
    const uint64_t received = received_frames_.load(std::memory_order_relaxed);
    const uint64_t dropped = dropped_frames_.load(std::memory_order_relaxed);
    if (received) {
        title += L" — latency " + std::to_wstring(last_present_latency_ms_) + L" ms";
        title += L" | dropped " + std::to_wstring(dropped) + L"/" + std::to_wstring(received);
    }
    SetWindowTextW(window_, title.c_str());
}

void App::update_performance_overlay()
{
    const uint64_t now = GetTickCount64();
    capture_rate_.observe(now, received_frames_.load());
    worker_rate_.observe(now, renderer_.worker_output_frames());
    if (!present_rate_.observe(now, displayed_frames_) || !performance_overlay_) return;
    const wchar_t* state = capture_interrupted_ ? L"INPUT LOST" : !nr_enabled_ ? L"OFF" : renderer_.worker_correction_active() ? L"ACTIVE"
        : renderer_.worker_correction_too_slow() ? L"TOO SLOW"
        : renderer_.worker_preparing() ? L"PREPARING" : L"ORIGINAL";
    std::wostringstream text;
    text << std::fixed << std::setprecision(1)
         << L"Capture " << capture_rate_.fps() << L" | Present " << present_rate_.fps() << L" FPS\n"
         << L"Worker " << worker_rate_.fps() << L" FPS | NR " << state << L"\n"
         << L"NR EMA " << renderer_.worker_average_processing_us() / 1000.0 << L" ms\n"
         << L"App latency " << (capture_interrupted_ ? L"N/A" : std::to_wstring(last_present_latency_ms_) + L" ms")
         << L" | Dropped " << dropped_frames_.load();
    renderer_.set_performance_text(text.str());
}

void App::copy_diagnostics()
{
    DiagnosticsReport report;
    report.add(L"Status", status_);
    report.add(L"Device", active_device_ ? devices_[*active_device_].name : L"None");
    report.add(L"Mode", active_mode_ ? modes_[*active_mode_].display_name() : L"None");
    const auto policy = active_mode_
        ? nr_speed_policy(modes_[*active_mode_].frame_rate_numerator,
                          modes_[*active_mode_].frame_rate_denominator)
        : nr_speed_policy(0, 0);
    report.add(L"NR enabled", nr_enabled_ ? L"yes" : L"no");
    report.add(L"Temporal enabled", nr_temporal_enabled_ ? L"yes" : L"no");
    report.add(L"Style", nr_style_);
    report.add(L"Preset", nr_preset_);
    report.add(L"Intensity percent", nr_intensity_percent_);
    report.add(L"Worker wait ms", nr_wait_ms_);
    report.add(L"Received frames", received_frames_.load());
    report.add(L"Displayed frames", displayed_frames_);
    report.add(L"Measured capture FPS", std::to_wstring(capture_rate_.fps()));
    report.add(L"Measured present FPS", std::to_wstring(present_rate_.fps()));
    report.add(L"Measured worker output FPS", std::to_wstring(worker_rate_.fps()));
    report.add(L"Dropped frames", dropped_frames_.load());
    report.add(L"Present latency ms", last_present_latency_ms_);
    report.add(L"Worker", nr_worker_status());
    report.add(L"Automatic worker recovery attempts", worker_recovery_attempts_);
    report.add(L"Last worker restart error", worker_restart_error_);
    report.add(L"Capture reconnect attempts", capture_recovery_attempts_);
    report.add(L"Capture input state", capture_interrupted_ ? L"Interrupted" : L"No interruption detected");
    report.add(L"Last capture recovery error", capture_recovery_error_);
    report.add(L"Audio selected", audio_expected_ ? reconnect_audio_name_ : L"Off");
    report.add(L"Audio reconnect attempts", audio_recovery_attempts_);
    report.add(L"Last audio recovery error", audio_recovery_error_);
    report.add(L"Processing EMA us", renderer_.worker_average_processing_us());
    report.add(L"Enable threshold us", policy.enable_us);
    report.add(L"Slow threshold us", policy.slow_us);
    report.add(L"Required fast samples", policy.fast_samples);
    report.add(L"Required slow samples", policy.slow_samples);
    if (temporal_state_) {
        const auto add = [&](const wchar_t* label, volatile LONG64* value) {
            report.add(label, static_cast<uint64_t>(InterlockedCompareExchange64(value, 0, 0)));
        };
        add(L"Worker processed frames", &temporal_state_->worker_processed_frames);
        add(L"NR total us", &temporal_state_->nr_total_us);
        add(L"Input us", &temporal_state_->nr_input_us);
        add(L"Setup us", &temporal_state_->nr_setup_us);
        add(L"Optical flow us", &temporal_state_->nr_optical_flow_us);
        add(L"Motion vectors us", &temporal_state_->nr_motion_vector_us);
        add(L"GPU prepare us", &temporal_state_->nr_gpu_prepare_us);
        add(L"GPU execute us", &temporal_state_->nr_gpu_execute_us);
        add(L"Bridge output us", &temporal_state_->nr_bridge_output_us);
        add(L"Correction output us", &temporal_state_->nr_correction_output_us);
    }
    const auto& text = report.text();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { show_error(L"Cannot allocate diagnostics clipboard data."); return; }
    void* buffer = GlobalLock(memory);
    if (!buffer) { GlobalFree(memory); show_error(L"Cannot lock diagnostics clipboard data."); return; }
    std::memcpy(buffer, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(window_)) {
        GlobalFree(memory);
        show_error(L"Cannot open clipboard. Please try again.");
        return;
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!copied) {
        GlobalFree(memory);
        show_error(L"Cannot copy diagnostics to clipboard.");
        return;
    }
    renderer_.show_notification(L"DIAGNOSTICS COPIED");
}

void App::show_error(const std::wstring& message)
{
    status_ = message;
    update_title();
    MessageBoxW(window_, message.c_str(), L"DLSS NR Capture error", MB_OK | MB_ICONERROR);
}
LRESULT CALLBACK App::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handle_message(window, message, wparam, lparam)
               : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK App::combo_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app && message == WM_MOUSEWHEEL && !app->modes_.empty()) {
        int selected = static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0));
        if (selected == CB_ERR) selected = 0;
        selected += GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
        selected = std::clamp(selected, 0, static_cast<int>(app->modes_.size()) - 1);
        SendMessageW(window, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        SendMessageW(app->window_, WM_COMMAND, MAKEWPARAM(mode_combo_id, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(window));
        return 0;
    }
    return app && app->original_combo_proc_
        ? CallWindowProcW(app->original_combo_proc_, window, message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
}
LRESULT App::handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_TIMER:
        if (wparam == health_timer_id) {
            capture_interrupted_ = capture_expected_ && capture_input_interrupted(
                GetTickCount64(), capture_started_ms_, last_capture_frame_ms_.load(), capture_failed_);
            renderer_.set_capture_interrupted(capture_interrupted_);
            update_performance_overlay();
            if (capture_interrupted_) renderer_.redraw_idle();
            ensure_capture_health();
            ensure_audio_health();
            ensure_nr_worker_health();
            update_title();
            return 0;
        }
        break;
    case frame_ready_message: {
        std::optional<VideoFrame> frame;
        { std::scoped_lock lock(frame_mutex_); frame.swap(pending_frame_); }
        if (frame) {
            const LONG adapter = temporal_state_
                ? InterlockedCompareExchange(&temporal_state_->worker_adapter_state, 0, 0) : 0;
            renderer_.set_worker_correction_enabled(nr_enabled_ && adapter == 2);
            const uint64_t nr_processing_us = temporal_state_
                ? static_cast<uint64_t>(InterlockedCompareExchange64(
                    &temporal_state_->nr_total_us, 0, 0)) : 0;
            renderer_.set_worker_processing_time_us(nr_processing_us);
            capture_interrupted_ = capture_failed_;
            renderer_.set_capture_interrupted(capture_interrupted_);
            renderer_.render(*frame);
            const uint64_t now = GetTickCount64();
            last_present_latency_ms_ = frame->arrival_tick_ms && now >= frame->arrival_tick_ms
                ? now - frame->arrival_tick_ms : 0;
            ++displayed_frames_;
            update_performance_overlay();
            if (displayed_frames_ % 15 == 0) update_title();
        }
        return 0;
    }
    case status_changed_message: {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lparam));
        if (error) show_error(*error);
        return 0;
    }
    case capture_failed_message: {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lparam));
        if (error && static_cast<uint64_t>(wparam) == capture_generation_.load()) {
            capture_failed_ = true;
            capture_recovery_error_ = *error;
            status_ = L"Capture interrupted; reconnect pending: " + *error;
            update_title();
        }
        return 0;
    }
    case test_capture_failure_message:
        if (isolated_test_settings_) {
            if (wparam == 1) return capture_interrupted_ ? 1 : 0;
            capture_failed_ = true;
            last_capture_retry_ms_ = GetTickCount64();
        }
        return 0;
    case audio_failed_message: {
        std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lparam));
        if (error && static_cast<uint64_t>(wparam) == audio_generation_.load() && audio_expected_) {
            audio_failed_ = true;
            audio_recovery_error_ = *error;
            update_title();
        }
        return 0;
    }
    case test_audio_failure_message:
        if (isolated_test_settings_) {
            if (wparam == 1) return audio_expected_ ? 1 : 0;
            if (wparam == 2) return static_cast<LRESULT>(audio_recovery_attempts_);
            // No real recording/output: only exercise missing-endpoint recovery.
            start_audio_capture(audio_devices_.size());
            audio_expected_ = audio_failed_ = true;
            reconnect_audio_id_ = L"DLSS-NR-REGRESSION-NONEXISTENT-ENDPOINT";
            reconnect_audio_name_ = L"Regression missing audio";
            last_audio_retry_ms_ = 0;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT combo_rect{}; GetWindowRect(mode_combo_, &combo_rect);
        if ((PtInRect(&combo_rect, point) || GetFocus() == mode_combo_) && !modes_.empty()) {
            int selected = static_cast<int>(SendMessageW(mode_combo_, CB_GETCURSEL, 0, 0));
            selected += GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
            selected = std::clamp(selected, 0, static_cast<int>(modes_.size()) - 1);
            if (!active_mode_ || static_cast<size_t>(selected) != *active_mode_) start_capture_mode(static_cast<size_t>(selected));
            return 0;
        }
        break;
    }
    case WM_COMMAND: {
        const UINT command = LOWORD(wparam);
        if (command == passthrough_command) { set_motion_analysis(false); return 0; }
        if (command == motion_analysis_command) { set_motion_analysis(true); return 0; }
        if (command == history_overlay_command) { set_history_overlay(!history_overlay_enabled_); return 0; }
        if (command == nr_enable_command) { set_nr_enabled(!nr_enabled_); return 0; }
        if (command == nr_temporal_command) {
            nr_temporal_enabled_ = !nr_temporal_enabled_;
            CheckMenuItem(nr_menu_, nr_temporal_command, MF_BYCOMMAND | (nr_temporal_enabled_ ? MF_CHECKED : MF_UNCHECKED));
            apply_nr_settings(false); return 0;
        }
        if (command >= nr_style_base && command < nr_style_base + 4) {
            for (UINT i = 0; i < 4; ++i) CheckMenuItem(nr_style_menu_, nr_style_base + i, MF_BYCOMMAND | (command == nr_style_base + i ? MF_CHECKED : MF_UNCHECKED));
            nr_style_ = command - nr_style_base; apply_nr_settings(false); return 0;
        }
        if (command > nr_preset_base && command <= nr_preset_base + 4) {
            for (UINT i = 1; i <= 4; ++i) CheckMenuItem(nr_preset_menu_, nr_preset_base + i, MF_BYCOMMAND | (command == nr_preset_base + i ? MF_CHECKED : MF_UNCHECKED));
            nr_preset_ = command - nr_preset_base; apply_nr_settings(false); return 0;
        }
        if (command >= nr_intensity_base && command < nr_intensity_base + 3) {
            const UINT values[] = {50, 75, 100};
            for (UINT i = 0; i < 3; ++i) CheckMenuItem(nr_intensity_menu_, nr_intensity_base + i, MF_BYCOMMAND | (command == nr_intensity_base + i ? MF_CHECKED : MF_UNCHECKED));
            nr_intensity_percent_ = values[command - nr_intensity_base]; apply_nr_settings(false); return 0;
        }
        if (command >= nr_latency_base && command < nr_latency_base + 3) {
            const UINT values[] = {2, 16, 33};
            for (UINT i = 0; i < 3; ++i) CheckMenuItem(nr_latency_menu_, nr_latency_base + i, MF_BYCOMMAND | (command == nr_latency_base + i ? MF_CHECKED : MF_UNCHECKED));
            nr_wait_ms_ = values[command - nr_latency_base]; apply_nr_settings(false); return 0;
        }
        if (command == fullscreen_command) { toggle_fullscreen(); return 0; }
        if (command == always_on_top_command) { set_always_on_top(!always_on_top_); return 0; }
        if (command == auto_size_command) { set_auto_size_to_resolution(!auto_size_to_resolution_); return 0; }
        if (command == diagnostics_command) { copy_diagnostics(); return 0; }
        if (command == refresh_devices_command) { refresh_capture_devices(); return 0; }
        if (command == performance_command) {
            performance_overlay_ = !performance_overlay_;
            CheckMenuItem(view_menu_, performance_command, MF_BYCOMMAND | (performance_overlay_ ? MF_CHECKED : MF_UNCHECKED));
            renderer_.set_performance_text(performance_overlay_ ? L"Measuring performance..." : L"");
            save_settings();
            return 0;
        }
        if (command == audio_off_command) { start_audio_capture(audio_devices_.size()); return 0; }
        if (command >= audio_command_base && command < audio_command_base + audio_devices_.size()) {
            start_audio_capture(command - audio_command_base); return 0;
        }
        if (command == flip_vertical_command) {
            set_vertical_flip(!flip_vertical_.load(std::memory_order_relaxed));
            return 0;
        }
        if (reinterpret_cast<HWND>(lparam) == mode_combo_ && HIWORD(wparam) == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(mode_combo_, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR) start_capture_mode(static_cast<size_t>(selected));
            return 0;
        }
        if (command >= device_command_base && command < device_command_base + devices_.size()) {
            start_capture_device(command - device_command_base); return 0;
        }
        if (command >= format_command_base && command < format_command_base + formats_.size()) {
            start_capture_format(command - format_command_base); return 0;
        }

        if (command >= resolution_command_base && command < resolution_command_base + resolutions_.size()) {
            start_capture_resolution(command - resolution_command_base); return 0;
        }

        if (command >= frame_rate_command_base && command < frame_rate_command_base + frame_rates_.size()) {
            start_capture_frame_rate(command - frame_rate_command_base); return 0;
        }        if (command >= mode_command_base && command < mode_command_base + modes_.size()) {
            start_capture_mode(command - mode_command_base); return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        if (auto_size_to_resolution_ && !fullscreen_) {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            const POINT position{suggested->left, suggested->top};
            resize_window_to_capture(LOWORD(wparam), &position);
            return 0;
        }
        break;
    }
    case WM_SIZE:
        if (mode_combo_) MoveWindow(mode_combo_, 92, 8, std::max(300, static_cast<int>(LOWORD(lparam)) - 100), 520, TRUE);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F11) { toggle_fullscreen(); return 0; }
        if (wparam == VK_ESCAPE) PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0;
    }
    case WM_DESTROY: KillTimer(window, health_timer_id); PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
