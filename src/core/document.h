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

    // move-only: nodes_ は view モード時に raw_wide_.data() を view_base_ に持つため、
    // move 後に InjectViewBase() で再注入する必要がある。
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;
    ~Document() = default;

    // ファクトリ。本体は wide 入力版。utf8 入力版は FileLoader からの UTF-8
    // バイト列（または埋め込みリソース）を直接受け取るための変換ラッパー。
    static Document FromMarkdown(std::pmr::wstring wide, size_t byte_size, std::wstring_view path);
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
    // パース入力の wide テキストへの参照。AnalyzeReloadDiff の比較用。
    // ノードレベルでは検出できない編集 (空行追加・末尾空白・改行種別変更等) も
    // UTF-16 オフセット精度で差分位置を求めるため、Document が常時保持する。
    constexpr const std::pmr::wstring& GetRawText() const noexcept
    {
        return raw_wide_;
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
    void ReplaceFromMarkdown(std::pmr::wstring wide, size_t byte_size);
    void ReplaceFromMarkdown(std::pmr::string utf8);
    int FindAnchorIndex(std::wstring_view anchor) const;
    // 既に anchor_id 形式（小文字 ASCII 正規化済み）と判明している入力向け。
    // 呼び出し側で正規化が保証されていれば、ToLowerAscii の確保を回避できる。
    int FindNormalizedAnchorIndex(std::wstring_view anchor) const;
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

    // raw_wide_.data() を view モードの全ノードに注入する。
    // ReplaceContent / move 経路で呼ぶ。raw_wide_ の relocate (resize/assign) は禁止契約のため、
    // 通常のパース完了後は再注入不要だが、Document 自身の move 後はノードのアドレス参照が更新済み
    // でも raw_wide_ のヒープバッファは move 前後で同一とは限らないため都度呼び直す。
    void InjectViewBase() noexcept;

    std::pmr::vector<Node> nodes_;
    std::pmr::wstring file_path_;
    // 注意: raw_wide_ は relocate 禁止 (assign / resize / += によるヒープ再確保で
    // ノードの view_base_ が dangling になるため)。差し替えは ReplaceFromMarkdown 経由で
    // 全 nodes 再構築 + InjectViewBase() を伴うパスのみ許される。
    std::pmr::wstring raw_wide_;
    size_t loaded_byte_size_ = 0;
    TableOfContents toc_;
    // キーは所有 wstring。nodes_ の再アロケート/構造変更でも索引が dangling しない。
    std::pmr::unordered_map<std::pmr::wstring, int,
                            WStringTransparentHash, std::equal_to<>>
        anchor_index_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> diagram_node_indices_;
};
