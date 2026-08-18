#include "ocr_window.hpp"
#include "capture/capture_object.hpp"
#include "capture/image_saver.hpp"
#include "ocr/ocr_formatter.hpp"
#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>

namespace {
constexpr wchar_t kClassName[] = L"AviUtl2OcrTextExtractor";
constexpr wchar_t kOverlayClassName[] = L"AviUtl2OcrSelectionOverlay";
constexpr wchar_t kFrameSelectionClassName[] = L"AviUtl2OcrFrameSelection";
constexpr UINT WM_OCR_COMPLETED = WM_APP + 42;
constexpr UINT WM_SCENE_FRAME_RENDERED = WM_APP + 43;
constexpr int kSettingsOcrCardTop = 52;
constexpr int kSettingsOcrCardBottom = 284;
constexpr int kSettingsSaveCardTop = 292;
constexpr int kSettingsSaveCardBottom = 492;
enum : int { IDC_OPTIONS = 1001, IDC_SELECT, IDC_RECOGNIZE, IDC_COPY, IDC_SAVE_IMAGE, IDC_TEXT, IDC_HISTORY, IDC_X, IDC_Y, IDC_WIDTH, IDC_HEIGHT, IDC_STATUS, IDC_PREVIEW,
    IDC_SETTINGS_CLOSE = 1101, IDC_PLACE_TIMELINE, IDC_SAVE_PROJECT, IDC_SAVE_CUSTOM, IDC_SAVE_PATH, IDC_SAVE_BROWSE, IDC_PROJECT_PATH, IDC_AUTO_FORMAT, IDC_ENHANCE_LOW_RES, IDC_AUTO_RECOGNIZE, IDC_AUTO_SECONDS, IDC_AUTO_COPY, IDC_SOURCE_SCENE, IDC_SOURCE_SCREEN };
constexpr UINT_PTR AUTO_RECOGNIZE_TIMER = 1;
struct Completion { std::wstring text, error; RECT rect{}; };
enum class SceneAction { Select, Recognize, Save };
struct SceneCompletion { CapturedBitmap bitmap; std::wstring error; SceneAction action{}; };
struct State {
    Hive* hive{}; RECT rect{};
    HWND text{}, history{}, status{}, preview{}, x{}, y{}, width{}, height{};
    HBITMAP previewImage{};
    CapturedBitmap captured;
    bool hasCaptured{};
    bool settingsMode{};
    HFONT titleFont{};
    HFONT sectionFont{};
    bool recognizing{};
    CapturedBitmap sourceFrame;
    bool hasSourceFrame{};
    bool renderingFrame{};
};
struct OverlayState { POINT start{}, current{}; bool dragging{}, done{}, cancelled{}; };
struct FrameSelectionState { const CapturedBitmap* bitmap{}; RECT selection{}; POINT start{}; bool dragging{}, done{}, cancelled{}; RECT viewport{}; };
void Layout(HWND hwnd, State* state);

std::wstring Timestamp(const SYSTEMTIME& time) {
    wchar_t buffer[32]; swprintf_s(buffer, L"%02d:%02d:%02d", time.wHour, time.wMinute, time.wSecond); return buffer;
}
void RefreshHistory(State* state) {
    SendMessage(state->history, LB_RESETCONTENT, 0, 0);
    for (const auto& item : state->hive->history.Items()) {
        const std::wstring first = item.text.substr(0, item.text.find(L'\n'));
        const std::wstring label = Timestamp(item.time) + L"  " + first;
        SendMessage(state->history, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
}
void ApplyTextBoxStyle(HWND control) {
    // 履歴欄に近い濃いグレーのテーマにする（純黒より目に優しい表示）。
    SendMessageW(control, EM_SETBKGNDCOLOR, 0, RGB(48, 48, 48));
    // 折返しではなく横スクロールで表示し、実際の改行だけを判別できるようにする。
    SendMessageW(control, EM_SETTARGETDEVICE, 0, 0);
    CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = RGB(235, 235, 235);
    SendMessageW(control, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
    SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
}
bool IsTextLineBreak(wchar_t c) {
    return c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f' || c == L'\x0085' || c == L'\x2028' || c == L'\x2029';
}
int CountLineBreaks(const std::wstring& text) {
    int count = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if (!IsTextLineBreak(text[index])) continue;
        ++count;
        if (text[index] == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') ++index;
    }
    return count;
}
void UpdateAutoRecognitionTimer(HWND hwnd, State* state) {
    KillTimer(hwnd, AUTO_RECOGNIZE_TIMER);
    if (state->hive->config.GetAutoRecognize()) SetTimer(hwnd, AUTO_RECOGNIZE_TIMER, static_cast<UINT>(state->hive->config.GetAutoRecognizeSeconds() * 1000), nullptr);
}
int EditInt(HWND control) {
    wchar_t value[32]{}; GetWindowTextW(control, value, static_cast<int>(std::size(value)));
    return _wtoi(value);
}
void SetEditInt(HWND control, int value) {
    wchar_t text[32]; swprintf_s(text, L"%d", value); SetWindowTextW(control, text);
}
void UpdateRectControls(State* state) {
    SetEditInt(state->x, state->rect.left); SetEditInt(state->y, state->rect.top);
    SetEditInt(state->width, state->rect.right - state->rect.left); SetEditInt(state->height, state->rect.bottom - state->rect.top);
}
bool ReadRectControls(State* state, RECT& result) {
    const int x = EditInt(state->x), y = EditInt(state->y), width = EditInt(state->width), height = EditInt(state->height);
    if (width <= 0 || height <= 0) return false;
    result = { x, y, x + width, y + height }; return true;
}
void DeletePreviewImages(HBITMAP ownedImage, HBITMAP controlImage) {
    if (ownedImage) DeleteObject(ownedImage);
    if (controlImage && controlImage != ownedImage) DeleteObject(controlImage);
}
void ClearPreviewImage(State* state) {
    HBITMAP controlImage = nullptr;
    if (state->preview && IsWindow(state->preview)) {
        controlImage = reinterpret_cast<HBITMAP>(SendMessageW(state->preview, STM_SETIMAGE, IMAGE_BITMAP, 0));
    }
    const HBITMAP ownedImage = state->previewImage;
    state->previewImage = nullptr;
    DeletePreviewImages(ownedImage, controlImage);
}
void ShowPreview(State* state, const CapturedBitmap& bitmap) {
    if (!state->preview || bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.bgra.size() < static_cast<size_t>(bitmap.width) * static_cast<size_t>(bitmap.height) * 4) return;
    RECT bounds{}; GetClientRect(state->preview, &bounds);
    const int maxWidth = (std::max)(1, static_cast<int>(bounds.right) - 8), maxHeight = (std::max)(1, static_cast<int>(bounds.bottom) - 8);
    const double scale = (std::min)(1.0, (std::min)(static_cast<double>(maxWidth) / bitmap.width, static_cast<double>(maxHeight) / bitmap.height));
    const int width = (std::max)(1, static_cast<int>(bitmap.width * scale)), height = (std::max)(1, static_cast<int>(bitmap.height * scale));
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = bitmap.width; info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr; HBITMAP source = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!source || !pixels) { if (source) DeleteObject(source); return; }
    memcpy(pixels, bitmap.bgra.data(), bitmap.bgra.size());
    HDC target = GetDC(state->preview);
    HDC from = target ? CreateCompatibleDC(target) : nullptr;
    HDC to = target ? CreateCompatibleDC(target) : nullptr;
    HBITMAP thumb = target && from && to ? CreateCompatibleBitmap(target, width, height) : nullptr;
    HGDIOBJ previousSource = nullptr, previousThumb = nullptr;
    bool rendered = thumb != nullptr;
    if (rendered) {
        previousSource = SelectObject(from, source); previousThumb = SelectObject(to, thumb);
        rendered = previousSource != nullptr && previousSource != HGDI_ERROR && previousThumb != nullptr && previousThumb != HGDI_ERROR;
        if (rendered) {
            SetStretchBltMode(to, HALFTONE);
            rendered = StretchBlt(to, 0, 0, width, height, from, 0, 0, bitmap.width, bitmap.height, SRCCOPY) != FALSE;
        }
    }
    if (previousThumb && previousThumb != HGDI_ERROR) SelectObject(to, previousThumb);
    if (previousSource && previousSource != HGDI_ERROR) SelectObject(from, previousSource);
    if (to) DeleteDC(to); if (from) DeleteDC(from); if (target) ReleaseDC(state->preview, target); DeleteObject(source);
    if (!rendered) { if (thumb) DeleteObject(thumb); return; }
    const HBITMAP controlImage = reinterpret_cast<HBITMAP>(SendMessageW(state->preview, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(thumb)));
    const HBITMAP ownedImage = state->previewImage;
    state->previewImage = thumb;
    DeletePreviewImages(ownedImage, controlImage);
    InvalidateRect(state->preview, nullptr, FALSE);
}
bool UpdateCapturedSelection(State* state, std::wstring& error) {
    if (!ReadRectControls(state, state->rect)) { error = L"幅と高さには1以上の数値を入力してください。"; return false; }
    const bool useScreen = state->hive->config.GetUseScreenCapture();
    if (useScreen) {
        if (!CaptureObject::CaptureScreenRect(state->rect, state->captured, error)) return false;
    } else {
        if (!state->hasSourceFrame || !CaptureObject::CropBitmap(state->sourceFrame, state->rect, state->captured, error)) return false;
    }
    state->hasCaptured = true; ShowPreview(state, state->captured); return true;
}
void ResetCaptureState(State* state) {
    state->rect = {}; state->captured = {}; state->sourceFrame = {}; state->hasCaptured = false; state->hasSourceFrame = false;
    UpdateRectControls(state);
    ClearPreviewImage(state);
}
void StartRecognition(HWND hwnd, State* state) {
    SetWindowTextW(state->status, L"Windows OCRで認識中…"); const RECT rect = state->rect;
    state->recognizing = true;
    state->hive->ocr.RecognizeAsync(state->captured, state->hive->config.GetEnhanceLowResolution(), [hwnd, rect](std::wstring text, std::wstring error) {
        auto* done = new Completion{ std::move(text), std::move(error), rect };
        if (!PostMessageW(hwnd, WM_OCR_COMPLETED, 0, reinterpret_cast<LPARAM>(done))) delete done;
    });
}
void SaveSelection(HWND hwnd, State* state) {
    const std::wstring projectPath = state->hive->getProjectFilePath ? state->hive->getProjectFilePath() : L"";
    std::wstring directory = state->hive->config.GetImageSaveDirectory();
    if (directory.empty() && !projectPath.empty()) directory = std::filesystem::path(projectPath).parent_path().wstring();
    if (directory.empty()) { SetWindowTextW(state->status, L"プロジェクトを保存するか、設定から画像保存先を指定してください。"); return; }
    const std::wstring projectName = projectPath.empty() ? L"NoProject" : std::filesystem::path(projectPath).stem().wstring();
    std::wstring saved, error;
    if (!SaveCapturedBitmapAsPng(state->captured, directory, projectName, saved, error)) { SetWindowTextW(state->status, error.c_str()); return; }
    if (state->hive->config.GetPlaceImageOnTimeline() && (!state->hive->placeImageOnTimeline || !state->hive->placeImageOnTimeline(saved))) { SetWindowTextW(state->status, (L"画像は保存しましたが、タイムラインへの配置に失敗しました: " + saved).c_str()); return; }
    SetWindowTextW(state->status, (L"画像を保存しました: " + saved).c_str());
}
void RequestSceneFrame(HWND hwnd, State* state, SceneAction action) {
    if (state->renderingFrame) return;
    if (!state->hive->renderCurrentScene) { SetWindowTextW(state->status, L"AviUtl2映像の取得機能を初期化できませんでした。"); return; }
    state->renderingFrame = true;
    EnableWindow(GetDlgItem(hwnd, IDC_SOURCE_SCENE), FALSE); EnableWindow(GetDlgItem(hwnd, IDC_SOURCE_SCREEN), FALSE);
    SetWindowTextW(state->status, L"AviUtl2の現在フレームを取得中…");
    state->hive->renderCurrentScene([hwnd, action](CapturedBitmap bitmap, std::wstring error) {
        auto* done = new SceneCompletion{ std::move(bitmap), std::move(error), action };
        if (!PostMessageW(hwnd, WM_SCENE_FRAME_RENDERED, 0, reinterpret_cast<LPARAM>(done))) delete done;
    });
}
bool ChooseSaveDirectory(HWND owner, std::wstring& directory) {
    BROWSEINFOW browse{}; browse.hwndOwner = owner; browse.lpszTitle = L"画像の保存先フォルダを選択"; browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse); if (!item) return false;
    wchar_t path[MAX_PATH]{}; const bool selected = SHGetPathFromIDListW(item, path) != FALSE; CoTaskMemFree(item);
    if (selected) directory = path; return selected;
}
void SetSaveDestination(HWND hwnd, bool custom) {
    SendMessageW(GetDlgItem(hwnd, IDC_SAVE_PROJECT), BM_SETCHECK, custom ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, IDC_SAVE_CUSTOM), BM_SETCHECK, custom ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(GetDlgItem(hwnd, IDC_SAVE_PATH), custom); EnableWindow(GetDlgItem(hwnd, IDC_SAVE_BROWSE), custom);
}
void ShowSettings(HWND hwnd, State* state, bool visible) {
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    state->settingsMode = visible;
    const int normal[] = { IDC_OPTIONS, IDC_SELECT, IDC_RECOGNIZE, IDC_COPY, IDC_SAVE_IMAGE, IDC_TEXT, IDC_HISTORY, IDC_X, IDC_Y, IDC_WIDTH, IDC_HEIGHT, IDC_STATUS, IDC_PREVIEW, 2001, 2002, 2003, 2004, 2005, 2006, 2007 };
    const int settings[] = { IDC_SETTINGS_CLOSE, IDC_PLACE_TIMELINE, IDC_SAVE_PROJECT, IDC_SAVE_CUSTOM, IDC_PROJECT_PATH, IDC_SAVE_PATH, IDC_SAVE_BROWSE, IDC_AUTO_FORMAT, IDC_ENHANCE_LOW_RES, IDC_AUTO_RECOGNIZE, IDC_AUTO_SECONDS, IDC_AUTO_COPY, IDC_SOURCE_SCENE, IDC_SOURCE_SCREEN, 3001, 3002, 3003, 3004, 3005 };
    for (int id : normal) ShowWindow(GetDlgItem(hwnd, id), visible ? SW_HIDE : SW_SHOW);
    for (int id : settings) ShowWindow(GetDlgItem(hwnd, id), visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        const std::wstring path = state->hive->config.GetImageSaveDirectory();
        SetSaveDestination(hwnd, !path.empty()); SetWindowTextW(GetDlgItem(hwnd, IDC_SAVE_PATH), path.empty() ? L"保存先選択後に表示されます" : path.c_str());
        const std::wstring projectFile = state->hive->getProjectFilePath ? state->hive->getProjectFilePath() : L"";
        const std::wstring projectFolder = projectFile.empty() ? L"プロジェクトが未保存です" : std::filesystem::path(projectFile).parent_path().wstring();
        SetWindowTextW(GetDlgItem(hwnd, IDC_PROJECT_PATH), projectFolder.c_str());
        SendMessageW(GetDlgItem(hwnd, IDC_PLACE_TIMELINE), BM_SETCHECK, state->hive->config.GetPlaceImageOnTimeline() ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd, IDC_AUTO_FORMAT), BM_SETCHECK, state->hive->config.GetAutoFormatOcrText() ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd, IDC_ENHANCE_LOW_RES), BM_SETCHECK, state->hive->config.GetEnhanceLowResolution() ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(hwnd, IDC_AUTO_RECOGNIZE), BM_SETCHECK, state->hive->config.GetAutoRecognize() ? BST_CHECKED : BST_UNCHECKED, 0); SetEditInt(GetDlgItem(hwnd, IDC_AUTO_SECONDS), state->hive->config.GetAutoRecognizeSeconds());
        SendMessageW(GetDlgItem(hwnd, IDC_AUTO_COPY), BM_SETCHECK, state->hive->config.GetAutoCopy() ? BST_CHECKED : BST_UNCHECKED, 0);
        const bool useScreen = state->hive->config.GetUseScreenCapture();
        SendMessageW(GetDlgItem(hwnd, IDC_SOURCE_SCENE), BM_SETCHECK, useScreen ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(GetDlgItem(hwnd, IDC_SOURCE_SCREEN), BM_SETCHECK, useScreen ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    Layout(hwnd, state);
    if (!visible && state->hasCaptured) ShowPreview(state, state->captured);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
RECT NormalizedRect(const OverlayState& state) {
    return { (std::min)(state.start.x, state.current.x), (std::min)(state.start.y, state.current.y), (std::max)(state.start.x, state.current.x), (std::max)(state.start.y, state.current.y) };
}
LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { state = reinterpret_cast<OverlayState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_LBUTTONDOWN: state->start = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }; state->current = state->start; state->dragging = true; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE: if (state->dragging) { state->current = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }; InvalidateRect(hwnd, nullptr, FALSE); } return 0;
    case WM_LBUTTONUP: if (state->dragging) { state->current = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }; state->dragging = false; ReleaseCapture(); state->done = true; } return 0;
    case WM_KEYDOWN: if (wparam == VK_ESCAPE) { state->cancelled = true; state->done = true; } return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); RECT client{}; GetClientRect(hwnd, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (state->dragging) { RECT selection = NormalizedRect(*state); HBRUSH clear = CreateSolidBrush(RGB(255, 255, 255)); FrameRect(dc, &selection, clear); InflateRect(&selection, -2, -2); FrameRect(dc, &selection, clear); DeleteObject(clear); }
        EndPaint(hwnd, &ps); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
bool SelectRect(RECT& result) {
    static bool registered = false;
    if (!registered) { WNDCLASSEXW wc{ sizeof(wc) }; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kOverlayClassName; wc.lpfnWndProc = OverlayProc; wc.hCursor = LoadCursor(nullptr, IDC_CROSS); if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false; registered = true; }
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    OverlayState state{};
    HWND overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, kOverlayClassName, L"", WS_POPUP, left, top, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!overlay) return false;
    SetLayeredWindowAttributes(overlay, 0, 96, LWA_ALPHA); ShowWindow(overlay, SW_SHOW); SetForegroundWindow(overlay); SetFocus(overlay);
    MSG message{}; while (!state.done && GetMessageW(&message, nullptr, 0, 0)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (GetCapture() == overlay) ReleaseCapture(); DestroyWindow(overlay);
    RECT local = NormalizedRect(state); result = { local.left + left, local.top + top, local.right + left, local.bottom + top };
    return !state.cancelled && result.right > result.left && result.bottom > result.top;
}
RECT FitFrameToClient(HWND hwnd, const CapturedBitmap& bitmap) {
    RECT client{}; GetClientRect(hwnd, &client);
    const int availableWidth = (std::max)(1, static_cast<int>(client.right) - 16), availableHeight = (std::max)(1, static_cast<int>(client.bottom) - 48);
    const double scale = (std::min)(static_cast<double>(availableWidth) / bitmap.width, static_cast<double>(availableHeight) / bitmap.height);
    const int width = (std::max)(1, static_cast<int>(bitmap.width * scale)), height = (std::max)(1, static_cast<int>(bitmap.height * scale));
    return { (client.right - width) / 2, 8, (client.right - width) / 2 + width, 8 + height };
}
int ToSourceCoordinate(int value, int start, int length, int sourceLength) {
    return (std::clamp)((value - start) * sourceLength / (std::max)(1, length), 0, sourceLength - 1);
}
POINT ToSourcePoint(FrameSelectionState* state, POINT point) {
    return { ToSourceCoordinate(point.x, state->viewport.left, state->viewport.right - state->viewport.left, state->bitmap->width),
        ToSourceCoordinate(point.y, state->viewport.top, state->viewport.bottom - state->viewport.top, state->bitmap->height) };
}
RECT ToPreviewRect(const FrameSelectionState* state, const RECT& source) {
    const int viewWidth = state->viewport.right - state->viewport.left, viewHeight = state->viewport.bottom - state->viewport.top;
    return { state->viewport.left + source.left * viewWidth / state->bitmap->width, state->viewport.top + source.top * viewHeight / state->bitmap->height,
        state->viewport.left + source.right * viewWidth / state->bitmap->width, state->viewport.top + source.bottom * viewHeight / state->bitmap->height };
}
void SetSelectionEnd(FrameSelectionState* state, POINT point) {
    state->selection = { (std::min)(state->start.x, point.x), (std::min)(state->start.y, point.y),
        (std::max)(state->start.x, point.x) + 1, (std::max)(state->start.y, point.y) + 1 };
}
void PaintFrameSelection(HDC dc, HWND hwnd, FrameSelectionState* state, const RECT& client) {
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
    state->viewport = FitFrameToClient(hwnd, *state->bitmap);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = state->bitmap->width; info.bmiHeader.biHeight = -state->bitmap->height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, state->viewport.left, state->viewport.top, state->viewport.right - state->viewport.left, state->viewport.bottom - state->viewport.top, 0, 0, state->bitmap->width, state->bitmap->height, state->bitmap->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    if (state->selection.right > state->selection.left && state->selection.bottom > state->selection.top) {
        RECT selection = ToPreviewRect(state, state->selection); HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 200, 255)); HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, selection.left, selection.top, selection.right, selection.bottom); SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
    }
    constexpr wchar_t instruction[] = L"映像上をドラッグして認識範囲を選択します。Escでキャンセル";
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255)); TextOutW(dc, 8, client.bottom - 30, instruction, static_cast<int>(std::size(instruction) - 1));
}
LRESULT CALLBACK FrameSelectionProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<FrameSelectionState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { state = static_cast<FrameSelectionState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_LBUTTONDOWN: {
        state->viewport = FitFrameToClient(hwnd, *state->bitmap); POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        if (PtInRect(&state->viewport, point)) { state->start = ToSourcePoint(state, point); SetSelectionEnd(state, state->start); state->dragging = true; SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->dragging) { POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }; point.x = (std::clamp)(point.x, state->viewport.left, state->viewport.right - 1); point.y = (std::clamp)(point.y, state->viewport.top, state->viewport.bottom - 1); SetSelectionEnd(state, ToSourcePoint(state, point)); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (state->dragging) { POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }; point.x = (std::clamp)(point.x, state->viewport.left, state->viewport.right - 1); point.y = (std::clamp)(point.y, state->viewport.top, state->viewport.bottom - 1); SetSelectionEnd(state, ToSourcePoint(state, point)); state->dragging = false; if (GetCapture() == hwnd) ReleaseCapture(); state->done = state->selection.right > state->selection.left && state->selection.bottom > state->selection.top; }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { state->cancelled = true; state->done = true; }
        return 0;
    case WM_CLOSE: state->cancelled = true; state->done = true; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint); RECT client{}; GetClientRect(hwnd, &client);
        HDC buffer = CreateCompatibleDC(dc); HBITMAP surface = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
        if (buffer && surface) {
            HGDIOBJ oldSurface = SelectObject(buffer, surface); PaintFrameSelection(buffer, hwnd, state, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY); SelectObject(buffer, oldSurface); DeleteObject(surface);
        } else {
            PaintFrameSelection(dc, hwnd, state, client);
        }
        if (buffer) DeleteDC(buffer); EndPaint(hwnd, &paint); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
bool SelectFrameRect(HWND owner, const CapturedBitmap& bitmap, RECT& result) {
    FrameSelectionState state{ &bitmap, result };
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int maxWidth = work.right - work.left - 80, maxHeight = work.bottom - work.top - 100;
    const int clientWidth = (std::min)(maxWidth, (std::max)(640, bitmap.width + 16)), clientHeight = (std::min)(maxHeight, (std::max)(420, bitmap.height + 56));
    RECT window{ 0, 0, clientWidth, clientHeight }; AdjustWindowRectEx(&window, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kFrameSelectionClassName, L"認識範囲を選択", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        window.right - window.left, window.bottom - window.top, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE); SetForegroundWindow(dialog);
    MSG message{}; while (!state.done && GetMessageW(&message, nullptr, 0, 0)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (GetCapture() == dialog) ReleaseCapture(); EnableWindow(owner, TRUE); SetForegroundWindow(owner); DestroyWindow(dialog);
    if (state.cancelled || state.selection.right <= state.selection.left || state.selection.bottom <= state.selection.top) return false;
    result = state.selection; return true;
}
void Layout(HWND hwnd, State* state) {
    RECT client{}; GetClientRect(hwnd, &client); const int width = client.right, height = client.bottom, margin = 12, gap = 8;
    if (state->settingsMode) {
        MoveWindow(GetDlgItem(hwnd, 3001), margin, margin, width - margin * 2 - 40, 34, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_SETTINGS_CLOSE), width - margin - 30, margin, 30, 28, FALSE);
        const int settingWidth = (std::max)(1, width - margin * 3);
        MoveWindow(GetDlgItem(hwnd, 3003), margin + 12, 64, settingWidth, 24, FALSE); MoveWindow(GetDlgItem(hwnd, 3005), margin + 12, 96, settingWidth, 20, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_SOURCE_SCENE), margin + 12, 116, settingWidth, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_SOURCE_SCREEN), margin + 12, 140, settingWidth, 24, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_AUTO_FORMAT), margin + 12, 172, 250, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_ENHANCE_LOW_RES), margin + 12, 196, 250, 24, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_AUTO_RECOGNIZE), margin + 12, 220, 180, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_AUTO_SECONDS), margin + 196, 220, 50, 24, FALSE); MoveWindow(GetDlgItem(hwnd, 3004), margin + 252, 223, 80, 20, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_AUTO_COPY), margin + 12, 248, 250, 24, FALSE);
        MoveWindow(GetDlgItem(hwnd, 3002), margin + 12, 304, settingWidth, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_PLACE_TIMELINE), margin + 12, 336, 220, 24, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE_PROJECT), margin + 12, 364, 150, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_PROJECT_PATH), margin + 38, 392, width - margin * 2 - 50, 24, FALSE);
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE_CUSTOM), margin + 12, 424, 150, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_SAVE_PATH), margin + 38, 452, width - margin * 2 - 80, 24, FALSE); MoveWindow(GetDlgItem(hwnd, IDC_SAVE_BROWSE), width - margin - 42, 452, 34, 24, FALSE); return;
    }
    const int buttonY = margin, buttonH = 28, rangeH = 166;
    const bool compactButtons = width < 540;
    int rangeY = buttonY + buttonH + gap;
    MoveWindow(GetDlgItem(hwnd, IDC_RECOGNIZE), margin, buttonY, 80, buttonH, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_SELECT), margin + 88, buttonY, 100, buttonH, FALSE);
    MoveWindow(GetDlgItem(hwnd, IDC_COPY), margin + 196, buttonY, 80, buttonH, FALSE);
    if (compactButtons) {
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE_IMAGE), margin, buttonY + buttonH + gap, 120, buttonH, FALSE);
        rangeY = buttonY + buttonH * 2 + gap * 2;
    } else {
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE_IMAGE), margin + 284, buttonY, 120, buttonH, FALSE);
    }
    MoveWindow(GetDlgItem(hwnd, IDC_OPTIONS), width - margin - 54, buttonY, 54, buttonH, FALSE);
    const int fieldY = rangeY + 24, fieldW = 88, columnGap = 132;
    MoveWindow(GetDlgItem(hwnd, 2001), margin, rangeY, 260, 20, FALSE);
    const int labels[] = { 2002, 2003, 2004, 2005 }; const HWND edits[] = { state->x, state->y, state->width, state->height };
    // X / 幅、Y / 高さを二行二列で配置し、その下に選択画像を表示する。
    for (int i = 0; i < 4; ++i) {
        const int column = i / 2, row = i % 2, x = margin + column * columnGap, y = fieldY + row * 29;
        MoveWindow(GetDlgItem(hwnd, labels[i]), x, y, 28, 20, FALSE); MoveWindow(edits[i], x + 30, y - 3, fieldW, 24, FALSE);
    }
    MoveWindow(state->preview, margin + columnGap * 2 + 4, rangeY + 4, (std::max)(120, width - margin * 2 - columnGap * 2 - 4), rangeH - 8, FALSE);
    const int contentY = rangeY + rangeH + gap, textHeaderH = 24;
    MoveWindow(GetDlgItem(hwnd, 2007), margin, contentY, 180, textHeaderH, FALSE);
    const int textY = contentY + textHeaderH;
    const int available = (std::max)(180, height - textY - 50); const int textH = (std::max)(70, available * 43 / 100);
    MoveWindow(state->text, margin, textY, width - margin * 2, textH, FALSE);
    const int historyLabelY = textY + textH + gap; MoveWindow(GetDlgItem(hwnd, 2006), margin, historyLabelY, 200, 20, FALSE);
    const int historyY = historyLabelY + 20; MoveWindow(state->history, margin, historyY, width - margin * 2, (std::max)(80, height - historyY - 28), FALSE);
    MoveWindow(state->status, margin, height - 23, width - margin * 2, 20, FALSE);
}
}

bool OCRWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{ sizeof(wc) }; wc.hInstance = instance; wc.lpszClassName = kClassName; wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    const bool mainRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    WNDCLASSEXW selection{ sizeof(selection) }; selection.hInstance = instance; selection.lpszClassName = kFrameSelectionClassName; selection.lpfnWndProc = FrameSelectionProc;
    selection.hCursor = LoadCursor(nullptr, IDC_CROSS); selection.hbrBackground = nullptr;
    const bool selectionRegistered = RegisterClassExW(&selection) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return mainRegistered && selectionRegistered;
}
HWND OCRWindow::Create(HINSTANCE instance, Hive* hive) {
    return CreateWindowExW(0, kClassName, L"OCR文字抽出", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 860, 760, nullptr, nullptr, instance, hive);
}
LRESULT CALLBACK OCRWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { state = new State{ reinterpret_cast<Hive*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams) }; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON", L"設定", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_OPTIONS), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"範囲選択", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SELECT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"認識", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_RECOGNIZE), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"コピー", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_COPY), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"画像として保存", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_IMAGE), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"選択範囲（X / Y / 幅 / 高さ）", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2001), nullptr, nullptr);
        const wchar_t* labels[] = { L"X", L"Y", L"幅", L"高さ" }; for (int i = 0; i < 4; ++i) CreateWindowW(L"STATIC", labels[i], WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2002 + i)), nullptr, nullptr);
        state->x = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_X), nullptr, nullptr);
        state->y = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_Y), nullptr, nullptr);
        state->width = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_WIDTH), nullptr, nullptr);
        state->height = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_HEIGHT), nullptr, nullptr);
        state->preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"キャプチャプレビュー", WS_VISIBLE | WS_CHILD | SS_BITMAP | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_PREVIEW), nullptr, nullptr);
        state->text = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TEXT), nullptr, nullptr);
        ApplyTextBoxStyle(state->text);
        CreateWindowW(L"STATIC", L"認識結果", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2007), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"履歴（最新20件）", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2006), nullptr, nullptr);
        state->history = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_HISTORY), nullptr, nullptr);
        state->status = CreateWindowW(L"STATIC", L"［範囲選択］で範囲を指定するか、数値を入力して［認識］を押してください。", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"設定画面", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3001), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"画像として保存", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3002), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"OCR認識", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3003), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"秒ごと", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3004), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"認識元", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3005), nullptr, nullptr);
        state->titleFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        state->sectionFont = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        SendMessageW(GetDlgItem(hwnd, 3001), WM_SETFONT, reinterpret_cast<WPARAM>(state->titleFont), TRUE); SendMessageW(GetDlgItem(hwnd, 3002), WM_SETFONT, reinterpret_cast<WPARAM>(state->sectionFont), TRUE);
        SendMessageW(GetDlgItem(hwnd, 3003), WM_SETFONT, reinterpret_cast<WPARAM>(state->sectionFont), TRUE);
        CreateWindowW(L"BUTTON", L"×", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_CLOSE), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"タイムラインに配置", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_PLACE_TIMELINE), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"認識文字を自動調整", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_AUTO_FORMAT), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"低解像度画像を補正", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_ENHANCE_LOW_RES), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"自動認識", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_AUTO_RECOGNIZE), nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"3", WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_AUTO_SECONDS), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"認識結果を自動コピー", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_AUTO_COPY), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"AviUtl2の現在フレーム", WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SOURCE_SCENE), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"画面全体をキャプチャ", WS_CHILD | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SOURCE_SCREEN), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"プロジェクト直下", WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_PROJECT), nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_PROJECT_PATH), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"保存先選択", WS_CHILD | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_CUSTOM), nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_PATH), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"…", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_BROWSE), nullptr, nullptr);
        RefreshHistory(state); UpdateAutoRecognitionTimer(hwnd, state); return 0;
    }
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = { 440, 540 }; return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        if (state->settingsMode) {
            RECT client{}; GetClientRect(hwnd, &client); HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150)); HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            RoundRect(dc, 8, kSettingsOcrCardTop, client.right - 8, kSettingsOcrCardBottom, 10, 10);
            RoundRect(dc, 8, kSettingsSaveCardTop, client.right - 8, kSettingsSaveCardBottom, 10, 10);
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        }
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_SIZE:
        Layout(hwnd, state);
        if (!state->settingsMode && state->hasCaptured) ShowPreview(state, state->captured);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;
    case WM_TIMER: if (wparam == AUTO_RECOGNIZE_TIMER && !state->recognizing && !state->renderingFrame && !state->settingsMode && state->rect.right > state->rect.left && state->rect.bottom > state->rect.top) SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_RECOGNIZE, 0), 0); return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_OPTIONS) {
            ShowSettings(hwnd, state, true);
        } else if (LOWORD(wparam) == IDC_SETTINGS_CLOSE) {
            ShowSettings(hwnd, state, false);
        } else if (LOWORD(wparam) == IDC_PLACE_TIMELINE) {
            state->hive->config.SetPlaceImageOnTimeline(SendMessageW(GetDlgItem(hwnd, IDC_PLACE_TIMELINE), BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (LOWORD(wparam) == IDC_AUTO_FORMAT) {
            state->hive->config.SetAutoFormatOcrText(SendMessageW(GetDlgItem(hwnd, IDC_AUTO_FORMAT), BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (LOWORD(wparam) == IDC_ENHANCE_LOW_RES) {
            state->hive->config.SetEnhanceLowResolution(SendMessageW(GetDlgItem(hwnd, IDC_ENHANCE_LOW_RES), BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (LOWORD(wparam) == IDC_AUTO_RECOGNIZE) {
            state->hive->config.SetAutoRecognize(SendMessageW(GetDlgItem(hwnd, IDC_AUTO_RECOGNIZE), BM_GETCHECK, 0, 0) == BST_CHECKED); UpdateAutoRecognitionTimer(hwnd, state);
        } else if (LOWORD(wparam) == IDC_AUTO_SECONDS && HIWORD(wparam) == EN_KILLFOCUS) {
            state->hive->config.SetAutoRecognizeSeconds(EditInt(GetDlgItem(hwnd, IDC_AUTO_SECONDS))); UpdateAutoRecognitionTimer(hwnd, state);
        } else if (LOWORD(wparam) == IDC_AUTO_COPY) {
            state->hive->config.SetAutoCopy(SendMessageW(GetDlgItem(hwnd, IDC_AUTO_COPY), BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (LOWORD(wparam) == IDC_SOURCE_SCENE) {
            if (state->renderingFrame) return 0;
            state->hive->config.SetUseScreenCapture(false);
            ResetCaptureState(state); SetWindowTextW(state->status, L"認識元をAviUtl2の現在フレームへ変更しました。範囲選択してください。");
        } else if (LOWORD(wparam) == IDC_SOURCE_SCREEN) {
            if (state->renderingFrame) return 0;
            state->hive->config.SetUseScreenCapture(true);
            ResetCaptureState(state); SetWindowTextW(state->status, L"認識元を画面全体キャプチャへ変更しました。範囲選択してください。");
        } else if (LOWORD(wparam) == IDC_SAVE_PROJECT) {
            state->hive->config.SetImageSaveDirectory(L""); SetSaveDestination(hwnd, false);
        } else if (LOWORD(wparam) == IDC_SAVE_CUSTOM) {
            SetSaveDestination(hwnd, true);
        } else if (LOWORD(wparam) == IDC_SAVE_BROWSE) {
            std::wstring directory; if (ChooseSaveDirectory(hwnd, directory)) { state->hive->config.SetImageSaveDirectory(directory); SetWindowTextW(GetDlgItem(hwnd, IDC_SAVE_PATH), directory.c_str()); SetSaveDestination(hwnd, true); }
        } else if (LOWORD(wparam) == IDC_SELECT) {
            if (!state->hive->config.GetUseScreenCapture()) { RequestSceneFrame(hwnd, state, SceneAction::Select); return 0; }
            SetWindowTextW(state->status, L"画面上で範囲をドラッグしてください（Escでキャンセル）。");
            if (!SelectRect(state->rect)) { SetWindowTextW(state->status, L"範囲選択をキャンセルしました。"); return 0; }
            UpdateRectControls(state); std::wstring error;
            if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            SetWindowTextW(state->status, L"範囲を選択し、プレビューを更新しました。［認識］で文字を抽出します。");
        } else if (LOWORD(wparam) == IDC_RECOGNIZE) {
            if (state->recognizing) return 0;
            if (!state->hive->config.GetUseScreenCapture()) { RequestSceneFrame(hwnd, state, SceneAction::Recognize); return 0; }
            std::wstring error; if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            StartRecognition(hwnd, state);
        } else if (LOWORD(wparam) == IDC_SAVE_IMAGE) {
            if (!state->hive->config.GetUseScreenCapture()) { RequestSceneFrame(hwnd, state, SceneAction::Save); return 0; }
            std::wstring error; if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            SaveSelection(hwnd, state);
        } else if (LOWORD(wparam) == IDC_COPY) { SendMessageW(state->text, EM_SETSEL, 0, -1); SendMessageW(state->text, WM_COPY, 0, 0); SetWindowTextW(state->status, L"認識結果をクリップボードにコピーしました。"); }
        else if (LOWORD(wparam) == IDC_HISTORY && HIWORD(wparam) == LBN_DBLCLK) { const int index = static_cast<int>(SendMessageW(state->history, LB_GETCURSEL, 0, 0)); if (index >= 0 && index < static_cast<int>(state->hive->history.Items().size())) { SetWindowTextW(state->text, state->hive->history.Items()[index].text.c_str()); ApplyTextBoxStyle(state->text); } }
        return 0;
    case WM_SCENE_FRAME_RENDERED: {
        std::unique_ptr<SceneCompletion> done(reinterpret_cast<SceneCompletion*>(lparam));
        state->renderingFrame = false;
        EnableWindow(GetDlgItem(hwnd, IDC_SOURCE_SCENE), TRUE); EnableWindow(GetDlgItem(hwnd, IDC_SOURCE_SCREEN), TRUE);
        if (!done || !done->error.empty()) { SetWindowTextW(state->status, done ? done->error.c_str() : L"映像フレームの取得に失敗しました。"); return 0; }
        state->sourceFrame = std::move(done->bitmap); state->hasSourceFrame = true;
        if (done->action == SceneAction::Select) {
            RECT selection = state->rect;
            if (selection.left < 0 || selection.top < 0 || selection.right > state->sourceFrame.width || selection.bottom > state->sourceFrame.height || selection.right <= selection.left || selection.bottom <= selection.top) selection = { 0, 0, state->sourceFrame.width, state->sourceFrame.height };
            if (!SelectFrameRect(hwnd, state->sourceFrame, selection)) { SetWindowTextW(state->status, L"範囲選択をキャンセルしました。"); return 0; }
            state->rect = selection; UpdateRectControls(state); std::wstring error;
            if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            SetWindowTextW(state->status, L"AviUtl2の映像から範囲を選択し、プレビューを更新しました。［認識］で文字を抽出します。");
        } else {
            std::wstring error;
            if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            if (done->action == SceneAction::Recognize) StartRecognition(hwnd, state);
            else SaveSelection(hwnd, state);
        }
        return 0;
    }
    case WM_OCR_COMPLETED: {
        std::unique_ptr<Completion> done(reinterpret_cast<Completion*>(lparam));
        state->recognizing = false; if (!done->error.empty()) { SetWindowTextW(state->status, done->error.c_str()); return 0; }
        std::wstring text = std::move(done->text); const int originalBreaks = CountLineBreaks(text);
        const bool autoFormat = state->hive->config.GetAutoFormatOcrText();
        bool adjusted = false;
        if (autoFormat) { std::wstring formatted = OCRFormatter::Normalize(text); adjusted = formatted != text; text = std::move(formatted); }
        const int adjustedBreaks = CountLineBreaks(text);
        SetWindowTextW(state->text, text.c_str()); ApplyTextBoxStyle(state->text); OCRResult result{ text, {}, done->rect }; GetLocalTime(&result.time);
        state->hive->history.Add(std::move(result)); state->hive->config.SaveHistory(state->hive->history); RefreshHistory(state); if (state->hive->config.GetAutoCopy()) { SendMessageW(state->text, EM_SETSEL, 0, -1); SendMessageW(state->text, WM_COPY, 0, 0); }
        if (adjusted) SetWindowTextW(state->status, (L"認識が完了しました。改行・日本語周辺の空白を自動調整しました（改行 " + std::to_wstring(originalBreaks) + L" → " + std::to_wstring(adjustedBreaks) + L"）。").c_str());
        else SetWindowTextW(state->status, L"認識が完了しました。");
        return 0;
    }
    case WM_DESTROY: ClearPreviewImage(state); return 0;
    case WM_NCDESTROY:
        KillTimer(hwnd, AUTO_RECOGNIZE_TIMER); ClearPreviewImage(state);
        if (state->titleFont) DeleteObject(state->titleFont); if (state->sectionFont) DeleteObject(state->sectionFont);
        delete state; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
