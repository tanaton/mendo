#include "draw_command.h"
#include "diagram_raster.h"
#include "inline_math.h"
#include "profiler.h"
#include "ui_constants.h"
#include "utility.h"
#include <dwrite.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <span>
#include <vector>

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

// 半透明のコマンドの間だけブラシに不透明度を掛ける。
class OpacityScope {
public:
    OpacityScope(ID2D1SolidColorBrush* brush, float alpha) noexcept : brush_(alpha != 1.0f ? brush : nullptr)
    {
        if (brush_) {
            brush_->SetOpacity(alpha);
        }
    }
    ~OpacityScope() noexcept
    {
        if (brush_) {
            brush_->SetOpacity(1.0f);
        }
    }
    OpacityScope(const OpacityScope&) = delete;
    OpacityScope& operator=(const OpacityScope&) = delete;

private:
    ID2D1SolidColorBrush* brush_;
};

// 文書のテキストレイアウトを描く。描画効果は計測時に付けた BrushEffect で、固定ブラシへ置き換える。
// D2D の DrawTextLayout は描画面外を含む全グリフランを処理するため、巨大なコードブロック等では
// 1 フレームに数十 ms かかる (1 万行で約 20ms)。可視縦範囲外のランを飛ばして描く。
// 構文トークンがあれば、グリフランをトークン境界で分けて種別の色で描く。SetDrawingEffect は
// 1 回ごとに既存の範囲数に比例して重くなる (巨大なコードブロックで O(n^2)) ので使わない。
// スタック上でのみ使うので参照カウントは持たない。
class VisibleRunRenderer final : public UncountedCom<IDWriteTextRenderer> {
public:
    VisibleRunRenderer(ID2D1RenderTarget* rt, ID2D1Brush* default_brush, const DrawTextLayoutCmd& cmd, const FixedBrushArray& brushes, const InlineDrawings& inline_drawings,
                       MathMaskCache* math_masks) noexcept
        : rt_(rt), default_brush_(default_brush), cmd_(cmd), brushes_(brushes), inline_(inline_drawings), math_masks_(math_masks)
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
        if (y + run->fontEmSize < cmd_.cull_top || y - run->fontEmSize > cmd_.cull_bottom) {
            return S_OK;
        }
        const bool split = !cmd_.syntax_tokens.empty() || std::isfinite(cmd_.cull_left);
        if (!split || !desc || !desc->clusterMap || effect) {
            rt_->DrawGlyphRun(D2D1::Point2F(x, y), run, BrushOr(BrushEffect::IdOf(effect)), mode);
            return S_OK;
        }
        DrawGlyphRunParts(x, y, mode, *run, *desc);
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
    // インライン数式は描画命令を周りの文字の色で描き、画像はビットマップを箱いっぱいに描く。
    // ほかのインラインオブジェクト (書式の省略記号) は自分で描かせる。
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) noexcept override
    {
        // 見えない行のものは数式か見分ける (QueryInterface の参照カウントの往復) 前に飛ばす。積分記号の端などのインクは
        // 箱から少しはみ出すので、その分広く見る。
        constexpr float kInkPad = 8.0f;
        DWRITE_INLINE_OBJECT_METRICS m{};
        if (!object || FAILED(object->GetMetrics(&m)) || y + m.height + kInkPad < cmd_.cull_top || y - kInkPad > cmd_.cull_bottom) {
            return S_OK;
        }
        if (const auto image = InlineImageObjectOf(object)) {
            DrawImage(*image, context, x, y);
            return S_OK;
        }
        const auto math = InlineMathObjectOf(object);
        if (!math) {
            return object->Draw(context, this, x, y, sideways, rtl, effect);
        }
        const MathDrawing* drawing = inline_.FindMath(math->key);
        if (!drawing) {
            return S_OK;
        }
        const float s = math->scale;
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(x, y) * *D2D1::Matrix3x2F::ReinterpretBaseType(&transform_));
        DrawMath(rt_, *drawing, 0.0f, drawing->height, BrushOr(BrushEffect::IdOf(effect)), math_masks_);
        rt_->SetTransform(transform_);
        return S_OK;
    }

private:
    ID2D1Brush* BrushOr(BrushId id) const noexcept
    {
        auto* brush = FixedBrush(brushes_, id);
        return brush ? brush : default_brush_;
    }

    // 読み込み中 (ビットマップがまだ無い) は何も描かない。読めない画像はアイコンと alt を描く。alt はこのレンダラで描き、
    // alt に付けたリンクの色を使う。
    void DrawImage(const InlineImageObjectRef& image, void* context, float x, float y) noexcept
    {
        if (image.alt) {
            DrawBrokenImageIcon(x, y + image.icon_top, image.icon_size);
            image.alt->Draw(context, this, x + image.alt_left, y + image.alt_top);
            return;
        }
        if (auto* bitmap = inline_.FindImage(image.key)) {
            rt_->DrawBitmap(bitmap, D2D1::RectF(x, y, x + image.size.width, y + image.size.height));
        }
    }

    // 一辺 size の正方形に、枠と山と太陽の絵 (画像のアイコン) を描く。寸法は一辺 16 の格子で決める。
    void DrawBrokenImageIcon(float x, float y, float size) const noexcept
    {
        auto* b = BrushOr(BrushId::BlockquoteText);
        const float k = size / 16.0f;
        const float stroke = std::max(1.2f * k, 1.0f);
        const auto at = [&](float px, float py) { return D2D1::Point2F(x + px * k, y + py * k); };
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 1.5f * k, y + 2.5f * k, x + 14.5f * k, y + 13.5f * k), 1.5f * k, 1.5f * k), b, stroke);
        const D2D1_POINT_2F mountain[] = { at(3.5f, 11.5f), at(7.0f, 7.5f), at(9.5f, 10.0f), at(11.0f, 8.5f), at(12.5f, 11.5f) };
        for (size_t i = 0; i + 1 < std::size(mountain); ++i) {
            rt_->DrawLine(mountain[i], mountain[i + 1], b, stroke);
        }
        rt_->DrawEllipse(D2D1::Ellipse(at(10.5f, 5.5f), 1.2f * k, 1.2f * k), b, stroke);
    }

    // ランをトークンの境界で分け、[cull_left, cull_right] に映らないグリフを飛ばして描く。
    // 境界がクラスタ (合字等) の途中にあれば、そのクラスタは後ろのトークンの色になる。
    // グリフは右から左のランでも文字順に並び、原点が右端になってペンが左へ進むだけ。
    void DrawGlyphRunParts(float x, float y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run, const DWRITE_GLYPH_RUN_DESCRIPTION& desc) const noexcept
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
        // 字形は送り幅の外へ 1em まではみ出すとみなす。
        const float left = cmd_.cull_left - run.fontEmSize;
        const float right = cmd_.cull_right + run.fontEmSize;
        const auto past_end = [&] { return direction > 0.0f ? pen_x > right : pen_x < left; };
        // [glyph, glyph_end) のうち映るグリフ [first, last) と、その両端のペンの位置。
        struct Span {
            uint32_t first;
            uint32_t last;
            float first_x;
            float last_x;
        };
        const auto take_until = [&](uint32_t glyph_end) {
            Span s{ glyph_end, glyph, pen_x, pen_x };
            for (; glyph < glyph_end && !past_end(); glyph++) {
                const float next_x = pen_x + direction * run.glyphAdvances[glyph];
                if (std::max(pen_x, next_x) >= left && std::min(pen_x, next_x) <= right) {
                    if (s.first == glyph_end) {
                        s.first = glyph;
                        s.first_x = pen_x;
                    }
                    s.last = glyph + 1;
                    s.last_x = next_x;
                }
                pen_x = next_x;
            }
            return s;
        };
        const auto tokens = cmd_.syntax_tokens;
        if (tokens.empty()) {
            const Span s = take_until(run.glyphCount);
            if (s.first < s.last) {
                DWRITE_GLYPH_RUN part = run;
                part.glyphCount = s.last - s.first;
                part.glyphIndices += s.first;
                part.glyphAdvances += s.first;
                if (part.glyphOffsets) {
                    part.glyphOffsets += s.first;
                }
                rt_->DrawGlyphRun(D2D1::Point2F(s.first_x, y), &part, default_brush_, mode);
            }
            return;
        }

        // トークンごとに DrawGlyphRun を呼ぶと、トークンの細かい JSON 60 行で 1 フレーム 1.5ms かかる。同じ色のグリフを 1 つのランに
        // 集め、間の別の色のグリフの送り幅は直前のグリフの送り幅に足して、色ごとに 1 回で描く (0.35ms。BM_CodeBlockDraw)。
        // 置き場は使い回し、映るグリフの数までしか伸びない。
        struct BrushGlyphs {
            ID2D1Brush* brush = nullptr;
            float start_x = 0.0f;
            float end_x = 0.0f;
            std::vector<UINT16> indices;
            std::vector<FLOAT> advances;
            std::vector<DWRITE_GLYPH_OFFSET> offsets;
        };
        thread_local std::vector<BrushGlyphs> groups;
        size_t used = 0;
        const auto collect_until = [&](uint32_t glyph_end, ID2D1Brush* brush) {
            const Span s = take_until(glyph_end);
            if (s.first >= s.last) {
                return;
            }
            const auto found = std::ranges::find(groups.begin(), groups.begin() + static_cast<ptrdiff_t>(used), brush, &BrushGlyphs::brush);
            const auto index = static_cast<size_t>(found - groups.begin());
            if (index == used) {
                if (groups.size() == used) {
                    groups.emplace_back();
                }
                BrushGlyphs& fresh = groups[used++];
                fresh.brush = brush;
                fresh.start_x = s.first_x;
                fresh.indices.clear();
                fresh.advances.clear();
                fresh.offsets.clear();
            }
            BrushGlyphs& g = groups[index];
            if (!g.advances.empty()) {
                g.advances.back() += (s.first_x - g.end_x) * direction;
            }
            g.indices.insert(g.indices.end(), run.glyphIndices + s.first, run.glyphIndices + s.last);
            g.advances.insert(g.advances.end(), run.glyphAdvances + s.first, run.glyphAdvances + s.last);
            if (run.glyphOffsets) {
                g.offsets.insert(g.offsets.end(), run.glyphOffsets + s.first, run.glyphOffsets + s.last);
            }
            g.end_x = s.last_x;
        };
        try {
            // トークンは重ならず start 昇順なので、終端も昇順に並ぶ。
            auto it = std::ranges::partition_point(tokens, [&](const SyntaxToken& t) { return t.start + t.length <= run_begin; });
            for (; it != tokens.end() && it->start < run_end && !past_end(); ++it) {
                collect_until(glyph_at(std::max(it->start, run_begin)), default_brush_);
                collect_until(glyph_at(std::min(it->start + it->length, run_end)), BrushOr(SyntaxBrushId(it->type)));
            }
            collect_until(run.glyphCount, default_brush_);
        } catch (const std::bad_alloc&) {
            // 集めかけの組は数がそろっていないので、このランは描かない。
            return;
        }
        for (const BrushGlyphs& g : std::span{ groups }.first(used)) {
            DWRITE_GLYPH_RUN part = run;
            part.glyphCount = static_cast<UINT32>(g.indices.size());
            part.glyphIndices = g.indices.data();
            part.glyphAdvances = g.advances.data();
            part.glyphOffsets = run.glyphOffsets ? g.offsets.data() : nullptr;
            rt_->DrawGlyphRun(D2D1::Point2F(g.start_x, y), &part, g.brush, mode);
        }
    }
    void FillLine(float x, float top, float width, float thickness, IUnknown* effect) const noexcept
    {
        if (top + thickness < cmd_.cull_top || top > cmd_.cull_bottom) {
            return;
        }
        rt_->FillRectangle(D2D1::RectF(x, top, x + width, top + thickness), BrushOr(BrushEffect::IdOf(effect)));
    }

    ID2D1RenderTarget* rt_;
    ID2D1Brush* default_brush_;
    const DrawTextLayoutCmd& cmd_;
    D2D1_MATRIX_3X2_F transform_{};
    float pixels_per_dip_ = 1.0f;
    const FixedBrushArray& brushes_;
    const InlineDrawings& inline_;
    MathMaskCache* math_masks_;
};

} // namespace

void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray& brushes, const InlineDrawings& inline_drawings,
                         MathMaskCache* math_masks)
{
    MENDO_PROFILE("ExecuteDrawCommands");
    // 図のコマンドリストを描くときに 1 フレーム 1 回だけ問い合わせる。
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;

    // clang-format off
    const auto draw = overloaded{
        [&](const FillRectCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->FillRectangle(c.rect, b);
            }
        },
        [&](const FillRoundedRectCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ b, c.opacity };
                rt->FillRoundedRectangle(D2D1_ROUNDED_RECT{ c.rect, c.radius, c.radius }, b);
            }
        },
        [&](const DrawRoundedRectCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ c.rect, c.radius, c.radius }, b, c.stroke_width);
            }
        },
        [&](const DrawLineCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                rt->DrawLine(c.p0, c.p1, b, c.stroke_width);
            }
        },
        [&](const DrawTextLayoutCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ b, c.opacity };
                // 描画効果を BrushEffect から置き換えるため、D2D の DrawTextLayout ではなく自前のレンダラで描く。
                VisibleRunRenderer renderer{ rt, b, c, brushes, inline_drawings, math_masks };
                c.layout->Draw(nullptr, &renderer, c.origin.x, c.origin.y);
            }
        },
        [&](const DrawBitmapCmd& c) {
            rt->DrawBitmap(c.bitmap, c.dest, c.opacity);
        },
        [&](const DrawDiagramCmd& c) {
            if (c.width <= 0.0f || (!dc && FAILED(rt->QueryInterface(IID_PPV_ARGS(&dc))))) {
                return;
            }
            D2D1::Matrix3x2F base;
            dc->GetTransform(&base);
            const float scale = (c.dest.right - c.dest.left) / c.width;
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(c.dest.left, c.dest.top) * base);
            dc->DrawImage(c.commands);
            dc->SetTransform(base);
        },
        [&](const DrawGeometryCmd& c) {
            if (auto* b = FixedBrush(brushes, c.brush_id)) {
                const OpacityScope opacity{ b, c.opacity };
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
    // clang-format on
    for (const auto& c : cmds) {
        std::visit(draw, c);
    }
}
