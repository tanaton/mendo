#pragma once
#include "anchor.h"
#include "document_types.h"
#include "parser.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 原文と、それをパースした結果。ノードのテキストは raw か parsed.arena を指すので、1 つの共有で寿命をそろえる。
// パース後は書き換えない。非同期リロードの worker が UI の差し替えと無関係に読めるよう共有で持つ。
struct DocumentData {
    // LF-only (LoadFile または 2 引数 FromMarkdown が正規化する)。バッファは relocate しないので、
    // 原文をそのまま表示するノードのテキストが指し続けられる。
    std::string raw;
    // ノードと、原文をそのまま表示できないノードのテキスト・全ノードの runs・リンク URL・テーブル本体。
    NodeStorage parsed;
};

// 埋め込みのヘルプを表す仮想パス。ファイルとしては読めない。
inline constexpr std::wstring_view HELP_PATH = L"mendo://help";
inline constexpr bool IsHelpPath(std::wstring_view path) noexcept
{
    return path == HELP_PATH;
}
// 埋め込みのサードパーティのライセンス (THIRD_PARTY_LICENSES.md) を表す仮想パス。配布する zip には exe だけを入れるので、
// ライブラリと KaTeX の書体 (SIL OFL 1.1) のライセンス表記はヘルプのリンクから開けるようにして添える。
inline constexpr std::wstring_view LICENSES_PATH = L"mendo://licenses";
// ファイルとしては読めない、埋め込みの文書の仮想パス。
inline constexpr bool IsEmbeddedPath(std::wstring_view path) noexcept
{
    return path == HELP_PATH || path == LICENSES_PATH;
}
// リンクの URL (UTF-8) 用。仮想パスは ASCII なので変換せずに比べる (非 ASCII のバイトは負の char になり一致しない)。
inline constexpr bool IsEmbeddedPath(std::string_view path) noexcept
{
    return std::ranges::equal(path, HELP_PATH) || std::ranges::equal(path, LICENSES_PATH);
}

class Document {
public:
    // ノードのテキストとテーブルは data_ の中を直接指す。共有で持つため、move しても relocate せず付け替えは不要。
    constexpr Document() noexcept = default;

    // text は LF 正規化済み (LoadFile の出力) であること。
    // 既定の stop_token ではキャンセルしない。
    static Document FromNormalizedMarkdown(
        std::string text, std::wstring_view path, std::stop_token stop_token = {});
    // LF 正規化を内蔵した便利ラッパー (テスト / Help リソース経路、入力は BOM 無し前提)。
    static Document FromMarkdown(std::string utf8, std::wstring_view path);

    const std::vector<Node>& GetNodes() const noexcept
    {
        return Storage().nodes;
    }
    constexpr const std::wstring& GetFilePath() const noexcept
    {
        return file_path_;
    }
    // ディスク上の実ファイルに対応するか。空パスと埋め込みの仮想パス (ヘルプ・ライセンス) は false。
    bool HasBackingFile() const noexcept;
    const std::vector<TocEntry>& GetToc() const noexcept
    {
        return Storage().indices.toc_entries;
    }
    bool IsEmpty() const noexcept
    {
        return GetNodes().empty();
    }
    // パース入力の原文。
    std::string_view GetRawText() const noexcept
    {
        return data_ ? std::string_view{ data_->raw } : std::string_view{};
    }
    // 非同期リロードの worker が旧文書と比べるための共有参照。同一性 (ポインタ一致) で「同じ文書か」を判定できる。
    const std::shared_ptr<const DocumentData>& ShareData() const noexcept
    {
        return data_;
    }
    constexpr const std::wstring& GetDirectory() const noexcept
    {
        return cached_directory_;
    }

    // anchor (ToLowerAnchorCopy の規則で大文字小文字を無視) を付けた見出し、または脚注の定義 / 参照のノード番号。無ければ -1。
    int FindAnchorIndex(std::string_view anchor);
    const std::vector<size_t>& GetImageNodeIndices() const noexcept
    {
        return Storage().indices.image_indices;
    }
    const std::vector<size_t>& GetDiagramNodeIndices() const noexcept
    {
        return Storage().indices.diagram_indices;
    }
    const std::vector<size_t>& GetTableNodeIndices() const noexcept
    {
        return Storage().indices.table_indices;
    }
    const std::vector<size_t>& GetMathNodeIndices() const noexcept
    {
        return Storage().indices.math_indices;
    }

private:
    const NodeStorage& Storage() const noexcept
    {
        static const NodeStorage kEmpty;
        return data_ ? data_->parsed : kEmpty;
    }

    // 設定するのは FromNormalizedMarkdown だけ。
    std::shared_ptr<const DocumentData> data_;
    std::wstring file_path_;
    std::wstring cached_directory_;
    // アンカーはリンクを辿るときにしか使わないので、パースでは作らず FindAnchorIndex で目次の先頭から
    // 見つかるまでだけ索引を延ばす。リンクを辿らない文書では確保しない。
    std::unique_ptr<HeadingAnchorIndex> anchor_index_;
};
