#include "ocr_manager.hpp"
#include "ocr_formatter.hpp"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <thread>

using namespace winrt;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Ocr;
using namespace Windows::Storage::Streams;

bool OCRManager::IsAvailable(std::wstring& reason) const {
    try { init_apartment(apartment_type::multi_threaded); if (!OcrEngine::IsLanguageSupported(Windows::Globalization::Language(L"ja-JP"))) reason = L"日本語 OCR 言語パックがインストールされていません。"; else return true; }
    catch (const hresult_error& e) { reason = e.message().c_str(); }
    return false;
}

void OCRManager::RecognizeAsync(CapturedBitmap bitmap, bool autoFormat, bool enhanceLowResolution, std::function<void(std::wstring, std::wstring)> completed) const {
    std::thread([bitmap = std::move(bitmap), autoFormat, enhanceLowResolution, completed = std::move(completed)]() mutable {
        try {
            if (enhanceLowResolution && (bitmap.width < 1280 || bitmap.height < 720)) {
                const int width = bitmap.width * 2, height = bitmap.height * 2; std::vector<unsigned char> enlarged(static_cast<size_t>(width) * height * 4);
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) { const size_t source = (static_cast<size_t>(y / 2) * bitmap.width + x / 2) * 4, target = (static_cast<size_t>(y) * width + x) * 4; memcpy(enlarged.data() + target, bitmap.bgra.data() + source, 4); }
                bitmap.width = width; bitmap.height = height; bitmap.bgra = std::move(enlarged);
            }
            init_apartment(apartment_type::multi_threaded);
            auto language = Windows::Globalization::Language(L"ja-JP");
            if (!OcrEngine::IsLanguageSupported(language)) { completed(L"", L"日本語 OCR 言語パックが見つかりません。"); return; }
            auto softwareBitmap = SoftwareBitmap(BitmapPixelFormat::Bgra8, bitmap.width, bitmap.height, BitmapAlphaMode::Premultiplied);
            DataWriter writer; writer.WriteBytes(array_view<const uint8_t>(bitmap.bgra));
            softwareBitmap.CopyFromBuffer(writer.DetachBuffer());
            auto engine = OcrEngine::TryCreateFromLanguage(language);
            if (!engine) { completed(L"", L"Windows OCR エンジンを作成できません。" ); return; }
            auto recognized = engine.RecognizeAsync(softwareBitmap).get();
            const std::wstring text = recognized.Text().c_str(); completed(autoFormat ? OCRFormatter::Normalize(text) : text, L"");
        } catch (const hresult_error& e) { completed(L"", e.message().c_str()); }
        catch (...) { completed(L"", L"OCR の実行中に予期しないエラーが発生しました。" ); }
    }).detach();
}
