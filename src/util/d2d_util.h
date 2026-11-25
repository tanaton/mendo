#pragma once
#include "ui_types.h"
#include "profiler.h"
#include "win32_util.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <windows.h>
#include <cstdint>
#include <vector>

// テキスト書式の作り方。描画・計測・ポップアップで共有する。
struct TextFormatSpec {
    const wchar_t* family = nullptr;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    float size = 0.0f;
    // 漢字の字形とフォールバック先が決まる。文書から来る文字は Theme::text_locale、UI の決まった文言は i18n::S().lang_locale を渡す。
    const wchar_t* locale = L"en-us";
    DWRITE_TEXT_ALIGNMENT text_align = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT para_align = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    bool no_wrap = false;
    // 0 より大きければ、主フォントの行の高さを size のこの倍にする。増減分は行の上下に半分ずつ配る。
    float line_height_factor = 0.0f;
    // 箱に収まらない行を省き、見えている最後の行の末尾を省略記号にする。箱の外には描かない。
    bool ellipsis = false;
    // ellipsis で、行の最後のこの文字から後ろを残し、その手前を省く (パスのファイル名を残す)。0 なら末尾を省く。
    wchar_t ellipsis_keep_after = 0;
};

// 比率指定の行間にして、フォールバックで高くなる行は既定と同じく高いまま、主フォントの行を line_height にする。
inline void SetLineHeight(IDWriteFactory* dw, IDWriteTextFormat* fmt, float line_height)
{
    Microsoft::WRL::ComPtr<IDWriteTextFormat3> fmt3;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> probe;
    if (FAILED(fmt->QueryInterface(IID_PPV_ARGS(&fmt3))) || FAILED(dw->CreateTextLayout(L"a", 1, fmt, 0.0f, 0.0f, &probe))) {
        return;
    }
    DWRITE_LINE_METRICS lm{};
    UINT32 count = 0;
    if (FAILED(probe->GetLineMetrics(&lm, 1, &count)) || count == 0 || lm.height <= 0.0f) {
        return;
    }
    const DWRITE_LINE_SPACING spacing{ DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, line_height / lm.height, 1.0f, 0.5f, DWRITE_FONT_LINE_GAP_USAGE_DEFAULT };
    fmt3->SetLineSpacing(&spacing);
}

// 無効なフォントファミリ名は Segoe UI で作り直す (「フォントが効かない」状態を出さないための最終防壁)。
inline Microsoft::WRL::ComPtr<IDWriteTextFormat> CreateTextFormat(IDWriteFactory* dw, const TextFormatSpec& s)
{
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
    const auto create = [&](const wchar_t* family) {
        return dw->CreateTextFormat(family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, s.locale, fmt.ReleaseAndGetAddressOf());
    };
    if (const HRESULT hr = create(s.family); FAILED(hr)) {
        LogHrFailure("CreateTextFormat (falling back to Segoe UI)", hr);
        if (const HRESULT fallback = create(L"Segoe UI"); FAILED(fallback)) {
            LogHrFailure("CreateTextFormat (Segoe UI fallback)", fallback);
            return nullptr;
        }
    }
    if (s.no_wrap) {
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (s.text_align != DWRITE_TEXT_ALIGNMENT_LEADING) {
        fmt->SetTextAlignment(s.text_align);
    }
    if (s.line_height_factor > 0.0f) {
        SetLineHeight(dw, fmt.Get(), s.size * s.line_height_factor);
    }
    if (s.para_align != DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        fmt->SetParagraphAlignment(s.para_align);
    }
    if (s.ellipsis) {
        Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.Get(), &sign))) {
            const DWRITE_TRIMMING trimming{
                .granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER,
                .delimiter = s.ellipsis_keep_after,
                .delimiterCount = s.ellipsis_keep_after != 0 ? 1u : 0u,
            };
            fmt->SetTrimming(&trimming, sign.Get());
        }
    }
    return fmt;
}

inline D2D1_RECT_F ToD2DRect(const PaneRect& r) noexcept
{
    return D2D1::RectF(r.x, r.y, r.x + r.width, r.y + r.height);
}

inline D2D1_RECT_F OffsetRectF(const D2D1_RECT_F& r, float origin_x, float origin_y) noexcept
{
    return D2D1::RectF(origin_x + r.left, origin_y + r.top, origin_x + r.right, origin_y + r.bottom);
}

// 失敗したら Magenta で作り直し、nullptr のブラシを DrawXXX に渡さない
inline void CreateSolidColorBrushOrFallback(
    ID2D1RenderTarget* rt,
    D2D1_COLOR_F color,
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>& out_brush) noexcept
{
    if (SUCCEEDED(rt->CreateSolidColorBrush(color, out_brush.ReleaseAndGetAddressOf()))) {
        return;
    }
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Magenta), out_brush.ReleaseAndGetAddressOf()))) {
        MENDO_TRACE("CreateSolidColorBrush failed even on magenta fallback");
    }
}

// ビットマップの最大辺。描画ターゲットを作る前は D3D11 (FL11) のテクスチャ最大辺を使う。
inline UINT MaxBitmapDim(ID2D1RenderTarget* rt) noexcept
{
    constexpr UINT kWithoutTarget = 16384;
    return rt ? rt->GetMaximumBitmapSize() : kWithoutTarget;
}

// rt の今の変換に DPI を掛けた、DIP から装置の画素への変換。
D2D1::Matrix3x2F DeviceTransform(ID2D1RenderTarget* rt) noexcept;

// Present の結果が、デバイスを失ったことを表すか。TDR / ドライバ内部障害もデバイスの作り直しで戻るので REMOVED/RESET と同じに
// 扱う。OCCLUDED (成功) / WAS_STILL_DRAWING / INVALID_CALL は誤爆になるので拾わない。
constexpr bool IsDeviceLostOnPresent(HRESULT hr) noexcept
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

bool IsColorFont(IDWriteFontFace* face) noexcept;

// 色のフォント (IsColorFont) の run を、rt の今の変換と DPI でフォントの色で描く。層の色の無い部分は brush で塗る。描けなければ
// false (呼び出し側が単色で描く)。COLR の層は自分で塗る。D2D の DrawGlyphRunWithColorSupport は陰影まで描くが遅く
// (BM_EmojiFrameDraw が約 150 倍)、平らな色が mendo の簡素な見た目に合うのでこちらを選んだ。層が無ければ D2D に任せる。
bool DrawColorGlyphRun(ID2D1RenderTarget* rt, D2D1_POINT_2F origin, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run,
                       const DWRITE_GLYPH_RUN_DESCRIPTION* desc, ID2D1Brush* brush) noexcept;

// IDWriteTextLayout::Draw で rt へ描くレンダラの土台。字形は作ったときの rt の変換と DPI で置く。
// スタック上でのみ使うので参照カウントは持たない。
class RenderTargetTextRenderer : public UncountedCom<IDWriteTextRenderer> {
public:
    explicit RenderTargetTextRenderer(ID2D1RenderTarget* rt) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override;
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override;
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override;
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels_per_dip) noexcept override;

protected:
    // 色のフォント (絵文字) のランを ::DrawColorGlyphRun で描く。色のフォントでないか描けなければ false。
    bool DrawColorGlyphRun(float x, float y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN& run, const DWRITE_GLYPH_RUN_DESCRIPTION* desc,
                           ID2D1Brush* brush) const noexcept;

    ID2D1RenderTarget* rt_;
    D2D1_MATRIX_3X2_F transform_{};
    float pixels_per_dip_ = 1.0f;
};

// layout を brush の単色で描く。絵文字は ::DrawColorGlyphRun で描く。
void DrawTextLayoutWithColorLayers(ID2D1RenderTarget* rt, D2D1_POINT_2F origin, IDWriteTextLayout* layout, ID2D1Brush* brush) noexcept;

// 単色のグリフランを、書体・大きさ・ブラシ・測り方の組ごとに 1 本にまとめて描く (DrawGlyphRun は 1 回ごとの固定費が大きい)。
// ベースラインの違いは字形のずらし (ascenderOffset) で、前のランの頭へ戻るのは前の字形の送り幅で付ける。本文 (command_executor) と
// 数式 (DrawMath) で使う。配列は残して使い回す。
class GlyphRunMerger {
public:
    // (x, y) はランのベースラインの頭。run は送り幅を持つ左から右の横書きのランであること。
    void Add(float x, float y, const DWRITE_GLYPH_RUN& run, ID2D1Brush* brush, DWRITE_MEASURING_MODE mode);
    void Flush(ID2D1RenderTarget* rt);

private:
    struct Group {
        IDWriteFontFace* face = nullptr;
        float size = 0.0f;
        ID2D1Brush* brush = nullptr;
        DWRITE_MEASURING_MODE mode = DWRITE_MEASURING_MODE_NATURAL;
        D2D1_POINT_2F origin{};
        float pen = 0.0f;
        std::vector<UINT16> glyphs;
        std::vector<FLOAT> advances;
        std::vector<DWRITE_GLYPH_OFFSET> offsets;
    };
    std::vector<Group> groups_;
    size_t used_ = 0;
};

struct ID3D11Device;
struct IDXGISwapChain1;
struct ID2D1Device;
struct ID2D1DeviceContext;

// スワップチェーンの今のバッファを、dpi の描画先として dc に付ける (dc の DPI も dpi にする)。本体の窓と WindowSwapTarget で共有する。
HRESULT BindSwapChainTarget(ID2D1DeviceContext* dc, IDXGISwapChain1* swap_chain, float dpi) noexcept;

// 本体の描画デバイスで、ほかの窓 (ポップアップ・図の拡大表示) を描くスワップチェーンと DeviceContext。
class WindowSwapTarget {
public:
    enum class Mode : uint8_t {
        // 伸ばさずに左上に置き、窓の大きさは Resize (ResizeBuffers) で合わせる。FLIP_DISCARD の 2 枚。
        Flip,
        // 角を丸める region (SetWindowRgn) が効く BitBlt 方式。DISCARD の 1 枚。
        BitBlt,
    };

    WindowSwapTarget() noexcept;
    ~WindowSwapTarget();
    WindowSwapTarget(const WindowSwapTarget&) = delete;
    WindowSwapTarget& operator=(const WindowSwapTarget&) = delete;

    bool Create(ID3D11Device* d3d, ID2D1Device* device, HWND hwnd, UINT width, UINT height, float dpi, Mode mode);
    bool Resize(UINT width, UINT height, float dpi);
    bool EndDrawAndPresent();
    void Reset() noexcept;
    ID2D1DeviceContext* dc() const noexcept
    {
        return dc_.Get();
    }

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
};
