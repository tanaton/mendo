#pragma once
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <cstdint>
#include <array>
#include <unordered_map>

namespace command_executor_internal {

// D2D1_COLOR_F を 8bit RGBA にパックしたキー。CommandExecutor のブラシキャッシュキーに使う。
// テーマ色は 8bit 精度で作られているため 8bit 量子化で衝突しない。
// command_executor.cpp の GetBrush から呼ばれる本番実装の一部のため MENDO_TESTING でガードしない。
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
// 色ごとに ID2D1SolidColorBrush をプールし、上限到達時は LRU で 1 エントリだけ追い出す。
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

    struct BrushEntry {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        uint64_t last_used = 0;
    };

    std::unordered_map<uint32_t, BrushEntry> brush_pool_;
    uint64_t use_counter_ = 0;
    ID2D1RenderTarget* bound_rt_ = nullptr;
};
