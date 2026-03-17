#pragma once
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>

// Executes a DrawCommandList on a Direct2D render target.
// Uses a single reusable brush for all solid-color operations.
class CommandExecutor {
public:
    void Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt);

private:
    ID2D1SolidColorBrush* GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    ID2D1RenderTarget* bound_rt_ = nullptr;
};
