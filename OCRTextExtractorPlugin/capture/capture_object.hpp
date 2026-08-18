#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct CapturedBitmap {
    int width{};
    int height{};
    std::vector<unsigned char> bgra;
};

class CaptureObject {
public:
    static bool CaptureScreenRect(const RECT& rect, CapturedBitmap& bitmap, std::wstring& error);
    static bool CropBitmap(const CapturedBitmap& source, const RECT& rect, CapturedBitmap& bitmap, std::wstring& error);
};
