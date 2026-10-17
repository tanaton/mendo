#pragma once
#include "brush_id.h"
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <array>
#include <utility>

using FixedBrushArray = std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, std::to_underlying(BrushId::Count)>;

// brushes が null、または該当ブラシが未作成のコマンドは描かない。
void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray* brushes);
