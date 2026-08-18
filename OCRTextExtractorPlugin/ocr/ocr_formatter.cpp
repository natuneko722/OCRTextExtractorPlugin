#include "ocr_formatter.hpp"

namespace {
std::wstring NormalizeLine(const std::wstring& line) {
    std::wstring result;
    bool previousWasSpace = false;
    for (wchar_t c : line) {
        if (c == L'\r') continue;
        if (c == L' ' || c == L'\t') { if (!result.empty() && !previousWasSpace) result += L' '; previousWasSpace = true; }
        else { result += c; previousWasSpace = false; }
    }
    while (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}
bool IsLineBreak(wchar_t c) {
    return c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f' || c == L'\x0085' || c == L'\x2028' || c == L'\x2029';
}
bool IsRemovableWhitespace(wchar_t c) {
    return c == L' ' || c == L'\t' || IsLineBreak(c) || c == L'\x00A0' || c == L'\x1680' ||
        (c >= L'\x2000' && c <= L'\x200A') || c == L'\x202F' || c == L'\x205F' || c == L'\x3000' || c == L'\xFEFF';
}
bool IsInlineWhitespace(wchar_t c) { return IsRemovableWhitespace(c) && !IsLineBreak(c); }
bool IsJapaneseTextCharacter(wchar_t c) {
    return (c >= L'\x3040' && c <= L'\x30FF') ||   // ひらがな・カタカナ
        (c >= L'\x31F0' && c <= L'\x31FF') ||      // カタカナ音声拡張
        (c >= L'\x3400' && c <= L'\x4DBF') ||      // CJK 拡張A
        (c >= L'\x4E00' && c <= L'\x9FFF') ||      // CJK 統合漢字
        (c >= L'\xF900' && c <= L'\xFAFF') ||      // CJK 互換漢字
        (c >= L'\xFF66' && c <= L'\xFF9D') ||      // 半角カタカナ
        (c >= L'\x3005' && c <= L'\x3007');        // 々・〆・〇
}
std::wstring RemoveJapaneseAdjacentWhitespace(const std::wstring& source) {
    std::wstring result;
    result.reserve(source.size());
    for (size_t start = 0; start < source.size();) {
        if (!IsInlineWhitespace(source[start])) { result += source[start++]; continue; }
        size_t end = start + 1;
        while (end < source.size() && IsInlineWhitespace(source[end])) ++end;
        const wchar_t previous = result.empty() ? L'\0' : result.back();
        const wchar_t next = end < source.size() ? source[end] : L'\0';
        // OCR が日本語の文字境界へ誤って入れた空白だけを除去し、英単語間の空白は残す。
        if (!IsJapaneseTextCharacter(previous) && !IsJapaneseTextCharacter(next)) result.append(source, start, end - start);
        start = end;
    }
    return result;
}
bool IsAsciiWord(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); }
}

std::wstring OCRFormatter::Normalize(const std::wstring& source) {
    std::wstring result;
    std::wstring currentLine;
    bool paragraphBreak = false;
    for (size_t index = 0; index <= source.size(); ++index) {
        if (index != source.size() && !IsLineBreak(source[index])) {
            currentLine += source[index];
            continue;
        }
        std::wstring line = NormalizeLine(currentLine);
        if (!line.empty()) {
            if (!result.empty()) {
                if (paragraphBreak) result += L'\n';
                else if (IsAsciiWord(result.back()) && IsAsciiWord(line.front())) result += L' ';
            }
            result += line;
            paragraphBreak = false;
        } else if (!result.empty()) {
            paragraphBreak = true;
        }
        currentLine.clear();
        if (index + 1 < source.size() && source[index] == L'\r' && source[index + 1] == L'\n') ++index;
    }
    return RemoveJapaneseAdjacentWhitespace(result);
}
