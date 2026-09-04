#pragma once
#include <cstdint>
#include <string>

class DiagnosticsReport {
public:
    void add(const std::wstring& label, const std::wstring& value) {
        text_ += label + L": " + value + L"\r\n";
    }
    void add(const std::wstring& label, uint64_t value) {
        add(label, std::to_wstring(value));
    }
    const std::wstring& text() const noexcept { return text_; }
private:
    std::wstring text_ = L"DLSS NR Capture diagnostics\r\nLive sample (fields may span adjacent frames)\r\n";
};
