#pragma once
#include "document_types.h"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory_resource>

class LayoutCache;

// 選択範囲に基づいてノードから選択テキストを抽出する。
// ノード間を \r\n で連結したテキストを返す。
[[nodiscard]] std::pmr::wstring ExtractSelectedText(const std::pmr::vector<Node>& nodes, const TextSelection& selection);

// 選択範囲を HTML フラグメントに変換する。
// 見出し/段落/リスト/引用/コードブロック/テーブル/インライン強調/リンクを HTML タグへ変換する。
// dark_mode 時はコードブロック背景とシンタックスハイライトの色、テーブル border 色が
// ダークテーマ相当（VS Code Dark+ 風）に切り替わる。貼付け先の背景色に合わせてユーザーが
// テーマを切り替えて使う想定。
// 戻り値は CF_HTML のフラグメント部（<!--StartFragment--> と <!--EndFragment--> の間に入る本文）。
[[nodiscard]] std::pmr::wstring ExtractSelectedTextAsHtml(
    const std::pmr::vector<Node>& nodes,
    const TextSelection& selection,
    bool dark_mode = false);

// ノードのラン内の指定テキスト位置にあるリンクURLを検索する。
// リンクラン内の位置であればリンクURLを返し、そうでなければnulloptを返す。
[[nodiscard]] std::optional<std::pmr::wstring> FindLinkAtPosition(const Node& node, uint32_t text_pos);

// 指定されたアンカーIDに一致する見出しノードのインデックスを検索する。
// アンカーは大文字小文字を区別せず（小文字化して）比較される。
// 見つからない場合は-1を返す。
int FindAnchorNodeIndex(const std::pmr::vector<Node>& nodes, std::wstring_view anchor);

// ダブルクリックによる単語選択の単語境界結果。
struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// テキスト内の指定位置周辺の単語境界を検索する。
// 「単語文字」は英数字またはアンダースコア。
// 位置が単語文字上にあれば {start, end, true} を返し、そうでなければ {0, 0, false} を返す。
WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept;

// ASCII範囲の大文字を小文字に変換する。
[[nodiscard]] std::pmr::wstring ToLowerAscii(std::wstring_view text);

// ファイルパスまたはファイル名がMarkdownファイル（.md, .markdown, .mkd）かどうかを判定する。
// 拡張子の大文字小文字は区別しない。
[[nodiscard]] bool IsMarkdownFile(std::wstring_view path);

// ヘルプ用仮想パス
inline constexpr std::wstring_view HELP_PATH = L"mendo://help";
inline bool IsHelpPath(std::wstring_view path) noexcept { return path == HELP_PATH; }

// 新旧コンテンツの最初の差分バイトオフセットを返す。同一の場合は npos。
[[nodiscard]] size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept;

// source_offset が diff_offset 以下の最後のノードを返す。該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, uint32_t diff_offset) noexcept;

// diff_pos が短い方の末尾と一致するかを判定する。
// 片方がもう片方の prefix であり、ファイルの伸縮（エディタの中間書き込み状態）を示す。
[[nodiscard]] inline bool IsPrefixOnlyDiff(size_t diff_pos, size_t old_size, size_t new_size) noexcept
{
    return diff_pos == std::min(old_size, new_size);
}

// 同一ファイルのリロード時に取るべき操作を示す。
enum class ReloadOp : uint8_t {
    NoChange,           // 差分なし。リロード不要。
    DeferPrefixShrink,  // prefix-only shrink。truncate→rewrite の前半として defer。
    PrefixGrowth,       // prefix-only growth。レイアウトキャッシュの prefix を保存して伸張。
    FullReload,         // 全体差分リロード。
};

struct ReloadDecision {
    ReloadOp op;
    size_t diff_pos;    // NoChange のとき std::string_view::npos、それ以外は差分開始位置。
};

// 旧/新コンテンツの UTF-8 バイト列を比較し、リロード方針を決定する純粋関数。
// FindFirstDifference + IsPrefixOnlyDiff + shrink 判定を 1 か所に集約し、
// OnParseComplete / DoReloadCurrentFile の同一ファイル再読み込み時の分岐を統一する。
[[nodiscard]] ReloadDecision AnalyzeReloadDiff(std::string_view old_utf8, std::string_view new_utf8) noexcept;

// diff 位置のノードに基づくスクロールY座標を計算する。
// ノードが見つからない場合は fallback_scroll を返す。
[[nodiscard]] float CalcScrollYForDiff(
    const std::pmr::vector<Node>& nodes,
    const LayoutCache& cache,
    std::string_view content,
    size_t diff_pos,
    float viewport_height,
    float fallback_scroll) noexcept;

// フルファイルパスからファイル名部分を抽出する。
// 例: "C:\\dir\\file.md" -> "file.md"
[[nodiscard]] std::pmr::wstring ExtractFilename(std::wstring_view path);

// ファイルパスからタイトル文字列を構築する。
// 例: "C:\\dir\\file.md" -> "file.md - mendo"
// パスが空の場合は "mendo" を返す。
// zoom_percent: 0 または 100 はデフォルト（省略）、それ以外は "(125%)" 等として表示される。
[[nodiscard]] std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent = 0);
