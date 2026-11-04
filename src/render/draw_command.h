#pragma once
#include "brush_id.h"
#include "syntax.h"
#include "utility.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <limits>
#include <memory>
#include <variant>
#include <vector>
#include <span>
#include <cstdint>
#include <utility>

// 色はすべて固定ブラシ (BrushId) で指定する。半透明は opacity で掛ける。
struct FillRectCmd {
    D2D1_RECT_F rect;
    BrushId brush_id;
};

struct FillRoundedRectCmd {
    D2D1_RECT_F rect;
    float radius;
    BrushId brush_id;
    float opacity = 1.0f;
};

struct DrawRoundedRectCmd {
    D2D1_RECT_F rect;
    float radius;
    float stroke_width;
    BrushId brush_id;
};

struct DrawLineCmd {
    D2D1_POINT_2F p0, p1;
    float stroke_width;
    BrushId brush_id;
};

struct DrawTextLayoutCmd {
    D2D1_POINT_2F origin;
    IDWriteTextLayout* layout; // 非所有; ライフタイムは LayoutCache か TextLayoutCache が管理。
    BrushId brush_id;
    // 可視範囲より高いレイアウトは [cull_top, cull_bottom] (origin と同じ座標系) 外のランを飛ばす。
    // 低いレイアウトでは範囲は既定の無限のままにする。
    float cull_top = -std::numeric_limits<float>::infinity();
    float cull_bottom = std::numeric_limits<float>::infinity();
    // 折り返さない長い行は、横スクロールの clip に映らないグリフを [cull_left, cull_right] で飛ばす
    // (1 行 12KB の JSON を 60 行描く 1 フレームが 127 → 4.5ms。BM_CodeBlockDraw)。
    float cull_left = -std::numeric_limits<float>::infinity();
    float cull_right = std::numeric_limits<float>::infinity();
    float opacity = 1.0f;
    // 非所有; layout と同じ NodeLayoutEntry が持つ。UTF-16 位置のトークンで、描画時にグリフランを色分けする。
    std::span<const SyntaxToken> syntax_tokens;
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムは LayoutCache::DiagramEntry か Renderer が管理。
    D2D1_RECT_F dest;
    float opacity = 1.0f;
};

// 描画命令から描く図。commands は図の左上を原点とする CSS px で記録してあり、dest に合わせて縮めて描く。
struct DrawDiagramCmd {
    ID2D1CommandList* commands; // 非所有; ライフタイムは LayoutCache::DiagramEntry が管理。
    D2D1_RECT_F dest;
    float width; // 記録した図の幅 (CSS px)
};

struct FillEllipseCmd {
    D2D1_POINT_2F center;
    float radius;
    BrushId brush_id;
    float opacity = 1.0f;
};

struct DrawEllipseCmd {
    D2D1_POINT_2F center;
    float radius;
    float stroke_width;
    BrushId brush_id;
};

// 一筆描きの線 (マウスジェスチャーの軌跡)。継ぎ目でアルファが重ならない。
struct DrawGeometryCmd {
    ID2D1Geometry* geometry;       // 非所有; Renderer がそのフレームの間持つ。
    ID2D1StrokeStyle* stroke_style; // 非所有; null なら既定。
    float stroke_width;
    BrushId brush_id;
    float opacity = 1.0f;
};

struct PushClipCmd {
    D2D1_RECT_F rect;
};

struct PopClipCmd {};

struct SetTransformCmd {
    D2D1_MATRIX_3X2_F transform;
};

using DrawCommand = std::variant<
    FillRectCmd,
    FillRoundedRectCmd,
    DrawRoundedRectCmd,
    DrawLineCmd,
    DrawTextLayoutCmd,
    DrawBitmapCmd,
    DrawDiagramCmd,
    FillEllipseCmd,
    DrawEllipseCmd,
    DrawGeometryCmd,
    PushClipCmd,
    PopClipCmd,
    SetTransformCmd>;

using DrawCommandList = std::vector<DrawCommand>;

using FixedBrushArray = std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, BRUSH_COUNT>;

// まとめて描いたインライン数式のページ 1 枚 (A8)。
struct InlineMathPage {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    // このページを指す型の数。読み直しで型がすべて別のページへ移ったら捨てる。
    size_t masks = 0;
    // 最後に描いたフレーム (InlineMathMasks::frame)。描くときに書く。
    uint64_t last_drawn = 0;
};

// インライン数式の型。黒で描いた数式のアルファだけを持ち、描くときに周りの文字の色で塗る
// (ダークモードやリンクの色で描き直さずに済む。数式の中の \color は効かない)。
struct InlineMathMask {
    InlineMathPage* page; // 非所有; InlineMathMasks::pages が持つ
    D2D1_RECT_F src;      // page の中の位置 (ピクセル)
    D2D1_RECT_F dest;     // 数式の箱の左上を原点とする描画先 (DIP)。箱からはみ出すインクの分だけ広い
};

// キーは InlineMathKey。ページは GPU のメモリに置かれ、数式 10000 個で約 38MB になるので、上限に収まる分だけ受け取り
// (App::ReserveInlineMathPage)、上限を超えたら長く描いていないページから捨てる (EvictPages)。型の無い数式は、
// 見えたときに組ごと読み直す (missing)。
struct InlineMathMasks {
    std::vector<std::unique_ptr<InlineMathPage>> pages;
    FlatHashMap<InlineMathMask> masks;
    size_t page_bytes = 0;
    // 描いたフレームの番号。Renderer が描くたびに進める。
    uint64_t frame = 0;
    // 見える位置にあるのに型が無かった数式。描いた後で呼び出し側が組を頼み直す。
    std::vector<uint64_t> missing;

    const InlineMathMask* Find(uint64_t key) const noexcept
    {
        return masks.Find(key);
    }
    InlineMathPage& AddPage(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap);
    // key の型を置き換える。前の型のページを指す型が無くなったら、そのページを捨てる。
    void Assign(uint64_t key, const InlineMathMask& mask);
    // ページの合計が budget を超えていたら、このフレームで描いていないページを古い順に捨てる。
    void EvictPages(size_t budget);
};

// 該当ブラシが未作成のコマンドは描かない。rt は null にしない。
// math には描いたページの last_drawn と、型の無かった数式 (missing) を書く。
void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray& brushes, InlineMathMasks& math);
