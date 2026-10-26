#include "document_utils.h"
#include "ascii_util.h"
#include "parallel_for.h"
#include "profiler.h"
#include "unicode_lower_table.h"
#include "utf8_codec.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>

std::pmr::string ToLowerAsciiCopy(std::string_view text)
{
    std::pmr::string result;
    result.resize_and_overwrite(text.size(), [&text](char* buf, size_t count) noexcept -> size_t {
        ascii_util::AsciiToLowerOnly(text.data(), buf, count);
        return count;
    });
    return result;
}

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
    result.found = true;
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

NodeWordRange FindNodeWordBoundaries(std::span<const Node> nodes, size_t node, uint32_t pos) noexcept
{
    const std::string_view text = nodes[node].GetText();
    const auto wb = FindWordBoundaries(text, pos);
    if (!wb.found) {
        return {};
    }
    const auto category = CategorizeCodePoint(utf8_codec::DecodeAt(text, wb.start).cp);
    NodeWordRange r{ node, wb.start, node, wb.end, true };
    // 空白も句読点も無い長い語 (base64 など) は 2 つ以上の継ぎ目をまたぐ。
    while (r.start == 0 && r.start_node > 0 && JoinsPrevInline(nodes[r.start_node])) {
        const std::string_view prev = nodes[r.start_node - 1].GetText();
        if (prev.empty() || CategorizeCodePoint(utf8_codec::DecodePrev(prev, static_cast<uint32_t>(prev.size())).cp) != category) {
            break;
        }
        --r.start_node;
        r.start = FindWordBoundaries(prev, static_cast<uint32_t>(prev.size()) - 1).start;
    }
    while (r.end == nodes[r.end_node].GetText().size() && r.end_node + 1 < nodes.size() && JoinsPrevInline(nodes[r.end_node + 1])) {
        const std::string_view next = nodes[r.end_node + 1].GetText();
        if (next.empty() || CategorizeCodePoint(utf8_codec::DecodeAt(next, 0).cp) != category) {
            break;
        }
        ++r.end_node;
        r.end = FindWordBoundaries(next, 0).end;
    }
    return r;
}

std::wstring_view ExtensionView(std::wstring_view path) noexcept
{
    const auto name = FilenameView(path);
    const auto dot_pos = name.rfind(L'.');
    return dot_pos == std::wstring_view::npos ? std::wstring_view{} : name.substr(dot_pos);
}

bool IsMarkdownFile(std::wstring_view path)
{
    static constexpr ascii_util::LowercaseAsciiLiteral kMarkdownExts[]{
        L".md",
        L".markdown",
        L".mkd",
    };
    const auto ext = ExtensionView(path);
    return std::ranges::any_of(kMarkdownExts, [&](const auto& e) noexcept {
        return ascii_util::iequal(ext, e);
    });
}

std::pmr::wstring ParentDirectory(std::wstring_view path)
{
    return std::pmr::wstring{ std::filesystem::path(path).parent_path().native() };
}

namespace {

// GitHub のスラグが使う toLowerCase に合わせる (UTF-8 長が伸びる変換は表から除いてある)。
constexpr uint32_t ToLowerNonAscii(uint32_t cp) noexcept
{
    // 見出しに多い CJK などは大文字を持たないので、表を引かずに返す (生成時に確認済み)。
    if (cp >= 0x3000 && cp < 0xA640) {
        return cp;
    }
    const auto it = std::ranges::lower_bound(kUnicodeLowerMappings, cp, {}, &UnicodeLowerMapping::upper);
    return it != std::ranges::end(kUnicodeLowerMappings) && it->upper == cp ? it->lower : cp;
}

static_assert(std::ranges::is_sorted(kUnicodeLowerMappings, {}, &UnicodeLowerMapping::upper));

// 非 ASCII の 1 コードポイントを小文字にして dst に書き、書いたバイト数を返す (decoded.len 以下)。
size_t WriteLowerNonAscii(utf8_codec::DecodedCp decoded, const char* src, char* dst) noexcept
{
    const uint32_t lower = ToLowerNonAscii(decoded.cp);
    if (lower == decoded.cp) {
        std::copy_n(src, decoded.len, dst);
        return decoded.len;
    }
    return utf8_codec::EncodeCp(lower, dst);
}

// GenerateAnchorIdInto の ASCII 処理を 1 回の表引きで決める。SlugLower は A-Z のみ。
enum AsciiSlugAction : uint8_t {
    SlugSkip,
    SlugKeep,
    SlugHyphen,
    SlugLower,
};

constexpr auto kAsciiSlugTable = [] static noexcept {
    std::array<AsciiSlugAction, 128> t{};
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = static_cast<char>(i);
        if (ascii_util::IsAsciiLower(c) || ascii_util::IsAsciiDigit(c) || c == '-' || c == '_') {
            t[i] = SlugKeep;
        }
        else if (ascii_util::IsAsciiUpper(c)) {
            t[i] = SlugLower;
        }
        else if (c == ' ' || c == '\t') {
            t[i] = SlugHyphen;
        }
        else {
            t[i] = SlugSkip;
        }
    }
    return t;
}();

// アンカー (GitHub 互換スラグ) に採用しない記号・句読点か。非 ASCII コードポイント単位で判定。
// Unicode 全カテゴリテーブルは持たないため、主要な記号ブロックの近似で判定する
// (文字 (Letter) を誤って落とすより、稀な記号を通す方向に倒す)。
constexpr bool IsAnchorSkippableSymbol(uint32_t cp) noexcept
{
    // ラテン1補助の記号 (U+00A0–U+00BF)
    if (cp >= 0x00A0 && cp <= 0x00BF) {
        return true;
    }
    // 一般句読点・通貨・矢印・数学記号等 (U+2000–U+2BFF)、補助句読点 (U+2E00–U+2E7F)
    if ((cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x2E00 && cp <= 0x2E7F)) {
        return true;
    }
    // CJK 記号と句読点 (U+3000–U+303F)
    if (cp >= 0x3000 && cp <= 0x303F) {
        return true;
    }
    // 全角 ASCII 対応の記号関連
    return (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
}

} // namespace

void GenerateAnchorIdInto(std::string_view text, std::pmr::string& slug)
{
    slug.clear();
    // 出力は入力サイズ以下で確定のため一括確保して書き出す。
    slug.resize_and_overwrite(text.size(), [text](char* buf, size_t /*count*/) noexcept -> size_t {
        char* dst = buf;
        const size_t n = text.size();
        size_t pos = 0;
        while (pos < n) {
            const auto cu = static_cast<uint32_t>(static_cast<unsigned char>(text[pos]));
            if (cu < 0x80) {
                // ASCII ファストパス: 1 回の table lookup で分岐を絞る。
                switch (kAsciiSlugTable[cu]) {
                case SlugKeep:
                    *dst++ = static_cast<char>(cu);
                    break;
                case SlugLower:
                    *dst++ = static_cast<char>(cu | 0x20);
                    break;
                case SlugHyphen:
                    *dst++ = '-';
                    break;
                case SlugSkip:
                    break;
                default:
                    std::unreachable();
                }
                ++pos;
                continue;
            }
            const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(pos));
            // U+FFFD は不正/truncated バイト由来。アンカーには採用しない。
            if (decoded.cp != utf8_codec::kReplacement && !IsAnchorSkippableSymbol(decoded.cp)) {
                dst += WriteLowerNonAscii(decoded, text.data() + pos, dst);
            }
            pos += decoded.len;
        }
        return static_cast<size_t>(dst - buf);
    });
}

std::pmr::string ToLowerAnchorCopy(std::string_view text)
{
    std::pmr::string result;
    result.resize_and_overwrite(text.size(), [text](char* buf, size_t /*count*/) noexcept -> size_t {
        char* dst = buf;
        size_t pos = 0;
        while (pos < text.size()) {
            const char c = text[pos];
            if (static_cast<unsigned char>(c) < 0x80) {
                *dst++ = ascii_util::ToLowerAscii(c);
                ++pos;
                continue;
            }
            const auto decoded = utf8_codec::DecodeAt(text, static_cast<uint32_t>(pos));
            // 不正バイトは U+FFFD に置き換えず元のバイトを残す (どの見出しとも一致しないままにする)。
            if (decoded.cp == utf8_codec::kReplacement) {
                *dst++ = c;
                ++pos;
                continue;
            }
            dst += WriteLowerNonAscii(decoded, text.data() + pos, dst);
            pos += decoded.len;
        }
        return static_cast<size_t>(dst - buf);
    });
    return result;
}

std::pmr::wstring BuildTitleString(std::wstring_view path, int zoom_percent)
{
    std::pmr::wstring title{ FilenameView(path) };
    title += title.empty() ? L"mendo" : L" - mendo";
    if (zoom_percent > 0 && zoom_percent != 100) {
        std::format_to(std::back_inserter(title), L" ({}%)", zoom_percent);
    }
    return title;
}

namespace {

bool NodeContentEquals(const Node& a, const Node& b) noexcept
{
    // 続きのコードの色分けは前のノードから続くトークン (code_lead_*) でも決まる。
    if (a.type != b.type || a.alert_type != b.alert_type || a.quote_depth != b.quote_depth ||
        a.quote_outer_indent != b.quote_outer_indent || a.indent_level != b.indent_level || a.line_count != b.line_count ||
        a.heading_level() != b.heading_level() || a.code_language() != b.code_language() || a.flags_ != b.flags_ ||
        a.list_number() != b.list_number() || a.alert_label_length() != b.alert_label_length() ||
        a.code_lead_length() != b.code_lead_length() || a.code_lead_type() != b.code_lead_type()) {
        return false;
    }
    // 参照定義が後ろで変わると、前のノードはテキストが同じでもリンク先が変わる。
    if (a.GetText() != b.GetText() || !std::ranges::equal(a.runs(), b.runs()) || !std::ranges::equal(a.view_link_urls(), b.view_link_urls())) {
        return false;
    }
    const auto* ai = a.image_data();
    const auto* bi = b.image_data();
    if ((ai == nullptr) != (bi == nullptr) || (ai && ai->src != bi->src)) {
        return false;
    }
    const auto* at = a.table_data();
    const auto* bt = b.table_data();
    return (at == nullptr) == (bt == nullptr) && (!at || *at == *bt);
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

size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept
{
    // MSVC STL の mismatch は 1 バイト要素をベクトル化する。
    const size_t pos = static_cast<size_t>(std::ranges::mismatch(old_text, new_text).in1 - old_text.begin());
    if (pos == old_text.size() && pos == new_text.size()) {
        return std::string_view::npos;
    }
    return pos;
}

ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text) noexcept
{
    const size_t diff_pos = FindFirstDifference(old_text, new_text);
    if (diff_pos == std::string_view::npos) {
        return { ReloadOp::NoChange, std::string_view::npos };
    }
    // 新しい方が古い方の prefix (エディタの truncate→rewrite の途中)。
    if (diff_pos == new_text.size()) {
        return { ReloadOp::DeferPrefixShrink, diff_pos };
    }
    return { ReloadOp::Replace, diff_pos };
}

int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, size_t diff_offset) noexcept
{
    // source_offset はパース順で基本的に単調増加するため二分探索を使用。
    // 未設定ノードに当たった場合は左に有効ノードを探してから判定する。
    int lo = 0;
    int hi = static_cast<int>(nodes.size()) - 1;
    int result = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        int probe = mid;
        while (probe >= lo && !nodes[probe].HasSourceOffset()) {
            probe--;
        }
        if (probe < lo) {
            lo = mid + 1;
            continue;
        }
        if (nodes[probe].SourceOffset() <= diff_offset) {
            result = probe;
            lo = mid + 1;
        }
        else {
            hi = probe - 1;
        }
    }
    return result;
}

std::pmr::vector<SyntaxToken> TokenizeCodeNode(const Node& node)
{
    const std::string_view text = node.GetText();
    const auto lead = static_cast<uint32_t>(std::min<size_t>(node.code_lead_length(), text.size()));
    if (lead == 0) {
        return Tokenize(text, node.code_language());
    }
    // 前のノードをつないで字句解析し直すと、1 ノードしか遡れず (16KB を超えるコメントや文字列の 3 つ目以降のノードで崩れる)、
    // 解析も 2 倍かかる (syntax.cpp の 16KB ノードで 47 → 97µs)。続くトークンの後ろは、トークンの外から解析を始めたのと同じになる。
    auto tokens = Tokenize(text.substr(lead), node.code_language());
    for (auto& t : tokens) {
        t.start += lead;
    }
    tokens.insert(tokens.begin(), { 0, lead, node.code_lead_type() });
    return tokens;
}

namespace {

// 区切ったコードの並びをつないだテキストでの、ノード k の終わり。begins は各ノードの始まり。
size_t RunEnd(std::span<const Node> run, std::span<const size_t> begins, size_t k) noexcept
{
    return begins[k] + run[k].GetText().size();
}

// 解析の進み具合。next は続くトークンがまだ決まっていない最初のノード、restart は次に解析を始められる位置
// (トークンの先頭か、どのトークンにも入らない位置)。
struct AnnotateCursor {
    size_t next = 0;
    size_t restart = 0;
};

// 並びの [c.restart, ノード last - 1 の終わり) を 1 つの窓として字句解析し、続くトークンが決まった継ぎ目 (c.next から
// last 未満) に書いて c を進める。窓の端 (並びの終わりでない) に届いたトークンは窓の先まで続くかもしれないので決めず、
// その先頭から解析し直させる。
void AnnotateWindow(std::span<Node> run, std::span<const size_t> begins, AnnotateCursor& c, size_t last, std::pmr::string& seg)
{
    const size_t restart = c.restart;
    const size_t end = RunEnd(run, begins, last - 1);
    size_t j = static_cast<size_t>(std::ranges::upper_bound(begins, restart) - begins.begin()) - 1;
    const std::string_view head = run[j].GetText();
    seg.reserve(end - restart);
    seg.assign(head.substr(std::min(restart - begins[j], head.size())));
    for (++j; j < last; ++j) {
        seg.append(ChunkSeparator(run[j])).append(run[j].GetText());
    }
    const auto tokens = Tokenize(seg, run.front().code_language());
    size_t limit = end;
    if (last < run.size() && !tokens.empty() && restart + tokens.back().start + tokens.back().length == end) {
        limit = restart + tokens.back().start;
    }
    size_t t = 0;
    for (; c.next < last && begins[c.next] <= limit; ++c.next) {
        const size_t b = begins[c.next];
        while (t < tokens.size() && restart + tokens[t].start + tokens[t].length <= b) {
            ++t;
        }
        if (t < tokens.size() && restart + tokens[t].start < b) {
            const size_t token_end = restart + tokens[t].start + tokens[t].length;
            run[c.next].set_code_lead(static_cast<uint32_t>(std::min(token_end, RunEnd(run, begins, c.next)) - b), tokens[t].type);
        }
        else {
            run[c.next].set_code_lead(0, SyntaxTokenType::Plain);
        }
    }
    c.restart = limit;
}

} // namespace

void AnnotateCodeChunkLeads(std::span<Node> run, TaskScheduler* scheduler)
{
    MENDO_PROFILE("AnnotateCodeChunkLeads");
    const size_t n = run.size();
    if (n < 2) {
        return;
    }
    std::pmr::vector<size_t> begins(n);
    size_t pos = 0;
    for (size_t j = 0; j < n; ++j) {
        pos += j > 0 ? ChunkSeparator(run[j]).size() : 0;
        begins[j] = pos;
        pos += run[j].GetText().size();
    }

    // 並びを前から順に解析すると、16MB のコードでパースが 18ms から 80ms になる (区間に分けて worker 4 本に配ると 32ms)。
    // 区間ごとに先頭がトークンの外から始まると仮定して並列に解析しておき、前の区間の解析が区間の終わりをトークンの外で
    // 迎えたら (ほとんどの区間) そのまま使い、外れた所だけ順に解析し直す。区間は並列に配れる数を、窓は解析し直しの短さを優先した大きさ。
    constexpr size_t kSegmentNodes = 8;
    constexpr size_t kWindowNodes = 4;
    const size_t segment_count = (n + kSegmentNodes - 1) / kSegmentNodes;
    std::pmr::vector<AnnotateCursor> spec(segment_count);
    // 区間 s は自分の区間のノード (先頭を除く) にだけ書くので、区間どうしは同期しなくてよい。
    ParallelFor(scheduler, segment_count, 1, [&](size_t begin, size_t end) {
        std::pmr::string seg;
        for (size_t s = begin; s < end; ++s) {
            const size_t first = s * kSegmentNodes;
            spec[s] = { first + 1, begins[first] };
            AnnotateWindow(run, begins, spec[s], std::min(n, first + kSegmentNodes), seg);
        }
    });

    std::pmr::string seg;
    AnnotateCursor c = spec[0];
    size_t window = kWindowNodes;
    while (c.next < n) {
        // 区間の先頭のノードは先読みでも書かないので、作ったときのまま (続くトークン無し) でよい。
        if (c.next % kSegmentNodes == 0 && c.restart == RunEnd(run, begins, c.next - 1)) {
            c = spec[c.next / kSegmentNodes];
            continue;
        }
        // 区間の終わりで窓を閉じ、トークンの外で終われば次の区間から先読みの結果に戻る。
        // 1 つのトークンが窓を越えて解析を始める位置が進まないときは、終わりが見つかるまで窓を広げる。
        const size_t last = std::min(n, (c.next + window + kSegmentNodes - 1) / kSegmentNodes * kSegmentNodes);
        const size_t before = c.restart;
        AnnotateWindow(run, begins, c, last, seg);
        window = last < n && c.restart == before ? window * 2 : kWindowNodes;
    }
}

namespace {

// CR は ASCII 1 byte なので UTF-8 multi-byte シーケンスの中間バイトとは絶対に衝突しない
// (UTF-8 continuation byte は 10xxxxxx で 0x80-0xBF)。memchr(_, _, 0) は規格上 nullptr を返す。
char* FindCr(char* p, size_t len) noexcept
{
    return static_cast<char*>(std::memchr(p, '\r', len));
}

} // namespace

void NormalizeNewlines(std::pmr::string& s)
{
    MENDO_PROFILE("NormalizeNewlines");
    const size_t n = s.size();
    if (n == 0) {
        return;
    }

    char* const data = s.data();
    char* const end = data + n;

    char* first_cr = FindCr(data, n);
    if (!first_cr) {
        MENDO_STATF("NormalizeNewlines: in={} out={} shrunk=0 (fast LF-only)", n, n);
        return;
    }

    // ループ進入時、src は必ず CR を指す (first_cr または直前反復の FindCr 結果)。
    char* dst = first_cr;
    char* src = first_cr;
    do {
        *dst++ = '\n';
        ++src;
        if (src < end && *src == '\n') {
            ++src; // CRLF を LF 1 つに縮約
        }

        char* next_cr = FindCr(src, static_cast<size_t>(end - src));
        const size_t chunk = next_cr ? static_cast<size_t>(next_cr - src) : static_cast<size_t>(end - src);
        if (chunk > 0) {
            std::memmove(dst, src, chunk * sizeof(char));
            src += chunk;
            dst += chunk;
        }
    } while (src < end);

    s.resize(static_cast<size_t>(dst - data));
    MENDO_STATF("NormalizeNewlines: in={} out={} shrunk={}", n, s.size(), n - s.size());
}
