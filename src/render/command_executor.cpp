#include "command_executor.h"
#include "overloaded.h"
#include "profiler.h"
#include <dwrite.h>
#include <bit>

namespace {

// D2D の DrawTextLayout は描画面外を含む全グリフランを処理するため、巨大なコードブロック等では
// 1 フレームに数十 ms かかる (1 万行で約 20ms)。可視縦範囲外のランを飛ばして描く。
class VisibleRunRenderer final : public IDWriteTextRenderer {
public:
    VisibleRunRenderer(ID2D1RenderTarget* rt, ID2D1Brush* default_brush, float cull_top, float cull_bottom) noexcept
        : rt_(rt), default_brush_(default_brush), cull_top_(cull_top), cull_bottom_(cull_bottom)
    {
        rt->GetTransform(&transform_);
        float dpi_x = 96.0f;
        float dpi_y = 96.0f;
        rt->GetDpi(&dpi_x, &dpi_y);
        pixels_per_dip_ = dpi_x / 96.0f;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // スタック上でのみ使うので参照カウントは持たない。
    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        return 1;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override
    {
        *disabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override
    {
        *transform = std::bit_cast<DWRITE_MATRIX>(transform_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels_per_dip) noexcept override
    {
        *pixels_per_dip = pixels_per_dip_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown* effect) noexcept override
    {
        // ベースラインから上下 1em あれば行の字形は収まる。
        if (y + run->fontEmSize < cull_top_ || y - run->fontEmSize > cull_bottom_) {
            return S_OK;
        }
        rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, BrushFor(effect), mode);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown* effect) noexcept override
    {
        FillLine(x, y + u->offset, u->width, u->thickness, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* st, IUnknown* effect) noexcept override
    {
        FillLine(x, y + st->offset, st->width, st->thickness, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) noexcept override
    {
        return S_OK;
    }

private:
    // effect は SetDrawingEffect で積んだ ID2D1Brush のみなので QI せずに使う。
    ID2D1Brush* BrushFor(IUnknown* effect) const noexcept
    {
        return effect ? static_cast<ID2D1Brush*>(effect) : default_brush_;
    }
    void FillLine(float x, float top, float width, float thickness, IUnknown* effect) const noexcept
    {
        if (top + thickness < cull_top_ || top > cull_bottom_) {
            return;
        }
        rt_->FillRectangle(D2D1::RectF(x, top, x + width, top + thickness), BrushFor(effect));
    }

    ID2D1RenderTarget* rt_;
    ID2D1Brush* default_brush_;
    float cull_top_;
    float cull_bottom_;
    D2D1_MATRIX_3X2_F transform_{};
    float pixels_per_dip_ = 1.0f;
};

} // namespace

ID2D1SolidColorBrush* CommandExecutor::ResolveBrush(ID2D1RenderTarget* rt, BrushId id, D2D1_COLOR_F color)
{
    if (id != BrushId::Custom && fixed_brushes_) {
        if (auto* fixed = (*fixed_brushes_)[std::to_underlying(id)].Get()) {
            return fixed;
        }
    }
    if (scratch_brush_ && scratch_rt_ == rt) {
        scratch_brush_->SetColor(color);
        return scratch_brush_.Get();
    }
    scratch_brush_.Reset();
    scratch_rt_ = rt;
    if (FAILED(rt->CreateSolidColorBrush(color, &scratch_brush_))) {
        return nullptr;
    }
    return scratch_brush_.Get();
}

void CommandExecutor::Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray* brushes)
{
    MENDO_PROFILE("CommandExecutor::Execute");
    MENDO_PLOT("draw.command_count", static_cast<int64_t>(cmds.size()));
    if (!rt) {
        return;
    }

    fixed_brushes_ = brushes;

    const auto draw = mendo::overloaded{
        [&](const FillRectCmd& c) {
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                rt->FillRectangle(c.rect, b);
            }
        },
        [&](const FillRoundedRectCmd& c) {
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                rt->FillRoundedRectangle(D2D1_ROUNDED_RECT{ c.rect, c.rx, c.ry }, b);
            }
        },
        [&](const DrawLineCmd& c) {
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                rt->DrawLine(c.p0, c.p1, b, c.stroke_width);
            }
        },
        [&](const DrawTextLayoutCmd& c) {
            if (!c.layout) {
                return;
            }
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                if (c.cull_runs) {
                    VisibleRunRenderer renderer{ rt, b, c.cull_top, c.cull_bottom };
                    c.layout->Draw(nullptr, &renderer, c.origin.x, c.origin.y);
                }
                else {
                    rt->DrawTextLayout(c.origin, c.layout, b);
                }
            }
        },
        [&](const DrawTextCmd& c) {
            if (!c.format || c.text_len == 0) {
                return;
            }
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                rt->DrawText(c.text(), static_cast<UINT32>(c.text_len), c.format, c.rect, b);
            }
        },
        [&](const DrawBitmapCmd& c) {
            if (c.bitmap) {
                rt->DrawBitmap(c.bitmap, c.dest);
            }
        },
        [&](const FillEllipseCmd& c) {
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                rt->FillEllipse(D2D1::Ellipse(c.center, c.rx, c.ry), b);
            }
        },
        [&](const DrawEllipseCmd& c) {
            if (auto* b = ResolveBrush(rt, c.brush_id, c.color)) {
                rt->DrawEllipse(D2D1::Ellipse(c.center, c.rx, c.ry), b, c.stroke_width);
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
    };
    for (const auto& c : cmds) {
        std::visit(draw, c);
    }
}
