#include "command_executor.h"

ID2D1SolidColorBrush* CommandExecutor::GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color) {
    if (rt != bound_rt_) {
        brush_.Reset();
        bound_rt_ = rt;
    }
    if (!brush_) {
        rt->CreateSolidColorBrush(color, &brush_);
    } else {
        brush_->SetColor(color);
    }
    return brush_.Get();
}

void CommandExecutor::Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt) {
    if (!rt) return;

    for (const auto& cmd : cmds) {
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, ClearCmd>) {
                rt->Clear(c.color);
            }
            else if constexpr (std::is_same_v<T, FillRectCmd>) {
                rt->FillRectangle(c.rect, GetBrush(rt, c.color));
            }
            else if constexpr (std::is_same_v<T, FillRoundedRectCmd>) {
                D2D1_ROUNDED_RECT rr = {c.rect, c.rx, c.ry};
                rt->FillRoundedRectangle(rr, GetBrush(rt, c.color));
            }
            else if constexpr (std::is_same_v<T, DrawLineCmd>) {
                rt->DrawLine(c.p0, c.p1, GetBrush(rt, c.color), c.stroke_width);
            }
            else if constexpr (std::is_same_v<T, DrawTextLayoutCmd>) {
                if (c.layout) {
                    rt->DrawTextLayout(c.origin, c.layout, GetBrush(rt, c.color));
                }
            }
            else if constexpr (std::is_same_v<T, DrawTextCmd>) {
                if (c.format && c.text_len > 0) {
                    rt->DrawText(c.text, static_cast<UINT32>(c.text_len),
                                 c.format, c.rect, GetBrush(rt, c.color));
                }
            }
            else if constexpr (std::is_same_v<T, DrawBitmapCmd>) {
                if (c.bitmap) {
                    rt->DrawBitmap(c.bitmap, c.dest, c.opacity);
                }
            }
            else if constexpr (std::is_same_v<T, FillEllipseCmd>) {
                D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry);
                rt->FillEllipse(e, GetBrush(rt, c.color));
            }
            else if constexpr (std::is_same_v<T, DrawEllipseCmd>) {
                D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry);
                rt->DrawEllipse(e, GetBrush(rt, c.color), c.stroke_width);
            }
            else if constexpr (std::is_same_v<T, PushClipCmd>) {
                rt->PushAxisAlignedClip(c.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            }
            else if constexpr (std::is_same_v<T, PopClipCmd>) {
                rt->PopAxisAlignedClip();
            }
            else if constexpr (std::is_same_v<T, SetTransformCmd>) {
                rt->SetTransform(c.transform);
            }
        }, cmd);
    }
}
