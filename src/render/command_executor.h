#pragma once
#include "brush_id.h"
#include "draw_command.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>

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

// 固定 BrushId に対応するブラシ配列のビュー。
// CommandExecutor::Execute に渡し、brush_id != Custom のコマンドを O(1) で解決する。
using FixedBrushArray = std::array<ID2D1SolidColorBrush*, std::to_underlying(BrushId::Count)>;

// DrawCommandList を Direct2D レンダーターゲット上で実行する。
// 固定色は brush_id 経由で配列ルックアップ、それ以外は色ごとに ID2D1SolidColorBrush をプールし、
// 上限到達時は LRU で 1 エントリだけ追い出す。
class CommandExecutor {
public:
    // brushes が null のときは全コマンドが brush_pool 経由になる (テスト互換)。
    void Execute(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray* brushes = nullptr);

#ifdef MENDO_TESTING
    size_t PoolSizeForTest() const noexcept
    {
        return brush_pool_.size();
    }
    constexpr const ID2D1RenderTarget* BoundRtForTest() const noexcept
    {
        return bound_rt_;
    }
#endif

private:
    ID2D1SolidColorBrush* GetBrush(ID2D1RenderTarget* rt, D2D1_COLOR_F color);
    // brush_id が Custom 以外なら brushes 配列でルックアップ。失敗時は color フォールバック。
    ID2D1SolidColorBrush* ResolveBrush(ID2D1RenderTarget* rt, BrushId id, D2D1_COLOR_F color);

    static constexpr size_t MAX_POOLED_BRUSHES = 256;

    // front = most recently used, back = oldest. splice で O(1) 昇格・追い出し。
    using LruList = std::list<uint32_t>;

    struct BrushEntry {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        LruList::iterator lru_pos;
    };

    // UI スレッド専用なので pmr の sync pool を経由せず標準アロケータを使う。
    std::unordered_map<uint32_t, BrushEntry> brush_pool_;
    LruList lru_keys_;
    ID2D1RenderTarget* bound_rt_ = nullptr;
    // 同色連続発行（罫線、ハイライト等）の hash lookup を省くための直前ブラシキャッシュ。
    // last_brush_==nullptr が「キャッシュ無効」を示し、PackColor の値域全体を
    // 非衝突に使えるようにする。
    uint32_t last_brush_key_ = 0;
    ID2D1SolidColorBrush* last_brush_ = nullptr;
    // 固定色ブラシ配列ビュー。null のとき配列ルックアップを行わず brush_pool 経由になる。
    const FixedBrushArray* fixed_brushes_ = nullptr;
};
