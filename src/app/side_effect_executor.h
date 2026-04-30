#pragma once
#include "side_effect.h"
#include <functional>
#include <string_view>

class IWin32Host;
class ResourceManager;
class DocumentService;
class LayoutService;
struct AppState;

class SideEffectExecutor {
public:
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
        std::move_only_function<void()> process_deferred_layout;
        std::move_only_function<void()> tick_loading_animation;
        std::move_only_function<void()> process_mermaid_batch_timer;
        std::move_only_function<void()> process_bitmap_manage;
        std::move_only_function<void()> mermaid_init_retry;
        std::move_only_function<void()> destroy;
        std::move_only_function<void()> handle_parse_complete;
        std::move_only_function<void(int, int)> show_context_menu;
        std::move_only_function<void()> sync_toc_active;
    };

    void Init(IWin32Host& host, ResourceManager& resource_manager,
              DocumentService& doc_service,
              AppState& state, LayoutService& layout_service, Callbacks cb);
    void Execute(const SideEffectList& effects);
    void ExecuteOne(const SideEffect& e);

private:
    // ドメインごとの内側 variant ハンドラ。ExecuteOne から委譲される。
    void ExecuteUi(const UiEffect& e);
    void ExecuteWindow(const WindowEffect& e);
    void ExecuteNavigation(const NavigationEffect& e);
    void ExecuteLayout(const LayoutEffect& e);
    void ExecuteResource(const ResourceEffect& e);
    void ExecuteTimer(const TimerEffect& e);
    void ExecuteLifecycle(const LifecycleEffect& e);

    IWin32Host* host_ = nullptr;
    ResourceManager* resource_manager_ = nullptr;
    DocumentService* doc_service_ = nullptr;
    AppState* state_ = nullptr;
    LayoutService* layout_service_ = nullptr;
    Callbacks cb_;
};
