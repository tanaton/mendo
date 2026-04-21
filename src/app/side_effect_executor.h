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
        std::move_only_function<void(std::wstring_view)> load_file;
        std::move_only_function<void()> reload_file;
        std::move_only_function<void()> open_file_dialog;
        std::move_only_function<void(PaneZone)> invalidate_pane_cache;
        std::move_only_function<void()> refresh_pane_layout;
        std::move_only_function<void(UINT, UINT)> renderer_resize;
        std::move_only_function<void(float)> renderer_set_dpi;
        std::move_only_function<void()> clear_file_cache;
        std::move_only_function<void()> perform_resize_end;
        std::move_only_function<void()> perform_sizing_update;
        std::move_only_function<void(const effect::ApplyThemeChange&)> apply_theme_change;
        std::move_only_function<void(const effect::CompensateScrollAfterLayout&)> compensate_scroll_after_layout;
        std::move_only_function<void()> process_deferred_layout;
        std::move_only_function<void()> tick_loading_animation;
        std::move_only_function<void()> process_mermaid_batch_timer;
        std::move_only_function<void()> process_bitmap_manage;
        std::move_only_function<void()> mermaid_init_retry;
        std::move_only_function<void()> destroy;
        std::move_only_function<void()> handle_parse_complete;
        std::move_only_function<void(int, int)> show_context_menu;
    };

    void Init(HWND hwnd, ResourceManager& resource_manager,
        CursorManager& cursors, DocumentService& doc_service,
        ConfigService& config, AppState& state,
        LayoutService& layout_service, Callbacks cb);
    void Execute(const SideEffectList& effects);
    void ExecuteOne(const SideEffect& e);

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
