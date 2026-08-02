#include "ocr_formatter.hpp"

std::wstring OCRFormatter::Normalize(const std::wstring& source) {
    std::wstring result;
    bool previousWasSpace = false;
    for (wchar_t c : source) {
        if (c == L'\r') continue;
        if (c == L'\n') { while (!result.empty() && result.back() == L' ') result.pop_back(); result += c; previousWasSpace = false; continue; }
        if (c == L' ' || c == L'\t') { if (!previousWasSpace) result += L' '; previousWasSpace = true; }
        else { result += c; previousWasSpace = false; }
    }
    while (!result.empty() && (result.back() == L' ' || result.back() == L'\n')) result.pop_back();
    return result;
}
