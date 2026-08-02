#include "capture_object.hpp"

bool CaptureObject::CaptureScreenRect(const RECT& rect, CapturedBitmap& bitmap, std::wstring& error) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) { error = L"範囲が選択されていません。"; return false; }
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, image);
    const bool copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    bitmap.width = width; bitmap.height = height; bitmap.bgra.resize(static_cast<size_t>(width) * height * 4);
    const bool read = copied && GetDIBits(memory, image, 0, height, bitmap.bgra.data(), &info, DIB_RGB_COLORS) != 0;
    SelectObject(memory, previous); DeleteObject(image); DeleteDC(memory); ReleaseDC(nullptr, screen);
    if (!read) { error = L"画面のキャプチャに失敗しました。"; bitmap = {}; }
    return read;
}
