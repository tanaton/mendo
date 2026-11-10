#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

// テキストラン: 同一スタイル（下の flags とリンク・数式・画像）を持つ連続テキスト範囲。
// 守る規則 (start 昇順で重ならない、テキストに収まる) は run の並びに対するものなので、1 つの run には
// 隠す状態を持たせず集成体にする。flags は下の定数の OR。
struct TextRun {
    static constexpr uint8_t kBold = 0x01;
    static constexpr uint8_t kItalic = 0x02;
    static constexpr uint8_t kCode = 0x04;
    static constexpr uint8_t kStrikethrough = 0x08;
    // インライン数式。範囲は前後の "$" を含み、テキストは原文のまま残してレイアウトでだけ数式に置き換える。
    static constexpr uint8_t kMath = 0x10;
    // $$...$$ と ```math。範囲は前後の "$$" を含む (```math はパーサが "$$" で囲む)。
    static constexpr uint8_t kMathDoubleDollar = 0x20;
    // 画像。範囲は alt (空ならパーサが置く kImagePlaceholder) で、レイアウトで画像に置き換える。
    // 画像 1 つが run 1 つで、alt は平文 (中の装飾・リンク・数式は run を分けない)。
    static constexpr uint8_t kImage = 0x40;

    uint32_t start = 0;
    uint32_t length = 0;
    int16_t link_url_index = -1; // -1 = リンクなし, >= 0 = Node::view_link_urls() へのインデックス
    uint8_t flags = 0;
    // kImage の run の src。Node::image_src で引く。TextRun の詰め物の 1 バイトを使うので、1 ノードの画像は kMaxImageSlots まで。
    uint8_t image_slot = 0;

    constexpr bool bold() const noexcept
    {
        return flags & kBold;
    }
    constexpr bool italic() const noexcept
    {
        return flags & kItalic;
    }
    constexpr bool code() const noexcept
    {
        return flags & kCode;
    }
    constexpr bool strikethrough() const noexcept
    {
        return flags & kStrikethrough;
    }
    // $...$ か $$...$$。
    constexpr bool math() const noexcept
    {
        return flags & (kMath | kMathDoubleDollar);
    }
    constexpr bool double_dollar_math() const noexcept
    {
        return flags & kMathDoubleDollar;
    }
    constexpr bool image() const noexcept
    {
        return flags & kImage;
    }
    constexpr bool has_link() const noexcept
    {
        return link_url_index >= 0;
    }

    bool operator==(const TextRun&) const = default;
};
// 文書の run は数百万になるので、image_slot は詰め物の中に置いて大きさを変えない。
static_assert(sizeof(TextRun) == 12);

// 1 ノードの画像の src の数。Node が数を 1 バイトで持つので 255 まで。超えた画像は alt を文字のまま出す。
inline constexpr size_t kMaxImageSlots = 255;
// alt の空な画像の範囲に置く文字 (U+FFFC OBJECT REPLACEMENT CHARACTER)。レイアウトで画像に置き換え、コピーと目次では外す。
inline constexpr std::string_view kImagePlaceholder = "\xEF\xBF\xBC";

// 画像の run の alt。置き換え文字なら空。
constexpr std::string_view ImageAlt(std::string_view text, const TextRun& image) noexcept
{
    const std::string_view alt = text.substr(image.start, image.length);
    return alt == kImagePlaceholder ? std::string_view{} : alt;
}

// テキスト選択: 位置は (node_index, char_offset) のペアで表す。
struct TextSelection {
    int start_node = -1;
    uint32_t start_pos = 0;
    int end_node = -1;
    uint32_t end_pos = 0;

    // 空でない範囲を選んでいる。
    constexpr bool active() const noexcept
    {
        return start_node != end_node || start_pos != end_pos;
    }

    struct NodeRange {
        uint32_t start;
        uint32_t end;
    };

    // node_index に対応する選択範囲 [start, end)。end は text_size でクランプ済み。
    constexpr NodeRange ClampedRange(int node_index, size_t text_size) const noexcept
    {
        uint32_t start = 0;
        uint32_t end = static_cast<uint32_t>(text_size);
        if (node_index == start_node) {
            start = start_pos;
        }
        if (node_index == end_node) {
            end = end_pos;
        }
        if (end > text_size) {
            end = static_cast<uint32_t>(text_size);
        }
        return { start, end };
    }

    // ClampedRange に加え、非アクティブや範囲外のノードでは空範囲を返す。
    constexpr NodeRange RangeIn(int node_index, size_t text_size) const noexcept
    {
        if (!active() || node_index < start_node || node_index > end_node) {
            return { 0, 0 };
        }
        return ClampedRange(node_index, text_size);
    }

    friend constexpr bool operator==(const TextSelection&, const TextSelection&) noexcept = default;

    constexpr void Clear() noexcept
    {
        *this = {};
    }

    // アンカー/キャレットをドキュメント順で start <= end に正規化する
    static constexpr TextSelection MakeOrdered(int node_a, uint32_t pos_a, int node_b, uint32_t pos_b) noexcept
    {
        if (std::pair{ node_b, pos_b } < std::pair{ node_a, pos_a }) {
            return { node_b, pos_b, node_a, pos_a };
        }
        return { node_a, pos_a, node_b, pos_b };
    }
};
