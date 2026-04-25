#pragma once
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_map>

namespace command_executor_internal {

// D2D1_COLOR_F を 8bit RGBA にパックしたキー。CommandExecutor のブラシキャッシュキーに使う。
// テーマ色は 8bit 精度で作られているため 8bit 量子化で衝突しない。
constexpr uint32_t PackColor(D2D1_COLOR_F c) noexcept
{
    const auto quant = [](float v) noexcept -> uint32_t {
        if (v <= 0.0f) {
            return 0;
        }
        if (v >= 1.0f) {
            return 255;
        }
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return (quant(c.r) << 24) | (quant(c.g) << 16) | (quant(c.b) << 8) | quant(c.a);
}

} // namespace command_executor_internal

// DrawCommandList を Direct2D レンダーターゲット上で実行する。
// 色ごとに ID2D1SolidColorBrush をプールし、SetColor 呼び出しも削減する。
class CommandExecutor {
public:
    void Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt);

#ifdef MENDO_TESTING
    size_t PoolSizeForTest() const noexcept { return brush_pool_.size(); }
    const ID2D1RenderTarget* BoundRtForTest() const noexcept { return bound_rt_; }
#endif

private:
    ID2D1SolidColorBrush* GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color);

    static constexpr size_t MAX_POOLED_BRUSHES = 256;

    std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brush_pool_;
    ID2D1RenderTarget* bound_rt_ = nullptr;
    uint32_t last_key_ = 0xFFFFFFFFu;
    ID2D1SolidColorBrush* last_brush_ = nullptr;
};
