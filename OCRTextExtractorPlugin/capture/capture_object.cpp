#include "capture_object.hpp"

bool CaptureObject::CaptureScreenRect(const RECT& rect, CapturedBitmap& bitmap, std::wstring& error) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) { error = L"範囲が選択されていません。"; return false; }
    HDC screen = GetDC(nullptr);
    if (!screen) { error = L"画面デバイスの取得に失敗しました。"; return false; }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) { ReleaseDC(nullptr, screen); error = L"画面キャプチャ用のメモリを確保できません。"; return false; }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP image = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!image) { DeleteDC(memory); ReleaseDC(nullptr, screen); error = L"画面キャプチャ用の画像を作成できません。"; return false; }
    HGDIOBJ previous = SelectObject(memory, image);
    if (!previous || previous == HGDI_ERROR) { DeleteObject(image); DeleteDC(memory); ReleaseDC(nullptr, screen); error = L"画面キャプチャの初期化に失敗しました。"; return false; }
    const bool copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    bitmap.width = width; bitmap.height = height; bitmap.bgra.resize(static_cast<size_t>(width) * height * 4);
    const bool read = copied && pixels;
    if (read) memcpy(bitmap.bgra.data(), pixels, bitmap.bgra.size());
    SelectObject(memory, previous); DeleteObject(image); DeleteDC(memory); ReleaseDC(nullptr, screen);
    if (!read) { error = L"画面のキャプチャに失敗しました。"; bitmap = {}; }
    return read;
}

bool CaptureObject::CropBitmap(const CapturedBitmap& source, const RECT& rect, CapturedBitmap& bitmap, std::wstring& error) {
    if (source.width <= 0 || source.height <= 0 || source.bgra.size() < static_cast<size_t>(source.width) * source.height * 4) {
        error = L"映像フレームを取得できていません。";
        return false;
    }
    const RECT bounds{ 0, 0, source.width, source.height };
    RECT clipped{};
    if (!IntersectRect(&clipped, &rect, &bounds) || clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
        error = L"選択範囲が映像の範囲外です。";
        return false;
    }
    const int width = clipped.right - clipped.left, height = clipped.bottom - clipped.top;
    bitmap.width = width; bitmap.height = height; bitmap.bgra.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const auto* from = source.bgra.data() + (static_cast<size_t>(clipped.top + y) * source.width + clipped.left) * 4;
        auto* to = bitmap.bgra.data() + static_cast<size_t>(y) * width * 4;
        memcpy(to, from, static_cast<size_t>(width) * 4);
    }
    return true;
}
