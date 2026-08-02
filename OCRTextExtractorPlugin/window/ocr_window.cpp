#include "ocr_window.hpp"
#include "capture/capture_object.hpp"
#include "capture/image_saver.hpp"
#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace {
constexpr wchar_t kClassName[] = L"AviUtl2OcrTextExtractor";
constexpr wchar_t kOverlayClassName[] = L"AviUtl2OcrSelectionOverlay";
constexpr UINT WM_OCR_COMPLETED = WM_APP + 42;
enum : int { IDC_OPTIONS = 1001, IDC_SELECT, IDC_RECOGNIZE, IDC_COPY, IDC_SAVE_IMAGE, IDC_TEXT, IDC_HISTORY, IDC_X, IDC_Y, IDC_WIDTH, IDC_HEIGHT, IDC_STATUS, IDC_PREVIEW,
    IDC_SETTINGS_CLOSE = 1101, IDC_PLACE_TIMELINE, IDC_SAVE_PROJECT, IDC_SAVE_CUSTOM, IDC_SAVE_PATH, IDC_SAVE_BROWSE, IDC_PROJECT_PATH, IDC_AUTO_FORMAT, IDC_ENHANCE_LOW_RES, IDC_AUTO_RECOGNIZE, IDC_AUTO_SECONDS, IDC_AUTO_COPY };
constexpr UINT_PTR AUTO_RECOGNIZE_TIMER = 1;
struct Completion { std::wstring text, error; RECT rect{}; };
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
};
struct OverlayState { POINT start{}, current{}; bool dragging{}, done{}, cancelled{}; };
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
    CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = RGB(235, 235, 235);
    SendMessageW(control, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
    SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
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
void ShowPreview(State* state, const CapturedBitmap& bitmap) {
    RECT bounds{}; GetClientRect(state->preview, &bounds);
    const int maxWidth = (std::max)(1, static_cast<int>(bounds.right) - 8), maxHeight = (std::max)(1, static_cast<int>(bounds.bottom) - 8);
    const double scale = (std::min)(1.0, (std::min)(static_cast<double>(maxWidth) / bitmap.width, static_cast<double>(maxHeight) / bitmap.height));
    const int width = (std::max)(1, static_cast<int>(bitmap.width * scale)), height = (std::max)(1, static_cast<int>(bitmap.height * scale));
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = bitmap.width; info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr; HBITMAP source = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!source || !pixels) return;
    memcpy(pixels, bitmap.bgra.data(), bitmap.bgra.size());
    HDC screen = GetDC(nullptr), from = CreateCompatibleDC(screen), to = CreateCompatibleDC(screen);
    HBITMAP thumb = CreateCompatibleBitmap(screen, width, height); SelectObject(from, source); SelectObject(to, thumb);
    SetStretchBltMode(to, HALFTONE); StretchBlt(to, 0, 0, width, height, from, 0, 0, bitmap.width, bitmap.height, SRCCOPY);
    DeleteDC(to); DeleteDC(from); ReleaseDC(nullptr, screen); DeleteObject(source);
    if (state->previewImage) DeleteObject(state->previewImage); state->previewImage = thumb;
    SendMessage(state->preview, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(thumb));
}
bool UpdateCapturedSelection(State* state, std::wstring& error) {
    if (!ReadRectControls(state, state->rect)) { error = L"幅と高さには1以上の数値を入力してください。"; return false; }
    if (!CaptureObject::CaptureScreenRect(state->rect, state->captured, error)) return false;
    state->hasCaptured = true; ShowPreview(state, state->captured); return true;
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
    state->settingsMode = visible;
    const int normal[] = { IDC_OPTIONS, IDC_SELECT, IDC_RECOGNIZE, IDC_COPY, IDC_SAVE_IMAGE, IDC_TEXT, IDC_HISTORY, IDC_X, IDC_Y, IDC_WIDTH, IDC_HEIGHT, IDC_STATUS, IDC_PREVIEW, 2001, 2002, 2003, 2004, 2005, 2006 };
    const int settings[] = { IDC_SETTINGS_CLOSE, IDC_PLACE_TIMELINE, IDC_SAVE_PROJECT, IDC_SAVE_CUSTOM, IDC_PROJECT_PATH, IDC_SAVE_PATH, IDC_SAVE_BROWSE, IDC_AUTO_FORMAT, IDC_ENHANCE_LOW_RES, IDC_AUTO_RECOGNIZE, IDC_AUTO_SECONDS, IDC_AUTO_COPY, 3001, 3002, 3003, 3004 };
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
    }
    Layout(hwnd, state); InvalidateRect(hwnd, nullptr, TRUE);
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
    if (!registered) { WNDCLASSEXW wc{ sizeof(wc) }; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kOverlayClassName; wc.lpfnWndProc = OverlayProc; wc.hCursor = LoadCursor(nullptr, IDC_CROSS); RegisterClassExW(&wc); registered = true; }
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    OverlayState state{};
    HWND overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, kOverlayClassName, L"", WS_POPUP, left, top, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    SetLayeredWindowAttributes(overlay, 0, 96, LWA_ALPHA); ShowWindow(overlay, SW_SHOW); SetForegroundWindow(overlay); SetFocus(overlay);
    MSG message{}; while (!state.done && GetMessageW(&message, nullptr, 0, 0)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (GetCapture() == overlay) ReleaseCapture(); DestroyWindow(overlay);
    RECT local = NormalizedRect(state); result = { local.left + left, local.top + top, local.right + left, local.bottom + top };
    return !state.cancelled && result.right > result.left && result.bottom > result.top;
}
void Layout(HWND hwnd, State* state) {
    RECT client{}; GetClientRect(hwnd, &client); const int width = client.right, height = client.bottom, margin = 12, gap = 8;
    if (state->settingsMode) {
        MoveWindow(GetDlgItem(hwnd, 3001), margin, margin, width - margin * 2 - 40, 34, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_SETTINGS_CLOSE), width - margin - 30, margin, 30, 28, TRUE);
        MoveWindow(GetDlgItem(hwnd, 3003), margin + 12, 80, 200, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_AUTO_FORMAT), margin + 12, 112, 250, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_ENHANCE_LOW_RES), margin + 12, 140, 250, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_AUTO_RECOGNIZE), margin + 12, 168, 180, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_AUTO_SECONDS), margin + 196, 168, 50, 24, TRUE); MoveWindow(GetDlgItem(hwnd, 3004), margin + 252, 171, 80, 20, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_AUTO_COPY), margin + 12, 204, 200, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, 3002), margin + 12, 262, 200, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_PLACE_TIMELINE), margin + 12, 294, 220, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE_PROJECT), margin + 12, 328, 150, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_PROJECT_PATH), margin + 38, 356, width - margin * 2 - 50, 24, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE_CUSTOM), margin + 12, 390, 150, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_SAVE_PATH), margin + 38, 418, width - margin * 2 - 80, 24, TRUE); MoveWindow(GetDlgItem(hwnd, IDC_SAVE_BROWSE), width - margin - 42, 418, 34, 24, TRUE); return;
    }
    const int buttonY = margin, buttonH = 28, rangeY = buttonY + buttonH + gap, rangeH = 166;
    MoveWindow(GetDlgItem(hwnd, IDC_RECOGNIZE), margin, buttonY, 80, buttonH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_SELECT), margin + 88, buttonY, 100, buttonH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_COPY), margin + 196, buttonY, 80, buttonH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_SAVE_IMAGE), margin + 284, buttonY, 120, buttonH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_OPTIONS), width - margin - 54, buttonY, 54, buttonH, TRUE);
    const int fieldY = rangeY + 24, fieldW = 88, columnGap = 132;
    MoveWindow(GetDlgItem(hwnd, 2001), margin, rangeY, 260, 20, TRUE);
    const int labels[] = { 2002, 2003, 2004, 2005 }; const HWND edits[] = { state->x, state->y, state->width, state->height };
    // X / 幅、Y / 高さを二行二列で配置し、その下に選択画像を表示する。
    for (int i = 0; i < 4; ++i) {
        const int column = i / 2, row = i % 2, x = margin + column * columnGap, y = fieldY + row * 29;
        MoveWindow(GetDlgItem(hwnd, labels[i]), x, y, 28, 20, TRUE); MoveWindow(edits[i], x + 30, y - 3, fieldW, 24, TRUE);
    }
    MoveWindow(state->preview, margin + columnGap * 2 + 4, rangeY + 4, (std::max)(120, width - margin * 2 - columnGap * 2 - 4), rangeH - 8, TRUE);
    const int contentY = rangeY + rangeH + gap;
    const int available = (std::max)(180, height - contentY - 50); const int textH = (std::max)(70, available * 43 / 100);
    MoveWindow(state->text, margin, contentY, width - margin * 2, textH, TRUE);
    const int historyLabelY = contentY + textH + gap; MoveWindow(GetDlgItem(hwnd, 2006), margin, historyLabelY, 200, 20, TRUE);
    const int historyY = historyLabelY + 20; MoveWindow(state->history, margin, historyY, width - margin * 2, (std::max)(80, height - historyY - 28), TRUE);
    MoveWindow(state->status, margin, height - 23, width - margin * 2, 20, TRUE);
}
}

bool OCRWindow::Register(HINSTANCE instance) {
    WNDCLASSEXW wc{ sizeof(wc) }; wc.hInstance = instance; wc.lpszClassName = kClassName; wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
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
        const wchar_t* labels[] = { L"X", L"Y", L"幅", L"高さ" }; for (int i = 0; i < 4; ++i) CreateWindowW(L"STATIC", labels[i], WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2002 + i), nullptr, nullptr);
        state->x = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_X), nullptr, nullptr);
        state->y = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_Y), nullptr, nullptr);
        state->width = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_WIDTH), nullptr, nullptr);
        state->height = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_HEIGHT), nullptr, nullptr);
        state->preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"キャプチャプレビュー", WS_VISIBLE | WS_CHILD | SS_BITMAP | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_PREVIEW), nullptr, nullptr);
        state->text = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TEXT), nullptr, nullptr);
        ApplyTextBoxStyle(state->text);
        CreateWindowW(L"STATIC", L"履歴（最新20件）", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(2006), nullptr, nullptr);
        state->history = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_VISIBLE | WS_CHILD | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_HISTORY), nullptr, nullptr);
        state->status = CreateWindowW(L"STATIC", L"［範囲選択］で範囲を指定するか、数値を入力して［認識］を押してください。", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"設定画面", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3001), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"画像として保存", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3002), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"OCR認識", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3003), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"秒ごと", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(3004), nullptr, nullptr);
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
        CreateWindowW(L"BUTTON", L"プロジェクト直下", WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_PROJECT), nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_PROJECT_PATH), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"保存先選択", WS_CHILD | BS_AUTORADIOBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_CUSTOM), nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_PATH), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"…", WS_CHILD, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SAVE_BROWSE), nullptr, nullptr);
        RefreshHistory(state); UpdateAutoRecognitionTimer(hwnd, state); return 0;
    }
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = { 440, 430 }; return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        if (state->settingsMode) {
            RECT client{}; GetClientRect(hwnd, &client); HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150)); HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            RoundRect(dc, 8, 64, client.right - 8, 246, 10, 10); RoundRect(dc, 8, 250, client.right - 8, 458, 10, 10); SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        }
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_SIZE: Layout(hwnd, state); if (state->settingsMode) InvalidateRect(hwnd, nullptr, TRUE); return 0;
    case WM_TIMER: if (wparam == AUTO_RECOGNIZE_TIMER && !state->recognizing && !state->settingsMode && state->rect.right > state->rect.left && state->rect.bottom > state->rect.top) SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_RECOGNIZE, 0), 0); return 0;
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
        } else if (LOWORD(wparam) == IDC_SAVE_PROJECT) {
            state->hive->config.SetImageSaveDirectory(L""); SetSaveDestination(hwnd, false);
        } else if (LOWORD(wparam) == IDC_SAVE_CUSTOM) {
            SetSaveDestination(hwnd, true);
        } else if (LOWORD(wparam) == IDC_SAVE_BROWSE) {
            std::wstring directory; if (ChooseSaveDirectory(hwnd, directory)) { state->hive->config.SetImageSaveDirectory(directory); SetWindowTextW(GetDlgItem(hwnd, IDC_SAVE_PATH), directory.c_str()); SetSaveDestination(hwnd, true); }
        } else if (LOWORD(wparam) == IDC_SELECT) {
            SetWindowTextW(state->status, L"画面上で範囲をドラッグしてください（Escでキャンセル）。");
            if (!SelectRect(state->rect)) { SetWindowTextW(state->status, L"範囲選択をキャンセルしました。"); return 0; }
            UpdateRectControls(state); std::wstring error;
            if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            SetWindowTextW(state->status, L"範囲を選択し、プレビューを更新しました。［認識］で文字を抽出します。");
        } else if (LOWORD(wparam) == IDC_RECOGNIZE) {
            if (state->recognizing) return 0;
            std::wstring error; if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            SetWindowTextW(state->status, L"Windows OCRで認識中…"); const RECT rect = state->rect;
            state->recognizing = true; state->hive->ocr.RecognizeAsync(std::move(state->captured), state->hive->config.GetAutoFormatOcrText(), state->hive->config.GetEnhanceLowResolution(), [hwnd, rect](std::wstring text, std::wstring error) { auto* done = new Completion{ std::move(text), std::move(error), rect }; PostMessageW(hwnd, WM_OCR_COMPLETED, 0, reinterpret_cast<LPARAM>(done)); }); state->hasCaptured = false;
        } else if (LOWORD(wparam) == IDC_SAVE_IMAGE) {
            std::wstring error; if (!UpdateCapturedSelection(state, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            const std::wstring projectPath = state->hive->getProjectFilePath ? state->hive->getProjectFilePath() : L"";
            std::wstring directory = state->hive->config.GetImageSaveDirectory();
            if (directory.empty() && !projectPath.empty()) directory = std::filesystem::path(projectPath).parent_path().wstring();
            if (directory.empty()) { SetWindowTextW(state->status, L"プロジェクトを保存するか、⚙から画像保存先を指定してください。"); return 0; }
            const std::wstring projectName = projectPath.empty() ? L"NoProject" : std::filesystem::path(projectPath).stem().wstring();
            std::wstring saved; if (!SaveCapturedBitmapAsPng(state->captured, directory, projectName, saved, error)) { SetWindowTextW(state->status, error.c_str()); return 0; }
            if (state->hive->config.GetPlaceImageOnTimeline() && (!state->hive->placeImageOnTimeline || !state->hive->placeImageOnTimeline(saved))) { SetWindowTextW(state->status, (L"画像は保存しましたが、タイムラインへの配置に失敗しました: " + saved).c_str()); return 0; }
            SetWindowTextW(state->status, (L"画像を保存しました: " + saved).c_str());
        } else if (LOWORD(wparam) == IDC_COPY) { SendMessageW(state->text, EM_SETSEL, 0, -1); SendMessageW(state->text, WM_COPY, 0, 0); SetWindowTextW(state->status, L"認識結果をクリップボードにコピーしました。"); }
        else if (LOWORD(wparam) == IDC_HISTORY && HIWORD(wparam) == LBN_DBLCLK) { const int index = static_cast<int>(SendMessageW(state->history, LB_GETCURSEL, 0, 0)); if (index >= 0 && index < static_cast<int>(state->hive->history.Items().size())) { SetWindowTextW(state->text, state->hive->history.Items()[index].text.c_str()); ApplyTextBoxStyle(state->text); } }
        return 0;
    case WM_OCR_COMPLETED: {
        std::unique_ptr<Completion> done(reinterpret_cast<Completion*>(lparam));
        state->recognizing = false; if (!done->error.empty()) { SetWindowTextW(state->status, done->error.c_str()); return 0; }
        SetWindowTextW(state->text, done->text.c_str()); ApplyTextBoxStyle(state->text); OCRResult result{ done->text, {}, done->rect }; GetLocalTime(&result.time);
        state->hive->history.Add(std::move(result)); state->hive->config.SaveHistory(state->hive->history); RefreshHistory(state); if (state->hive->config.GetAutoCopy()) { SendMessageW(state->text, EM_SETSEL, 0, -1); SendMessageW(state->text, WM_COPY, 0, 0); } SetWindowTextW(state->status, L"認識が完了しました。"); return 0;
    }
    case WM_NCDESTROY: KillTimer(hwnd, AUTO_RECOGNIZE_TIMER); if (state->previewImage) DeleteObject(state->previewImage); if (state->titleFont) DeleteObject(state->titleFont); if (state->sectionFont) DeleteObject(state->sectionFont); delete state; SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
