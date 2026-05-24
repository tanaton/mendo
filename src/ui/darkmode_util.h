#pragma once
#include <windows.h>

// app と tooltip など複数 TU から呼ばれるためフリー関数として独立配置。
// 実装は app.cpp に存在し、`<dwmapi.h>` / `<uxtheme.h>` への依存をヘッダで露出しないために
// 宣言だけをここに置く。
void ApplyDarkModeToWindow(HWND hwnd, bool dark);
