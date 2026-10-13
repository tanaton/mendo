#pragma once
#include "document_types.h"
#include "footnote_anchor.h"
#include "heading_anchor_index.h"
#include "raw_text.h"
#include "text_arena.h"
#include "toc.h"
#include <memory>
#include <memory_resource>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct ParseResult;

class Document {
public:
    constexpr Document() noexcept = default;

    // move-only: nodes_ のテキストは raw_text_ のバッファか text_arena_ を直接指す。RawText は共有文字列を、
    // text_arena_ は unique_ptr で持つため、move してもバッファは relocate せず付け替えは不要。
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept = default;
    Document& operator=(Document&& other) noexcept = default;
    ~Document() = default;

    // ファクトリ。本体は (text, byte_size, path) の 3 引数版で、text は LF 正規化済み (FileLoader 出力) であること。
    // 2 引数版は byte_size 計算と LF 正規化を内蔵した便利ラッパー (テスト / Help リソース経路、入力は BOM 無し前提)。
    // default-constructed の stop_token はキャンセル不可で従来通り動作。
    static Document FromMarkdown(
        std::pmr::string text, size_t byte_size, std::wstring_view path,
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
    // ディスク上の実ファイルに対応するか。空パスとヘルプの仮想パスは false。
    bool HasBackingFile() const noexcept;
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
    constexpr const std::pmr::wstring& GetDirectory() const noexcept
    {
        return cached_directory_;
    }

    void SetFilePath(std::wstring_view path)
    {
        file_path_ = path;
        RebuildCachedDirectory();
    }
    // text は LF 正規化済みであること。
    void ReplaceFromMarkdown(std::pmr::string text, size_t byte_size, std::stop_token stop_token = {});
    // anchor (大文字小文字は ASCII のみ無視) を付けた見出し、または脚注の定義 / 参照のノード番号。無ければ -1。
    int FindAnchorIndex(std::string_view anchor);
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
    // 契約: 入力 ParseResult は raw_text_ をパースした結果であること。
    void ReplaceContent(ParseResult&& result);

    void RebuildCachedDirectory();

    std::pmr::vector<Node> nodes_;
    // 原文をそのまま表示できないノードのテキストと、全ノードの runs・リンク URL の置き場所。
    std::unique_ptr<TextArena> text_arena_;
    std::pmr::wstring file_path_;
    // RawText は append/resize 等の relocate API を提供しないため、ノードのテキストが
    // 指し続けるバッファの安全性が型レベルで担保される。差し替えは
    // ReplaceFromMarkdown / FromMarkdown 経由の Replace() のみ許される。
    // 常に LF-only (FileLoader または 2 引数 FromMarkdown が正規化する)。
    RawText raw_text_;
    size_t loaded_byte_size_ = 0;
    TableOfContents toc_;
    std::pmr::vector<size_t> image_node_indices_;
    std::pmr::vector<size_t> diagram_node_indices_;
    std::pmr::vector<size_t> table_node_indices_;
    std::pmr::vector<int32_t> footnote_def_indices_;
    std::pmr::vector<FootnoteRef> footnote_refs_;
    std::pmr::wstring cached_directory_;
    // アンカーはリンクを辿るときにしか使わないので、パースでは作らず FindAnchorIndex で目次の先頭から
    // 見つかるまでだけ索引を延ばす。リンクを辿らない文書では確保しない。
    std::unique_ptr<HeadingAnchorIndex> anchor_index_;
};
