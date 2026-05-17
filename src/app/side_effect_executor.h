#pragma once
#include "side_effect.h"
#include <string_view>

class IWin32Host;
class FileWatcher;
class LayoutService;
struct AppState;

// Cb は以下のメソッドを提供する duck type。
//   void load_file(std::wstring_view);
//   void reload_file();
//   void open_file_dialog();
//   void invalidate_pane_cache(PaneZone);
//   void refresh_pane_layout();
//   void renderer_resize(UINT, UINT);
//   void renderer_set_dpi(float);
//   void clear_file_cache();
//   void perform_resize_end();
//   void perform_sizing_update();
//   void apply_theme_change(const effect::ApplyThemeChange&);
//   void process_deferred_layout();
//   void tick_loading_animation();
//   void process_mermaid_batch_timer();
//   void process_bitmap_manage();
//   void mermaid_init_retry();
//   void destroy();
//   void handle_parse_complete();
//   void show_context_menu(int, int);
//   void sync_toc_active();
//   // 旧 ResourceManager* 経由のものを吸収
//   void schedule_bitmap_manage();
//   void schedule_mermaid_batch();
//   void load_images();
//   void request_mermaid_renders();
//   void cancel_mermaid_batch();
//   void on_app_image_loaded();
template <class Cb>
class SideEffectExecutorT {
public:
    void Init(IWin32Host& host,
              FileWatcher& file_watcher,
              AppState& state,
              LayoutService& layout_service,
              Cb cb);
    void Execute(const SideEffectList& effects);
    void ExecuteOne(const SideEffect& e);

private:
    IWin32Host* host_ = nullptr;
    FileWatcher* file_watcher_ = nullptr;
    AppState* state_ = nullptr;
    LayoutService* layout_service_ = nullptr;
    Cb cb_{};
};
