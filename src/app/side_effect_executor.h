#pragma once
#include "side_effect.h"
#include <windows.h>

class ResourceManager;

// SideEffectExecutor: Reducer が返した副作用リストを実行する。
// Win32 API 呼び出し等のプラットフォーム固有処理をカプセル化する。
class SideEffectExecutor {
public:
    void Init(HWND hwnd, ResourceManager& resource_manager) noexcept;
    void Execute(const SideEffectList& effects);

private:
    HWND hwnd_ = nullptr;
    ResourceManager* resource_manager_ = nullptr;
};
