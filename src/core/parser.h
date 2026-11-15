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

// 段落・引用・コードブロックのテキストをこの長さ前後で区切り、続きのノードの並びにする。DirectWrite の計測は 1 つのテキスト
// レイアウトが長いほど 1 文字あたりも遅くなる。区切らない 1 ノードを測ると (2026-11-15、1 スレッド)、1MB で 16KB ずつ測った合計の
// 和英混在 5.7 倍 (2.5 秒)・コード 2.3 倍・和文だけ 1.5 倍で、英文だけは長さに比例する (140ms)。混在文はフォントフォールバックで遅く、
// 明示する区間を 1 レイアウト 1024 まで (doc_dwrite_bridge.cpp の kMaxFallbackRanges) にしているので、長いと残りがシステムの
// フォールバックになる。64KB で区切ると、和英混在で同じ量を測る時間が 1.6 倍になる。区切りを入れた頃 (明示の前) は空行の無い 4MB で
// 28 秒かかり、NO_WRAP にしてもレイアウト内を改行で分けても変わらなかった。長さに比例する文でも、1 ノードのままだと最初の画面の前に
// 全体を測ることになり、並列に計測できず、見えている間は解放もできない。
// 改行が目安の kTextChunkWindow 手前を過ぎたらそこで、1 行が長ければ目安の前後 kTextChunkWindow にある空白か和文の句読点で、
// どちらも無ければ目安の長さで区切る。区切ったノードは別のレイアウトなので、行の途中で区切るとそこで改行して見える。
// コードブロックは折り返さないので、1 行がノード 1 つに収まる限りその行の前の改行で区切り、行の途中では区切らない。
inline constexpr size_t kTextChunkTarget = 16 * 1024;
inline constexpr size_t kTextChunkWindow = kTextChunkTarget / 8;

// パース後の全ノード走査を避けるため、パース中に構築する特殊ノードのインデックス。
struct NodeIndices {
    std::vector<TocEntry> toc_entries;
    // 画像 (kImage の run) を持つノード (表はどれかのセル)。読み込む画像を見えている範囲から探すのに使う。
    std::vector<size_t> image_indices;
    std::vector<size_t> diagram_indices;
    std::vector<size_t> table_indices;
    // インライン数式の run を持つノード (表はどれかのセル)。開く・読み直し・ズームのたびに数式の組を作るのに全ノードを
    // 回らないため (100MB の文書で 5.8ms)。
    std::vector<size_t> math_indices;
    // 脚注番号順の定義ノード。脚注は文書末尾に参照順で並ぶ。
    std::vector<int32_t> footnote_def_indices;
    std::vector<FootnoteRef> footnote_refs;
};

// パーサーの出力。ノードと、ノードが指すもの (テキスト・runs・リンク URL を置いた arena とテーブル) と特殊ノードのインデックス。
// ノードは arena を指すので、nodes だけを取り出すときは arena ごと持ち続けること。
struct NodeStorage {
    ReservedArray<Node> nodes;
    // 原文をそのまま使えないテキストと、runs・リンク URL の置き場所。
    TextArena arena;
    // Table ノードが指すテーブル本体。
    std::vector<std::unique_ptr<NodeTableData>> tables;
    NodeIndices indices;
};

// stop_token を渡すと md4c コールバックから途中で abort できる。既定の stop_token では最後まで走る。
// 確保に失敗したら (md4c の中でも) std::bad_alloc を投げる。
NodeStorage ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {});

// ノードは入力テキストを参照するため、一時文字列を渡すと戻り値が即座にダングリングする。
template <class S>
    requires(!std::is_lvalue_reference_v<S> &&
             std::same_as<std::remove_cv_t<S>, std::string>)
NodeStorage ParseMarkdown(S&& markdown_text, std::stop_token stop_token = {}) = delete;

// HTML エンティティ (例: "&amp;", "&#x1F600;") を解決する。解決できなければ entity をそのまま返す。
// view が指す領域は (a) static なリテラル、(b) 呼び出し側が渡した buffer、(c) entity のいずれか。
// buffer と entity のスコープ内でのみ valid。UTF-8 では U+10FFFF までを最大 4 byte で表現できる。
[[nodiscard]] std::string_view ResolveHtmlEntity(std::string_view entity, char (&buffer)[4]);
