#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// 脚注参照の位置。id は脚注番号、ref_id は同じ脚注への何回目の参照か (どちらも 1 始まり)。
struct FootnoteRef {
    uint32_t id = 0;
    uint32_t ref_id = 0;
    int32_t node_index = -1;
};

// アンカーは GitHub と同じ "user-content-fn-N" (定義) / "user-content-fnref-N[-M]" (M 回目の参照) 形式。
inline constexpr std::string_view kFootnoteDefAnchorPrefix = "user-content-fn-";
inline constexpr std::string_view kFootnoteRefAnchorPrefix = "user-content-fnref-";

// 先頭の '#' を含むリンク URL。
std::string FootnoteDefUrl(uint32_t id);
std::string FootnoteRefUrl(uint32_t id, uint32_t ref_id);

// anchor (ASCII 小文字) が指す脚注定義 / 参照のノード番号。脚注のアンカーでないか、該当が無ければ -1。
// def_nodes[id - 1] が脚注 id の定義ノード。
int FindFootnoteAnchor(std::string_view anchor, std::span<const int32_t> def_nodes, std::span<const FootnoteRef> refs) noexcept;
