#include <windows.h>
#include "plugin2.h"
#include "config2.h"
#include "app/app.hpp"

namespace { App app; CONFIG_HANDLE* config_handle = nullptr; HINSTANCE module = nullptr; }
COMMON_PLUGIN_TABLE common_plugin_table{ L"OCR文字抽出", L"Windows OCR を利用して画面の選択範囲から文字を抽出します。" };

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() { return 2003300; }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { module = GetModuleHandleW(nullptr); return true; }
EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) { config_handle = handle; }
EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() { return &common_plugin_table; }
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) { app.Initialize(module, config_handle ? config_handle->app_data_path : L"."); app.RegisterWithHost(host); }
EXTERN_C __declspec(dllexport) void UninitializePlugin() {}
