#pragma once
#include "capture/capture_object.hpp"
#include <functional>
#include <string>

class OCRManager {
public:
    // 非同期で Windows.Media.Ocr を実行する。完了コールバックは UI スレッドへ PostMessage する側で受ける。
    void RecognizeAsync(CapturedBitmap bitmap, bool enhanceLowResolution, std::function<void(std::wstring, std::wstring)> completed) const;
    bool IsAvailable(std::wstring& reason) const;
};
