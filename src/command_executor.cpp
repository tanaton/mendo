#include "command_executor.h"
#include "utility.h"

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
                    rt->DrawBitmap(c.bitmap, c.dest, c.opacity);
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
                rt->PushAxisAlignedClip(c.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
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
