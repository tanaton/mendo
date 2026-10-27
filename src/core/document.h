#pragma once
#include "anchor.h"
#include "document_types.h"
#include "parser.h"
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Document {
public:
    constexpr Document() noexcept = default;

    // move-only: ノードのテキストは raw_text_ のバッファか storage_ の arena を、テーブルは storage_ の要素を直接指す。
    // どちらも共有で持つため、move しても relocate せず付け替えは不要。
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept = default;
    Document& operator=(Document&& other) noexcept = default;
    ~Document() = default;

    // text は LF 正規化済み (LoadFile の出力) であること。
    // 既定の stop_token ではキャンセルしない。scheduler はパースの一部を並列にする先 (ParseMarkdown)。
    static Document FromNormalizedMarkdown(
        std::pmr::string text, std::wstring_view path, std::stop_token stop_token = {}, TaskScheduler* scheduler = nullptr);
    // LF 正規化を内蔵した便利ラッパー (テスト / Help リソース経路、入力は BOM 無し前提)。
    static Document FromMarkdown(std::pmr::string utf8, std::wstring_view path);

    const std::pmr::vector<Node>& GetNodes() const noexcept
    {
        return Storage().nodes;
    }
    // 非同期リロードの worker が旧ノードと比べるための弱参照。ノードが指す原文は ShareRawText() で別に生かす。
    std::weak_ptr<const NodeStorage> ShareNodes() const noexcept
    {
        return storage_;
    }
    constexpr const std::pmr::wstring& GetFilePath() const noexcept
    {
        return file_path_;
    }
    // ディスク上の実ファイルに対応するか。空パスとヘルプの仮想パスは false。
    bool HasBackingFile() const noexcept;
    const std::pmr::vector<TocEntry>& GetToc() const noexcept
    {
        return Storage().indices.toc_entries;
    }
    bool IsEmpty() const noexcept
    {
        return GetNodes().empty();
    }
    // パース入力の原文。AnalyzeReloadDiff の比較用。ノードレベルでは検出できない編集
    // (空行追加・末尾空白・改行種別変更等) も UTF-8 byte offset 精度で差分位置を求めるため、常時保持する。
    std::string_view GetRawText() const noexcept
    {
        return raw_text_ ? std::string_view{ *raw_text_ } : std::string_view{};
    }
    // 同一性 (ポインタ一致) で「同じテキストか」を判定できる共有参照。
    const std::shared_ptr<const std::pmr::string>& ShareRawText() const noexcept
    {
        return raw_text_;
    }
    constexpr const std::pmr::wstring& GetDirectory() const noexcept
    {
        return cached_directory_;
    }

    // anchor (大文字小文字は ASCII のみ無視) を付けた見出し、または脚注の定義 / 参照のノード番号。無ければ -1。
    int FindAnchorIndex(std::string_view anchor);
    const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept
    {
        return Storage().indices.image_indices;
    }
    const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept
    {
        return Storage().indices.diagram_indices;
    }
    const std::pmr::vector<size_t>& GetTableNodeIndices() const noexcept
    {
        return Storage().indices.table_indices;
    }

private:
    const NodeStorage& Storage() const noexcept
    {
        static const NodeStorage kEmpty;
        return storage_ ? *storage_ : kEmpty;
    }

    // ノードと、原文をそのまま表示できないノードのテキスト・全ノードの runs・リンク URL・テーブル本体。
    // パース後は書き換えない。非同期リロードの worker が UI の差し替えと無関係に読めるよう共有で持つ。
    std::shared_ptr<const NodeStorage> storage_;
    std::pmr::wstring file_path_;
    // 設定するのは FromMarkdown だけ。常に LF-only (LoadFile または 2 引数 FromMarkdown が正規化する)。
    // 共有で持つのは、非同期リロードの worker が Document の差し替えと無関係に旧テキストを読めるようにするため。
    // バッファは relocate しないので、原文をそのまま表示するノードのテキストが指し続けられる。
    std::shared_ptr<const std::pmr::string> raw_text_;
    std::pmr::wstring cached_directory_;
    // アンカーはリンクを辿るときにしか使わないので、パースでは作らず FindAnchorIndex で目次の先頭から
    // 見つかるまでだけ索引を延ばす。リンクを辿らない文書では確保しない。
    std::unique_ptr<HeadingAnchorIndex> anchor_index_;
};
