#pragma once
#include "history/history_manager.hpp"
#include <string>

class ConfigIO {
public:
    void SetDirectory(const std::wstring& directory);
    void LoadHistory(HistoryManager& history) const;
    void SaveHistory(const HistoryManager& history) const;
    std::wstring GetImageSaveDirectory() const;
    void SetImageSaveDirectory(const std::wstring& directory) const;
    bool GetPlaceImageOnTimeline() const;
    void SetPlaceImageOnTimeline(bool enabled) const;
    bool GetAutoFormatOcrText() const;
    void SetAutoFormatOcrText(bool enabled) const;
    bool GetEnhanceLowResolution() const;
    void SetEnhanceLowResolution(bool enabled) const;
    bool GetAutoRecognize() const;
    void SetAutoRecognize(bool enabled) const;
    int GetAutoRecognizeSeconds() const;
    void SetAutoRecognizeSeconds(int seconds) const;
    bool GetAutoCopy() const;
    void SetAutoCopy(bool enabled) const;
private:
    std::wstring path_;
    std::wstring settingsPath_;
};
