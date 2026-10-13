#pragma once
#include "document_types.h"
#include "footnote_anchor.h"
#include "text_arena.h"
#include "toc.h"
#include <concepts>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <memory_resource>

// ノードと、そのテキスト・runs・リンク URL を置いた arena の組。
struct NodeStorage {
    std::pmr::vector<Node> nodes;
    std::unique_ptr<TextArena> arena;
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
// stop_token を渡すと md4c コールバックから途中で abort できる。default-constructed の場合は
// 永久に stop_requested() == false なので、従来どおり最後まで走る。
ParseResult ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {});

// ノードは入力テキストを参照するため、一時文字列を渡すと戻り値が即座にダングリングする。
template <class S>
    requires(!std::is_lvalue_reference_v<S> &&
             (std::same_as<std::remove_cv_t<S>, std::string> || std::same_as<std::remove_cv_t<S>, std::pmr::string>))
ParseResult ParseMarkdown(S&& markdown_text, std::stop_token stop_token = {}) = delete;

// BlockQuoteノードからGitHub Alertsを検出し、マーカー除去・ラベル挿入・グルーピングを行う（テスト用に公開）。
// blockquote_indices は ParseMarkdown が収集した BlockQuote ノードのインデックス。書き換えたテキストと runs は arena に置く。
void DetectAlerts(std::pmr::vector<Node>& nodes, TextArena& arena, std::span<const size_t> blockquote_indices);

// text が Alert マーカー ([!TYPE] と直後の区切り 1 文字) だけからなるか。
bool IsAlertMarkerOnly(std::string_view text);

// nodes[i] が Alert マーカーを持ちうる位置 (最外側 blockquote の先頭段落) か。
bool IsAlertHeadCandidate(const std::pmr::vector<Node>& nodes, size_t i) noexcept;

// AlertTypeに対応するラベル文字列を返す（テスト用に公開）
std::string_view GetAlertLabel(AlertType type) noexcept;

// AlertTypeに対応するアイコン文字列を返す（テスト用に公開）
std::string_view GetAlertIcon(AlertType type) noexcept;

