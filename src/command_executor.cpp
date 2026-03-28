#include "command_executor.h"

ID2D1SolidColorBrush* CommandExecutor::GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color)
{
    if (rt != bound_rt_) {
        brush_.Reset();
        bound_rt_ = rt;
        last_color_ = { -1.0f, -1.0f, -1.0f, -1.0f };
    }
    if (!brush_) {
        rt->CreateSolidColorBrush(color, &brush_);
        last_color_ = color;
    }
    else if (color.r != last_color_.r || color.g != last_color_.g ||
        color.b != last_color_.b || color.a != last_color_.a) {
        brush_->SetColor(color);
        last_color_ = color;
    }
    return brush_.Get();
}

void CommandExecutor::Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt)
{
    if (!rt) {
        return;
    }

    for (const auto& cmd : cmds) {
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, ClearCmd>) {
                rt->Clear(c.color);
            }
            else if constexpr (std::is_same_v<T, FillRectCmd>) {
                auto* b = GetBrush(rt, c.color);
                if (b) {
                    rt->FillRectangle(c.rect, b);
                }
            }
            else if constexpr (std::is_same_v<T, FillRoundedRectCmd>) {
                auto* b = GetBrush(rt, c.color);
                if (b) { D2D1_ROUNDED_RECT rr = { c.rect, c.rx, c.ry }; rt->FillRoundedRectangle(rr, b); }
            }
            else if constexpr (std::is_same_v<T, DrawLineCmd>) {
                auto* b = GetBrush(rt, c.color);
                if (b) {
                    rt->DrawLine(c.p0, c.p1, b, c.stroke_width);
                }
            }
            else if constexpr (std::is_same_v<T, DrawTextLayoutCmd>) {
                if (c.layout) {
                    auto* b = GetBrush(rt, c.color);
                    if (b) {
                        rt->DrawTextLayout(c.origin, c.layout, b);
                    }
                }
            }
            else if constexpr (std::is_same_v<T, DrawTextCmd>) {
                if (c.format && c.text_len > 0) {
                    auto* b = GetBrush(rt, c.color);
                    if (b) {
                        rt->DrawText(c.text, static_cast<UINT32>(c.text_len),
                            c.format, c.rect, b);
                    }
                }
            }
            else if constexpr (std::is_same_v<T, DrawBitmapCmd>) {
                if (c.bitmap) {
                    rt->DrawBitmap(c.bitmap, c.dest, c.opacity);
                }
            }
            else if constexpr (std::is_same_v<T, FillEllipseCmd>) {
                auto* b = GetBrush(rt, c.color);
                if (b) { D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry); rt->FillEllipse(e, b); }
            }
            else if constexpr (std::is_same_v<T, DrawEllipseCmd>) {
                auto* b = GetBrush(rt, c.color);
                if (b) { D2D1_ELLIPSE e = D2D1::Ellipse(c.center, c.rx, c.ry); rt->DrawEllipse(e, b, c.stroke_width); }
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
