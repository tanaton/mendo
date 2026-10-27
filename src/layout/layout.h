#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "text_measurer.h"
#include "theme.h"
#include <chrono>
#include <memory_resource>
#include <span>

class Document;
class TaskScheduler;
class ViewportManager;

// 計測を打ち切る時刻。過ぎても 1 ノード目は計測するので、呼び出しごとに必ず進む。
using LayoutDeadline = std::chrono::steady_clock::time_point;
inline constexpr LayoutDeadline kNoLayoutDeadline = LayoutDeadline::max();

namespace layout {

// indices のノードを scheduler の worker で並列計測する。scheduler が null なら呼び出しスレッドで直列に計測する。
// 戻り値は dirty のまま残したノード数 (例外で計測できなかったか、期限を過ぎて手を付けなかった)。
int MeasureIndicesParallel(
    const std::pmr::vector<Node>& nodes,
    LayoutCache& cache,
    float content_width,
    const Theme& theme,
    const ITextMeasurer& backend,
    std::span<const size_t> indices,
    YRange viewport,
    TaskScheduler* scheduler,
    LayoutDeadline deadline);

} // namespace layout

class LayoutEngine {
public:
    // 小刻みな WM_SIZE で全ノード再レイアウトが頻発するのを防ぐ幅変化の閾値。
    static constexpr float kWidthChangeThreshold = 2.0f;

    // theme は measurer に渡し、以後は measurer が持つものを使う。
    bool Init(ITextMeasurer* measurer, const Theme& theme);
    // measurer に渡す (ITextMeasurer::SetScheduler の契約)。ノード単位の並列計測もこれを使う。
    void SetLayoutScheduler(TaskScheduler* scheduler) noexcept
    {
        if (measurer_) {
            measurer_->SetScheduler(scheduler);
        }
    }
    // 書式を作り直すのはズームだけで、全ノードの dirty は呼び出し側の InvalidateAllLayouts が付ける。
    bool RecreateFormats();
    // 本文幅かズームがキャッシュの推定時 (EstimateBasis) から変わったら、全ノードを dirty にして推定値で Y を組み直す
    // (DirectWrite は呼ばない)。戻り値は全ノードを dirty にしたか。
    bool Reestimate(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    // [viewport_top, viewport_bottom] に入る dirty を計測し、退避したテーブル行を戻す。
    // 計測で高さが変わると範囲に入るノードも変わるため、範囲内の dirty が無くなるまで繰り返す。
    // 戻り値は高さ・位置・テーブル行のいずれかが変わったか。
    bool EnsureVisibleLayout(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        float viewport_top, float viewport_bottom);
    // range (文書座標) 内の dirty を deadline まで計測する。range 内に dirty が残れば true。
    bool ProcessDirtyBatch(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache,
        float viewport_width, LayoutDeadline deadline, YRange range);
    const Theme& GetTheme() const noexcept
    {
        return measurer_->GetTheme();
    }

private:
    struct PassResult {
        bool updated = false;
        // 期限か計測失敗で、範囲内の dirty を計測しきれなかった。
        bool remaining = false;
    };
    // 範囲内の dirty を deadline まで計測し、退避したテーブル行を戻して Y を組み直す。
    PassResult MeasurePass(
        const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
        YRange range, LayoutDeadline deadline);

    // lifecycle 系 (Init/RecreateFormats) は UI スレッドからのみ呼び、
    // MeasureNode は const 経由で measurer の scheduler の worker から並列呼び出しされる。
    ITextMeasurer* measurer_ = nullptr;
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
    void ViewportLayout(const Document& doc, LayoutCache& cache, float width, float height);

    // ProcessDirtyBatch / EnsureVisibleLayout は見ている位置を保つ (前後で scroll target を確定・適用する)。
    // ProcessDirtyBatch は可視範囲の上下 height * buffer_screens までの dirty を計測する。height <= 0 なら全 dirty が対象。
    bool ProcessDirtyBatch(
        const Document& doc, LayoutCache& cache, float width, LayoutDeadline deadline,
        float height, float buffer_screens);
    bool EnsureVisibleLayout(const Document& doc, LayoutCache& cache, float width, float height);
    // 画面外の node も計測する。見ている位置は保つ。
    bool EnsureNodeLayout(const Document& doc, LayoutCache& cache, float width, int node);
    void RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, layout::HeightChangeRange changed) noexcept;

private:
    // 前後で scroll target を確定・適用し、見ている位置を保ったまま op を行う。op の結果を返す。
    template <class Op>
    bool KeepingAnchor(LayoutCache& cache, Op&& op);
    // [top, bottom] を計測する。見ている位置は保つ。
    bool EnsureRangeLayout(const Document& doc, LayoutCache& cache, float width, float top, float bottom);

    LayoutEngine& engine_;
    ViewportManager& viewport_;
};
