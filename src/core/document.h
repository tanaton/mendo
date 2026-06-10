#pragma once
#include "document_types.h"
#include "raw_text.h"
#include "toc.h"
#include "parser.h"
#include "utility.h"
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <memory_resource>

class Document {
public:
    constexpr Document() noexcept = default;

    // move-only: nodes_ は view モード時に raw_text_.data() を view_.data() に持つため、
    // move 後に RebaseViews() で旧 base から新 base への delta を反映する必要がある。
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;
    ~Document() = default;

    // ファクトリ。本体は (text, byte_size, path) の 3 引数版。
    // 2 引数版は BOM 除去 + byte_size 計算を内蔵した便利ラッパー (テスト / Help リソース経路)。
    // default-constructed の stop_token はキャンセル不可で従来通り動作。
    static Document FromMarkdown(std::pmr::string text, size_t byte_size, std::wstring_view path,
                                 std::stop_token stop_token = {});
    static Document FromMarkdown(std::pmr::string utf8, std::wstring_view path);

    constexpr const std::pmr::vector<Node>& GetNodes() const noexcept
    {
        return nodes_;
    }
    constexpr std::pmr::vector<Node>& GetNodesMut() noexcept
    {
        return nodes_;
    }
    constexpr const std::pmr::wstring& GetFilePath() const noexcept
    {
        return file_path_;
    }
    constexpr const TableOfContents& GetToc() const noexcept
    {
        return toc_;
    }
    constexpr bool IsEmpty() const noexcept
    {
        return nodes_.empty();
    }
    // パース入力の string テキストへの参照。AnalyzeReloadDiff の比較用。
    // ノードレベルでは検出できない編集 (空行追加・末尾空白・改行種別変更等) も
    // UTF-8 byte offset 精度で差分位置を求めるため、Document が常時保持する。
    // 戻り値は RawText 専用ラッパで、append/resize 等の relocate API を遮断している。
    // std::string_view への暗黙変換あり。
    const RawText& GetRawText() const noexcept
    {
        return raw_text_;
    }
    // 元ファイル(UTF-8)バイト数。エディタの中間書き込み検出（IsFileLargerThan）で参照する。
    constexpr size_t GetLoadedByteSize() const noexcept
    {
        return loaded_byte_size_;
    }
    const std::pmr::wstring& GetDirectory() const noexcept { return cached_directory_; }

    void SetFilePath(std::wstring_view path)
    {
        file_path_ = path;
        RebuildCachedDirectory();
    }
    void ReplaceFromMarkdown(std::pmr::string text, size_t byte_size);
    int FindAnchorIndex(std::string_view anchor) const;
    // 既に anchor_id 形式（小文字 ASCII 正規化済み）と判明している入力向け。
    // 呼び出し側で正規化が保証されていれば、ToLowerAscii の確保を回避できる。
    int FindNormalizedAnchorIndex(std::string_view anchor) const;
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept
    {
        return image_node_indices_;
    }
    constexpr const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept
    {
        return diagram_node_indices_;
    }
    constexpr const std::pmr::vector<size_t>& GetTableNodeIndices() const noexcept
    {
        return table_node_indices_;
    }

private:
    // ParseResult を nodes_ に取り込み、TOC / anchor_index_ / image / diagram の各種
    // インデックスを再構築する。
    // 契約: 入力 ParseResult のノード view_ は raw_text_.data() を base にしていること。
    // FromMarkdown / ReplaceFromMarkdown 経由でのみ呼ぶ。Document を後で move する際の
    // RebaseViews が異種 array 間のポインタ減算を起こさないようこの不変条件が必要。
    void ReplaceContent(ParseResult&& result);

    void BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices);

    // move 後にノードの view_ を新しい raw_text_.data() へ rebase する。
    // raw_text_ の relocate (resize/assign) は禁止契約のため、通常のパース完了後は不要だが、
    // Document 自身の move 後は raw_text_ のヒープバッファが移動する可能性があるため呼び直す。
    // old_base は move 前 (raw_text_ を move する直前) に取得しておく必要がある。
    void RebaseViews(const char* old_base) noexcept;

    // ムーブ構築/ムーブ代入で共有する移送処理。other の raw_text_ を move する前に
    // old_base を確保しないと、move 後の other.raw_text_.data() が空文字列を指し
    // rebase が壊れる。共通化により両経路で同じ順序を保証する。
    void MoveFrom(Document&& other) noexcept;

    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    // RawText は append/resize 等の relocate API を提供しないため、view モードノードの
    // view_.data() が指し続けるバッファの安全性が型レベルで担保される。差し替えは
    // ReplaceFromMarkdown / FromMarkdown 経由の Replace() のみ許される。
    RawText raw_text_;
    size_t loaded_byte_size_ = 0;
    TableOfContents toc_;
    // hash 昇順 → node_index 昇順でソート。重複 anchor_id や稀な hash 衝突は
    // lookup 側で equal_range + 文字列比較し、最小 node_index を選ぶ。
    std::pmr::vector<std::pair<std::uint64_t, int>> anchor_index_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> diagram_node_indices_;
    std::pmr::vector<size_t> table_node_indices_;
    std::pmr::wstring cached_directory_;

    void RebuildCachedDirectory();
};
