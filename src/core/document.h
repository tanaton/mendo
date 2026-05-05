#pragma once
#include "document_types.h"
#include "toc.h"
#include "parser.h"
#include "utility.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory_resource>

class Document {
public:
    constexpr Document() noexcept = default;

    // move-only: nodes_ は view モード時に raw_text_.data() を view_base_ に持つため、
    // move 後に InjectViewBase() で再注入する必要がある。
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;
    ~Document() = default;

    // ファクトリ。本体は (text, byte_size, path) の 3 引数版。
    // 2 引数版は BOM 除去 + byte_size 計算を内蔵した便利ラッパー (テスト / Help リソース経路)。
    static Document FromMarkdown(mendo::doc_string text, size_t byte_size, std::wstring_view path);
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
    // パース入力の doc_string テキストへの参照。AnalyzeReloadDiff の比較用。
    // ノードレベルでは検出できない編集 (空行追加・末尾空白・改行種別変更等) も
    // UTF-8 byte offset 精度で差分位置を求めるため、Document が常時保持する。
    constexpr const mendo::doc_string& GetRawText() const noexcept
    {
        return raw_text_;
    }
    // 元ファイル(UTF-8)バイト数。エディタの中間書き込み検出（IsFileLargerThan）で参照する。
    constexpr size_t GetLoadedByteSize() const noexcept
    {
        return loaded_byte_size_;
    }
    std::pmr::wstring GetDirectory() const;

    constexpr void SetFilePath(std::wstring_view path)
    {
        file_path_ = path;
    }
    void ReplaceContent(ParseResult&& result);
    void ReplaceFromMarkdown(mendo::doc_string text, size_t byte_size);
    void ReplaceFromMarkdown(std::pmr::string utf8);
    int FindAnchorIndex(mendo::doc_string_view anchor) const;
    // 既に anchor_id 形式（小文字 ASCII 正規化済み）と判明している入力向け。
    // 呼び出し側で正規化が保証されていれば、ToLowerAscii の確保を回避できる。
    int FindNormalizedAnchorIndex(mendo::doc_string_view anchor) const;
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept
    {
        return image_node_indices_;
    }
    constexpr const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept
    {
        return diagram_node_indices_;
    }

private:
    void BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices);

    // raw_text_.data() を view モードの全ノードに注入する。
    // ReplaceContent / move 経路で呼ぶ。raw_text_ の relocate (resize/assign) は禁止契約のため、
    // 通常のパース完了後は再注入不要だが、Document 自身の move 後はノードのアドレス参照が更新済み
    // でも raw_text_ のヒープバッファは move 前後で同一とは限らないため都度呼び直す。
    void InjectViewBase() noexcept;

    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    // 注意: raw_text_ は relocate 禁止 (assign / resize / += によるヒープ再確保で
    // ノードの view_base_ が dangling になるため)。差し替えは ReplaceFromMarkdown 経由で
    // 全 nodes 再構築 + InjectViewBase() を伴うパスのみ許される。
    mendo::doc_string raw_text_;
    size_t loaded_byte_size_ = 0;
    TableOfContents toc_;
    // キーは所有 doc_string。nodes_ の再アロケート/構造変更でも索引が dangling しない。
    std::pmr::unordered_map<mendo::doc_string, int,
                            mendo::DocStringTransparentHash, std::equal_to<>>
        anchor_index_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> diagram_node_indices_;
};
