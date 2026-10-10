#include <gtest/gtest.h>
#include "side_effect_executor.h"
#include "win32_host.h"
#include "app_constants.h"
#include "app_state.h"
#include "file_watcher.h"

namespace {

// 副作用発火を記録する IWin32Host mock
class RecordingWin32Host final : public IWin32Host {
public:
    int invalidate_count = 0;
    std::vector<std::pair<app_timer::Id, UINT>> set_timer_calls;
    std::vector<app_timer::Id> kill_timer_calls;
    int set_capture_count = 0;
    int release_capture_count = 0;
    std::vector<std::string> clipboard_text_calls;
    std::vector<std::pair<std::string, std::string>> clipboard_html_calls;
    std::vector<std::wstring> shell_open_calls;
    std::vector<std::tuple<int, int, int, int>> set_window_position_calls;

    void Invalidate() override
    {
        invalidate_count++;
    }
    void SetTimer(app_timer::Id id, UINT ms) override
    {
        set_timer_calls.emplace_back(id, ms);
    }
    void KillTimer(app_timer::Id id) override
    {
        kill_timer_calls.push_back(id);
    }
    void SetCapture() override
    {
        set_capture_count++;
    }
    void ReleaseCapture() override
    {
        release_capture_count++;
    }
    void WriteClipboardText(std::string_view text) override
    {
        clipboard_text_calls.emplace_back(text);
    }
    void WriteClipboardHtml(std::string_view html, std::string_view plain) override
    {
        clipboard_html_calls.emplace_back(std::string{ html }, std::string{ plain });
    }
    void ShellOpen(const std::pmr::wstring& url) override
    {
        shell_open_calls.emplace_back(std::wstring_view{ url });
    }
    std::vector<effect::SearchFocus> search_focus_calls;
    std::vector<effect::SearchUnfocus> search_unfocus_calls;
    void SearchFocus(effect::SearchFocus action) override
    {
        search_focus_calls.push_back(action);
    }
    void SearchUnfocus(effect::SearchUnfocus action) override
    {
        search_unfocus_calls.push_back(action);
    }
    void SetWindowPosition(int x, int y, int cx, int cy) override
    {
        set_window_position_calls.emplace_back(x, y, cx, cy);
    }
};

struct CallbackTracker {
    std::vector<std::wstring> load_file_paths;
    int reload_file_count = 0;
    int open_file_dialog_count = 0;
    std::vector<PaneZone> invalidate_pane_cache_calls;
    int refresh_pane_layout_count = 0;
    std::pair<UINT, UINT> last_renderer_resize{ 0, 0 };
    int renderer_resize_count = 0;
    float last_renderer_dpi = 0.0f;
    int renderer_set_dpi_count = 0;
    int clear_file_cache_count = 0;
    int perform_resize_end_count = 0;
    int perform_sizing_update_count = 0;
    effect::ApplyThemeChange last_theme_change{};
    int apply_theme_change_count = 0;
    int process_deferred_layout_count = 0;
    int tick_loading_animation_count = 0;
    int process_mermaid_batch_timer_count = 0;
    int process_bitmap_manage_count = 0;
    int mermaid_init_retry_count = 0;
    int mermaid_idle_count = 0;
    std::pair<int, int> last_context_menu_pos{ 0, 0 };
    int show_context_menu_count = 0;
    std::vector<bool> sync_toc_auto_scroll_calls;
};

struct TestSideEffectCallbacks {
    CallbackTracker* t = nullptr;

    void load_file(std::wstring_view p)
    {
        t->load_file_paths.emplace_back(p);
    }
    void reload_file()
    {
        t->reload_file_count++;
    }
    void open_file_dialog()
    {
        t->open_file_dialog_count++;
    }
    void invalidate_pane_cache(PaneZone p)
    {
        t->invalidate_pane_cache_calls.push_back(p);
    }
    void refresh_pane_layout()
    {
        t->refresh_pane_layout_count++;
    }
    void renderer_resize(UINT w, UINT h)
    {
        t->last_renderer_resize = { w, h };
        t->renderer_resize_count++;
    }
    void renderer_set_dpi(float dpi)
    {
        t->last_renderer_dpi = dpi;
        t->renderer_set_dpi_count++;
    }
    void clear_file_cache()
    {
        t->clear_file_cache_count++;
    }
    void perform_resize_end()
    {
        t->perform_resize_end_count++;
    }
    void perform_sizing_update()
    {
        t->perform_sizing_update_count++;
    }
    void apply_theme_change(const effect::ApplyThemeChange& e)
    {
        t->last_theme_change = e;
        t->apply_theme_change_count++;
    }
    void process_deferred_layout()
    {
        t->process_deferred_layout_count++;
    }
    void tick_loading_animation()
    {
        t->tick_loading_animation_count++;
    }
    void process_mermaid_batch_timer()
    {
        t->process_mermaid_batch_timer_count++;
    }
    void process_bitmap_manage()
    {
        t->process_bitmap_manage_count++;
    }
    void mermaid_init_retry()
    {
        t->mermaid_init_retry_count++;
    }
    void mermaid_idle()
    {
        t->mermaid_idle_count++;
    }
    void show_context_menu(int x, int y)
    {
        t->last_context_menu_pos = { x, y };
        t->show_context_menu_count++;
    }
    void sync_toc_active(bool auto_scroll)
    {
        t->sync_toc_auto_scroll_calls.push_back(auto_scroll);
    }

    void schedule_bitmap_manage()
    {}
    void on_app_image_loaded()
    {}
};

} // namespace

class SideEffectExecutorTest : public ::testing::Test {
protected:
    // --- 依存クラスの実体（default-construct） ---
    RecordingWin32Host host_;
    FileWatcher watcher_;
    AppState state_;
    CallbackTracker tracker_;
    SideEffectExecutorT<TestSideEffectCallbacks> exec_;

    void SetUp() override
    {
        exec_.Init(
            SideEffectExecutorDeps{
                .host = &host_,
                .file_watcher = &watcher_,
                .state = &state_,
            },
            TestSideEffectCallbacks{ &tracker_ });
    }
};

// ═══════════════════════════════════════════════
// Callback 委譲型副作用
// ═══════════════════════════════════════════════

TEST_F(SideEffectExecutorTest, LoadFileDispatchesToCallback)
{
    exec_.ExecuteOne(effect::LoadFile{ std::pmr::wstring(L"C:/doc.md") });
    ASSERT_EQ(tracker_.load_file_paths.size(), 1u);
    EXPECT_EQ(tracker_.load_file_paths[0], L"C:/doc.md");
}

TEST_F(SideEffectExecutorTest, ReloadFileDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ReloadFile{});
    EXPECT_EQ(tracker_.reload_file_count, 1);
}

TEST_F(SideEffectExecutorTest, OpenFileDialogDispatchesToCallback)
{
    exec_.ExecuteOne(effect::OpenFileDialog{});
    EXPECT_EQ(tracker_.open_file_dialog_count, 1);
}

TEST_F(SideEffectExecutorTest, InvalidatePaneCacheForwardsPaneZone)
{
    exec_.ExecuteOne(effect::InvalidatePaneCache{ PaneZone::MdPane });
    exec_.ExecuteOne(effect::InvalidatePaneCache{ PaneZone::FilePane });
    ASSERT_EQ(tracker_.invalidate_pane_cache_calls.size(), 2u);
    EXPECT_EQ(tracker_.invalidate_pane_cache_calls[0], PaneZone::MdPane);
    EXPECT_EQ(tracker_.invalidate_pane_cache_calls[1], PaneZone::FilePane);
}

TEST_F(SideEffectExecutorTest, RefreshPaneLayoutDispatchesToCallback)
{
    exec_.ExecuteOne(effect::RefreshPaneLayout{});
    EXPECT_EQ(tracker_.refresh_pane_layout_count, 1);
}

TEST_F(SideEffectExecutorTest, SyncTocActiveForwardsAutoScrollFlag)
{
    exec_.ExecuteOne(effect::SyncTocActive{});
    exec_.ExecuteOne(effect::SyncTocActive{ /*auto_scroll=*/false });
    ASSERT_EQ(tracker_.sync_toc_auto_scroll_calls.size(), 2u);
    EXPECT_TRUE(tracker_.sync_toc_auto_scroll_calls[0]);
    EXPECT_FALSE(tracker_.sync_toc_auto_scroll_calls[1]);
}

TEST_F(SideEffectExecutorTest, RendererResizeForwardsDimensions)
{
    exec_.ExecuteOne(effect::RendererResize{ 1920, 1080 });
    EXPECT_EQ(tracker_.renderer_resize_count, 1);
    EXPECT_EQ(tracker_.last_renderer_resize, std::make_pair(UINT{ 1920 }, UINT{ 1080 }));
}

TEST_F(SideEffectExecutorTest, RendererSetDpiForwardsDpiValue)
{
    exec_.ExecuteOne(effect::RendererSetDpi{ 144.0f });
    EXPECT_EQ(tracker_.renderer_set_dpi_count, 1);
    EXPECT_FLOAT_EQ(tracker_.last_renderer_dpi, 144.0f);
}

TEST_F(SideEffectExecutorTest, ClearFileCacheDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ClearFileCache{});
    EXPECT_EQ(tracker_.clear_file_cache_count, 1);
}

TEST_F(SideEffectExecutorTest, PerformResizeEndDispatchesToCallback)
{
    exec_.ExecuteOne(effect::PerformResizeEnd{});
    EXPECT_EQ(tracker_.perform_resize_end_count, 1);
}

TEST_F(SideEffectExecutorTest, PerformSizingUpdateDispatchesToCallback)
{
    exec_.ExecuteOne(effect::PerformSizingUpdate{});
    EXPECT_EQ(tracker_.perform_sizing_update_count, 1);
}

TEST_F(SideEffectExecutorTest, ApplyThemeChangeForwardsStruct)
{
    exec_.ExecuteOne(effect::ApplyThemeChange{ effect::ApplyThemeChange::Type::Zoom });
    EXPECT_EQ(tracker_.apply_theme_change_count, 1);
    EXPECT_EQ(tracker_.last_theme_change.type, effect::ApplyThemeChange::Type::Zoom);
}

TEST_F(SideEffectExecutorTest, ProcessDeferredLayoutDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ProcessDeferredLayout{});
    EXPECT_EQ(tracker_.process_deferred_layout_count, 1);
}

TEST_F(SideEffectExecutorTest, TickLoadingAnimationDispatchesToCallback)
{
    exec_.ExecuteOne(effect::TickLoadingAnimation{});
    EXPECT_EQ(tracker_.tick_loading_animation_count, 1);
}

TEST_F(SideEffectExecutorTest, ProcessMermaidBatchTimerDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ProcessMermaidBatchTimer{});
    EXPECT_EQ(tracker_.process_mermaid_batch_timer_count, 1);
}

TEST_F(SideEffectExecutorTest, ProcessBitmapManageDispatchesToCallback)
{
    exec_.ExecuteOne(effect::ProcessBitmapManage{});
    EXPECT_EQ(tracker_.process_bitmap_manage_count, 1);
}

TEST_F(SideEffectExecutorTest, MermaidInitRetryDispatchesToCallback)
{
    exec_.ExecuteOne(effect::MermaidInitRetry{});
    EXPECT_EQ(tracker_.mermaid_init_retry_count, 1);
}

TEST_F(SideEffectExecutorTest, MermaidIdleForwardsToCallback)
{
    exec_.ExecuteOne(effect::MermaidIdle{});
    EXPECT_EQ(tracker_.mermaid_idle_count, 1);
}

TEST_F(SideEffectExecutorTest, ShowContextMenuForwardsScreenPosition)
{
    exec_.ExecuteOne(effect::ShowContextMenu{ 150, 200 });
    EXPECT_EQ(tracker_.show_context_menu_count, 1);
    EXPECT_EQ(tracker_.last_context_menu_pos, std::make_pair(150, 200));
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
    list.emplace_back(effect::LoadFile{ std::pmr::wstring(L"A") });
    list.emplace_back(effect::LoadFile{ std::pmr::wstring(L"B") });
    list.emplace_back(effect::MermaidInitRetry{});

    exec_.Execute(list);

    EXPECT_EQ(tracker_.reload_file_count, 1);
    EXPECT_EQ(tracker_.mermaid_init_retry_count, 1);
    ASSERT_EQ(tracker_.load_file_paths.size(), 2u);
    EXPECT_EQ(tracker_.load_file_paths[0], L"A");
    EXPECT_EQ(tracker_.load_file_paths[1], L"B");
}

TEST_F(SideEffectExecutorTest, ExecuteEmptyListIsNoop)
{
    std::pmr::vector<SideEffect> list;
    exec_.Execute(list);
    EXPECT_EQ(tracker_.reload_file_count, 0);
    EXPECT_EQ(tracker_.mermaid_init_retry_count, 0);
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
    exec_.ExecuteOne(effect::SetTimer{ app_timer::Id::TOAST, 100 });
    exec_.ExecuteOne(effect::KillTimer{ app_timer::Id::TOAST });
    ASSERT_EQ(host_.set_timer_calls.size(), 1u);
    EXPECT_EQ(host_.set_timer_calls[0], std::make_pair(app_timer::Id::TOAST, UINT{ 100 }));
    ASSERT_EQ(host_.kill_timer_calls.size(), 1u);
    EXPECT_EQ(host_.kill_timer_calls[0], app_timer::Id::TOAST);
}

TEST_F(SideEffectExecutorTest, SetCaptureAndReleaseCaptureForwardToHost)
{
    exec_.ExecuteOne(effect::SetCapture{});
    exec_.ExecuteOne(effect::ReleaseCapture{});
    EXPECT_EQ(host_.set_capture_count, 1);
    EXPECT_EQ(host_.release_capture_count, 1);
}

TEST_F(SideEffectExecutorTest, ClipboardEffectsForwardToHost)
{
    exec_.ExecuteOne(effect::ClipboardWrite{ std::pmr::string{ "hello" } });
    exec_.ExecuteOne(effect::ClipboardWriteHtml{ std::pmr::string{ "<p>html</p>" },
                                                 std::pmr::string{ "plain" } });
    ASSERT_EQ(host_.clipboard_text_calls.size(), 1u);
    EXPECT_EQ(host_.clipboard_text_calls[0], "hello");
    ASSERT_EQ(host_.clipboard_html_calls.size(), 1u);
    EXPECT_EQ(host_.clipboard_html_calls[0].first, "<p>html</p>");
    EXPECT_EQ(host_.clipboard_html_calls[0].second, "plain");
}

TEST_F(SideEffectExecutorTest, SearchFocusForwardsToHost)
{
    exec_.ExecuteOne(effect::SearchFocus{
        effect::SearchFocus::Mode::SetSelection, 3, 7 });
    ASSERT_EQ(host_.search_focus_calls.size(), 1u);
    EXPECT_EQ(host_.search_focus_calls[0].mode, effect::SearchFocus::Mode::SetSelection);
    EXPECT_EQ(host_.search_focus_calls[0].anchor, 3);
    EXPECT_EQ(host_.search_focus_calls[0].caret, 7);
}

TEST_F(SideEffectExecutorTest, SearchUnfocusForwardsToHost)
{
    exec_.ExecuteOne(effect::SearchUnfocus{ /*clear_text=*/true });
    ASSERT_EQ(host_.search_unfocus_calls.size(), 1u);
    EXPECT_TRUE(host_.search_unfocus_calls[0].clear_text);
}

TEST_F(SideEffectExecutorTest, SetWindowPositionForwardsToHost)
{
    exec_.ExecuteOne(effect::SetWindowPosition{ 10, 20, 800, 600 });
    ASSERT_EQ(host_.set_window_position_calls.size(), 1u);
    EXPECT_EQ(host_.set_window_position_calls[0], std::make_tuple(10, 20, 800, 600));
}

TEST_F(SideEffectExecutorTest, ShowToastSchedulesTimerAndInvalidates)
{
    exec_.ExecuteOne(effect::ShowToast{ L"Copied" });
    ASSERT_EQ(host_.set_timer_calls.size(), 1u);
    EXPECT_EQ(host_.set_timer_calls[0].first, app_timer::Id::TOAST);
    EXPECT_EQ(host_.invalidate_count, 1);
}
