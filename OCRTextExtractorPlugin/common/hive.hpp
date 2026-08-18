#pragma once
#include "ocr/ocr_manager.hpp"
#include "history/history_manager.hpp"
#include "config/config_io.hpp"
#include <functional>

// プラグイン内で共有する状態。UI はここを介して OCR と履歴だけを利用する。
struct Hive {
    OCRManager ocr;
    HistoryManager history;
    ConfigIO config;
    std::function<std::wstring()> getProjectFilePath;
    std::function<bool(const std::wstring&)> placeImageOnTimeline;
    std::function<void(std::function<void(CapturedBitmap, std::wstring)>)> renderCurrentScene;
};
