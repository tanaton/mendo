#pragma once
#include "anchor.h"
#include "document_types.h"
#include "text_arena.h"
#include <concepts>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// 段落・引用・コードブロックのテキストをこの長さ前後で区切り、続きのノードの並びにする。DirectWrite の計測は 1 つのレイアウトが
// 長いほど 1 文字あたりも遅く (和英混在はフォールバックを明示する区間が 1 レイアウト 1024 までなので特に)、1 ノードのままだと
// 最初の画面の前に全体を測り、並列にも測れず、見えている間は解放もできない。区切る位置は目安の手前の改行、1 行が長ければ
// 前後 kTextChunkWindow の空白か和文の句読点 (行の途中で区切るとそこで改行して見える)。コードブロックは 1 行が収まる限り
// 行の途中で区切らない。
inline constexpr size_t kTextChunkTarget = 16 * 1024;
inline constexpr size_t kTextChunkWindow = kTextChunkTarget / 8;

// パース後の全ノード走査を避けるため、パース中に構築する特殊ノードのインデックス。
struct NodeIndices {
    std::vector<TocEntry> toc_entries;
    // 画像 (kImage の run) を持つノード (表はどれかのセル)。読み込む画像を見えている範囲から探すのに使う。
    std::vector<size_t> image_indices;
    std::vector<size_t> diagram_indices;
    std::vector<size_t> table_indices;
    // インライン数式の run を持つノード (表はどれかのセル)。開く・読み直し・ズームのたびに全ノードを回らないため。
    std::vector<size_t> math_indices;
    // 脚注番号順の定義ノード。脚注は文書末尾に参照順で並ぶ。
    std::vector<int32_t> footnote_def_indices;
    std::vector<FootnoteRef> footnote_refs;
};

// パーサーの出力。ノードと、ノードが指すもの (テキスト・runs・リンク URL を置いた arena とテーブル) と特殊ノードのインデックス。
// ノードは arena を指すので、nodes だけを取り出すときは arena ごと持ち続けること。
struct NodeStorage {
    ReservedArray<Node> nodes;
    TextArena arena;
    std::vector<std::unique_ptr<NodeTableData>> tables;
    NodeIndices indices;
};

// stop_token を渡すと md4c コールバックから途中で abort できる。既定の stop_token では最後まで走る。
NodeStorage ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {});

// ノードは入力テキストを参照するため、一時文字列を渡すと戻り値が即座にダングリングする。
template <class S>
    requires(!std::is_lvalue_reference_v<S> &&
             std::same_as<std::remove_cv_t<S>, std::string>)
NodeStorage ParseMarkdown(S&& markdown_text, std::stop_token stop_token = {}) = delete;

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する
[[nodiscard]] std::string_view ResolveHtmlEntity(std::string_view entity, char (&buffer)[4]);
