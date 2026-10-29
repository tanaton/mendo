#pragma once
#include <cstdint>

// テキストラン: 同一スタイル（太字/斜体/コード/取り消し線/リンク）を持つ連続テキスト範囲。
struct TextRun {
    // 呼び出し側が複数フラグを OR で組み上げて set_raw_flags() で一括設定するため公開。
    static constexpr uint8_t kBold = 0x01;
    static constexpr uint8_t kItalic = 0x02;
    static constexpr uint8_t kCode = 0x04;
    static constexpr uint8_t kStrikethrough = 0x08;
    // インライン数式。範囲は前後の "$" を含み、テキストは原文のまま残してレイアウトでだけ数式に置き換える。
    static constexpr uint8_t kMath = 0x10;
    // $$...$$ と ```math。範囲は前後の "$$" を含み、ディスプレイ形式で本文の中に置く (```math はパーサが "$$" で囲む)。
    static constexpr uint8_t kMathDisplay = 0x20;

    uint32_t start = 0;
    uint32_t length = 0;
    int16_t link_url_index = -1; // -1 = リンクなし, >= 0 = Node::view_link_urls() へのインデックス

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
        return flags & (kMath | kMathDisplay);
    }
    constexpr bool display_math() const noexcept
    {
        return flags & kMathDisplay;
    }
    constexpr bool has_link() const noexcept
    {
        return link_url_index >= 0;
    }

    constexpr uint8_t raw_flags() const noexcept
    {
        return flags;
    }
    constexpr void set_raw_flags(uint8_t f) noexcept
    {
        flags = f;
    }

    bool operator==(const TextRun&) const = default;

private:
    uint8_t flags = 0;
};

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
        start_node = -1;
        start_pos = 0;
        end_node = -1;
        end_pos = 0;
    }

    // アンカー/キャレットをドキュメント順で start <= end に正規化する
    static constexpr TextSelection MakeOrdered(int node_a, uint32_t pos_a, int node_b, uint32_t pos_b) noexcept
    {
        TextSelection s;
        if (node_a < node_b || (node_a == node_b && pos_a <= pos_b)) {
            s.start_node = node_a;
            s.start_pos = pos_a;
            s.end_node = node_b;
            s.end_pos = pos_b;
        }
        else {
            s.start_node = node_b;
            s.start_pos = pos_b;
            s.end_node = node_a;
            s.end_pos = pos_a;
        }
        return s;
    }
};
