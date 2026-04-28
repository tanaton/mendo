#include <gtest/gtest.h>
#include "side_effect_executor.h"
#include "win32_host.h"
#include "app_constants.h"
#include "app_state.h"
#include "resource_manager.h"
#include "document_service.h"
#include "config_service.h"
#include "layout.h"
#include "file_watcher.h"

// tooltip.cpp (mendo_core) が extern 参照するシンボルのダミー。本体は app.cpp。
// テストでは実行されないが、リンク充足のために必要。
void ApplyDarkModeToWindow(HWND, bool) {}

namespace {

// 副作用発火を記録する IWin32Host mock
class RecordingWin32Host final : public IWin32Host {
public:
    int invalidate_count = 0;
    std::vector<std::pair<int, int>> invalidate_titlebar_calls;
    std::vector<std::pair<UINT_PTR, UINT>> set_timer_calls;
    std::vector<UINT_PTR> kill_timer_calls;
    int set_capture_count = 0;
    int release_capture_count = 0;
    std::vector<effect::CursorType> set_cursor_calls;
    std::vector<std::wstring> clipboard_text_calls;
    std::vector<std::pair<std::wstring, std::wstring>> clipboard_html_calls;
    std::vector<std::wstring> shell_open_calls;
    std::vector<int> show_window_cmd_calls;
    std::vector<std::tuple<UINT, WPARAM, LPARAM>> post_message_calls;
    std::vector<std::wstring> set_window_title_calls;
    std::vector<std::tuple<int, int, int, int>> set_window_position_calls;
    std::vector<POINT> client_to_screen_inputs;
    POINT client_to_screen_translation{ 0, 0 };
    std::vector<bool> apply_dark_mode_calls;

    void Invalidate() override { invalidate_count++; }
    void InvalidateTitleBarArea(int width_px, int height_px) override
    {
        invalidate_titlebar_calls.emplace_back(width_px, height_px);
    }
    void SetTimer(UINT_PTR id, UINT ms) override { set_timer_calls.emplace_back(id, ms); }
    void KillTimer(UINT_PTR id) override { kill_timer_calls.push_back(id); }
    void SetCapture() override { set_capture_count++; }
    void ReleaseCapture() override { release_capture_count++; }
    void SetCursor(effect::CursorType type) override { set_cursor_calls.push_back(type); }
    void WriteClipboardText(std::wstring_view text) override { clipboard_text_calls.emplace_back(text); }
    void WriteClipboardHtml(std::wstring_view html, std::wstring_view plain) override
    {
        clipboard_html_calls.emplace_back(std::wstring{ html }, std::wstring{ plain });
    }
    void ShellOpen(const std::pmr::wstring& url) override { shell_open_calls.emplace_back(std::wstring_view{ url }); }
    void ShowWindowCmd(int cmd) override { show_window_cmd_calls.push_back(cmd); }
    void PostWindowMessage(UINT msg, WPARAM wp, LPARAM lp) override
    {
        post_message_calls.emplace_back(msg, wp, lp);
    }
    void SetWindowTitle(const std::pmr::wstring& title) override { set_window_title_calls.emplace_back(std::wstring_view{ title }); }
    void SetWindowPosition(int x, int y, int cx, int cy) override
    {
        set_window_position_calls.emplace_back(x, y, cx, cy);
    }
    POINT ClientToScreen(POINT client_pt) override
    {
        client_to_screen_inputs.push_back(client_pt);
        return { client_pt.x + client_to_screen_translation.x,
                 client_pt.y + client_to_screen_translation.y };
    }
    void ApplyDarkMode(bool dark) override { apply_dark_mode_calls.push_back(dark); }
};

} // namespace

class SideEffectExecutorTest : public ::testing::Test {
protected:
    // --- 依存クラスの実体（default-construct） ---
    RecordingWin32Host host_;
    FileWatcher watcher_;
    DocumentService doc_service_{ watcher_ };
    ConfigService config_;
    ResourceManager resource_manager_;
    AppState state_;
    LayoutEngine engine_;
    LayoutService layout_service_{ engine_, state_.view.viewport };
    SideEffectExecutor exec_;

    // --- スパイ記録 ---
    std::vector<std::pmr::wstring> load_file_paths_;
    int reload_file_count_ = 0;
    int open_file_dialog_count_ = 0;
    std::vector<PaneZone> invalidate_pane_cache_calls_;
    int refresh_pane_layout_count_ = 0;
    std::pair<UINT, UINT> last_renderer_resize_{ 0, 0 };
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
    std::pair<int, int> last_context_menu_pos_{ 0, 0 };
    int show_context_menu_count_ = 0;

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
            .show_context_menu = [this](int x, int y) {
                last_context_menu_pos_ = {x, y};
                show_context_menu_count_++;
            },
        };
    }

    void SetUp() override
    {
        exec_.Init(host_, resource_manager_, doc_service_, state_, layout_service_, MakeCallbacks());
    }
};

// ═══════════════════════════════════════════════
// Callback 委譲型副作用
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, LoadFileDispatchesToCallback)
{
    exec_.ExecuteOne(effect::LoadFile{ std::pmr::wstring{L"C:/doc.md"} });
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
    exec_.ExecuteOne(effect::InvalidatePaneCache{ PaneZone::MdPane });
    exec_.ExecuteOne(effect::InvalidatePaneCache{ PaneZone::FilePane });
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
    exec_.ExecuteOne(effect::RendererResize{ 1920, 1080 });
    EXPECT_EQ(renderer_resize_count_, 1);
    EXPECT_EQ(last_renderer_resize_, std::make_pair(UINT{ 1920 }, UINT{ 1080 }));
}

TEST_F(SideEffectExecutorTest, RendererSetDpiForwardsDpiValue)
{
    exec_.ExecuteOne(effect::RendererSetDpi{ 144.0f });
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
    in.new_zoom = 1.25f;
    in.zoom_index = 4;
    exec_.ExecuteOne(in);
    EXPECT_EQ(apply_theme_change_count_, 1);
    EXPECT_EQ(last_theme_change_.type, effect::ApplyThemeChange::Type::Zoom);
    EXPECT_FLOAT_EQ(last_theme_change_.new_zoom, 1.25f);
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

TEST_F(SideEffectExecutorTest, ShowContextMenuForwardsScreenPosition)
{
    exec_.ExecuteOne(effect::ShowContextMenu{ 150, 200 });
    EXPECT_EQ(show_context_menu_count_, 1);
    EXPECT_EQ(last_context_menu_pos_, std::make_pair(150, 200));
}

// ═══════════════════════════════════════════════
// AppState を変更する副作用
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, ShowToastUpdatesToastState)
{
    exec_.ExecuteOne(effect::ShowToast{ L"Copied" });
    EXPECT_TRUE(state_.interaction.toast.IsVisible());
    EXPECT_EQ(state_.interaction.toast.GetMessage(), L"Copied");
}

TEST_F(SideEffectExecutorTest, ShowToastOverwritesPreviousMessage)
{
    exec_.ExecuteOne(effect::ShowToast{ L"First" });
    exec_.ExecuteOne(effect::ShowToast{ L"Second" });
    EXPECT_EQ(state_.interaction.toast.GetMessage(), L"Second");
}

// ═══════════════════════════════════════════════
// Execute: 副作用リストを順番に実行する
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, ExecuteRunsAllEffectsInOrder)
{
    std::pmr::vector<SideEffect> list;
    list.emplace_back(effect::ReloadFile{});
    list.emplace_back(effect::LoadFile{ std::pmr::wstring{L"A"} });
    list.emplace_back(effect::LoadFile{ std::pmr::wstring{L"B"} });
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

// ═══════════════════════════════════════════════
// IWin32Host 経由の副作用（mock で発火を検証）
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, InvalidateWindowCallsHostInvalidate)
{
    exec_.ExecuteOne(effect::InvalidateWindow{});
    EXPECT_EQ(host_.invalidate_count, 1);
}

TEST_F(SideEffectExecutorTest, SetTimerAndKillTimerForwardToHost)
{
    exec_.ExecuteOne(effect::SetTimer{ 42, 100 });
    exec_.ExecuteOne(effect::KillTimer{ 42 });
    ASSERT_EQ(host_.set_timer_calls.size(), 1u);
    EXPECT_EQ(host_.set_timer_calls[0], std::make_pair(UINT_PTR{ 42 }, UINT{ 100 }));
    ASSERT_EQ(host_.kill_timer_calls.size(), 1u);
    EXPECT_EQ(host_.kill_timer_calls[0], UINT_PTR{ 42 });
}

TEST_F(SideEffectExecutorTest, SetCaptureAndReleaseCaptureForwardToHost)
{
    exec_.ExecuteOne(effect::SetCapture{});
    exec_.ExecuteOne(effect::ReleaseCapture{});
    EXPECT_EQ(host_.set_capture_count, 1);
    EXPECT_EQ(host_.release_capture_count, 1);
}

TEST_F(SideEffectExecutorTest, SetCursorForwardsTypeToHost)
{
    exec_.ExecuteOne(effect::SetCursor{ effect::CursorType::Arrow });
    exec_.ExecuteOne(effect::SetCursor{ effect::CursorType::Hand });
    exec_.ExecuteOne(effect::SetCursor{ effect::CursorType::IBeam });
    exec_.ExecuteOne(effect::SetCursor{ effect::CursorType::SizeWE });
    ASSERT_EQ(host_.set_cursor_calls.size(), 4u);
    EXPECT_EQ(host_.set_cursor_calls[0], effect::CursorType::Arrow);
    EXPECT_EQ(host_.set_cursor_calls[1], effect::CursorType::Hand);
    EXPECT_EQ(host_.set_cursor_calls[2], effect::CursorType::IBeam);
    EXPECT_EQ(host_.set_cursor_calls[3], effect::CursorType::SizeWE);
}

TEST_F(SideEffectExecutorTest, ClipboardEffectsForwardToHost)
{
    exec_.ExecuteOne(effect::ClipboardWrite{ std::pmr::wstring{L"hello"} });
    exec_.ExecuteOne(effect::ClipboardWriteHtml{ std::pmr::wstring{L"<p>html</p>"},
                                                std::pmr::wstring{L"plain"} });
    ASSERT_EQ(host_.clipboard_text_calls.size(), 1u);
    EXPECT_EQ(host_.clipboard_text_calls[0], L"hello");
    ASSERT_EQ(host_.clipboard_html_calls.size(), 1u);
    EXPECT_EQ(host_.clipboard_html_calls[0].first, L"<p>html</p>");
    EXPECT_EQ(host_.clipboard_html_calls[0].second, L"plain");
}

TEST_F(SideEffectExecutorTest, ShellOpenForwardsUrlToHost)
{
    exec_.ExecuteOne(effect::ShellOpen{ std::pmr::wstring{L"https://example.com"} });
    ASSERT_EQ(host_.shell_open_calls.size(), 1u);
    EXPECT_EQ(host_.shell_open_calls[0], L"https://example.com");
}

TEST_F(SideEffectExecutorTest, ShowWindowCmdForwardsValueToHost)
{
    exec_.ExecuteOne(effect::ShowWindowCmd{ SW_MAXIMIZE });
    ASSERT_EQ(host_.show_window_cmd_calls.size(), 1u);
    EXPECT_EQ(host_.show_window_cmd_calls[0], SW_MAXIMIZE);
}

TEST_F(SideEffectExecutorTest, PostWindowMessageForwardsToHost)
{
    exec_.ExecuteOne(effect::PostWindowMessage{ WM_USER + 1, 7, 13 });
    ASSERT_EQ(host_.post_message_calls.size(), 1u);
    EXPECT_EQ(std::get<0>(host_.post_message_calls[0]), UINT{ WM_USER + 1 });
    EXPECT_EQ(std::get<1>(host_.post_message_calls[0]), WPARAM{ 7 });
    EXPECT_EQ(std::get<2>(host_.post_message_calls[0]), LPARAM{ 13 });
}

TEST_F(SideEffectExecutorTest, SetWindowTitleForwardsToHost)
{
    exec_.ExecuteOne(effect::SetWindowTitle{ std::pmr::wstring{L"mendo — doc.md"} });
    ASSERT_EQ(host_.set_window_title_calls.size(), 1u);
    EXPECT_EQ(host_.set_window_title_calls[0], L"mendo — doc.md");
}

TEST_F(SideEffectExecutorTest, SetWindowPositionForwardsToHost)
{
    exec_.ExecuteOne(effect::SetWindowPosition{ 10, 20, 800, 600 });
    ASSERT_EQ(host_.set_window_position_calls.size(), 1u);
    EXPECT_EQ(host_.set_window_position_calls[0], std::make_tuple(10, 20, 800, 600));
}

TEST_F(SideEffectExecutorTest, ApplyDarkModeForwardsFlagToHost)
{
    exec_.ExecuteOne(effect::ApplyDarkMode{ true });
    exec_.ExecuteOne(effect::ApplyDarkMode{ false });
    ASSERT_EQ(host_.apply_dark_mode_calls.size(), 2u);
    EXPECT_TRUE(host_.apply_dark_mode_calls[0]);
    EXPECT_FALSE(host_.apply_dark_mode_calls[1]);
}

TEST_F(SideEffectExecutorTest, InvalidateTitleBarComputesRectFromCachedWidthAndDpi)
{
    state_.window.cached_dpi_scale = 2.0f;
    state_.cached_window_width_for_layout = 800.0f;
    // Titlebar::GetHeight() は constexpr 32.0f を返す。
    // height: 32.0f * 2.0f + 0.5f = 64.5f → int cast で 64
    // width:  800.0f * 2.0f → 1600 +1 で 1601 (境界ピクセル切れ防止)
    exec_.ExecuteOne(effect::InvalidateTitleBar{});
    ASSERT_EQ(host_.invalidate_titlebar_calls.size(), 1u);
    EXPECT_EQ(host_.invalidate_titlebar_calls[0], std::make_pair(1601, 64));
    EXPECT_EQ(host_.invalidate_count, 0);
}

TEST_F(SideEffectExecutorTest, InvalidateTitleBarFallsBackToFullInvalidateWhenWidthUnknown)
{
    state_.window.cached_dpi_scale = 1.0f;
    state_.cached_window_width_for_layout = 0.0f;
    exec_.ExecuteOne(effect::InvalidateTitleBar{});
    EXPECT_TRUE(host_.invalidate_titlebar_calls.empty());
    EXPECT_EQ(host_.invalidate_count, 1);
}

TEST_F(SideEffectExecutorTest, ShowToastSchedulesTimerAndInvalidates)
{
    exec_.ExecuteOne(effect::ShowToast{ L"Copied" });
    ASSERT_EQ(host_.set_timer_calls.size(), 1u);
    EXPECT_EQ(host_.set_timer_calls[0].first, UINT_PTR{ app_timer::TOAST });
    EXPECT_EQ(host_.invalidate_count, 1);
}
