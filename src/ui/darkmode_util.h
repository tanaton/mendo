#pragma once
#include <windows.h>

// app と tooltip など複数 TU から呼ばれるためフリー関数として独立配置。
// `<dwmapi.h>` / `<uxtheme.h>` への依存をヘッダで露出しないために宣言だけをここに置く。
void ApplyDarkModeToWindow(HWND hwnd, bool dark);
