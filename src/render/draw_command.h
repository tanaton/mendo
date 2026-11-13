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

// 円 (箇条書きの記号・スピナーの点) は半径 = 半辺の角丸矩形で描く。楕円と画素まで同じになる
// (DPI 96〜192・半径 2.5〜4.25・中心の端数で、塗りと線の 160 通りを比べた。2026-11-06)。
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
    // (1 行 12KB の JSON を 60 行描く 1 フレームが 127 → 4.5ms。BM_CodeBlockDraw)。
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

// 描画命令から描く図。commands は図の左上を原点とする CSS px で記録してあり、dest に合わせて縮めて描く。
struct DrawDiagramCmd {
    ID2D1CommandList* commands; // 非所有; ライフタイムは LayoutCache::DiagramEntry が管理。
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

// スクロールの間、見えている図を装置の画素のビットマップに描いておき、次のフレームからはそれを写す。図は毎フレームベクタで
// 描くと 1 個 0.25ms (重い図は 1ms 超) かかるが、写すのは数十 µs。ビットマップは見えている部分の上下に 1/4 画面ずつ足した
// 帯だけを持ち、帯から出たら描き直す。持つのは描いたフレームで見えていた図の分だけで、合計は画面の約 1.5 倍まで。
// 止まったら Clear で捨ててベクタに戻す (くっきり描き、静止中は持たない)。全部の図をビットマップで持つ案は GPU +161MB /
// private +125MB で見送った。見えている分だけをスクロールの間だけ持つ形は、ユーザーが選んだ (2026-11-13)。
// 帯の大きさが変わるたびにビットマップを作り直す。帯の最大 (図の高さか 1.5 画面) で確保して使い回すと、作り直しが 96 → 27 回、
// 1 フレームが 7〜10% 速くなるが、図を積んだ文書で同時に持つ最大が 8.4 → 10.0MB (+19%) になり上の合計が崩れるので、
// ユーザーが今のままを選んだ (2026-11-14)。
class DiagramBitmapCache {
public:
    DiagramBitmapCache();
    ~DiagramBitmapCache();
    DiagramBitmapCache(const DiagramBitmapCache&) = delete;
    DiagramBitmapCache& operator=(const DiagramBitmapCache&) = delete;

    // 持っているビットマップを捨て、以後は device の描画先に描く。null なら描かない。
    void Reset(ID2D1Device* device) noexcept;
    // 持っているビットマップを捨てる。
    void Clear() noexcept;
    // 1 フレームを描き終えたら呼ぶ。このフレームで描かなかった図のビットマップを捨てる。
    void EndFrame() noexcept;
    // dc の今の変換で、図の左上を (left, top)、拡大を scale として commands を描く。height は図の高さ (記録の単位)。
    // dc は Reset で渡したデバイスの描画先。描けなければ false で、呼び出し側がベクタで描く。
    bool Draw(ID2D1DeviceContext* dc, ID2D1CommandList* commands, float left, float top, float scale, float width, float height);

    size_t Count() const noexcept;
    size_t Bytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 該当ブラシが未作成のコマンドは描かない。rt は null にしない。math_masks があればインライン数式の SVG の形をマスクで塗る。
// diagram_bitmaps があれば図をビットマップで描く (スクロールの間)。
void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray& brushes, const InlineDrawings& inline_drawings,
                         MathMaskCache* math_masks = nullptr, DiagramBitmapCache* diagram_bitmaps = nullptr);
