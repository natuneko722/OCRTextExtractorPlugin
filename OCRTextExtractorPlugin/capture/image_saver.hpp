#pragma once
#include "capture_object.hpp"
#include <string>

bool SaveCapturedBitmapAsPng(const CapturedBitmap& bitmap, const std::wstring& directory, const std::wstring& projectName, std::wstring& savedPath, std::wstring& error);
