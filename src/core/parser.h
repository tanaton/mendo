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
#include <memory_resource>

// 段落・引用・コードブロックのテキストをこの長さ前後で区切り、続きのノードの並びにする。DirectWrite の計測は
// 1 つのテキストレイアウトの長さに対して二乗に近く遅くなり (空行の無い 4MB のテキストで 28 秒)、NO_WRAP にしても
// レイアウト内を改行で分けても変わらない。1 ノードのままだと並列に計測できず、見えている間は解放もできない。改行が目安の kTextChunkWindow 手前を過ぎたらそこで、1 行が長ければ目安の前後
// kTextChunkWindow にある空白で、空白も無ければ目安の長さで区切る。
inline constexpr size_t kTextChunkTarget = 16 * 1024;
inline constexpr size_t kTextChunkWindow = kTextChunkTarget / 8;

// ノードと、ノードが指すもの (テキスト・runs・リンク URL を置いた arena とテーブル) の組。
struct NodeStorage {
    std::pmr::vector<Node> nodes;
    std::unique_ptr<TextArena> arena;
    std::pmr::vector<std::unique_ptr<NodeTableData>> tables;
};

// パーサーの出力: ノード本体と、パース中に構築された特殊ノードインデックス
struct ParseResult {
    ParseResult() = default;
    explicit ParseResult(NodeStorage storage) noexcept : storage_(std::move(storage)) {}

    // ノードは arena を指すため、一時オブジェクトからノードだけを参照する書き方
    // (ParseMarkdown(md).nodes()) はコンパイルエラーにする。取り出すときは TakeNodes で arena ごと受け取る。
    std::pmr::vector<Node>& nodes() & noexcept
    {
        return storage_.nodes;
    }
    const std::pmr::vector<Node>& nodes() const& noexcept
    {
        return storage_.nodes;
    }
    void nodes() const&& = delete;

    NodeStorage TakeNodes() noexcept
    {
        return std::move(storage_);
    }

    std::pmr::vector<TocEntry> toc_entries;
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> diagram_indices;
    std::pmr::vector<size_t> table_indices;
    // 脚注番号順の定義ノード。脚注は文書末尾に参照順で並ぶ。
    std::pmr::vector<int32_t> footnote_def_indices;
    std::pmr::vector<FootnoteRef> footnote_refs;

private:
    NodeStorage storage_;
};

// 本物の Markdown パーサ。md4c を UTF-8 モード (MD_CHAR=char) で起動するため入力は char 列。
// stop_token を渡すと md4c コールバックから途中で abort できる。既定の stop_token では最後まで走る。
ParseResult ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {});

// ノードは入力テキストを参照するため、一時文字列を渡すと戻り値が即座にダングリングする。
template <class S>
    requires(!std::is_lvalue_reference_v<S> &&
             (std::same_as<std::remove_cv_t<S>, std::string> || std::same_as<std::remove_cv_t<S>, std::pmr::string>))
ParseResult ParseMarkdown(S&& markdown_text, std::stop_token stop_token = {}) = delete;

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。
// view が指す領域は (a) static なリテラル または (b) 呼び出し側が渡した buffer のいずれか。
// buffer のスコープ内でのみ valid。UTF-8 では U+10FFFF までを最大 4 byte で表現できる。
[[nodiscard]] std::optional<std::string_view> ResolveHtmlEntity(std::string_view entity, char (&buffer)[4]);
