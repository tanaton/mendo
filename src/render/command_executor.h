#pragma once
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <unordered_map>

// DrawCommandList を Direct2D レンダーターゲット上で実行する。
// 色ごとに ID2D1SolidColorBrush をプールし、SetColor 呼び出しも削減する。
class CommandExecutor {
public:
    void Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt);

private:
    ID2D1SolidColorBrush* GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color);

    // ブラシ数が超過したらプールを一掃し、肥大化を防ぐ。
    // テーマ + UI アクセント色は通常 ~50 個以内に収まる。
    static constexpr size_t MAX_POOLED_BRUSHES = 256;

    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brush_pool_;
    ID2D1RenderTarget* bound_rt_ = nullptr;
    uint32_t last_key_ = 0xFFFFFFFFu;
    ID2D1SolidColorBrush* last_brush_ = nullptr;
};
