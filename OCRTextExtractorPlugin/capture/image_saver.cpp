#include "image_saver.hpp"
#include <windows.h>
#include <wincodec.h>
#include <filesystem>

bool SaveCapturedBitmapAsPng(const CapturedBitmap& bitmap, const std::wstring& directory, const std::wstring& projectName, std::wstring& savedPath, std::wstring& error) {
    if (bitmap.bgra.empty() || directory.empty()) { error = L"保存する画像または保存先がありません。"; return false; }
    std::error_code ec; std::filesystem::create_directories(directory, ec);
    if (ec) { error = L"保存先フォルダを作成できません。"; return false; }
    std::wstring safeProjectName = projectName.empty() ? L"NoProject" : projectName;
    for (auto& c : safeProjectName) if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'\"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
    SYSTEMTIME time{}; GetLocalTime(&time); wchar_t name[512]; swprintf_s(name, L"OCR_%s_%04d%02d%02d_%02d%02d%02d.png", safeProjectName.c_str(), time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::filesystem::path path = std::filesystem::path(directory) / name;
    int suffix = 2; while (std::filesystem::exists(path)) { swprintf_s(name, L"OCR_%s_%04d%02d%02d_%02d%02d%02d_%d.png", safeProjectName.c_str(), time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, suffix++); path = std::filesystem::path(directory) / name; }
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory{}; IWICBitmap* image{}; IWICStream* stream{}; IWICBitmapEncoder* encoder{}; IWICBitmapFrameEncode* frame{}; IPropertyBag2* properties{};
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromMemory(bitmap.width, bitmap.height, GUID_WICPixelFormat32bppBGRA, bitmap.width * 4, static_cast<UINT>(bitmap.bgra.size()), const_cast<BYTE*>(bitmap.bgra.data()), &image);
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(hr)) hr = frame->Initialize(properties);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(image, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (properties) properties->Release(); if (frame) frame->Release(); if (encoder) encoder->Release(); if (stream) stream->Release(); if (image) image->Release(); if (factory) factory->Release(); if (SUCCEEDED(apartment)) CoUninitialize();
    if (FAILED(hr)) { error = L"PNG画像の保存に失敗しました。"; return false; }
    savedPath = path.wstring(); return true;
}
