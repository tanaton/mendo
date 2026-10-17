#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "parallel_measure.h"
#include "text_measurer.h"
#include "theme.h"
#include <memory_resource>
#include <optional>

class Document;
class TaskScheduler;
class ViewportManager;

class LayoutEngine {
public:
    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ幅変化の閾値。
    static constexpr float kWidthChangeThreshold = 2.0f;

    bool Init(ITextMeasurer* measurer, const Theme& theme);
    // nullptr なら呼び出しスレッドで直列に計測する。Shutdown 時は scheduler の Shutdown より前に
    // SetLayoutScheduler(nullptr) を呼んで参照を切る契約。
    void SetLayoutScheduler(TaskScheduler* scheduler) noexcept
    {
        layout_scheduler_ = scheduler;
    }
    void UpdateTheme(const Theme& theme) noexcept
    {
        theme_ = &theme;
        measurer_->UpdateTheme(theme);
    }
    bool RecreateFormats();
    // 幅が変わったら全ノードを dirty にして推定値で Y を組み直す (DirectWrite は呼ばない)。
    // 未推定のキャッシュ (位置が全て 0) も組み直す。
    void Reestimate(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    // [viewport_top, viewport_bottom] に入る dirty を計測し、退避したテーブル行を戻す。
    // 計測で高さが変わると範囲に入るノードも変わるため、範囲内の dirty が無くなるまで繰り返す。
    // 戻り値は高さ・位置・テーブル行のいずれかが変わったか。
    bool EnsureVisibleLayout(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top, float viewport_bottom);
    // Reestimate してから範囲を計測する。範囲を省略すると全ノードを計測する。
    void ComputeLayout(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top = -MeasureViewportRange::kInf, float viewport_bottom = MeasureViewportRange::kInf);
    // clip 内の dirty を最大 batch_size 件 (0 = 無制限) 計測する。clip 内に dirty が残れば true。
    // clip 省略時は全 dirty を対象にする。
    bool ProcessDirtyBatch(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache,
        float viewport_width, int batch_size,
        layout::ViewportClip clip = {});

private:
    struct PassResult {
        bool updated = false;
        // 上限か計測失敗で、範囲内の dirty を計測しきれなかった。
        bool remaining = false;
    };
    // 範囲内の dirty を最大 max_nodes 件 (0 = 無制限) 計測し、退避したテーブル行を戻して Y を組み直す。
    PassResult MeasurePass(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        MeasureViewportRange range, size_t max_nodes);

    // dirty なノード (mark_all_dirty なら全ノード) の高さを推定値で更新し、全ノードの Y を組み直す。
    void EstimateDirtyHeights(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float content_width, bool mark_all_dirty);

    // lifecycle 系 (RecreateFormats/UpdateTheme) は UI スレッドからのみ呼び、
    // MeasureNode は const 経由で layout_scheduler_ 上の worker から並列呼び出しされる。
    ITextMeasurer* measurer_ = nullptr;
    const Theme* theme_ = nullptr;
    TaskScheduler* layout_scheduler_ = nullptr;

    // nullopt = まだ一度も組んでいない (計測済みのノードが無いので初回は組み直さない)。
    std::optional<float> last_viewport_width_;
    // 旧 format で計測したノードを、幅が同じでも組み直させる。
    bool formats_changed_ = false;
};

// LayoutEngine + ViewportManager の組み合わせを薄くラップし、
// スクロール target 管理付きのレイアウト操作を提供する。
class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept
        : engine_(engine), viewport_(viewport)
    {
    }

    // 推定し直した Y で見ている位置を求めてから、その可視範囲を計測する。
    void ViewportLayout(Document& doc, LayoutCache& cache, float width, float height);

    // 増分レイアウトのビューポート絞り込み。height > 0 で可視範囲 + 周辺バッファのみ
    // 処理し、height <= 0 なら全 dirty を順次処理する。
    struct ViewportLimit {
        float height = 0.0f;
        float buffer_screens = 0.0f;
    };
    // ProcessDirtyBatch / EnsureVisibleLayout は見ている位置を保つ (前後で scroll target を確定・適用する)。
    bool ProcessDirtyBatch(
        Document& doc, LayoutCache& cache, float width, int batch_size,
        ViewportLimit viewport);
    bool EnsureVisibleLayout(Document& doc, LayoutCache& cache, float width, float height);
    // 画面外の node も計測する。見ている位置は保つ。
    bool EnsureNodeLayout(Document& doc, LayoutCache& cache, float width, int node);
    void RecomputeAfterDiagram(Document& doc, LayoutCache& cache, const Theme& theme,
                               layout::HeightChangeRange changed) noexcept;

private:
    // [top, bottom] を計測する。前後で scroll target を確定・適用して見ている位置を保つ。
    bool EnsureRangeLayout(Document& doc, LayoutCache& cache, float width, float top, float bottom);

    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
