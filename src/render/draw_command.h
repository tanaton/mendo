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

// 色はすべて固定ブラシ (BrushId) で指定する。半透明は opacity で掛ける。
struct FillRectCmd {
    D2D1_RECT_F rect;
    BrushId brush_id;
};

// 円 (箇条書きの記号・スピナーの点) は半径 = 半辺の角丸矩形で描く
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
    // [cull_top, cull_bottom] (origin と同じ座標系) の外のランを飛ばす。本文は可視範囲を渡す。既定の無限なら飛ばさない。
    float cull_top = -std::numeric_limits<float>::infinity();
    float cull_bottom = std::numeric_limits<float>::infinity();
    // 折り返さない長い行は、横スクロールの clip に映らないグリフを [cull_left, cull_right] で飛ばす
    // (BM_CodeBlockDraw/json_12kb が 127 → 4.5ms)。
    float cull_left = -std::numeric_limits<float>::infinity();
    float cull_right = std::numeric_limits<float>::infinity();
    float opacity = 1.0f;
    // 非所有; layout と同じ NodeLayoutEntry が持つ。UTF-16 位置のトークンで、描画時にグリフランを色分けする。
    std::span<const SyntaxToken> syntax_tokens;
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムは Renderer が管理。
    D2D1_RECT_F dest;
    float opacity = 1.0f;
};

// コマンドリストで描く図。commands は図の左上を原点とする CSS px で記録してあり、dest に合わせて縮めて描く。
struct DrawDiagramCmd {
    ID2D1CommandList* commands; // 非所有; ライフタイムは LayoutCache の DiagramEntry が管理。
    D2D1_RECT_F dest;
    float width; // 記録した図の幅 (CSS px)
};

// 一筆描きの線 (マウスジェスチャーの軌跡)。継ぎ目でアルファが重ならない。
struct DrawGeometryCmd {
    ID2D1Geometry* geometry;        // 非所有; Renderer がそのフレームの間持つ。
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
    DrawGeometryCmd,
    PushClipCmd,
    PopClipCmd,
    SetTransformCmd>;

using DrawCommandList = std::vector<DrawCommand>;

using FixedBrushArray = std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, BRUSH_COUNT>;

struct MathDrawing;
class MathMaskCache;

// 本文中のインラインオブジェクトの描画に使うもの。
struct InlineDrawings {
    // インライン数式の描画命令。キーは InlineMathKey。\color などで色を指定した部分のほかは、描くときに周りの文字の色で塗る
    // (ダークモードやリンクの色で描き直さずに済む)。
    FlatHashMap<std::shared_ptr<const MathDrawing>> math;
    // 画像のビットマップ。キーは InlineImageKey。見えている範囲の画像だけ持つ (ResourceManager が入れ替える)。
    FlatHashMap<Microsoft::WRL::ComPtr<ID2D1Bitmap>> images;

    const MathDrawing* FindMath(uint64_t key) const noexcept
    {
        const auto* found = math.Find(key);
        return found ? found->get() : nullptr;
    }
    ID2D1Bitmap* FindImage(uint64_t key) const noexcept
    {
        const auto* found = images.Find(key);
        return found ? found->Get() : nullptr;
    }
};

// スクロールの間、見えている図を装置の画素のビットマップに描いておき、次のフレームからはそれを写す。持つのは見えている部分の
// 上下に 1/4 画面ずつ足した帯だけで、止まったら捨てる。全部の図を持つ案と、帯の最大で確保して使い回す案は、メモリが増えるので
// ユーザーが今の形を選んだ。
class DiagramBitmapCache {
public:
    DiagramBitmapCache();
    ~DiagramBitmapCache();
    DiagramBitmapCache(const DiagramBitmapCache&) = delete;
    DiagramBitmapCache& operator=(const DiagramBitmapCache&) = delete;

    void Reset(ID2D1Device* device) noexcept;
    void Clear() noexcept;
    void EndFrame() noexcept;
    bool Draw(ID2D1DeviceContext* dc, ID2D1CommandList* commands, float left, float top, float scale, float width, float height);

    size_t Count() const noexcept;
    size_t Bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray& brushes, const InlineDrawings& inline_drawings,
                         MathMaskCache* math_masks = nullptr, DiagramBitmapCache* diagram_bitmaps = nullptr);
