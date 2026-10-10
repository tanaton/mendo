#pragma once
#include "brush_id.h"
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <array>
#include <utility>

using FixedBrushArray = std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, std::to_underlying(BrushId::Count)>;

class CommandExecutor {
public:
    // brushes が null のときは全コマンドをコマンドの color で描く (テスト用)。
    void Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray* brushes = nullptr);

private:
    ID2D1SolidColorBrush* ResolveBrush(ID2D1RenderTarget* rt, BrushId id, D2D1_COLOR_F color);

    // 動的色はアラート背景とオーバーレイボタン程度なので、色ごとにブラシを持たず 1 本を SetColor で使い回す。
    // ブラシは RT 付随リソースなので RT が替わったら作り直す。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scratch_brush_;
    ID2D1RenderTarget* scratch_rt_ = nullptr;
    const FixedBrushArray* fixed_brushes_ = nullptr;
};
