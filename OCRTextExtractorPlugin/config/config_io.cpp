#include "config_io.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <algorithm>

void ConfigIO::SetDirectory(const std::wstring& directory) { path_ = directory + L"\\OCRTextExtractor.history.tsv"; settingsPath_ = directory + L"\\OCRTextExtractor.settings.ini"; }
std::wstring ConfigIO::GetImageSaveDirectory() const { wchar_t value[32768]{}; GetPrivateProfileStringW(L"Image", L"SaveDirectory", L"", value, static_cast<DWORD>(std::size(value)), settingsPath_.c_str()); return value; }
void ConfigIO::SetImageSaveDirectory(const std::wstring& directory) const { WritePrivateProfileStringW(L"Image", L"SaveDirectory", directory.c_str(), settingsPath_.c_str()); }
bool ConfigIO::GetPlaceImageOnTimeline() const { return GetPrivateProfileIntW(L"Image", L"PlaceOnTimeline", 0, settingsPath_.c_str()) != 0; }
void ConfigIO::SetPlaceImageOnTimeline(bool enabled) const { WritePrivateProfileStringW(L"Image", L"PlaceOnTimeline", enabled ? L"1" : L"0", settingsPath_.c_str()); }
bool ConfigIO::GetAutoFormatOcrText() const { return GetPrivateProfileIntW(L"OCR", L"AutoFormatText", 1, settingsPath_.c_str()) != 0; }
void ConfigIO::SetAutoFormatOcrText(bool enabled) const { WritePrivateProfileStringW(L"OCR", L"AutoFormatText", enabled ? L"1" : L"0", settingsPath_.c_str()); }
bool ConfigIO::GetEnhanceLowResolution() const { return GetPrivateProfileIntW(L"OCR", L"EnhanceLowResolution", 0, settingsPath_.c_str()) != 0; }
void ConfigIO::SetEnhanceLowResolution(bool enabled) const { WritePrivateProfileStringW(L"OCR", L"EnhanceLowResolution", enabled ? L"1" : L"0", settingsPath_.c_str()); }
bool ConfigIO::GetAutoRecognize() const { return GetPrivateProfileIntW(L"OCR", L"AutoRecognize", 0, settingsPath_.c_str()) != 0; }
void ConfigIO::SetAutoRecognize(bool enabled) const { WritePrivateProfileStringW(L"OCR", L"AutoRecognize", enabled ? L"1" : L"0", settingsPath_.c_str()); }
int ConfigIO::GetAutoRecognizeSeconds() const { return (std::max)(1, static_cast<int>(GetPrivateProfileIntW(L"OCR", L"AutoRecognizeSeconds", 3, settingsPath_.c_str()))); }
void ConfigIO::SetAutoRecognizeSeconds(int seconds) const { wchar_t value[16]; swprintf_s(value, L"%d", (std::max)(1, seconds)); WritePrivateProfileStringW(L"OCR", L"AutoRecognizeSeconds", value, settingsPath_.c_str()); }
bool ConfigIO::GetAutoCopy() const { return GetPrivateProfileIntW(L"OCR", L"AutoCopy", 0, settingsPath_.c_str()) != 0; }
void ConfigIO::SetAutoCopy(bool enabled) const { WritePrivateProfileStringW(L"OCR", L"AutoCopy", enabled ? L"1" : L"0", settingsPath_.c_str()); }
void ConfigIO::LoadHistory(HistoryManager& history) const {
    if (path_.empty()) return;
    std::wifstream stream(path_); if (!stream) return;
    std::vector<OCRResult> values; OCRResult value;
    while (stream >> value.time.wYear >> value.time.wMonth >> value.time.wDay >> value.time.wHour >> value.time.wMinute >> value.time.wSecond
        >> value.captureRect.left >> value.captureRect.top >> value.captureRect.right >> value.captureRect.bottom) {
        stream.get(); std::getline(stream, value.text);
        for (size_t pos; (pos = value.text.find(L"\\n")) != std::wstring::npos;) value.text.replace(pos, 2, L"\n");
        values.push_back(value);
    }
    history.Replace(std::move(values));
}
void ConfigIO::SaveHistory(const HistoryManager& history) const {
    if (path_.empty()) return;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
    std::wofstream stream(path_, std::ios::trunc); if (!stream) return;
    for (const auto& value : history.Items()) {
        auto text = value.text; for (size_t pos = 0; (pos = text.find(L'\n', pos)) != std::wstring::npos; pos += 2) text.replace(pos, 1, L"\\n");
        stream << value.time.wYear << L' ' << value.time.wMonth << L' ' << value.time.wDay << L' ' << value.time.wHour << L' ' << value.time.wMinute << L' ' << value.time.wSecond << L' '
            << value.captureRect.left << L' ' << value.captureRect.top << L' ' << value.captureRect.right << L' ' << value.captureRect.bottom << L' ' << text << L'\n';
    }
}
