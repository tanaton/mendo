#include "command_executor.h"
#include "profiler.h"
#include "utility.h"

#ifdef MENDO_USE_TRACY
namespace {

// 累積カウンタ（UI スレッド単一前提のため非アトミック）。
struct BrushStats {
    int64_t fastpath_hit = 0; // last_brush_ 直前キャッシュヒット
    int64_t pool_hit = 0;     // brush_pool_ 内ヒット (PackColor で同一色)
    int64_t pool_miss = 0;    // 新規 CreateSolidColorBrush
    int64_t pool_evict = 0;   // LRU で 1 件追い出し
    int64_t rt_switch = 0;    // RT 切替によるプール全クリア
};
BrushStats g_brush_stats;

void PublishBrushStats() noexcept
{
    MENDO_PLOT("brush.fastpath_hit", g_brush_stats.fastpath_hit);
    MENDO_PLOT("brush.pool_hit", g_brush_stats.pool_hit);
    MENDO_PLOT("brush.pool_miss", g_brush_stats.pool_miss);
    MENDO_PLOT("brush.pool_evict", g_brush_stats.pool_evict);
    MENDO_PLOT("brush.rt_switch", g_brush_stats.rt_switch);
}

} // namespace
#endif

ID2D1SolidColorBrush* CommandExecutor::ResolveBrush(ID2D1RenderTarget* rt, BrushId id, D2D1_COLOR_F color)
{
    // 固定 BrushId は配列ルックアップで即解決。Custom と配列未設定時のみ brush_pool 経由。
    if (id == BrushId::Custom || !fixed_brushes_) {
        return GetBrush(rt, color);
    }
    if (auto* fixed = (*fixed_brushes_)[std::to_underlying(id)]) {
        return fixed;
    }
    return GetBrush(rt, color);
}

ID2D1SolidColorBrush* CommandExecutor::GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color)
{
    if (rt != bound_rt_) {
        // ブラシは RT 付随リソースなので、RT 切替・デバイス再作成では破棄する
        brush_pool_.clear();
        lru_keys_.clear();
        bound_rt_ = rt;
        last_brush_ = nullptr;
        MENDO_COUNT_INC(g_brush_stats.rt_switch);
    }
    const uint32_t key = command_executor_internal::PackColor(color);
    // 直前と同色なら hash lookup を完全にスキップ。同色連続発行（罫線、ハイライト、
    // 同テーマ色のテキスト等）が多いためヒット率が高い。
    if (last_brush_ && key == last_brush_key_) {
        MENDO_COUNT_INC(g_brush_stats.fastpath_hit);
        return last_brush_;
    }
    if (const auto it = brush_pool_.find(key); it != brush_pool_.end()) {
        lru_keys_.splice(lru_keys_.begin(), lru_keys_, it->second.lru_pos);
        last_brush_key_ = key;
        last_brush_ = it->second.brush.Get();
        MENDO_COUNT_INC(g_brush_stats.pool_hit);
        return last_brush_;
    }
    if (brush_pool_.size() >= MAX_POOLED_BRUSHES) {
        // 全消去によるフレームスパイクを避けるため最古エントリ 1 つだけ追い出す。
        const uint32_t oldest_key = lru_keys_.back();
        const auto oldest_it = brush_pool_.find(oldest_key);
        if (oldest_it != brush_pool_.end()) {
            if (last_brush_ == oldest_it->second.brush.Get()) {
                last_brush_ = nullptr;
            }
            brush_pool_.erase(oldest_it);
        }
        lru_keys_.pop_back();
        MENDO_COUNT_INC(g_brush_stats.pool_evict);
    }
    MENDO_COUNT_INC(g_brush_stats.pool_miss);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(color, &brush)) || !brush) {
        return nullptr;
    }
    lru_keys_.push_front(key);
    auto [it, _] = brush_pool_.emplace(key, BrushEntry{ std::move(brush), lru_keys_.begin() });
    last_brush_key_ = key;
    last_brush_ = it->second.brush.Get();
    return last_brush_;
}

void CommandExecutor::Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray* brushes)
{
    MENDO_PROFILE("CommandExecutor::Execute");
    MENDO_PLOT("draw.command_count", static_cast<int64_t>(cmds.size()));
    if (!rt) {
        return;
    }

    fixed_brushes_ = brushes;

    cmds.Visit(overloaded{
        [&](const ClearCmd& c) {
            rt->Clear(c.color);
        },
        [&](const FillRectCmd& c) {
            auto* b = ResolveBrush(rt, c.brush_id, c.color);
            if (b) {
                rt->FillRectangle(c.rect, b);
            }
        },
        [&](const FillRoundedRectCmd& c) {
            auto* b = ResolveBrush(rt, c.brush_id, c.color);
            if (b) {
                const D2D1_ROUNDED_RECT rr = { c.rect, c.rx, c.ry };
                rt->FillRoundedRectangle(rr, b);
            }
        },
        [&](const DrawLineCmd& c) {
            auto* b = ResolveBrush(rt, c.brush_id, c.color);
            if (b) {
                rt->DrawLine(c.p0, c.p1, b, c.stroke_width);
            }
        },
        [&](const DrawTextLayoutCmd& c) {
            if (c.layout) {
                auto* b = ResolveBrush(rt, c.brush_id, c.color);
                if (b) {
                    rt->DrawTextLayout(c.origin, c.layout, b);
                }
            }
        },
        [&](const DrawTextCmd& c) {
            if (c.format && c.text_len > 0) {
                auto* b = ResolveBrush(rt, c.brush_id, c.color);
                if (b) {
                    rt->DrawText(c.text(), static_cast<UINT32>(c.text_len), c.format, c.rect, b);
                }
            }
        },
        [&](const DrawBitmapCmd& c) {
            if (c.bitmap) {
                rt->DrawBitmap(c.bitmap, c.dest, c.opacity, c.interpolation_mode);
            }
        },
        [&](const FillEllipseCmd& c) {
            auto* b = ResolveBrush(rt, c.brush_id, c.color);
            if (b) {
                const D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry);
                rt->FillEllipse(e, b);
            }
        },
        [&](const DrawEllipseCmd& c) {
            auto* b = ResolveBrush(rt, c.brush_id, c.color);
            if (b) {
                const D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry);
                rt->DrawEllipse(e, b, c.stroke_width);
            }
        },
        [&](const PushClipCmd& c) {
            rt->PushAxisAlignedClip(c.rect, D2D1_ANTIALIAS_MODE_ALIASED);
        },
        [&](const PopClipCmd&) {
            rt->PopAxisAlignedClip();
        },
        [&](const SetTransformCmd& c) {
            rt->SetTransform(c.transform);
        },
    });

    MENDO_IF_TRACY(PublishBrushStats());
    MENDO_PLOT("brush.pool_size", static_cast<int64_t>(brush_pool_.size()));
}
