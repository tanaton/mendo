#pragma once
#include "side_effect.h"
#include <functional>
#include <string_view>
#include <windows.h>

class ResourceManager;
class CursorManager;
class DocumentService;
class ConfigService;
class LayoutService;
struct AppState;

// SideEffectExecutor: Reducer が返した副作用リストを実行する。
// Win32 API 呼び出し等のプラットフォーム固有処理をカプセル化する。
class SideEffectExecutor {
public:
    // App のメソッドチェーンに依存する複雑な操作のコールバック
    struct Callbacks {
        std::function<void(std::wstring_view)> load_file;
        std::function<void()> reload_file;
        std::function<void()> open_file_dialog;
    };

    void Init(HWND hwnd, ResourceManager& resource_manager,
              CursorManager& cursors, DocumentService& doc_service,
              ConfigService& config, AppState& state,
              LayoutService& layout_service, Callbacks cb);
    void Execute(const SideEffectList& effects);

private:
    HWND hwnd_ = nullptr;
    ResourceManager* resource_manager_ = nullptr;
    CursorManager* cursors_ = nullptr;
    DocumentService* doc_service_ = nullptr;
    ConfigService* config_ = nullptr;
    AppState* state_ = nullptr;
    LayoutService* layout_service_ = nullptr;
    Callbacks cb_;
};
