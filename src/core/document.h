#pragma once
#include "document_types.h"
#include "toc.h"
#include "parser.h"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory_resource>

class Document {
public:
    constexpr Document() noexcept = default;

    // ファクトリ。本体は wide 入力版。utf8 入力版は FileLoader からの UTF-8
    // バイト列（または埋め込みリソース）を直接受け取るための変換ラッパー。
    static Document FromMarkdown(std::pmr::wstring wide, size_t byte_size, std::wstring_view path);
    static Document FromMarkdown(std::pmr::string utf8, std::wstring_view path);

    // アクセサ
    constexpr const std::pmr::vector<Node>& GetNodes() const noexcept { return nodes_; }
    constexpr std::pmr::vector<Node>& GetNodesMut() noexcept { return nodes_; }
    constexpr const std::pmr::wstring& GetFilePath() const noexcept { return file_path_; }
    constexpr const TableOfContents& GetToc() const noexcept { return toc_; }
    constexpr bool IsEmpty() const noexcept { return nodes_.empty(); }
    // パース入力の wide テキストへの参照。AnalyzeReloadDiff の比較用。
    constexpr const std::pmr::wstring& GetRawText() const noexcept { return raw_wide_; }
    // 元ファイル(UTF-8)バイト数。エディタの中間書き込み検出（IsFileLargerThan）で参照する。
    constexpr size_t GetLoadedByteSize() const noexcept { return loaded_byte_size_; }
    std::pmr::wstring GetDirectory() const;

    constexpr void SetFilePath(std::wstring_view path) { file_path_ = path; }
    void ReplaceContent(ParseResult&& result);
    void ReplaceFromMarkdown(std::pmr::wstring wide, size_t byte_size);
    void ReplaceFromMarkdown(std::pmr::string utf8);
    int FindAnchorIndex(std::wstring_view anchor) const;
    // 既に anchor_id 形式（小文字 ASCII 正規化済み）と判明している入力向け。
    // 呼び出し側で正規化が保証されていれば、ToLowerAscii の確保を回避できる。
    int FindNormalizedAnchorIndex(std::wstring_view anchor) const;
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept { return image_node_indices_; }
    constexpr const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept { return diagram_node_indices_; }

private:
    void BuildHeadingIndices(const std::pmr::vector<size_t>& heading_indices);

    // anchor_index_ のキー用の透過ハッシュ。wstring_view / pmr::wstring 双方で
    // 同じハッシュになるよう wstring_view 経由で計算する。
    struct AnchorKeyHash {
        using is_transparent = void;
        size_t operator()(std::wstring_view sv) const noexcept
        {
            return std::hash<std::wstring_view>{}(sv);
        }
        size_t operator()(const std::pmr::wstring& s) const noexcept
        {
            return std::hash<std::wstring_view>{}(s);
        }
    };

    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    std::pmr::wstring raw_wide_;
    size_t loaded_byte_size_ = 0;
    TableOfContents toc_;
    // キーは所有 wstring。nodes_ の再アロケート/構造変更でも索引が dangling しない。
    std::pmr::unordered_map<std::pmr::wstring, int,
        AnchorKeyHash, std::equal_to<>> anchor_index_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> diagram_node_indices_;
};
