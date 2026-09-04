#pragma once
#include <windows.h>
#include <string>
class EventLog {
    std::wstring path_;
    DWORD limit_{1024 * 1024};
public:
    void set_path(std::wstring path, DWORD limit = 1024 * 1024) { path_ = std::move(path); limit_ = limit; }
    const std::wstring& path() const { return path_; }
    bool append(const std::wstring& message) noexcept {
        if (path_.empty()) return false;
        try {
            SYSTEMTIME now{}; GetSystemTime(&now);
            wchar_t stamp[64]{};
            swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02uZ [%lu] ",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
            std::wstring line = stamp + message.substr(0, 4000) + L"\r\n";
            for (auto& ch : line) if (ch == L'\0') ch = L' ';
            const int size = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
            std::string bytes(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), bytes.data(), size, nullptr, nullptr);
            HANDLE file = CreateFileW(path_.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER length{};
            if (GetFileSizeEx(file, &length) && length.QuadPart + size > limit_) {
                CloseHandle(file);
                if (!MoveFileExW(path_.c_str(), (path_ + L".previous").c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
                file = CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) return false;
            }
            DWORD written{};
            const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
            CloseHandle(file); return ok;
        } catch (...) { return false; } // Logging must not interrupt capture.
    }
};
