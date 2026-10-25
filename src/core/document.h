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

// Document の raw_text_ 専用の薄いラッパ。
// 書き換え API として `Replace(std::pmr::string)` のみを公開し、
// `append/resize/operator+=` のような relocate を発生させ得る関数を型レベルで遮断する。
//
// 不変条件: 一度 Replace されたヒープバッファは Document の生存期間中、再 Replace か
// move-assign 以外で relocate しない。これによって、原文をそのまま表示するノードのテキストが
// 指し続けるバッファの安全性が型レベルで担保される。
// 本体は不変の共有文字列で持ち、非同期リロードの worker が Document の差し替えと無関係に
// 旧テキストを読めるようにする (Share)。move でバッファが relocate しない利点もある。
class RawText {
public:
    RawText() = default;

    RawText(const RawText&) = delete;
    RawText& operator=(const RawText&) = delete;
    RawText(RawText&&) noexcept = default;
    RawText& operator=(RawText&&) noexcept = default;

    size_t size() const noexcept
    {
        return text_ ? text_->size() : 0;
    }
    bool empty() const noexcept
    {
        return size() == 0;
    }
    operator std::string_view() const noexcept
    {
        return view();
    }

    // 比較は std::string_view ベース (gtest の EXPECT_EQ などで利用)。
    friend bool operator==(const RawText& a, std::string_view b) noexcept
    {
        return a.view() == b;
    }

    // 完全置換のみを許す書き換え API。append/resize は意図的に提供しない。
    void Replace(std::pmr::string text)
    {
        text_ = std::make_shared<const std::pmr::string>(std::move(text));
    }

    // 同一性 (ポインタ一致) で「同じテキストか」を判定できる共有参照。
    const std::shared_ptr<const std::pmr::string>& Share() const noexcept
    {
        return text_;
    }

private:
    std::string_view view() const noexcept
    {
        return text_ ? std::string_view{ *text_ } : std::string_view{ "" };
    }

    std::shared_ptr<const std::pmr::string> text_;
};

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

    // ファクトリ。本体は (text, byte_size, path) の 3 引数版で、text は LF 正規化済み (FileLoader 出力) であること。
    // 2 引数版は byte_size 計算と LF 正規化を内蔵した便利ラッパー (テスト / Help リソース経路、入力は BOM 無し前提)。
    // 既定の stop_token ではキャンセルしない。
    static Document FromMarkdown(
        std::pmr::string text, size_t byte_size, std::wstring_view path,
        std::stop_token stop_token = {});
    static Document FromMarkdown(std::pmr::string utf8, std::wstring_view path);

    const std::pmr::vector<Node>& GetNodes() const noexcept
    {
        static const std::pmr::vector<Node> kEmpty;
        return storage_ ? storage_->nodes : kEmpty;
    }
    // 非同期リロードの worker が旧ノードと比べるための弱参照。ノードが指す原文は GetRawText().Share() で別に生かす。
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
    constexpr const std::pmr::vector<TocEntry>& GetToc() const noexcept
    {
        return indices_.toc_entries;
    }
    bool IsEmpty() const noexcept
    {
        return GetNodes().empty();
    }
    // パース入力の原文。AnalyzeReloadDiff の比較用。ノードレベルでは検出できない編集
    // (空行追加・末尾空白・改行種別変更等) も UTF-8 byte offset 精度で差分位置を求めるため、常時保持する。
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

    // anchor (大文字小文字は ASCII のみ無視) を付けた見出し、または脚注の定義 / 参照のノード番号。無ければ -1。
    int FindAnchorIndex(std::string_view anchor);
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept
    {
        return indices_.image_indices;
    }
    constexpr const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept
    {
        return indices_.diagram_indices;
    }
    constexpr const std::pmr::vector<size_t>& GetTableNodeIndices() const noexcept
    {
        return indices_.table_indices;
    }

private:
    // ノードと、原文をそのまま表示できないノードのテキスト・全ノードの runs・リンク URL・テーブル本体。
    // パース後は書き換えない。非同期リロードの worker が UI の差し替えと無関係に読めるよう共有で持つ。
    std::shared_ptr<const NodeStorage> storage_;
    std::pmr::wstring file_path_;
    // 設定するのは FromMarkdown だけ。常に LF-only (FileLoader または 2 引数 FromMarkdown が正規化する)。
    RawText raw_text_;
    size_t loaded_byte_size_ = 0;
    NodeIndices indices_;
    std::pmr::wstring cached_directory_;
    // アンカーはリンクを辿るときにしか使わないので、パースでは作らず FindAnchorIndex で目次の先頭から
    // 見つかるまでだけ索引を延ばす。リンクを辿らない文書では確保しない。
    std::unique_ptr<HeadingAnchorIndex> anchor_index_;
};
