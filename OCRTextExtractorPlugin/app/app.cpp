#include "app.hpp"
#include "window/ocr_window.hpp"
#include "plugin2.h"
#include <richedit.h>
#include <filesystem>

App* App::instance_ = nullptr;
void App::Initialize(HINSTANCE instance, LPCWSTR configDirectory) {
    instance_ = this; module_ = instance; LoadLibraryW(L"Msftedit.dll");
    hive_.config.SetDirectory(configDirectory ? configDirectory : L"."); hive_.config.LoadHistory(hive_.history);
    hive_.getProjectFilePath = [this] { return GetProjectFilePath(); };
    hive_.placeImageOnTimeline = [this](const std::wstring& file) { return PlaceImageOnTimeline(file); };
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
