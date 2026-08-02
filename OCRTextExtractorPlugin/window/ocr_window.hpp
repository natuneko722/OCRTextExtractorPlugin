#pragma once
#include "common/hive.hpp"
#include <windows.h>

class OCRWindow {
public:
    static bool Register(HINSTANCE instance);
    static HWND Create(HINSTANCE instance, Hive* hive);
private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};
