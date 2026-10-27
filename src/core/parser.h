#pragma once
#include "anchor.h"
#include "document_types.h"
#include "text_arena.h"
#include <concepts>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

class TaskScheduler;

// 段落・引用・コードブロックのテキストをこの長さ前後で区切り、続きのノードの並びにする。DirectWrite の計測は
// 1 つのテキストレイアウトの長さに対して二乗に近く遅くなり (空行の無い 4MB のテキストで 28 秒)、NO_WRAP にしても
// レイアウト内を改行で分けても変わらない。1 ノードのままだと並列に計測できず、見えている間は解放もできない。
// 改行が目安の kTextChunkWindow 手前を過ぎたらそこで、1 行が長ければ目安の前後 kTextChunkWindow にある空白か和文の句読点で、
// どちらも無ければ目安の長さで区切る。区切ったノードは別のレイアウトなので、行の途中で区切るとそこで改行して見える。
// コードブロックは折り返さないので、1 行がノード 1 つに収まる限りその行の前の改行で区切り、行の途中では区切らない。
inline constexpr size_t kTextChunkTarget = 16 * 1024;
inline constexpr size_t kTextChunkWindow = kTextChunkTarget / 8;

// パース後の全ノード走査を避けるため、パース中に構築する特殊ノードのインデックス。
struct NodeIndices {
    std::vector<TocEntry> toc_entries;
    std::vector<size_t> image_indices;
    std::vector<size_t> diagram_indices;
    std::vector<size_t> table_indices;
    // 脚注番号順の定義ノード。脚注は文書末尾に参照順で並ぶ。
    std::vector<int32_t> footnote_def_indices;
    std::vector<FootnoteRef> footnote_refs;
};

// パーサーの出力。ノードと、ノードが指すもの (テキスト・runs・リンク URL を置いた arena とテーブル) と特殊ノードのインデックス。
// ノードは arena を指すので、nodes だけを取り出すときは arena ごと持ち続けること。
struct NodeStorage {
    std::vector<Node> nodes;
    TextArena arena;
    std::vector<std::unique_ptr<NodeTableData>> tables;
    NodeIndices indices;
};

// 本物の Markdown パーサ。md4c を UTF-8 モード (MD_CHAR=char) で起動するため入力は char 列。
// stop_token を渡すと md4c コールバックから途中で abort できる。既定の stop_token では最後まで走る。
// scheduler は区切ったコードブロックの字句解析 (AnnotateCodeChunkLeads) を並列にする先。
NodeStorage ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {}, TaskScheduler* scheduler = nullptr);

// ノードは入力テキストを参照するため、一時文字列を渡すと戻り値が即座にダングリングする。
template <class S>
    requires(!std::is_lvalue_reference_v<S> &&
             std::same_as<std::remove_cv_t<S>, std::string>)
NodeStorage ParseMarkdown(S&& markdown_text, std::stop_token stop_token = {}, TaskScheduler* scheduler = nullptr) = delete;

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。UTF-8 では U+10FFFF までを最大 4 byte で表現できる。
[[nodiscard]] std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4]);
