#include "command_executor.h"
#include "utility.h"

ID2D1SolidColorBrush* CommandExecutor::GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color)
{
    if (rt != bound_rt_) {
        // ブラシは RT 付随リソースなので、RT 切替・デバイス再作成では破棄する
        brush_pool_.clear();
        use_counter_ = 0;
        bound_rt_ = rt;
        last_brush_ = nullptr;
    }
    const uint32_t key = command_executor_internal::PackColor(color);
    // 直前と同色なら hash lookup を完全にスキップ。同色連続発行（罫線、ハイライト、
    // 同テーマ色のテキスト等）が多いためヒット率が高い。
    if (last_brush_ && key == last_brush_key_) {
        return last_brush_;
    }
    const uint64_t now = ++use_counter_;
    if (const auto it = brush_pool_.find(key); it != brush_pool_.end()) {
        it->second.last_used = now;
        last_brush_key_ = key;
        last_brush_ = it->second.brush.Get();
        return last_brush_;
    }
    if (brush_pool_.size() >= MAX_POOLED_BRUSHES) {
        // LRU: 全消去によるフレームスパイクを避けるため最古エントリ 1 つだけ追い出す。
        auto oldest = brush_pool_.begin();
        for (auto it = std::next(brush_pool_.begin()); it != brush_pool_.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) {
                oldest = it;
            }
        }
        // 追い出すエントリが直前キャッシュと一致する場合はキャッシュも無効化する
        if (last_brush_ == oldest->second.brush.Get()) {
            last_brush_ = nullptr;
        }
        brush_pool_.erase(oldest);
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(color, &brush)) || !brush) {
        return nullptr;
    }
    auto [it, _] = brush_pool_.emplace(key, BrushEntry{ std::move(brush), now });
    last_brush_key_ = key;
    last_brush_ = it->second.brush.Get();
    return last_brush_;
}

void CommandExecutor::Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt)
{
    if (!rt) {
        return;
    }

    for (const auto& cmd : cmds) {
        // clang-format off
        std::visit(overloaded{
            [&](const ClearCmd& c) {
                rt->Clear(c.color);
            },
            [&](const FillRectCmd& c) {
                auto* b = GetBrush(rt, c.color);
                if (b) {
                    rt->FillRectangle(c.rect, b);
                }
            },
            [&](const FillRoundedRectCmd& c) {
                auto* b = GetBrush(rt, c.color);
                if (b) {
                    const D2D1_ROUNDED_RECT rr = { c.rect, c.rx, c.ry };
                    rt->FillRoundedRectangle(rr, b);
                }
            },
            [&](const DrawLineCmd& c) {
                auto* b = GetBrush(rt, c.color);
                if (b) {
                    rt->DrawLine(c.p0, c.p1, b, c.stroke_width);
                }
            },
            [&](const DrawTextLayoutCmd& c) {
                if (c.layout) {
                    auto* b = GetBrush(rt, c.color);
                    if (b) {
                        rt->DrawTextLayout(c.origin, c.layout, b);
                    }
                }
            },
            [&](const DrawTextCmd& c) {
                if (c.format && c.text_len > 0) {
                    auto* b = GetBrush(rt, c.color);
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
                auto* b = GetBrush(rt, c.color);
                if (b) {
                    const D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry);
                    rt->FillEllipse(e, b);
                }
            },
            [&](const DrawEllipseCmd& c) {
                auto* b = GetBrush(rt, c.color);
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
        }, cmd);
        // clang-format on
    }
}
