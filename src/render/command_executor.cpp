#include "command_executor.h"
#include "utility.h"

namespace {

// D2D1_COLOR_F を 8bit RGBA にパックしたキー。テーマ色は 8bit 精度で作られているため十分。
constexpr uint32_t PackColor(D2D1_COLOR_F c) noexcept
{
    const auto quant = [](float v) noexcept -> uint32_t {
        if (v <= 0.0f) {
            return 0;
        }
        if (v >= 1.0f) {
            return 255;
        }
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return (quant(c.r) << 24) | (quant(c.g) << 16) | (quant(c.b) << 8) | quant(c.a);
}

} // namespace

ID2D1SolidColorBrush* CommandExecutor::GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color)
{
    if (rt != bound_rt_) {
        // ブラシは RT 付随リソースなので、RT 切替・デバイス再作成では破棄する
        brush_pool_.clear();
        bound_rt_ = rt;
        last_key_ = 0xFFFFFFFFu;
        last_brush_ = nullptr;
    }
    const uint32_t key = PackColor(color);
    if (key == last_key_ && last_brush_) {
        return last_brush_;
    }
    if (const auto it = brush_pool_.find(key); it != brush_pool_.end()) {
        last_key_ = key;
        last_brush_ = it->second.Get();
        return last_brush_;
    }
    if (brush_pool_.size() >= MAX_POOLED_BRUSHES) {
        // clear で既存ブラシを破棄するので、直近キャッシュもダングリング防止に無効化
        brush_pool_.clear();
        last_key_ = 0xFFFFFFFFu;
        last_brush_ = nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(rt->CreateSolidColorBrush(color, &brush)) || !brush) {
        return nullptr;
    }
    auto [it, _] = brush_pool_.emplace(key, std::move(brush));
    last_key_ = key;
    last_brush_ = it->second.Get();
    return last_brush_;
}

void CommandExecutor::Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt)
{
    if (!rt) {
        return;
    }

    for (const auto& cmd : cmds) {
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
                        rt->DrawText(c.text, static_cast<UINT32>(c.text_len), c.format, c.rect, b);
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
    }
}
