#include "draw_command.h"
#include "d2d_util.h"
#include "inline_math.h"
#include "profiler.h"
#include "ui_constants.h"
#include "utility.h"
#include <dwrite.h>
#include <algorithm>
#include <bit>

namespace {

// 構文トークンの色。Plain はトークンにならないので BrushId::None。
constexpr BrushId SyntaxBrushId(SyntaxTokenType type) noexcept
{
    static_assert(std::to_underlying(BrushId::SyntaxDeleted) - std::to_underlying(BrushId::SyntaxKeyword) ==
                  std::to_underlying(SyntaxTokenType::Deleted) - std::to_underlying(SyntaxTokenType::Keyword));
    static_assert(std::to_underlying(BrushId::SyntaxFunction) - std::to_underlying(BrushId::SyntaxKeyword) ==
                  std::to_underlying(SyntaxTokenType::Function) - std::to_underlying(SyntaxTokenType::Keyword));
    if (type < SyntaxTokenType::Keyword || type > SyntaxTokenType::Deleted) {
        return BrushId::None;
    }
    return BrushIdAt(BrushId::SyntaxKeyword, std::to_underlying(type) - std::to_underlying(SyntaxTokenType::Keyword));
}

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
        std::span<const SyntaxToken> tokens, const FixedBrushArray& brushes, InlineMathMasks* math) noexcept
        : rt_(rt), default_brush_(default_brush), cull_top_(cull_top), cull_bottom_(cull_bottom), tokens_(tokens), brushes_(brushes), math_(math)
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
    // インライン数式は型を塗る。型が無ければ (受け取らなかったページ・捨てたページ・デバイスロストで読み直し中) 何も描かない。
    // ほかのインラインオブジェクト (書式の省略記号) は自分で描かせる。
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) noexcept override
    {
        // 見えない行のものは数式か見分ける (QueryInterface の参照カウントの往復) 前に飛ばす。型は箱よりインクの幅と
        // 物理 px への丸め (1px 未満) の分だけ広く切り出してあるので、その分広く見る。
        constexpr float kInkPad = INLINE_MATH_INK_MARGIN + 1.0f;
        DWRITE_INLINE_OBJECT_METRICS m{};
        if (!object || FAILED(object->GetMetrics(&m)) || y + m.height + kInkPad < cull_top_ || y - kInkPad > cull_bottom_) {
            return S_OK;
        }
        const auto key = InlineMathObjectKey(object);
        if (!key) {
            return object->Draw(context, this, x, y, sideways, rtl, effect);
        }
        if (!math_) {
            return S_OK;
        }
        const auto* mask = math_->Find(*key);
        if (!mask) {
            math_->missing.push_back(*key);
            return S_OK;
        }
        const D2D1_RECT_F dest = OffsetRectF(mask->dest, x, y);
        mask->page->last_drawn = math_->frame;
        // FillOpacityMask は ALIASED のときしか描かない。
        const D2D1_ANTIALIAS_MODE mode = rt_->GetAntialiasMode();
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        rt_->FillOpacityMask(mask->page->bitmap.Get(), BrushFor(effect), D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dest, &mask->src);
        rt_->SetAntialiasMode(mode);
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
    InlineMathMasks* math_;
};

} // namespace

InlineMathPage& InlineMathMasks::AddPage(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, bool reloadable)
{
    auto& page = *pages.emplace_back(std::make_unique<InlineMathPage>(InlineMathPage{
        .bitmap = std::move(bitmap), .reloadable = reloadable, .last_drawn = frame }));
    page_bytes += BitmapBytes(page.bitmap.Get());
    return page;
}

void InlineMathMasks::Assign(uint64_t key, const InlineMathMask& mask)
{
    ++mask.page->masks;
    const auto [it, inserted] = masks.try_emplace(key, mask);
    if (inserted) {
        return;
    }
    InlineMathPage* const old = std::exchange(it->second, mask).page;
    if (--old->masks == 0) {
        page_bytes -= BitmapBytes(old->bitmap.Get());
        std::erase_if(pages, [old](const auto& p) { return p.get() == old; });
    }
}

void InlineMathMasks::EvictPages(size_t budget)
{
    if (page_bytes <= budget) {
        return;
    }
    std::vector<InlineMathPage*> order;
    for (const auto& p : pages) {
        if (p->reloadable && p->last_drawn != frame) {
            order.push_back(p.get());
        }
    }
    if (order.empty()) {
        return;
    }
    std::ranges::sort(order, {}, &InlineMathPage::last_drawn);
    for (auto* p : order) {
        if (page_bytes <= budget) {
            break;
        }
        page_bytes -= BitmapBytes(p->bitmap.Get());
        // ビットマップを手放したページを捨てる印にする。
        p->bitmap.Reset();
    }
    std::erase_if(masks, [](const auto& m) { return !m.second.page->bitmap; });
    std::erase_if(pages, [](const auto& p) { return !p->bitmap; });
}

void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray& brushes, InlineMathMasks* math)
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
                VisibleRunRenderer renderer{ rt, b, c.cull_top, c.cull_bottom, c.syntax_tokens, brushes, math };
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
