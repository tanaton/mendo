#pragma once
#include "document_types.h"
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// カテゴリ: ASCII単語(英数+_) / ひらがな / カタカナ / 漢字 / 全角英数 / その他。
// 「その他」(空白・記号など) は {0, 0, false}。形態素解析は行わない (UAX#29 簡易版)。
// pos がマルチバイト/サロゲートの途中を指した場合は code point 先頭へスナップする。
WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept;
WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept;

std::pmr::string ToLowerAsciiCopy(std::string_view text);

// GitHub スタイル: 小文字化、空白→'-'、CJK 保持、句読点スキップ。結果を slug に書く。
void GenerateAnchorIdInto(std::string_view text, std::pmr::string& slug);

// リンク先のアンカーを GenerateAnchorIdInto と同じ規則で小文字にする。
std::pmr::string ToLowerAnchorCopy(std::string_view text);

// リロードで新旧のノード列の先頭 head 個と末尾 tail 個が、表示に効く中身で一致した範囲 (両者は重ならない)。
// ソース位置と引用グループ番号は比べない。
// 編集は 1 か所に固まることが多いので、先頭と末尾から一致する範囲だけを見る。
struct UnchangedEnds {
    size_t head = 0;
    size_t tail = 0;
    size_t old_count = 0;
    size_t new_count = 0;

    constexpr size_t OldTailBegin() const noexcept
    {
        return old_count - tail;
    }
    constexpr size_t NewTailBegin() const noexcept
    {
        return new_count - tail;
    }
};
UnchangedEnds FindUnchangedEnds(std::span<const Node> old_nodes, std::span<const Node> new_nodes) noexcept;

// コードブロックのトークン。区切ったコードの続きは、前のノードからの続きとしてトークン化する
// (ブロックコメントや複数行の文字列の途中から始まっても色が崩れないように)。node は文書のノード配列の要素であること。
std::pmr::vector<SyntaxToken> TokenizeCodeNode(const Node& node);

// 最後のパス区切り以降。区切りで終わるパスでは空。
constexpr std::wstring_view FilenameView(std::wstring_view path) noexcept
{
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring_view::npos ? path : path.substr(pos + 1);
}

// 最後のパス区切り以降の "." を含む拡張子。無ければ空。
std::wstring_view ExtensionView(std::wstring_view path) noexcept;

// 拡張子の大文字小文字は区別しない。
bool IsMarkdownFile(std::wstring_view path);

inline constexpr std::wstring_view HELP_PATH = L"mendo://help";
inline constexpr bool IsHelpPath(std::wstring_view path) noexcept
{
    return path == HELP_PATH;
}

// 空パスや区切りを含まないパスでは空を返す。
std::pmr::wstring ParentDirectory(std::wstring_view path);

// zoom_percent: 0 または 100 はデフォルト（省略）、それ以外は "(125%)" 等として表示。
std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent = 0);

// 以下は同一ファイルのリロード判定。旧/新の UTF-8 テキストを比べてリロード方針を決め、差分位置 (UTF-8 byte オフセット) を
// ノードへ写像する。OnParseComplete / DoReloadCurrentFile の分岐を揃えるために 1 か所に集める。

// 新旧コンテンツの最初の差分 UTF-8 byte 位置を返す。同一の場合は npos。
// 前提: 両テキストとも LF 正規化済みであること。old は raw_text_、new は FileLoader 由来で
// どちらも正規化済み (issue #273)。
size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept;

// 同一ファイルのリロード時に取るべき操作を示す。
enum class ReloadOp : uint8_t {
    NoChange,          // 差分なし。リロード不要。
    DeferPrefixShrink, // prefix-only shrink。truncate→rewrite の前半として defer。
    DeferPartialWrite, // 読み終えた後もファイルが伸びている (書き込み途中)。差分判定もパースもせず defer。
    Replace,           // 再パースした文書に差し替え、差分位置へスクロールする。
};

struct ReloadDecision {
    ReloadOp op;
    size_t diff_pos; // NoChange のとき std::string_view::npos、それ以外は差分開始位置。
};

// 旧/新コンテンツの UTF-8 テキストを比較し、リロード方針を決定する純粋関数。
ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text) noexcept;

// still_growing: 読み終えた後もファイルが伸びている。書き込み途中なので差分を取らずに defer する。
inline ReloadDecision DecideReload(bool still_growing, std::string_view old_text, std::string_view new_text) noexcept
{
    if (still_growing) {
        return { ReloadOp::DeferPartialWrite, std::string_view::npos };
    }
    return AnalyzeReloadDiff(old_text, new_text);
}

// 末尾を削っただけの保存は何度読み直しても prefix shrink になるため、defer 回数に上限を設け、
// 超えたら縮んだ内容を確定版として受け入れる。
class PrefixShrinkRetry {
public:
    static constexpr uint8_t kMaxDefers = 10;

    // true なら defer を続ける (回数を 1 つ消費する)。
    bool TryDefer() noexcept
    {
        if (Exhausted()) {
            return false;
        }
        ++count_;
        return true;
    }
    bool Exhausted() const noexcept
    {
        return count_ >= kMaxDefers;
    }
    void Reset() noexcept
    {
        count_ = 0;
    }

private:
    uint8_t count_ = 0;
};

// source_offset が diff_offset 以下の最後のノードを返す。該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, size_t diff_offset) noexcept;
