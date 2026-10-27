#pragma once
#include "app_constants.h"
#include "layout_computer.h"
#include "utility.h"
#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

class Document;
class ViewportManager;
class ImageLoader;
class IMermaidRenderer;
class MermaidFileCache;

// ResourceManager が依存するサービス群を 1 つにまとめる DI コンテナ。
// 各ポインタは ResourceManager の生存期間中 valid である必要がある。
struct ResourceManagerDeps {
    Document* doc = nullptr;
    LayoutCache* cache = nullptr;
    ViewportManager* viewport = nullptr;
    ImageLoader* image_loader = nullptr;
    IMermaidRenderer* mermaid = nullptr;
    // リロード時の高さ適用にだけ使う。nullptr なら適用しない。
    MermaidFileCache* mermaid_file_cache = nullptr;
};

// ResourceManager が App に頼むこと。
class ResourceManagerHost {
public:
    virtual void SetTimer(app_timer::Id id, UINT ms) = 0;
    virtual void KillTimer(app_timer::Id id) = 0;
    virtual float MdContentWidth() = 0;
    virtual float MdPaneHeight() = 0;
    virtual const Theme& GetTheme() = 0;
    // 画像・図の高さが changed の範囲で変わった。見ている位置を保って Y を組み直し、再描画させる。
    virtual void RecomputeLayout(layout::HeightChangeRange changed) = 0;

protected:
    ~ResourceManagerHost() = default;
};

// 画像・Mermaidリソースのライフサイクル管理。
// Appから画像読み込み、Mermaidバッチ処理、ビットマップ解放の責務を分離する。
class ResourceManager {
public:
    // 画像/図のビットマップを残す範囲。遅延レイアウトの計測範囲も兼ねる。範囲外へのスクロールは EnsureVisibleLayout が同期計測で拾う。
    static constexpr float EVICT_BUFFER_SCREENS = 2.0f;
    // evict 範囲より広く先読みすると、evict 直後の flush で範囲外の画像/図を毎回読み直す。
    static constexpr float PREFETCH_BUFFER_SCREENS = 2.0f;
    static_assert(PREFETCH_BUFFER_SCREENS <= EVICT_BUFFER_SCREENS);
    // text_layout と表のセルを残す範囲。先読み範囲と同じだと、少し戻るだけで作り直しになる。
    // 文書の大きさによらず上下 8 画面 (5 → 2 → 8 と揺れた末に決定)。1MB の文書を最後まで読んで測ると、
    // 8 画面と 2 画面ではメモリに差が出なかった (プライベートワーキングセット 63〜84MB)。
    // 1MB 以下の文書で全部残すのは、物理メモリ +50MB (全体の約 4 割) かかったのでやめた。
    static constexpr float LAYOUT_EVICT_BUFFER_SCREENS = 8.0f;
    static_assert(EVICT_BUFFER_SCREENS < LAYOUT_EVICT_BUFFER_SCREENS);

    constexpr void Init(const ResourceManagerDeps& deps, ResourceManagerHost& host) noexcept
    {
        deps_ = deps;
        host_ = &host;
    }

    // range のノードの画像を全部走査、未キャッシュは無視（リロード時のスクロール計算前用）。高さを変えたら true。
    // 読み込みは後続の RelayoutViewport (LoadImages) が起動する。
    bool ApplyCachedImagesForReload(VisibleRange range);
    // リロード直後のスクロール位置計算用に、range のノードの図の推定高さをディスクキャッシュにある寸法で上書きする。
    // ビットマップは読まないので、描画は後続の RequestMermaidRenders に任せる。
    bool ApplyMermaidCacheHeightsForReload(VisibleRange range);
    void LoadImages();
    // 届いたデコード結果を取り込み、あれば適用して再レイアウトを 1 回行う。
    void OnImagesDecoded(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);

    void RequestMermaidRenders();
    void CancelMermaidBatch();
    // レイアウトが落ち着いてから (次のタイマーで) 図を要求する。
    void ScheduleMermaidBatch();
    void ProcessMermaidBatch();

    void EvictOffscreenBitmaps();
    // 届いたデコード結果も ApplyCachedImages でまとめて適用し、再レイアウトを 1 回にする。
    void FlushPendingResources();
    void ScheduleBitmapManage();
    void OnBitmapManageTimer();

private:
    // 以下の適用と要求は再レイアウトしない。高さか表示が変わったノードは height_changed_ に溜まる。
    // 可視範囲のみ走査し、未キャッシュは非同期ロード起動（通常描画用）。
    void ApplyCachedImages();
    // slices は走査する画像ノードの範囲の並び。start_loads なら未キャッシュの画像の読み込みを起動する。高さを変えたノードは changed に積む。
    void ApplyCachedImagesIn(std::span<const std::span<const size_t>> slices, bool start_loads, layout::HeightChangeRange& changed);
    void RequestVisibleDiagrams();
    void RequestDiagramRender(size_t i, float content_width, bool dark_mode);
    // 画像・図の寸法が確定したノードの高さを決め、changed に積む。
    void ApplyBitmapHeight(size_t i, const DiagramEntry& bitmap, float node_width, layout::HeightChangeRange& changed);
    // 解決済みパスは src 単位でキャッシュする。開かない src も空文字列で覚え、毎回の解決を避ける。
    const std::wstring* ResolveImagePath(std::string_view src, const std::wstring& doc_dir);
    // 保持範囲 (±EVICT_BUFFER_SCREENS) を越えて移動していたら、旧位置で積んだ図の描画待ちと画像のデコード待ちを捨てる。
    // FIFO のままだと TOC ジャンプや Ctrl+End の後に、見えている図や画像が旧位置の分の後回しになる。
    void DropQueuesIfJumped();
    layout::HeightChangeRange TakeHeightChanges() noexcept;
    // height_changed_ が空でなければ、その範囲で再レイアウトさせる。
    void RecomputeChangedHeights();

    struct BufferedRange {
        float viewport_top;
        float viewport_bottom;
        YRange keep;
        VisibleRange range;
    };
    // 可視範囲 ± viewport_height * screens に交差するノード範囲。viewport_height <= 0 (初期化中等) は範囲が決まらない。
    std::optional<BufferedRange> ComputeBufferedRange(float screens);
    // indices のうち ComputeBufferedRange に交差する部分を、可視部分 → 下側 → 上側の順の 3 区間に分ける。
    // 範囲が決まらないときは全件を可視部分として返す。要求キューは FIFO なので、
    // スクロール位置の復元やジャンプの直後に、画面より上の分が見えている分より先に処理されないようにする。
    std::array<std::span<const size_t>, 3> PrioritizedSlices(std::span<const size_t> indices, float screens);
    void InvalidateMermaidForWidthChange(float content_width);

    ResourceManagerDeps deps_{};
    ResourceManagerHost* host_ = nullptr;

    float last_mermaid_content_width_ = 0.0f;
    float last_request_scroll_ = 0.0f;
    std::unordered_map<std::string, std::wstring, StringTransparentHash, std::equal_to<>> resolved_image_paths_;
    std::wstring resolved_dir_;
    std::chrono::steady_clock::time_point last_flush_time_{};
    // 画像/図の適用で高さを更新したノード範囲。Y 再計算をこの範囲に限定する。
    layout::HeightChangeRange height_changed_ = {};
};
