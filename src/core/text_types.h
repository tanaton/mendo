#pragma once
#include <cstdint>

// テキストラン: 同一スタイル（太字/斜体/コード/取り消し線/リンク）を持つ連続テキスト範囲。
struct TextRun {
    // 呼び出し側が複数フラグを OR で組み上げて set_raw_flags() で一括設定するため公開。
    static constexpr uint8_t kBold = 0x01;
    static constexpr uint8_t kItalic = 0x02;
    static constexpr uint8_t kCode = 0x04;
    static constexpr uint8_t kStrikethrough = 0x08;

    uint32_t start = 0;
    uint32_t length = 0;
    int16_t link_url_index = -1; // -1 = リンクなし, >= 0 = Node::link_urls へのインデックス

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
    constexpr bool has_link() const noexcept
    {
        return link_url_index >= 0;
    }

    constexpr void set_bold(bool v) noexcept
    {
        set_flag(kBold, v);
    }
    constexpr void set_italic(bool v) noexcept
    {
        set_flag(kItalic, v);
    }
    constexpr void set_code(bool v) noexcept
    {
        set_flag(kCode, v);
    }
    constexpr void set_strikethrough(bool v) noexcept
    {
        set_flag(kStrikethrough, v);
    }

    constexpr void set_raw_flags(uint8_t f) noexcept
    {
        flags = f;
    }

private:
    constexpr void set_flag(uint8_t mask, bool v) noexcept
    {
        flags = v ? (flags | mask) : static_cast<uint8_t>(flags & ~mask);
    }

    uint8_t flags = 0;
};

// テキスト選択: 位置は (node_index, char_offset) のペアで表す。
struct TextSelection {
    int start_node = -1;
    uint32_t start_pos = 0;
    int end_node = -1;
    uint32_t end_pos = 0;
    bool active = false;

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

    friend constexpr bool operator==(const TextSelection&, const TextSelection&) noexcept = default;

    constexpr void Clear() noexcept
    {
        start_node = -1;
        end_node = -1;
        active = false;
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
        s.active = (s.start_node != s.end_node || s.start_pos != s.end_pos);
        return s;
    }
};
