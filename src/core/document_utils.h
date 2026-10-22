#pragma once
#include "document_types.h"
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

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

// 表示に効く中身 (種別・テキスト・runs・リンク先・入れ子の深さ・表や画像など) が同じか。
// ソース位置と引用グループ番号は比べない。リロードで計測結果を使い回せるかの判定に使う。
bool NodeContentEquals(const Node& a, const Node& b) noexcept;

// リロードで新旧のノード列の先頭 head 個と末尾 tail 個が NodeContentEquals で一致した範囲 (両者は重ならない)。
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
