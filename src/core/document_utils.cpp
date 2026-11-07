#include "document_utils.h"
#include "document.h"
#include "ascii_util.h"
#include "file_io.h"
#include "profiler.h"
#include "string_convert.h"
#include "utf8_codec.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <ranges>

namespace {

// ダブルクリック選択時の文字種カテゴリ。同じカテゴリが連続する code point を 1 単語として扱う。
enum class CharCategory : uint8_t {
    // 英数字・_ とラテン文字・ギリシャ文字・キリル文字・ハングル。空白で語を区切る文字体系なので、
    // UAX#29 と同じく混ざっても 1 語にする (「API를」「café」)。
    Word,
    Hiragana,
    Katakana,
    Han,
    FullwidthAlnum,
    Other,
};

constexpr CharCategory CategorizeCodePoint(uint32_t cp) noexcept
{
    if (cp < 0x80) {
        return ascii_util::IsAsciiWordChar(static_cast<char>(cp)) ? CharCategory::Word : CharCategory::Other;
    }
    // ラテン文字・IPA・結合文字・ギリシャ文字・キリル文字・ラテン拡張追加 (ベトナム語)。
    // 結合文字を含めるのは NFD (e + U+0301) の語を切らないため。× ÷ とギリシャ文字の ; · は記号なので除く。
    if (((cp >= 0xC0 && cp <= 0x2AF) || (cp >= 0x300 && cp <= 0x52F) || (cp >= 0x1E00 && cp <= 0x1EFF)) &&
        cp != 0xD7 && cp != 0xF7 && cp != 0x37E && cp != 0x387) {
        return CharCategory::Word;
    }
    // 漢字繰り返し記号 (々〆〇) はひらがな/カタカナ範囲より先に判定。
    if (cp == 0x3005 || cp == 0x3006 || cp == 0x3007) {
        return CharCategory::Han;
    }
    if (cp >= 0x3041 && cp <= 0x309F) {
        return CharCategory::Hiragana;
    }
    if ((cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF65 && cp <= 0xFF9F)) {
        return CharCategory::Katakana;
    }
    if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2FFFF)) {
        return CharCategory::Han;
    }
    // ハングルの音節・字母・互換字母・半角字母。
    if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3131 && cp <= 0x318E) ||
        (cp >= 0xA960 && cp <= 0xA97C) || (cp >= 0xD7B0 && cp <= 0xD7FB) || (cp >= 0xFFA0 && cp <= 0xFFDC)) {
        return CharCategory::Word;
    }
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) {
        return CharCategory::FullwidthAlnum;
    }
    return CharCategory::Other;
}

template <typename SV>
WordBoundary FindWordBoundariesImpl(SV text, uint32_t pos) noexcept
{
    WordBoundary result;
    if (text.empty()) {
        return result;
    }
    if (pos >= text.size()) {
        pos = static_cast<uint32_t>(text.size()) - 1;
    }
    pos = utf8_codec::SnapToCpStart(text, pos);

    const auto here = utf8_codec::DecodeAt(text, pos);
    const auto cat = CategorizeCodePoint(here.cp);
    if (cat == CharCategory::Other) {
        return result;
    }

    uint32_t start = pos;
    while (start > 0) {
        const auto prev = utf8_codec::DecodePrev(text, start);
        if (CategorizeCodePoint(prev.cp) != cat) {
            break;
        }
        start -= prev.len;
    }

    uint32_t end = pos + here.len;
    while (end < text.size()) {
        const auto next = utf8_codec::DecodeAt(text, end);
        if (CategorizeCodePoint(next.cp) != cat) {
            break;
        }
        end += next.len;
    }

    result.start = start;
    result.end = end;
    return result;
}

} // namespace

WordBoundary FindWordBoundaries(std::string_view text, uint32_t pos) noexcept
{
    return FindWordBoundariesImpl(text, pos);
}

WordBoundary FindWordBoundaries(std::wstring_view text, uint32_t pos) noexcept
{
    return FindWordBoundariesImpl(text, pos);
}

namespace {
// runs は start 昇順で重ならない。
const TextRun* RunContaining(std::span<const TextRun> runs, uint32_t pos) noexcept
{
    // pos を含みうるのは start <= pos の最後の run だけ。
    const auto it = std::ranges::upper_bound(runs, pos, {}, &TextRun::start);
    if (it == runs.begin()) {
        return nullptr;
    }
    const TextRun& run = *std::prev(it);
    return pos < run.start + run.length ? &run : nullptr;
}
} // namespace

NodeRunHit FindRunAt(const Node& node, uint32_t pos) noexcept
{
    const auto* tbl = node.table_data();
    if (!tbl) {
        return { RunContaining(node.runs(), pos), 0 };
    }
    const size_t idx = tbl->CellIndexAt(pos);
    if (idx >= static_cast<size_t>(tbl->row_count) * tbl->col_count) {
        return {};
    }
    const uint32_t base = tbl->cell_text_starts[idx];
    // 区切り文字の位置は前のセルに当たるので、セルの終端で外す。
    if (pos - base >= tbl->CellLength(idx)) {
        return {};
    }
    return { RunContaining(tbl->CellRuns(idx), pos - base), base };
}

NodeWordRange FindNodeWordBoundaries(std::span<const Node> nodes, size_t node, uint32_t pos) noexcept
{
    // インライン数式と画像は 1 つの塊として描くので、どこを指しても範囲全体を選ぶ。
    if (const auto [run, base] = FindRunAt(nodes[node], pos); run && (run->math() || run->image())) {
        const uint32_t start = base + run->start;
        return { node, start, node, start + run->length };
    }
    const std::string_view text = nodes[node].GetText();
    const auto wb = FindWordBoundaries(text, pos);
    if (!wb.found()) {
        return {};
    }
    const auto category = CategorizeCodePoint(utf8_codec::DecodeAt(text, wb.start).cp);
    NodeWordRange r{ node, wb.start, node, wb.end };
    // 空白も句読点も無い長い語 (base64 など) は 2 つ以上の継ぎ目をまたぐ。
    while (r.start == 0 && r.start_node > 0 && JoinsPrevInline(nodes[r.start_node])) {
        const std::string_view prev = nodes[r.start_node - 1].GetText();
        if (prev.empty() || CategorizeCodePoint(utf8_codec::DecodePrev(prev, static_cast<uint32_t>(prev.size())).cp) != category) {
            break;
        }
        --r.start_node;
        r.start = FindWordBoundaries(prev, static_cast<uint32_t>(prev.size()) - 1).start;
    }
    while (r.end == nodes[r.end_node].GetText().size() && JoinsNextInline(nodes, r.end_node)) {
        const std::string_view next = nodes[r.end_node + 1].GetText();
        if (next.empty() || CategorizeCodePoint(utf8_codec::DecodeAt(next, 0).cp) != category) {
            break;
        }
        ++r.end_node;
        r.end = FindWordBoundaries(next, 0).end;
    }
    return r;
}

namespace {

// run の並びが同じ 2 つのノードで、画像の src も同じか。src は run の image_slot で引く。
bool SameImageSrcs(const Node& a, const Node& b, std::span<const TextRun> runs) noexcept
{
    return std::ranges::all_of(runs, [&](const TextRun& r) { return !r.image() || a.image_src(r) == b.image_src(r); });
}

bool NodeContentEquals(const Node& a, const Node& b) noexcept
{
    // 続きのコードの色分けは前のノードから続くトークン (code_lead_*) でも決まる。
    if (a.type != b.type || a.alert_type != b.alert_type || a.quote_depth != b.quote_depth ||
        a.quote_outer_indent != b.quote_outer_indent || a.indent_level != b.indent_level || a.line_count != b.line_count ||
        a.heading_level != b.heading_level || a.code_language != b.code_language || a.flags_ != b.flags_ ||
        a.list_number() != b.list_number() ||
        a.code_lead_length() != b.code_lead_length() || a.code_lead_type() != b.code_lead_type()) {
        return false;
    }
    const auto* at = a.table_data();
    // 表のテキストは concat_text そのものなので、NodeTableData の比較に任せて 2 回比べない。
    // 参照定義が後ろで変わると、前のノードはテキストが同じでもリンク先が変わる。
    if ((!at && a.GetText() != b.GetText()) || !std::ranges::equal(a.runs(), b.runs()) ||
        !std::ranges::equal(a.view_link_urls(), b.view_link_urls())) {
        return false;
    }
    if (!at && !SameImageSrcs(a, b, a.runs())) {
        return false;
    }
    return !at || (*at == *b.table_data() && SameImageSrcs(a, b, at->all_runs));
}

} // namespace

UnchangedEnds FindUnchangedEnds(std::span<const Node> old_nodes, std::span<const Node> new_nodes) noexcept
{
    UnchangedEnds ends{ .old_count = old_nodes.size(), .new_count = new_nodes.size() };
    const size_t common = std::min(ends.old_count, ends.new_count);
    while (ends.head < common && NodeContentEquals(old_nodes[ends.head], new_nodes[ends.head])) {
        ++ends.head;
    }
    while (ends.head + ends.tail < common &&
           NodeContentEquals(old_nodes[ends.old_count - 1 - ends.tail], new_nodes[ends.new_count - 1 - ends.tail])) {
        ++ends.tail;
    }
    return ends;
}

std::vector<std::pair<size_t, size_t>> PairChangedDiagrams(std::span<const Node> old_nodes, std::span<const Node> new_nodes, const UnchangedEnds& ends)
{
    const auto diagrams_in = [](std::span<const Node> nodes, size_t begin, size_t end) static {
        std::vector<size_t> indices;
        std::ranges::copy_if(std::views::iota(begin, end), std::back_inserter(indices), [nodes](size_t i) { return nodes[i].type == NodeType::Diagram; });
        return indices;
    };
    const auto old_diagrams = diagrams_in(old_nodes, ends.head, ends.OldTailBegin());
    const auto new_diagrams = diagrams_in(new_nodes, ends.head, ends.NewTailBegin());
    if (old_diagrams.size() != new_diagrams.size()) {
        return {};
    }
    return std::views::zip(old_diagrams, new_diagrams) | std::ranges::to<std::vector<std::pair<size_t, size_t>>>();
}

int MapDiagramAcrossReload(std::span<const Node> old_nodes, std::span<const Node> new_nodes, const UnchangedEnds& ends, size_t old_index)
{
    if (old_index < ends.head) {
        return static_cast<int>(old_index);
    }
    if (old_index >= ends.OldTailBegin()) {
        return static_cast<int>(old_index - ends.OldTailBegin() + ends.NewTailBegin());
    }
    const auto pairs = PairChangedDiagrams(old_nodes, new_nodes, ends);
    const auto it = std::ranges::find(pairs, old_index, &std::pair<size_t, size_t>::first);
    return it != pairs.end() ? static_cast<int>(it->second) : -1;
}

int FindNodeBySourceOffset(const std::vector<Node>& nodes, size_t diff_offset) noexcept
{
    // 本文の位置を持たないノードは前のノードの位置を受け継いでいるので、同じ位置の先頭のノードを返す。
    const auto it = std::ranges::upper_bound(nodes, diff_offset, {}, &Node::SourceOffset);
    if (it == nodes.begin()) {
        return -1;
    }
    return static_cast<int>(std::ranges::lower_bound(nodes, std::prev(it)->SourceOffset(), {}, &Node::SourceOffset) - nodes.begin());
}

std::vector<SyntaxToken> TokenizeCodeNode(const Node& node)
{
    const std::string_view text = node.GetText();
    const uint32_t lead = node.code_lead_length();
    if (lead == 0) {
        return Tokenize(text, node.code_language);
    }
    // 前のノードをつないで字句解析し直すと、1 ノードしか遡れず (16KB を超えるコメントや文字列の 3 つ目以降のノードで崩れる)、
    // 解析も 2 倍かかる (syntax.cpp の 16KB ノードで 47 → 97µs)。続くトークンの後ろは、トークンの外から解析を始めたのと同じになる。
    const std::string_view rest = text.substr(lead);
    std::vector<SyntaxToken> tokens;
    tokens.reserve(rest.size() / 32 + 1);
    tokens.push_back({ 0, lead, node.code_lead_type() });
    AppendTokens(rest, node.code_language, tokens);
    for (auto& t : std::span{ tokens }.subspan(1)) {
        t.start += lead;
    }
    return tokens;
}

void AnnotateCodeChunkLeads(std::span<Node> run)
{
    MENDO_PROFILE("AnnotateCodeChunkLeads");
    const size_t n = run.size();
    if (n < 2) {
        return;
    }
    std::vector<size_t> begins(n);
    size_t pos = 0;
    for (size_t j = 0; j < n; ++j) {
        pos += j > 0 ? ChunkSeparator(run[j]).size() : 0;
        begins[j] = pos;
        pos += run[j].GetText().size();
    }
    // 区切ったコードの並びをつないだテキストでの、ノード k の終わり。
    const auto run_end = [&](size_t k) noexcept { return begins[k] + run[k].GetText().size(); };

    // 全トークンを作ると 16MB のコードでパースが 18ms から 65ms になり、worker 4 本に配っても 27ms かかる。
    // TokensCrossing は語を飛ばすので直列で約 20ms (実際の C++ で 107ms が約 40ms) で済み、worker に配っても 1〜2ms しか縮まない。
    // 語を飛ばせない言語は全トークンを作るので、16MB で HTML 45ms、YAML 100ms、Lisp 130ms ほどかかるが、並列にするほど現れない。
    constexpr size_t kWindowNodes = 8;
    std::string seg;
    std::vector<size_t> boundaries;
    // next は続くトークンがまだ決まっていない最初のノード、restart は次に解析を始められる位置
    // (トークンの先頭か、どのトークンにも入らない位置)。
    size_t next = 1;
    size_t restart = 0;
    size_t window = kWindowNodes;
    while (next < n) {
        const size_t last = std::min(n, next + window);
        const size_t end = run_end(last - 1);
        const std::string_view head = run[next - 1].GetText();
        seg.reserve(end - restart);
        seg.assign(head.substr(std::min(restart - begins[next - 1], head.size())));
        for (size_t j = next; j < last; ++j) {
            seg.append(ChunkSeparator(run[j])).append(run[j].GetText());
        }
        boundaries.clear();
        for (size_t k = next; k < last; ++k) {
            boundaries.push_back(begins[k] - restart);
        }
        const auto tokens = TokensCrossing(seg, run.front().code_language, boundaries);

        // 窓の端 (並びの終わりでない) に届いたトークンは窓の先まで続くかもしれないので決めず、その先頭から解析し直す。
        size_t limit = end;
        if (last < n && !tokens.empty() && restart + tokens.back().start + tokens.back().length == end) {
            limit = restart + tokens.back().start;
        }
        size_t t = 0;
        for (; next < last && begins[next] <= limit; ++next) {
            const size_t b = begins[next];
            while (t < tokens.size() && restart + tokens[t].start + tokens[t].length <= b) {
                ++t;
            }
            if (t < tokens.size() && restart + tokens[t].start < b) {
                const size_t token_end = restart + tokens[t].start + tokens[t].length;
                run[next].set_code_lead(static_cast<uint32_t>(std::min(token_end, run_end(next)) - b), tokens[t].type);
            }
        }
        // 1 つのトークンが窓を越えて解析を始める位置が進まないときは、終わりが見つかるまで窓を広げる。
        window = limit == restart ? window * 2 : kWindowNodes;
        restart = limit;
    }
}

namespace {

void ReplaceNulls(std::string& s)
{
    const auto nuls = static_cast<size_t>(std::ranges::count(s, '\0'));
    if (nuls == 0) {
        return;
    }
    // U+FFFD は 3 byte なので、伸ばしてから後ろから詰める。
    constexpr std::string_view kFffd = utf8_codec::kReplacementUtf8;
    size_t src = s.size();
    s.resize(src + nuls * (kFffd.size() - 1));
    char* const data = s.data();
    for (size_t dst = s.size(); src > 0;) {
        const char c = data[--src];
        if (c != '\0') {
            data[--dst] = c;
            continue;
        }
        dst -= kFffd.size();
        std::memcpy(data + dst, kFffd.data(), kFffd.size());
    }
}

void NormalizeNewlines(std::string& s)
{
    const size_t n = s.size();
    char* const data = s.data();
    char* const end = data + n;

    const auto find_cr = [end](char* p) noexcept {
        return static_cast<char*>(std::memchr(p, '\r', static_cast<size_t>(end - p)));
    };
    char* src = find_cr(data);
    if (!src) {
        return;
    }

    // ループ進入時、src は必ず CR を指す。
    char* dst = src;
    while (src < end) {
        *dst++ = '\n';
        ++src;
        if (src < end && *src == '\n') {
            ++src; // CRLF を LF 1 つに縮約
        }
        char* const next_cr = find_cr(src);
        const size_t chunk = static_cast<size_t>((next_cr ? next_cr : end) - src);
        std::memmove(dst, src, chunk);
        src += chunk;
        dst += chunk;
    }

    s.resize(static_cast<size_t>(dst - data));
}

} // namespace

void NormalizeSource(std::string& s)
{
    MENDO_PROFILE("NormalizeSource");
    // CR と NUL は ASCII 1 byte なので UTF-8 multi-byte シーケンスの中間バイトとは衝突しない。
    if (std::string_view{ s }.find_first_of(std::string_view{ "\r\0", 2 }) == std::string_view::npos) {
        return;
    }
    NormalizeNewlines(s);
    ReplaceNulls(s);
}

// 外部で開く・HTML に残してよいリンクのスキームか。file:// などは通さない。
bool IsSafeUrlScheme(std::string_view url) noexcept
{
    return ascii_util::istarts_with(url, "http://") || ascii_util::istarts_with(url, "https://") || ascii_util::istarts_with(url, "mailto:");
}

std::string PercentDecode(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned int byte = 0;
        if (s[i] == '%' && i + 2 < s.size() && std::from_chars(s.data() + i + 1, s.data() + i + 3, byte, 16).ptr == s.data() + i + 3) {
            out += static_cast<char>(byte);
            i += 2;
            continue;
        }
        out += s[i];
    }
    return out;
}

namespace {

// ShellExecuteW は関連付けのコマンド ("%1" を引用符で囲むだけの形式も多い) に URL を差し込むので、
// 引用符を閉じて引数を足せる " と、区切りになる空白・制御文字を %XX にする。URL としての意味は変わらない。
std::string EncodeShellUnsafeChars(std::string_view url)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(url.size());
    for (const char ch : url) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F || c == '"') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
        else {
            out += ch;
        }
    }
    return out;
}

} // namespace

LinkClickResult ClassifyLinkClick(std::string_view url, std::wstring_view doc_path)
{
    LinkClickResult result;
    if (url.empty()) {
        return result;
    }
    if (url[0] == '#') {
        result.type = LinkClickResult::Type::Anchor;
        result.target = PercentDecode(url.substr(1));
        return result;
    }
    if (IsEmbeddedPath(url)) {
        result.type = LinkClickResult::Type::Embedded;
        result.path = string_convert::Utf8ToWide(url);
        return result;
    }
    if (IsSafeUrlScheme(url)) {
        result.type = LinkClickResult::Type::ExternalUrl;
        result.target = EncodeShellUnsafeChars(url);
        return result;
    }
    // 埋め込みの文書には文書のフォルダが無い。res/ の元のファイルからの相対パスで書いたライセンスへのリンクだけを読み替える。
    if (IsEmbeddedPath(doc_path)) {
        if (url == LICENSES_LINK_IN_EMBEDDED) {
            result.type = LinkClickResult::Type::Embedded;
            result.path = LICENSES_PATH;
        }
        return result;
    }
    if (doc_path.empty()) {
        return result;
    }
    // ローカルのファイルは Markdown だけを mendo で開く。ShellExecute に渡すと実行ファイルも起動できてしまう。
    const auto hash = url.find('#');
    auto path = ResolveLocalPath(url.substr(0, hash), path_util::ParentDirectory(doc_path));
    if (path.empty() || !path_util::IsMarkdownFile(path)) {
        return result;
    }
    std::string anchor = hash == std::string_view::npos ? std::string{} : PercentDecode(url.substr(hash + 1));
    // 開き直すとリロードになり履歴も積まれるので、同じ文書なら見出しへ飛ぶだけにする。
    if (path_util::iequal(path, std::filesystem::path(doc_path).lexically_normal().native())) {
        if (!anchor.empty()) {
            result.type = LinkClickResult::Type::Anchor;
            result.target = std::move(anchor);
        }
        return result;
    }
    result.type = LinkClickResult::Type::LocalFile;
    result.target = std::move(anchor);
    result.path = std::move(path);
    return result;
}

std::wstring ResolveLocalPath(std::string_view src, std::wstring_view doc_dir)
{
    if (src.contains("://")) {
        return {};
    }
    // "my%20images/a.png" のようなエンコード済みの記述も多いので、デコードしてから引く。
    const auto decoded = PercentDecode(src);
    // 制御文字は Windows のファイル名に使えない。NUL は Win32 API がそこでパスを切るので、拡張子の確認をすり抜ける。
    if (std::ranges::any_of(decoded, [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
        return {};
    }
    // src は UTF-8。char から直接構築すると ACP 解釈になり非 ASCII パスが壊れる。
    std::filesystem::path local_path(string_convert::Utf8ToWide(decoded));
    // 相対パスは文書と同じ場所を引くだけなので、文書が共有フォルダ上でも通す。
    if (!path_util::IsLocalPath(local_path.native())) {
        return {};
    }
    if (local_path.is_relative()) {
        // \\?\ 付きの文書フォルダのまま結合すると、.. で \\?\UNC\host へ抜けられる。
        auto base = path_util::ToWin32Path(doc_dir);
        if (!base) {
            return {};
        }
        local_path = std::filesystem::path(std::move(*base)) / local_path;
    }
    // canonical() は symlink 解決のためにファイルシステムを叩くので、
    // UI 同期パスから外すため absolute() + lexically_normal() を使う。
    // symlink を跨ぐ参照はレアケースとして許容する。
    std::error_code ec;
    const auto abs_path = std::filesystem::absolute(local_path, ec);
    if (ec) {
        return {};
    }
    auto resolved = abs_path.lexically_normal().wstring();
    // ドライブ名のほかのコロンは、スキーム (javascript: など) か NTFS の代替データストリーム。
    if (resolved.find(L':', 2) != std::wstring::npos) {
        return {};
    }
    return resolved;
}
