#pragma once
#include <windows.h>
#include <string>

struct OCRResult {
    std::wstring text;
    SYSTEMTIME time{};
    RECT captureRect{};
};
