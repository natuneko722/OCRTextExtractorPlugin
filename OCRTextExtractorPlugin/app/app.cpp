#include "app.hpp"
#include "window/ocr_window.hpp"
#include "plugin2.h"
#include <richedit.h>
#include <filesystem>
#include <memory>

App* App::instance_ = nullptr;
void App::Initialize(HINSTANCE instance, LPCWSTR configDirectory) {
    instance_ = this; module_ = instance; LoadLibraryW(L"Msftedit.dll");
    hive_.config.SetDirectory(configDirectory ? configDirectory : L"."); hive_.config.LoadHistory(hive_.history);
    hive_.getProjectFilePath = [this] { return GetProjectFilePath(); };
    hive_.placeImageOnTimeline = [this](const std::wstring& file) { return PlaceImageOnTimeline(file); };
    hive_.renderCurrentScene = [this](std::function<void(CapturedBitmap, std::wstring)> completed) { RenderCurrentScene(std::move(completed)); };
    OCRWindow::Register(instance); window_ = OCRWindow::Create(instance, &hive_);
}
void App::ShowWindowClient() { if (!instance_ || !instance_->window_) return; ShowWindow(instance_->window_, SW_SHOW); SetForegroundWindow(instance_->window_); }
void App::RegisterWithHost(HOST_APP_TABLE* host) {
    if (!host || !window_) return;
    host->register_window_client(L"OCR文字抽出", window_);
    editHandle_ = host->create_edit_handle();
    host->register_project_load_handler(OnProjectLoad);
    host->register_project_save_handler(OnProjectSave);
}
void App::OnProjectLoad(PROJECT_FILE* project) { if (instance_) instance_->RememberProjectFile(project); }
void App::OnProjectSave(PROJECT_FILE* project) { if (instance_) instance_->RememberProjectFile(project); }
void App::RememberProjectFile(PROJECT_FILE* project) {
    if (!project || !project->get_project_file_path) return;
    const LPCWSTR path = project->get_project_file_path();
    if (path && *path) projectFilePath_ = path;
}
std::wstring App::GetProjectFilePath() const {
    std::wstring path = projectFilePath_;
    if (path.empty() && editHandle_) {
        struct Context { EDIT_HANDLE* handle; std::wstring path; } context{ editHandle_ };
        editHandle_->call_read_section_param(&context, [](void* value, EDIT_SECTION* edit) {
            auto* context = static_cast<Context*>(value); auto* project = edit->get_project_file(context->handle);
            if (project && project->get_project_file_path) { const LPCWSTR projectPath = project->get_project_file_path(); if (projectPath) context->path = projectPath; }
        });
        path = std::move(context.path);
    }
    return path;
}
bool App::PlaceImageOnTimeline(const std::wstring& file) const {
    if (!editHandle_ || file.empty()) return false;
    struct Context { EDIT_HANDLE* handle; const std::wstring* file; bool placed{}; } context{ editHandle_, &file };
    return editHandle_->call_edit_section_param(&context, [](void* value, EDIT_SECTION* edit) {
        auto* context = static_cast<Context*>(value);
        context->placed = edit->create_object_from_media_file(context->file->c_str(), edit->info->layer, edit->info->frame, 0) != nullptr;
    }) && context.placed;
}

struct App::SceneRenderRequest {
    std::function<void(CapturedBitmap, std::wstring)> completed;
};

void App::OnSceneRendered(void* value, int, const void* buffer, int width, int height, int pitch) {
    std::unique_ptr<SceneRenderRequest> request(static_cast<SceneRenderRequest*>(value));
    if (!buffer || width <= 0 || height <= 0 || pitch < width * 4) {
        request->completed({}, L"AviUtl2の映像フレームを取得できませんでした。");
        return;
    }
    CapturedBitmap bitmap{}; bitmap.width = width; bitmap.height = height; bitmap.bgra.resize(static_cast<size_t>(width) * height * 4);
    const auto* source = static_cast<const unsigned char*>(buffer);
    for (int y = 0; y < height; ++y) {
        const auto* from = source + static_cast<size_t>(y) * pitch;
        auto* to = bitmap.bgra.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            to[x * 4 + 0] = from[x * 4 + 2];
            to[x * 4 + 1] = from[x * 4 + 1];
            to[x * 4 + 2] = from[x * 4 + 0];
            to[x * 4 + 3] = from[x * 4 + 3];
        }
    }
    request->completed(std::move(bitmap), L"");
}

void App::RenderCurrentScene(std::function<void(CapturedBitmap, std::wstring)> completed) const {
    if (!editHandle_) { completed({}, L"AviUtl2の編集画面に接続できません。"); return; }
    auto* request = new SceneRenderRequest{ std::move(completed) };
    EDIT_INFO info{};
    // EDIT_SECTION::info は call_read_section() 内では利用できないため、専用APIで取得する。
    editHandle_->get_edit_info(&info, sizeof(info));
    if (info.frame < 0 || info.width <= 0 || info.height <= 0) {
        std::unique_ptr<SceneRenderRequest> failed(request);
        failed->completed({}, L"AviUtl2の現在シーンを取得できません。プロジェクトとシーンを開いてから再試行してください。");
        return;
    }
    if (!editHandle_->rendering_scene_video(info.frame, request, OnSceneRendered)) {
        std::unique_ptr<SceneRenderRequest> failed(request);
        const int editState = editHandle_->get_edit_state();
        failed->completed({}, editState == EDIT_HANDLE::EDIT_STATE_SAVE
            ? L"AviUtl2が出力中のため、現在フレームを取得できません。"
            : L"AviUtl2の現在フレームのレンダリングを開始できませんでした。AviUtl2本体を最新版へ更新してから再試行してください。");
    }
}
