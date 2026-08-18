#pragma once
#include "common/hive.hpp"
#include <windows.h>

class App {
public:
    void Initialize(HINSTANCE instance, LPCWSTR configDirectory);
    void RegisterWithHost(struct HOST_APP_TABLE* host);
private:
    std::wstring GetProjectFilePath() const;
    bool PlaceImageOnTimeline(const std::wstring& file) const;
    void RenderCurrentScene(std::function<void(CapturedBitmap, std::wstring)> completed) const;
    void RememberProjectFile(struct PROJECT_FILE* project);
    struct SceneRenderRequest;
    static void OnSceneRendered(void* value, int frame, const void* buffer, int width, int height, int pitch);
    static void OnProjectLoad(struct PROJECT_FILE* project);
    static void OnProjectSave(struct PROJECT_FILE* project);
    static void ShowWindowClient();
    static App* instance_;
    Hive hive_;
    HWND window_{};
    HINSTANCE module_{};
    struct EDIT_HANDLE* editHandle_{};
    std::wstring projectFilePath_;
};
