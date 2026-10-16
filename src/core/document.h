#pragma once
#include "anchor.h"
#include "document_types.h"
#include "text_arena.h"
#include "toc.h"
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
    std::shared_ptr<const std::pmr::string> Share() const noexcept
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
    // FromMarkdown だけが使う。生成後の Document の中身を差し替える経路は持たない。
    void SetFilePath(std::wstring_view path)
    {
        file_path_ = path;
        RebuildCachedDirectory();
    }
    // text は LF 正規化済みであること。
    void ReplaceFromMarkdown(std::pmr::string text, size_t byte_size, std::stop_token stop_token);

    // 契約: 入力 ParseResult は raw_text_ をパースした結果であること。
    void ReplaceContent(ParseResult&& result);

    void RebuildCachedDirectory();

    std::pmr::vector<Node> nodes_;
    // 原文をそのまま表示できないノードのテキストと、全ノードの runs・リンク URL の置き場所。
    std::unique_ptr<TextArena> text_arena_;
    std::pmr::wstring file_path_;
    // RawText は append/resize 等の relocate API を提供しないため、ノードのテキストが
    // 指し続けるバッファの安全性が型レベルで担保される。設定するのは
    // FromMarkdown (ReplaceFromMarkdown) だけ。
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
