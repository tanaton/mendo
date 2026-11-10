#pragma once
#include "document_types.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;

    constexpr bool found() const noexcept
    {
        return end > start;
    }
};

// カテゴリ: 語 (英数+_・ラテン文字・ギリシャ文字・キリル文字・ハングル) / ひらがな / カタカナ / 漢字 / 全角英数 / その他。
// 「その他」(空白・記号など) は {0, 0}。形態素解析は行わない (UAX#29 簡易版)。
// pos がマルチバイト/サロゲートの途中を指した場合は code point 先頭へスナップする。
WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept;
WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept;

// nodes[node] の pos を含む語。パーサが改行を挟まずに区切った継ぎ目で切れた語は、隣のノードへ伸ばす。
struct NodeWordRange {
    size_t start_node = 0;
    uint32_t start = 0;
    size_t end_node = 0;
    uint32_t end = 0;

    constexpr bool found() const noexcept
    {
        return start_node < end_node || start < end;
    }
};
NodeWordRange FindNodeWordBoundaries(std::span<const Node> nodes, size_t node, uint32_t pos) noexcept;

// 数式の run の前後にある区切り ($ か $$) の長さ。数式は区切りの内側 (TeX) だけを描いてコピーする。
// 数式でない run と、区切りがそろっていない run (長い本文を区切って片側の $$ が無いもの) は 0 で、原文のまま扱う。
constexpr uint32_t MathDelimiterLength(std::string_view text, const TextRun& run) noexcept
{
    if (!run.math()) {
        return 0;
    }
    const std::string_view delim = run.double_dollar_math() ? "$$" : "$";
    const std::string_view whole = text.substr(run.start, run.length);
    const bool balanced = whole.size() > delim.size() * 2 && whole.starts_with(delim) && whole.ends_with(delim);
    return balanced ? static_cast<uint32_t>(delim.size()) : 0;
}

// $$..$$ だけの段落と ```math (パーサが $$ で囲む)。段落いっぱいの数式を 1 つだけ持つ。
// 長い $$..$$ を区切った途中のノードも run が段落いっぱいになるが、区切りがそろわないので数式ではない。
constexpr bool IsBlockMath(const Node& node) noexcept
{
    const auto runs = node.runs();
    const std::string_view text = node.GetText();
    return runs.size() == 1 && runs[0].double_dollar_math() && runs[0].start == 0 && runs[0].length == text.size() &&
           MathDelimiterLength(text, runs[0]) > 0;
}

// node の text_pos にあるリンク・画像。表は linearized 形式 (concat_text) の offset で、セルの区切り文字には当たらない。
struct LinkHit {
    std::string_view url; // node 内の URL を指す
    uint32_t index = 0;   // node.view_link_urls() の添字
};
[[nodiscard]] std::optional<LinkHit> FindLinkHitAtPosition(const Node& node, uint32_t text_pos);

// ツールチップに出す画像の alt と src。
struct ImageHit {
    std::string_view src;
    // 空なら alt の無い画像。
    std::string_view alt;
    // 画像の範囲の node のテキストでの位置。同じノードの画像を見分ける。
    uint32_t start = 0;
};
[[nodiscard]] std::optional<ImageHit> FindImageHitAtPosition(const Node& node, uint32_t text_pos);

// 見出しの文字から画像 (alt) を除いたもの。GitHub はアンカーも目次も見出しの文字 (textContent) から作り、alt は属性なので入らない。
// 画像が無ければ GetText() をそのまま返し、あれば scratch に組んで返す。アンカーは除いた跡の空白もそのまま使う (GitHub と同じ)。
std::string_view TextWithoutImages(const Node& heading, std::string& scratch);
// 目次に出す見出しの文字。画像を除き、HTML の表示と同じく除いた跡で並んだ空白を 1 つにし、前後の空白を落とす。
std::string_view TocHeadingText(const Node& heading, std::string& scratch);

// パースの前の原文の正規化 (in-place)。CRLF / 旧式 CR を LF に、NUL を U+FFFD にする。
// 改行を LF に揃えておくと、パーサ中の current_text と DocumentData::raw の memcmp 一致判定が成立し、
// 大半の code block が DocumentData::raw をそのまま指せる (arena へのコピーが要らない)。
// 複数行の paragraph は SOFTBR を空白に置き換えるため常に arena へコピーする。
// NUL は CommonMark (§2.3) と GitHub と同じく入力の段で置き換える。md4c は NUL を行内・コードブロック・コードスパン・
// 数式で別々の形で渡す (コードスパンと数式では置き換えない) ので、パーサで受けると経路ごとの継ぎ当てになる。
// CR まで一気にスキップし、その間は memmove でブロックコピーする (MSVC UCRT で SIMD)。
// CR も NUL も無いファイルは find_first_of 1 回で素通し (memchr 1 回と比べて 64MB で +0.6ms、2 回だと +3.7ms)。
void NormalizeSource(std::string& s);

// リロードで新旧のノード列の先頭 head 個と末尾 tail 個が、表示に効く中身で一致した範囲 (両者は重ならない)。
// ソース位置と引用グループ番号は比べない。
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
// 変わった範囲の図の (リロード前, リロード後) の添字の組。図の数が新旧で同じなら出現順に対応させ、数が違うとどれが同じ図か
// 分からないので空。
std::vector<std::pair<size_t, size_t>> PairChangedDiagrams(std::span<const Node> old_nodes, std::span<const Node> new_nodes, const UnchangedEnds& ends);
// リロード前の図 (old_index) がリロード後のどのノードかを返す。無ければ -1。
int MapDiagramAcrossReload(std::span<const Node> old_nodes, std::span<const Node> new_nodes, const UnchangedEnds& ends, size_t old_index);

// コードブロックのトークン。区切ったコードの続きは、前のノードから続くトークン (code_lead_length) を先頭に置き、
// その後ろからトークン化する (ブロックコメントや複数行の文字列の途中から始まっても色が崩れないように)。
std::vector<SyntaxToken> TokenizeCodeNode(const Node& node);

// 区切ったコードブロックの並び (先頭から末尾まで) の続きのノードに、前のノードから続くトークンを記録する。
// 並びをつないだテキストを数ノードずつの窓で字句解析し、窓の端で切れたトークンはその先頭から次の窓で解析し直す。
// 並び全体のトークンを一度に持たない。
void AnnotateCodeChunkLeads(std::span<Node> run);

// diff_offset 以下で最も後ろの source_offset を持つノードのうち、先頭のものを返す (位置を受け継いだノードより前)。
// 該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::vector<Node>& nodes, size_t diff_offset) noexcept;

struct LinkClickResult {
    enum class Type : uint8_t {
        None,
        Anchor,
        ExternalUrl,
        Embedded,  // 埋め込みの文書 (mendo://help・mendo://licenses)
        LocalFile, // 文書からの相対パスなどで指したローカルの Markdown ファイル
    };
    Type type = Type::None;
    // Anchor・LocalFile: 飛ぶ見出し (UTF-8、'#' より後ろをパーセントデコード済み)。NavigateAnchorAction・OpenPathAction に
    // そのまま渡せる。ブラウザのアドレスバーなどからコピーしたリンクはエンコードされている。LocalFile で無ければ空。
    // ExternalUrl: URL (UTF-8)。App::ShellOpenAsync に渡す直前で wstring 化する。
    std::string target;
    // Embedded: 仮想パス。LocalFile: 絶対パス。OpenPathAction で開く。
    std::wstring path;
};

// doc_path はリンクのある文書のパス。相対パスはその文書のフォルダから引く。埋め込みの文書には文書のフォルダが無いので、
// LICENSES_LINK_IN_EMBEDDED だけを読み替える。
LinkClickResult ClassifyLinkClick(std::string_view url, std::wstring_view doc_path);

// 本文のリンクを指している間、本文ペインの下端に出す行き先。
struct LinkDestination {
    std::wstring text;
    // 押すと移る。押しても何も起きないリンク (Markdown 以外のファイル・開かないスキーム) は手の形のカーソルにしない。
    bool navigable = false;
};
// 押したら実際に開くものを出す。ローカルの Markdown は解決した絶対パス、文書の中の見出しは "#見出し"。
// 外部の URL と、押しても何も起きないリンクは書いたままの URL。どれも ToDisplayUrl を通す。
LinkDestination DescribeLinkDestination(std::string_view url, std::wstring_view doc_path);
// 文書に書かれた URL・パスを画面に出す形にする。見えない文字 (双方向の上書きなど) は並びを入れ替えて別の行き先に
// 見せられるので、ブラウザのアドレスバーと同じく %XX (UTF-8) にする。data: の URL などは数百 KB になるので、
// MAX_DISPLAY_URL_LEN を超えれば、パスの末尾と URL の先頭がどちらも見えるよう両端を残して中を切る。
inline constexpr size_t MAX_DISPLAY_URL_LEN = 1024;
std::wstring ToDisplayUrl(std::wstring_view text);
// 文書に書かれたパス (UTF-8、%XX のエンコード可) を絶対パスにする。相対パスは doc_dir から引く。
// 開かないもの (URL・ネットワーク上の絶対パス・使えない文字を含むパス・解決の失敗) は空。画像とリンクで共有する。
std::wstring ResolveLocalPath(std::string_view src, std::wstring_view doc_dir);
// 外部で開く・HTML に残してよいリンクのスキームか。file:// などは通さない。
bool IsSafeUrlScheme(std::string_view url) noexcept;
// md4c は URL をエンコードされたまま渡すので、ファイルパスやアンカーとして引く前に通す。
// 16 進 2 桁が続かない % はそのまま残す。
std::string PercentDecode(std::string_view s);
