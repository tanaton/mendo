#include <gtest/gtest.h>
#include "side_effect_executor.h"
#include "app_state.h"
#include "resource_manager.h"
#include "cursor_manager.h"
#include "document_service.h"
#include "config_service.h"
#include "layout_service.h"
#include "layout.h"
#include "file_watcher.h"

// side_effect_executor.cpp が extern 参照するシンボルのダミー。本体は app.cpp。
// ApplyDarkMode 副作用はテストで発火しないが、LTCG が効かない場合のリンク保険。
void ApplyDarkModeToWindow(HWND, bool) {}

class SideEffectExecutorTest : public ::testing::Test {
protected:
    // --- 依存クラスの実体（default-construct） ---
    FileWatcher watcher_;
    DocumentService doc_service_{watcher_};
    ConfigService config_;
    ResourceManager resource_manager_;
    CursorManager cursors_;
    AppState state_;
    LayoutEngine engine_;
    LayoutService layout_service_{engine_, state_.view.viewport};
    SideEffectExecutor exec_;

    // --- スパイ記録 ---
    std::vector<std::pmr::wstring> load_file_paths_;
    int reload_file_count_ = 0;
    int open_file_dialog_count_ = 0;
    std::vector<PaneZone> invalidate_pane_cache_calls_;
    int refresh_pane_layout_count_ = 0;
    std::pair<UINT, UINT> last_renderer_resize_{0, 0};
    int renderer_resize_count_ = 0;
    float last_renderer_dpi_ = 0.0f;
    int renderer_set_dpi_count_ = 0;
    int clear_file_cache_count_ = 0;
    int perform_resize_end_count_ = 0;
    int perform_sizing_update_count_ = 0;
    effect::ApplyThemeChange last_theme_change_{};
    int apply_theme_change_count_ = 0;
    int process_deferred_layout_count_ = 0;
    int tick_loading_animation_count_ = 0;
    int process_mermaid_batch_timer_count_ = 0;
    int process_bitmap_manage_count_ = 0;
    int mermaid_init_retry_count_ = 0;
    int destroy_count_ = 0;
    int handle_parse_complete_count_ = 0;
    struct MouseEventRecord { effect::MouseEventType type; int px; int py; };
    std::vector<MouseEventRecord> mouse_events_;
    std::pair<int, int> last_context_menu_{0, 0};
    int context_menu_count_ = 0;

    SideEffectExecutor::Callbacks MakeCallbacks()
    {
        return {
            .load_file = [this](std::wstring_view p) {
                load_file_paths_.emplace_back(std::pmr::wstring{p});
            },
            .reload_file = [this] { reload_file_count_++; },
            .open_file_dialog = [this] { open_file_dialog_count_++; },
            .invalidate_pane_cache = [this](PaneZone p) {
                invalidate_pane_cache_calls_.push_back(p);
            },
            .refresh_pane_layout = [this] { refresh_pane_layout_count_++; },
            .renderer_resize = [this](UINT w, UINT h) {
                last_renderer_resize_ = {w, h};
                renderer_resize_count_++;
            },
            .renderer_set_dpi = [this](float dpi) {
                last_renderer_dpi_ = dpi;
                renderer_set_dpi_count_++;
            },
            .clear_file_cache = [this] { clear_file_cache_count_++; },
            .perform_resize_end = [this] { perform_resize_end_count_++; },
            .perform_sizing_update = [this] { perform_sizing_update_count_++; },
            .apply_theme_change = [this](const effect::ApplyThemeChange& e) {
                last_theme_change_ = e;
                apply_theme_change_count_++;
            },
            .process_deferred_layout = [this] { process_deferred_layout_count_++; },
            .tick_loading_animation = [this] { tick_loading_animation_count_++; },
            .process_mermaid_batch_timer = [this] { process_mermaid_batch_timer_count_++; },
            .process_bitmap_manage = [this] { process_bitmap_manage_count_++; },
            .mermaid_init_retry = [this] { mermaid_init_retry_count_++; },
            .destroy = [this] { destroy_count_++; },
            .handle_parse_complete = [this] { handle_parse_complete_count_++; },
            .handle_mouse_event = [this](effect::MouseEventType t, int px, int py) {
                mouse_events_.push_back({t, px, py});
            },
            .handle_context_menu = [this](int x, int y) {
                last_context_menu_ = {x, y};
                context_menu_count_++;
            },
        };
    }

    void SetUp() override
    {
        exec_.Init(/*hwnd=*/nullptr, resource_manager_, cursors_, doc_service_,
                   config_, state_, layout_service_, MakeCallbacks());
    }
};

// ═══════════════════════════════════════════════
// Callback 委譲型副作用
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, LoadFileDispatchesToCallback)
{
    exec_.ExecuteOne(effect::LoadFile{std::pmr::wstring{L"C:/doc.md"}});
    ASSERT_EQ(load_file_paths_.size(), 1u);
    EXPECT_EQ(load_file_paths_[0], L"C:/doc.md");
}

TEST_F(SideEffectExecutorTest, ReloadFileDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ReloadFile{});
    EXPECT_EQ(reload_file_count_, 1);
}

TEST_F(SideEffectExecutorTest, OpenFileDialogDispatchesToCallback)
{
    exec_.ExecuteOne(effect::OpenFileDialog{});
    EXPECT_EQ(open_file_dialog_count_, 1);
}

TEST_F(SideEffectExecutorTest, InvalidatePaneCacheForwardsPaneZone)
{
    exec_.ExecuteOne(effect::InvalidatePaneCache{PaneZone::MdPane});
    exec_.ExecuteOne(effect::InvalidatePaneCache{PaneZone::FilePane});
    ASSERT_EQ(invalidate_pane_cache_calls_.size(), 2u);
    EXPECT_EQ(invalidate_pane_cache_calls_[0], PaneZone::MdPane);
    EXPECT_EQ(invalidate_pane_cache_calls_[1], PaneZone::FilePane);
}

TEST_F(SideEffectExecutorTest, RefreshPaneLayoutDispatchesToCallback)
{
    exec_.ExecuteOne(effect::RefreshPaneLayout{});
    EXPECT_EQ(refresh_pane_layout_count_, 1);
}

TEST_F(SideEffectExecutorTest, RendererResizeForwardsDimensions)
{
    exec_.ExecuteOne(effect::RendererResize{1920, 1080});
    EXPECT_EQ(renderer_resize_count_, 1);
    EXPECT_EQ(last_renderer_resize_, std::make_pair(UINT{1920}, UINT{1080}));
}

TEST_F(SideEffectExecutorTest, RendererSetDpiForwardsDpiValue)
{
    exec_.ExecuteOne(effect::RendererSetDpi{144.0f});
    EXPECT_EQ(renderer_set_dpi_count_, 1);
    EXPECT_FLOAT_EQ(last_renderer_dpi_, 144.0f);
}

TEST_F(SideEffectExecutorTest, ClearFileCacheDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ClearFileCache{});
    EXPECT_EQ(clear_file_cache_count_, 1);
}

TEST_F(SideEffectExecutorTest, PerformResizeEndDispatchesToCallback)
{
    exec_.ExecuteOne(effect::PerformResizeEnd{});
    EXPECT_EQ(perform_resize_end_count_, 1);
}

TEST_F(SideEffectExecutorTest, PerformSizingUpdateDispatchesToCallback)
{
    exec_.ExecuteOne(effect::PerformSizingUpdate{});
    EXPECT_EQ(perform_sizing_update_count_, 1);
}

TEST_F(SideEffectExecutorTest, ApplyThemeChangeForwardsStruct)
{
    effect::ApplyThemeChange in{};
    in.type = effect::ApplyThemeChange::Type::Zoom;
    in.anchor_idx = 5;
    in.anchor_y_before = 100.0f;
    in.anchor_offset = 10.0f;
    in.offset_scale = 1.25f;
    in.new_zoom = 1.25f;
    in.zoom_index = 4;
    exec_.ExecuteOne(in);
    EXPECT_EQ(apply_theme_change_count_, 1);
    EXPECT_EQ(last_theme_change_.type, effect::ApplyThemeChange::Type::Zoom);
    EXPECT_EQ(last_theme_change_.anchor_idx, 5);
    EXPECT_FLOAT_EQ(last_theme_change_.anchor_y_before, 100.0f);
    EXPECT_FLOAT_EQ(last_theme_change_.offset_scale, 1.25f);
    EXPECT_EQ(last_theme_change_.zoom_index, 4);
}

TEST_F(SideEffectExecutorTest, ProcessDeferredLayoutDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ProcessDeferredLayout{});
    EXPECT_EQ(process_deferred_layout_count_, 1);
}

TEST_F(SideEffectExecutorTest, TickLoadingAnimationDispatchesToCallback)
{
    exec_.ExecuteOne(effect::TickLoadingAnimation{});
    EXPECT_EQ(tick_loading_animation_count_, 1);
}

TEST_F(SideEffectExecutorTest, ProcessMermaidBatchTimerDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ProcessMermaidBatchTimer{});
    EXPECT_EQ(process_mermaid_batch_timer_count_, 1);
}

TEST_F(SideEffectExecutorTest, ProcessBitmapManageDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ProcessBitmapManage{});
    EXPECT_EQ(process_bitmap_manage_count_, 1);
}

TEST_F(SideEffectExecutorTest, MermaidInitRetryDispatchesToCallback)
{
    exec_.ExecuteOne(effect::MermaidInitRetry{});
    EXPECT_EQ(mermaid_init_retry_count_, 1);
}

TEST_F(SideEffectExecutorTest, DestroyDispatchesToCallback)
{
    exec_.ExecuteOne(effect::Destroy{});
    EXPECT_EQ(destroy_count_, 1);
}

TEST_F(SideEffectExecutorTest, HandleParseCompleteDispatchesToCallback)
{
    exec_.ExecuteOne(effect::HandleParseComplete{});
    EXPECT_EQ(handle_parse_complete_count_, 1);
}

TEST_F(SideEffectExecutorTest, HandleMouseEventForwardsAllFields)
{
    exec_.ExecuteOne(effect::HandleMouseEvent{effect::MouseEventType::LButtonDown, 100, 200});
    exec_.ExecuteOne(effect::HandleMouseEvent{effect::MouseEventType::MouseMove, 150, 250});
    ASSERT_EQ(mouse_events_.size(), 2u);
    EXPECT_EQ(mouse_events_[0].type, effect::MouseEventType::LButtonDown);
    EXPECT_EQ(mouse_events_[0].px, 100);
    EXPECT_EQ(mouse_events_[0].py, 200);
    EXPECT_EQ(mouse_events_[1].type, effect::MouseEventType::MouseMove);
    EXPECT_EQ(mouse_events_[1].px, 150);
}

TEST_F(SideEffectExecutorTest, HandleContextMenuForwardsScreenCoords)
{
    exec_.ExecuteOne(effect::HandleContextMenu{800, 600});
    EXPECT_EQ(context_menu_count_, 1);
    EXPECT_EQ(last_context_menu_, std::make_pair(800, 600));
}

// ═══════════════════════════════════════════════
// AppState を変更する副作用
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, ShowToastUpdatesToastState)
{
    exec_.ExecuteOne(effect::ShowToast{L"Copied"});
    EXPECT_TRUE(state_.interaction.toast.IsVisible());
    EXPECT_EQ(state_.interaction.toast.GetMessage(), L"Copied");
}

TEST_F(SideEffectExecutorTest, ShowToastOverwritesPreviousMessage)
{
    exec_.ExecuteOne(effect::ShowToast{L"First"});
    exec_.ExecuteOne(effect::ShowToast{L"Second"});
    EXPECT_EQ(state_.interaction.toast.GetMessage(), L"Second");
}

// ═══════════════════════════════════════════════
// Execute: 副作用リストを順番に実行する
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, ExecuteRunsAllEffectsInOrder)
{
    std::pmr::vector<SideEffect> list;
    list.emplace_back(effect::ReloadFile{});
    list.emplace_back(effect::LoadFile{std::pmr::wstring{L"A"}});
    list.emplace_back(effect::LoadFile{std::pmr::wstring{L"B"}});
    list.emplace_back(effect::Destroy{});

    exec_.Execute(list);

    EXPECT_EQ(reload_file_count_, 1);
    EXPECT_EQ(destroy_count_, 1);
    ASSERT_EQ(load_file_paths_.size(), 2u);
    EXPECT_EQ(load_file_paths_[0], L"A");
    EXPECT_EQ(load_file_paths_[1], L"B");
}

TEST_F(SideEffectExecutorTest, ExecuteEmptyListIsNoop)
{
    std::pmr::vector<SideEffect> list;
    exec_.Execute(list);
    EXPECT_EQ(reload_file_count_, 0);
    EXPECT_EQ(destroy_count_, 0);
}

// HWND=nullptr で Win32 API 呼び出し分岐（InvalidateRect/ShowWindow/PostMessage/
// SetWindowText/SetWindowPos）が内部で失敗終了することの確認。
// メッセージ定数は傍受されにくい WM_USER+α を使う（WM_CLOSE 等はテストハーネスの
// メッセージポンプと干渉しうるため避ける）。
// CursorManager 未初期化ルートの SetCursor もここでまとめて走らせる。
TEST_F(SideEffectExecutorTest, Win32EffectsWithNullHwndDoNotCrash)
{
    exec_.ExecuteOne(effect::InvalidateWindow{});
    exec_.ExecuteOne(effect::InvalidateTitleBar{});
    exec_.ExecuteOne(effect::ShowWindowCmd{1});
    exec_.ExecuteOne(effect::PostMessage{WM_USER + 42, 0, 0});
    exec_.ExecuteOne(effect::SetWindowTitle{std::pmr::wstring{L"hello"}});
    exec_.ExecuteOne(effect::SetWindowPosition{10, 20, 800, 600});
    exec_.ExecuteOne(effect::SetCursor{effect::CursorType::Arrow});
    exec_.ExecuteOne(effect::SetCursor{effect::CursorType::Hand});
    exec_.ExecuteOne(effect::SetCursor{effect::CursorType::IBeam});
    exec_.ExecuteOne(effect::SetCursor{effect::CursorType::SizeWE});
}
