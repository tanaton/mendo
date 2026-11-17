#pragma once
#include "app_constants.h"
#include "file_watcher.h"
#include "layout_computer.h"
#include "utility.h"
#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct AppState;
struct InlineDrawings;
struct InlineImageEntry;
class DeliveredImages;
class ImageLoader;
class IMermaidRenderer;
class InlineImageTable;
class MermaidFileCache;

// ResourceManager が App に頼むこと。通知は取り込み (FlushPendingResources・LoadImages) の中から届くので、
// 組み直しと描き直しだけをさせ、取り込みに入り直させない。
class ResourceManagerHost {
public:
    virtual void SetTimer(app_timer::Id id, UINT ms) = 0;
    virtual void KillTimer(app_timer::Id id) = 0;
    // 図の高さが changed の範囲で変わった。見ている位置を保って Y を組み直し、再描画させる。
    virtual void RecomputeLayout(IndexRange changed) = 0;
    virtual void OnInlineImagesUpdated(bool pending_resolved, bool sizes_changed) = 0;
    // 別の文書を読み込んでいる。この間は表示中の文書の画像と図 (拡大表示の図も) を新たに頼まない (届いた画像は取り込む)。
    virtual bool IsLoadingOtherDocument() const = 0;

protected:
    ~ResourceManagerHost() = default;
};

// 画像・Mermaidリソースのライフサイクル管理。
// Appから画像読み込み、Mermaidの要求、画面外のレイアウト・図・画像を捨てる責務を分離する。
class ResourceManager {
public:
    // 遅延レイアウトで計測する範囲。範囲外へのスクロールは EnsureVisibleLayout が同期計測で拾う。
    static constexpr float DEFERRED_LAYOUT_BUFFER_SCREENS = 2.0f;
    // 画像を読む範囲。遅延レイアウトの範囲より広くしても、組んでいない表は行の位置が分からず寸法しか読めない。
    static constexpr float PREFETCH_BUFFER_SCREENS = 2.0f;
    static_assert(PREFETCH_BUFFER_SCREENS <= DEFERRED_LAYOUT_BUFFER_SCREENS);
    // 画像のビットマップを残す範囲。読む範囲と同じだと、境目の画像を小刻みなスクロールのたびに捨てて読み直す。写真 40 枚の
    // 表で上下の往復のデコードが 76 → 35 回 (3 でも同じ)、めくって戻ったときのメモリは private +2%・GPU +2〜10%。
    static constexpr float EVICT_BUFFER_SCREENS = 2.5f;
    static_assert(PREFETCH_BUFFER_SCREENS < EVICT_BUFFER_SCREENS);
    // text_layout と表のセルを残す範囲。先読み範囲と同じだと、少し戻るだけで作り直しになる。文書の大きさによらず上下 8 画面
    // (5 → 2 → 8 と揺れた末に決定)。1MB の文書を最後まで読んで、8 画面と 2 画面で private WS に差が無かった (63〜84MB)。
    // 1MB 以下の文書を全部残すのは、物理メモリ +50MB (全体の約 4 割) かかったのでやめた。
    static constexpr float LAYOUT_EVICT_BUFFER_SCREENS = 8.0f;
    static_assert(EVICT_BUFFER_SCREENS < LAYOUT_EVICT_BUFFER_SCREENS);

    ResourceManager(AppState& state, ImageLoader& image_loader, IMermaidRenderer& mermaid, MermaidFileCache& mermaid_file_cache,
                    InlineImageTable& inline_images, InlineDrawings& inline_drawings, ResourceManagerHost& host) noexcept
        : state_(state), image_loader_(image_loader), mermaid_(mermaid), mermaid_file_cache_(mermaid_file_cache),
          inline_images_(inline_images), inline_drawings_(inline_drawings), host_(host)
    {}

    bool ApplyCachedImageSizes(IndexRange range);
    bool ApplyCachedHeights(IndexRange range);
    void LoadImages();
    void OnImagesDecoded(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kImportSliceBudget);
    void RequestMermaidRenders();
    void EvictOffscreenResources();
    void FlushPendingResources();
    void ScheduleResourceManage();
    void OnResourceManageTimer();
    void DropQueues();
    // 開いた文書の画像は src を前の文書のフォルダで解決したものなので、寸法とビットマップ・解決したパス・監視する画像を外す。
    // フォルダの読み取りは残し、枠が埋まったら今の文書で使っていないものから前に使った順にやめる (ユーザーの判断)。
    // フォルダが変わったときだけ外すと前の文書のフォルダが枠を占め続け、開くたびにフォルダごと捨てると同じフォルダの文書を
    // 渡り歩くたびに開き直す。ImageLoader に残る寸法は外した後の更新を知らないが、戻ったときの読み直しで入れ替わる。
    void ForgetInlineImages();
    void OnRenderDeviceRecreated();

    // 本文中の画像のファイルが更新されたら読み直す。フォルダは worker で開き、開き終えるまでそのフォルダの画像は読まない
    // (読んでから監視を始めるまでの更新を取りこぼさない)。
    static constexpr size_t kMaxImageWatchEvents = ImageFolderWatcher::kMaxEvents;
    size_t ImageWatchEvents(std::span<HANDLE> out) const noexcept
    {
        return image_watcher_.EventHandles(out);
    }
    // 画像のフォルダを開いている最中 (そのフォルダの画像をまだ頼んでいない) か。
    bool IsOpeningImageFolders() const noexcept
    {
        return image_watcher_.AwaitsWorker();
    }
    void OnImageFolderEvent();
    void OnImageReloadTimer();

private:
    // range のノードの図の推定高さをディスクキャッシュにある寸法で上書きする
    bool ApplyMermaidCacheHeights(IndexRange range);
    // 本文中の画像の表の変わり方
    struct InlineImageUpdate {
        bool pending_resolved = false;
        bool sizes_changed = false;
        bool bitmaps_changed = false;
        // 行の位置が決まらないので、寸法だけを読んだ表があった。
        bool unsettled_tables = false;
    };
    void ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, const DeliveredImages* delivered, InlineImageUpdate& update);
    void ApplyCachedImage(
        std::string_view src, const std::wstring& doc_dir, float max_width, const DeliveredImages* delivered, bool needs_bitmap,
        InlineImageUpdate& update);
    void SetInlineImage(uint64_t key, const InlineImageEntry& entry, InlineImageUpdate& update);
    void NotifyInlineImages(const InlineImageUpdate& update);
    void RequestVisibleDiagrams(IndexRange& changed);
    void RequestDiagramRender(size_t i, float content_width, bool dark_mode, IndexRange& changed);
    const std::wstring* ResolveImagePath(std::string_view src, const std::wstring& doc_dir);
    void DropQueuesIfJumped();

    struct BufferedRange {
        float viewport_top;
        float viewport_bottom;
        YRange keep;
        IndexRange range;
    };
    std::optional<BufferedRange> ComputeBufferedRange(float screens);
    // indices のうち先読み範囲 (PREFETCH_BUFFER_SCREENS) に交差する部分を、可視部分 → 下側 → 上側の順の 3 区間に分ける。
    std::array<std::span<const size_t>, 3> PrioritizedSlices(std::span<const size_t> indices);
    // 前回の要求から量子化幅かテーマが変わっていれば、描き直す図を stale にして旧条件の描画待ちを捨てる。
    void InvalidateDiagramsForRequestChange(float content_width, bool dark_mode);

    AppState& state_;
    ImageLoader& image_loader_;
    IMermaidRenderer& mermaid_;
    MermaidFileCache& mermaid_file_cache_;
    InlineImageTable& inline_images_;
    InlineDrawings& inline_drawings_;
    ResourceManagerHost& host_;

    float last_mermaid_content_width_ = 0.0f;
    bool last_mermaid_dark_mode_ = false;
    float last_request_scroll_ = 0.0f;
    std::unordered_map<std::string, std::wstring, StringTransparentHash, std::equal_to<>> resolved_image_paths_;
    // resolved_image_paths_ に入れた画像のファイルの監視と、更新の通知が来て IMAGE_RELOAD タイマーを待っているパス。
    ImageFolderWatcher image_watcher_;
    std::vector<std::wstring> changed_image_paths_;
    std::chrono::steady_clock::time_point last_flush_time_{};
};
