#pragma once
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>

// DrawCommandList を Direct2D レンダーターゲット上で実行する。
// すべての単色描画操作に対して再利用可能な単一ブラシを使用する。
class CommandExecutor {
public:
    void Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt);

private:
    ID2D1SolidColorBrush* GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1RenderTarget* bound_rt_ = nullptr;
};
