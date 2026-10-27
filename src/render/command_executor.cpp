#include "draw_command.h"
#include "d2d_util.h"
#include "profiler.h"
#include "ui_constants.h"
#include "utility.h"
#include <dwrite.h>
#include <algorithm>
#include <bit>

namespace {

// id がブラシ無し・未作成なら nullptr。
ID2D1SolidColorBrush* FixedBrush(const FixedBrushArray& brushes, BrushId id) noexcept
{
    return id != BrushId::None ? brushes[std::to_underlying(id)].Get() : nullptr;
}

// 文書のテキストレイアウトを描く。描画効果は計測時に付けた BrushEffect で、固定ブラシへ置き換える。
// D2D の DrawTextLayout は描画面外を含む全グリフランを処理するため、巨大なコードブロック等では
// 1 フレームに数十 ms かかる (1 万行で約 20ms)。可視縦範囲外のランを飛ばして描く。
// 構文トークンがあれば、グリフランをトークン境界で分けて種別の色で描く。SetDrawingEffect は
// 1 回ごとに既存の範囲数に比例して重くなる (巨大なコードブロックで O(n^2)) ので使わない。
class VisibleRunRenderer final : public IDWriteTextRenderer {
public:
    VisibleRunRenderer(ID2D1RenderTarget* rt, ID2D1Brush* default_brush, float cull_top, float cull_bottom,
        std::span<const SyntaxToken> tokens, const FixedBrushArray& brushes) noexcept
        : rt_(rt), default_brush_(default_brush), cull_top_(cull_top), cull_bottom_(cull_bottom), tokens_(tokens), brushes_(brushes)
    {
        rt->GetTransform(&transform_);
        float dpi_x = DEFAULT_DPI;
        float dpi_y = DEFAULT_DPI;
        rt->GetDpi(&dpi_x, &dpi_y);
        pixels_per_dip_ = DpiScaleFrom(dpi_x);
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
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown* effect) noexcept override
    {
        // ベースラインから上下 1em あれば行の字形は収まる。
        if (y + run->fontEmSize < cull_top_ || y - run->fontEmSize > cull_bottom_) {
            return S_OK;
        }
        if (tokens_.empty() || !desc || !desc->clusterMap || effect) {
            rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, BrushFor(effect), mode);
            return S_OK;
        }
        DrawSyntaxGlyphRun(x, y, mode, *run, *desc);
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
    ID2D1Brush* BrushFor(IUnknown* effect) const noexcept
    {
        auto* brush = FixedBrush(brushes_, BrushEffect::IdOf(effect));
        return brush ? brush : default_brush_;
    }
    ID2D1Brush* TokenBrush(SyntaxTokenType type) const noexcept
    {
        auto* brush = FixedBrush(brushes_, SyntaxBrushId(type));
        return brush ? brush : default_brush_;
    }

    // 境界がクラスタ (合字等) の途中にあれば、そのクラスタは後ろのトークンの色になる。
    // グリフは右から左のランでも文字順に並び、原点が右端になってペンが左へ進むだけ。
    void DrawSyntaxGlyphRun(float x, float y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run, const DWRITE_GLYPH_RUN_DESCRIPTION& desc) const noexcept
    {
        const float direction = (run.bidiLevel & 1) != 0 ? -1.0f : 1.0f;
        const uint32_t run_begin = desc.textPosition;
        const uint32_t run_end = run_begin + desc.stringLength;
        // 文字位置 → その文字を含むクラスタの先頭グリフ
        const auto glyph_at = [&](uint32_t pos) -> uint32_t {
            return pos < run_end ? desc.clusterMap[pos - run_begin] : run.glyphCount;
        };
        uint32_t glyph = 0;
        float pen_x = x;
        const auto draw_until = [&](uint32_t glyph_end, ID2D1Brush* brush) {
            if (glyph_end <= glyph) {
                return;
            }
            DWRITE_GLYPH_RUN part = run;
            part.glyphCount = glyph_end - glyph;
            part.glyphIndices += glyph;
            part.glyphAdvances += glyph;
            if (part.glyphOffsets) {
                part.glyphOffsets += glyph;
            }
            rt_->DrawGlyphRun(D2D1::Point2F(pen_x, y), &part, brush, mode);
            for (; glyph < glyph_end; glyph++) {
                pen_x += direction * run.glyphAdvances[glyph];
            }
        };
        // トークンは重ならず start 昇順なので、終端も昇順に並ぶ。
        auto it = std::ranges::partition_point(tokens_, [&](const SyntaxToken& t) { return t.start + t.length <= run_begin; });
        for (; it != tokens_.end() && it->start < run_end; ++it) {
            draw_until(glyph_at(std::max(it->start, run_begin)), default_brush_);
            draw_until(glyph_at(std::min(it->start + it->length, run_end)), TokenBrush(it->type));
        }
        draw_until(run.glyphCount, default_brush_);
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
    std::span<const SyntaxToken> tokens_;
    const FixedBrushArray& brushes_;
};

} // namespace

void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray& brushes)
{
    MENDO_PROFILE("ExecuteDrawCommands");

    const auto draw = overloaded{
        [&](const FillRectCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->FillRectangle(c.rect, b);
            }
        },
        [&](const FillRoundedRectCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ c.opacity != 1.0f ? b : nullptr, c.opacity };
                rt->FillRoundedRectangle(D2D1_ROUNDED_RECT{ c.rect, c.rx, c.ry }, b);
            }
        },
        [&](const DrawRoundedRectCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ c.rect, c.rx, c.ry }, b, c.stroke_width);
            }
        },
        [&](const DrawLineCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->DrawLine(c.p0, c.p1, b, c.stroke_width);
            }
        },
        [&](const DrawTextLayoutCmd& c) {
            if (!c.layout) {
                return;
            }
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ c.opacity != 1.0f ? b : nullptr, c.opacity };
                // 描画効果を BrushEffect から置き換えるため、D2D の DrawTextLayout ではなく自前のレンダラで描く。
                VisibleRunRenderer renderer{ rt, b, c.cull_top, c.cull_bottom, c.syntax_tokens, brushes };
                c.layout->Draw(nullptr, &renderer, c.origin.x, c.origin.y);
            }
        },
        [&](const DrawBitmapCmd& c) {
            if (c.bitmap) {
                rt->DrawBitmap(c.bitmap, c.dest, c.opacity);
            }
        },
        [&](const FillEllipseCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ c.opacity != 1.0f ? b : nullptr, c.opacity };
                rt->FillEllipse(D2D1::Ellipse(c.center, c.rx, c.ry), b);
            }
        },
        [&](const DrawEllipseCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->DrawEllipse(D2D1::Ellipse(c.center, c.rx, c.ry), b, c.stroke_width);
            }
        },
        [&](const DrawGeometryCmd& c) {
            if (!c.geometry) {
                return;
            }
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ c.opacity != 1.0f ? b : nullptr, c.opacity };
                rt->DrawGeometry(c.geometry, b, c.stroke_width, c.stroke_style);
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
