#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// 繰り返し描く短い文字列 (アイコン・リスト番号・プレースホルダ・サイドペインの項目等) のテキストレイアウトを
// (書式, 文字列, 箱の大きさ) ごとに作り置く。DrawText は呼ぶたびに内部でレイアウトを作るため、
// これで得たレイアウトを DrawTextLayout で描く。レイアウトは書式に結び付くので、書式を
// 作り直したら SetFactory で捨てる。
class TextLayoutCache {
public:
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        factory_ = factory;
        entries_.clear();
    }

    // 作れなければ nullptr。返したレイアウトは次の BeginFrame / SetFactory まで有効。
    IDWriteTextLayout* Get(IDWriteTextFormat* fmt, std::wstring_view text, D2D1_SIZE_F box)
    {
        if (!factory_ || !fmt || text.empty()) {
            return nullptr;
        }
        if (const auto it = entries_.find(KeyView{ fmt, text, box.width, box.height }); it != entries_.end()) {
            it->second.last_used = frame_;
            return it->second.layout.Get();
        }
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, box.width, box.height, &layout))) {
            return nullptr;
        }
        auto* const raw = layout.Get();
        entries_.emplace(Key{ fmt, std::wstring(text), box.width, box.height }, Entry{ std::move(layout), frame_ });
        return raw;
    }

    // box 指定時はその大きさのレイアウトを rect 左上に、未指定時は大きさ 0 のレイアウトを rect 中心に描く。
    void Draw(ID2D1RenderTarget* rt, IDWriteTextFormat* fmt, std::wstring_view text, const D2D1_RECT_F& rect,
              ID2D1Brush* brush, D2D1_SIZE_F box)
    {
        auto* const layout = Get(fmt, text, box);
        if (!layout || !brush) {
            return;
        }
        const D2D1_POINT_2F origin = (box.width > 0.0f) ? D2D1::Point2F(rect.left, rect.top)
                                                        : D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        rt->DrawTextLayout(origin, layout, brush);
    }

    // 箱の大きさや番号ごとに増え続けないよう、描画の合間 (前フレームのレイアウトを参照し終えた後) に上限で捨てる。
    // 全消去すると目次やファイル一覧が上限を超えるたびに表示中の項目まで作り直すので、直前のフレームで使ったものは残す。
    void BeginFrame() noexcept
    {
        if (entries_.size() > kMaxEntries) {
            std::erase_if(entries_, [prev = frame_](const auto& kv) noexcept { return kv.second.last_used != prev; });
        }
        ++frame_;
    }

    // テスト用: layout を作った文字列。見つからなければ空。
    std::wstring_view TextOf(const IDWriteTextLayout* layout) const noexcept
    {
        for (const auto& [key, value] : entries_) {
            if (value.layout.Get() == layout) {
                return key.text;
            }
        }
        return {};
    }

    static constexpr size_t kMaxEntries = 512;

private:
    struct KeyView {
        IDWriteTextFormat* fmt;
        std::wstring_view text;
        float width;
        float height;
        bool operator==(const KeyView&) const = default;
    };
    struct Key {
        IDWriteTextFormat* fmt;
        std::wstring text;
        float width;
        float height;
        operator KeyView() const noexcept
        {
            return { fmt, text, width, height };
        }
    };
    struct KeyHash {
        using is_transparent = void;
        static size_t operator()(const KeyView& k) noexcept
        {
            size_t h = std::hash<std::wstring_view>{}(k.text);
            const auto mix = [&h](size_t v) noexcept { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
            mix(std::hash<const void*>{}(k.fmt));
            mix(std::bit_cast<uint32_t>(k.width));
            mix(std::bit_cast<uint32_t>(k.height));
            return h;
        }
    };
    struct KeyEqual {
        using is_transparent = void;
        static bool operator()(const KeyView& a, const KeyView& b) noexcept
        {
            return a == b;
        }
    };

    struct Entry {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        uint32_t last_used;
    };

    IDWriteFactory* factory_ = nullptr;
    uint32_t frame_ = 0;
    std::unordered_map<Key, Entry, KeyHash, KeyEqual> entries_;
};
