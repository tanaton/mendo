#include "syntax.h"
#include "syntax_keywords.h"
#include "ascii_util.h"
#include "utility.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>

namespace kw = syntax_keywords;

namespace {

using ascii_util::IsAsciiAlnum;
using ascii_util::IsAsciiAlpha;
using ascii_util::IsAsciiDigit;
using ascii_util::IsAsciiUpper;

constexpr auto npos = std::string_view::npos;

// ASCII の文字集合。1 文字ごとの判定と連続のスキップを表引きにする。std::string_view の contains や
// find_first_not_of は、ベクトル化した検索関数を呼び出すので 1 文字・短い連続に使うと呼び出しの分だけ遅い
// (16KB のトークン化で C++ 50→45µs、HTML 34→27µs、Emacs Lisp 119→79µs、TOML 52→37µs)。
class CharSet {
public:
    template <size_t N>
    consteval CharSet(const char (&chars)[N]) noexcept : CharSet(std::string_view{ chars, N - 1 })
    {
    }
    consteval CharSet(std::string_view chars) noexcept
    {
        for (const char c : chars) {
            const auto u = static_cast<unsigned char>(c);
            bits_[u >> 6] |= uint64_t{ 1 } << (u & 63);
        }
    }
    template <std::predicate<char> Pred>
    consteval explicit CharSet(Pred pred) noexcept
    {
        for (size_t u = 0; u < 256; ++u) {
            if (pred(static_cast<char>(u))) {
                bits_[u >> 6] |= uint64_t{ 1 } << (u & 63);
            }
        }
    }
    constexpr bool contains(char c) const noexcept
    {
        const auto u = static_cast<unsigned char>(c);
        return ((bits_[u >> 6] >> (u & 63)) & 1) != 0;
    }

private:
    std::array<uint64_t, 4> bits_{};
};

template <class Pred>
constexpr size_t SkipWhile(std::string_view text, size_t pos, size_t end, Pred pred) noexcept
{
    while (pos < end && pred(text[pos])) {
        pos++;
    }
    return pos;
}

template <class Pred>
constexpr size_t SkipWhile(std::string_view text, size_t pos, Pred pred) noexcept
{
    return SkipWhile(text, pos, text.size(), pred);
}

// 識別子先頭文字: ASCII 英字 + '_' に加え、CJK 等の非 ASCII (UTF-8 の 0x80 以上の byte) も許可する。
// char は signed のため unsigned 比較を明示しないと非 ASCII が負値で弾かれる。
constexpr bool IsIdentStart(char c) noexcept
{
    return IsAsciiAlpha(c) || c == '_' || static_cast<std::make_unsigned_t<char>>(c) >= 0x80;
}

inline constexpr CharSet kIdentChars{ [](char c) static noexcept { return IsIdentStart(c) || IsAsciiDigit(c); } };

constexpr bool IsIdentChar(char c) noexcept
{
    return kIdentChars.contains(c);
}

// [start, end) を type のトークンとして出し、end を返す (呼び出し側はそのまま走査位置にする)。
// Plain は描画・HTML 化ともトークン間の隙間と同じ扱いなので出力しない (出すとトークンの大半を占める)。
template <class Tokens>
constexpr size_t EmitRange(Tokens& tokens, size_t start, size_t end, SyntaxTokenType type)
{
    if (end > start && type != SyntaxTokenType::Plain) {
        tokens.emplace_back(static_cast<uint32_t>(start), static_cast<uint32_t>(end - start), type);
    }
    return end;
}

// pos から最初の '\n' まで（または末尾まで）一気に進める。返り値は '\n' の位置（未消費）または text.size()。
constexpr size_t SkipToEol(std::string_view text, size_t pos) noexcept
{
    const auto p = text.find('\n', pos);
    return p == npos ? text.size() : p;
}

// pos から chars に含まれない最初の文字位置を返す（無ければ text.size()）。
constexpr size_t SkipChars(std::string_view text, size_t pos, CharSet chars) noexcept
{
    return SkipWhile(text, pos, [chars](char c) { return chars.contains(c); });
}

// posから始まる文字列リテラルをスキャン（posは開始引用符を指す）。
// 閉じ引用符の次の位置を返す（未終端の場合はテキストの末尾）。
constexpr size_t ScanString(std::string_view text, size_t pos, char quote, bool allow_multiline, bool handle_escape = true) noexcept
{
    size_t i = pos + 1;
    while (i < text.size()) {
        if (handle_escape && text[i] == '\\') {
            i = std::min(i + 2, text.size());
        }
        else if (text[i] == quote) {
            return i + 1;
        }
        else if (!allow_multiline && text[i] == '\n') {
            return i; // 未終端
        }
        else {
            i++;
        }
    }
    return i;
}

// R" に続く区切り文字列の直後の '(' 位置を返す。区切りは最大 16 文字で空白・括弧・'\\' を含まない (規格)。
// 満たさなければ生文字列ではない。上限なしに '(' を探すと R" ごとに文書末尾まで走査しうる。
constexpr size_t FindRawStringParen(std::string_view text, size_t quote_pos) noexcept
{
    constexpr size_t kMaxDelimiterLength = 16;
    const size_t limit = std::min(text.size(), quote_pos + 2 + kMaxDelimiterLength);
    for (size_t k = quote_pos + 1; k < limit; ++k) {
        const char c = text[k];
        if (c == '(') {
            return k;
        }
        if (c == ')' || c == '\\' || static_cast<unsigned char>(c) <= ' ') {
            break;
        }
    }
    return npos;
}

// C++ 生文字列 R"DELIM(...)DELIM" をスキャン。quote_pos は開きの '"'、paren は '(' の位置。
// 終端 )DELIM" を見つけたらその直後位置を返し、未終端なら text.size() を返す。
constexpr size_t ScanCppRawString(std::string_view text, size_t quote_pos, size_t paren) noexcept
{
    const std::string_view delim = text.substr(quote_pos + 1, paren - quote_pos - 1);
    const size_t end_marker_len = 1 + delim.size() + 1; // ')' + delim + '"'
    for (size_t found = text.find(')', paren + 1); found != npos && found + end_marker_len <= text.size(); found = text.find(')', found + 1)) {
        if (text[found + 1 + delim.size()] == '"' && text.substr(found + 1).starts_with(delim)) {
            return found + end_marker_len;
        }
    }
    return text.size();
}

// トリプルクォート文字列をスキャン（pos はトリプルクォートの最初の引用符を指す）。
// escapes=false は Kotlin の raw string や TOML の ''' のように \ をエスケープにしない形。
constexpr size_t ScanTripleQuote(std::string_view text, size_t pos, char quote, bool escapes) noexcept
{
    size_t i = pos + 3;
    const char delims_buf[]{ quote, '\\' };
    const std::string_view delims(delims_buf, escapes ? 2 : 1);
    while (i + 2 < text.size()) {
        const auto p = text.find_first_of(delims, i);
        if (p == npos || p + 2 >= text.size()) {
            return text.size();
        }
        if (text[p] == '\\') {
            i = p + 2;
        }
        else if (text[p + 1] == quote && text[p + 2] == quote) {
            return p + 3;
        }
        else {
            i = p + 1;
        }
    }
    return text.size(); // 未終端
}

// C# 11 の raw string literal。開きの " の数 (3 以上) と同じ数以上の連続で閉じる。
// 本文には開きより短い " の連続を書けるので、""""a"""b"""" の内側の """ では閉じない。
constexpr size_t ScanCountedRawQuote(std::string_view text, size_t pos) noexcept
{
    const size_t open_end = SkipChars(text, pos, "\"");
    const size_t n = open_end - pos;
    for (size_t p = text.find('"', open_end); p != npos; p = text.find('"', p)) {
        const size_t e = SkipChars(text, p, "\"");
        if (e - p >= n) {
            return e;
        }
        p = e;
    }
    return text.size(); // 未終端
}

// posから始まる数値リテラルをスキャン。桁区切りは C++ の ' と、Rust / Java / Python 等の _ を受け付ける。
constexpr size_t ScanNumber(std::string_view text, size_t pos) noexcept
{
    static constexpr CharSet kHexDigits{ "0123456789abcdefABCDEF'_" };
    static constexpr CharSet kBinDigits{ "01'_" };
    static constexpr CharSet kOctDigits{ "01234567_" };
    static constexpr CharSet kDecDigitsSep{ "0123456789'_" };
    static constexpr CharSet kDecDigits{ "0123456789" };
    static constexpr CharSet kIntSuffix{ "uUlL" };
    static constexpr CharSet kNumSuffix{ "fFlLuUndDmM" }; // 'n' は JS BigInt、'd' / 'm' は Java / C# の double / decimal

    size_t i = pos;

    if (i + 1 < text.size() && text[i] == '0') {
        const char next = text[i + 1];
        if (next == 'x' || next == 'X') {
            i = SkipChars(text, i + 2, kHexDigits);
            return SkipChars(text, i, kIntSuffix);
        }
        if (next == 'b' || next == 'B') {
            return SkipChars(text, i + 2, kBinDigits);
        }
        if (next == 'o' || next == 'O') {
            return SkipChars(text, i + 2, kOctDigits);
        }
    }

    i = SkipChars(text, i, kDecDigitsSep);

    if (i < text.size() && text[i] == '.') {
        i = SkipChars(text, i + 1, kDecDigitsSep);
    }

    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        i = SkipChars(text, i, kDecDigits);
    }

    return SkipChars(text, i, kNumSuffix);
}

constexpr bool IsBlank(char c) noexcept
{
    return c == ' ' || c == '\t';
}

constexpr bool IsMarkupSpace(char c) noexcept
{
    return IsBlank(c) || c == '\n';
}

// end の後に空白を挟んで '(' が続くか。たいていは空白が無いので、すぐ下の文字だけを見て終わる。
constexpr bool IsFollowedByParen(std::string_view text, size_t end) noexcept
{
    const size_t i = SkipWhile(text, end, IsBlank);
    return i < text.size() && text[i] == '(';
}

// 行頭の判定を崩さない文字。cmd の @ (エコーの抑止) の後の rem と :: もコメントになる。
template <bool AtSuppressesEcho>
constexpr bool KeepsLineStart(char c) noexcept
{
    return IsBlank(c) || (AtSuppressesEcho && c == '@');
}

// [begin, end) の末尾のスペース・タブを除いた終端。
constexpr size_t TrimEnd(std::string_view text, size_t begin, size_t end) noexcept
{
    while (end > begin && IsBlank(text[end - 1])) {
        end--;
    }
    return end;
}

// 行末の '\' で継続する論理行の終端（'\n' の位置または text.size()）。プリプロセッサ行や Dockerfile / Makefile の行。
constexpr size_t SkipToLogicalEol(std::string_view text, size_t pos) noexcept
{
    while (true) {
        const size_t eol = SkipToEol(text, pos);
        const size_t last = TrimEnd(text, pos, eol);
        if (eol == text.size() || last == pos || text[last - 1] != '\\') {
            return eol;
        }
        pos = eol + 1;
    }
}

// エスケープが引用符の二重化 ("" や '') だけの文字列をスキャン（pos は開始引用符を指す）。
constexpr size_t ScanDoubledQuoteString(std::string_view text, size_t pos, char quote, bool allow_multiline) noexcept
{
    size_t end = ScanString(text, pos, quote, allow_multiline, false);
    while (end < text.size() && text[end] == quote && text[end - 1] == quote) {
        end = ScanString(text, end, quote, allow_multiline, false);
    }
    return end;
}

// Lua の長括弧 [[ / [==[ の開き (pos は最初の '[') なら、対応する ]] / ]==] の直後を返す。開きでなければ npos。
constexpr size_t ScanLuaLongBracket(std::string_view text, size_t pos) noexcept
{
    const size_t level_start = pos + 1;
    const size_t i = SkipChars(text, level_start, "=");
    if (i >= text.size() || text[i] != '[') {
        return npos;
    }
    const size_t level = i - level_start;
    for (size_t k = text.find(']', i + 1); k != npos; k = text.find(']', k + 1)) {
        const size_t close_start = k + 1;
        const size_t j = SkipWhile(text, close_start, std::min(text.size(), close_start + level), [](char c) static noexcept { return c == '='; });
        if (j - close_start == level && j < text.size() && text[j] == ']') {
            return j + 1;
        }
    }
    return text.size();
}

// 行頭の =begin から =end の行末までをスキャン（Ruby）。
constexpr size_t ScanRubyBlockComment(std::string_view text, size_t pos) noexcept
{
    const auto p = text.find("\n=end", pos);
    return p == npos ? text.size() : SkipToEol(text, p + 1);
}

// posから始まるブロックコメントをスキャン（posは開始ペアの最初の文字を指す）。
// 閉じペアの次の位置を返す。未終端の場合はtext.size()を返す。
constexpr size_t ScanBlockComment(std::string_view text, size_t pos, char close1, char close2) noexcept
{
    // close1 はソース中で比較的レアな文字（'*' や '#'）なので、find で間引いてから close2 を確認する。
    size_t i = pos + 2;
    while (true) {
        const auto p = text.find(close1, i);
        if (p == npos || p + 1 >= text.size()) {
            return text.size();
        }
        if (text[p + 1] == close2) {
            return p + 2;
        }
        i = p + 1;
    }
}

enum class TripleQuote : uint8_t {
    None,
    Escaped,    // \ をエスケープにする（Python・Java・Swift）
    Raw,        // \ をエスケープにしない（Kotlin の raw string）
    CountedRaw, // 開きと同じ数以上の " で閉じ、\ もエスケープにしない（C# 11 の """" ... """"）
};

struct LexerConfig {
    bool line_comment_slash = false;              // //
    bool block_comment = false;                   // /* */
    bool hash_comment = false;                    // #
    bool hash_comment_word_start = false;         // # が単語の先頭にあるときだけコメント (Bash の $# や ${#a} を除く)
    bool preprocessor = false;                    // 行頭の#
    TripleQuote triple_quote = TripleQuote::None; // """ '''
    bool raw_string = false;                      // C++ R"(...)"
    bool backtick_string = false;                 // `
    bool angle_block_comment = false;             // <# #>
    bool double_colon_comment = false;            // ::
    bool rem_comment = false;                     // REM
    bool case_insensitive = false;                // 大文字小文字を区別しないキーワードマッチング
    bool skip_single_quote = false;               // 'を文字列デリミタとして扱わない
    bool raw_backtick = false;                    // エスケープなしのバッククォート文字列（Go）
    bool dash_dash_comment = false;               // --
    bool verbatim_at_string = false;              // C# の @"..." と @$"..."（\ はエスケープでなく "" が引用符）
    bool hash_attribute_bracket = false;          // #[ はコメントでなく属性（PHP 8）
    bool dollar_variable = false;                 // $class や $list は変数名なのでキーワードにしない（PHP）
    bool ruby_block_comment = false;              // 行頭の =begin ... =end
    bool backslash_line_string = false;           // \\ から行末までの複数行文字列の 1 行（Zig）
    bool at_builtin = false;                      // @import 等の組み込み関数（Zig）
    bool lua_long_bracket = false;                // [[ ]] / [==[ ]==] の長文字列と --[[ ]] の長コメント（Lua）
};

// Cfg の言語で、識別子と数値以外のトークンを始める文字。rem は語だが、語として飛ばさないように頭文字を入れる。
// TokenizeRange の分岐に開始文字を足したらここにも足す。漏れると kPlainChars でその分岐に届かない。Tokenize と TokensCrossing が
// 同じように壊れて突き合わせでは分からないので、SyntaxSequence.TokenStartCharsReachTheirBranches にも足す。
template <LexerConfig Cfg>
inline constexpr auto kTokenStartStorage = [] {
    constexpr std::pair<bool, char> rules[] = {
        { true,                                                            '"'  },
        { !Cfg.skip_single_quote || Cfg.triple_quote != TripleQuote::None, '\'' },
        { Cfg.line_comment_slash || Cfg.block_comment,                     '/'  },
        { Cfg.hash_comment || Cfg.preprocessor,                            '#'  },
        { Cfg.angle_block_comment,                                         '<'  },
        { Cfg.lua_long_bracket,                                            '['  },
        { Cfg.dash_dash_comment,                                           '-'  },
        { Cfg.double_colon_comment,                                        ':'  },
        { Cfg.verbatim_at_string || Cfg.at_builtin,                        '@'  },
        { Cfg.backslash_line_string,                                       '\\' },
        { Cfg.backtick_string,                                             '`'  },
        { Cfg.ruby_block_comment,                                          '='  },
        { Cfg.rem_comment,                                                 'r'  },
        { Cfg.rem_comment,                                                 'R'  },
    };
    struct {
        std::array<char, std::size(rules)> chars{};
        size_t size = 0;
    } r;
    for (const auto& [enabled, c] : rules) {
        if (enabled) {
            r.chars[r.size++] = c;
        }
    }
    return r;
}();

template <LexerConfig Cfg>
inline constexpr std::string_view kTokenStarts{ kTokenStartStorage<Cfg>.chars.data(), kTokenStartStorage<Cfg>.size };

template <LexerConfig Cfg>
inline constexpr CharSet kTokenStartSet{ kTokenStarts<Cfg> };

// Cfg の言語でどのトークンも始めない文字。IsIdentChar の表引きと合わせて、16KB の C++ のトークン化が 46→39µs になる。
template <LexerConfig Cfg>
inline constexpr CharSet kPlainChars{ [](char c) static noexcept { return !IsIdentChar(c) && c != '.' && !kTokenStartSet<Cfg>.contains(c); } };

// TokensCrossing の出力先。区切りをまたぐか末尾に届くトークンだけを残す。トークンは位置の順に届く。
class CrossingTokens {
public:
    CrossingTokens(size_t text_size, std::span<const size_t> boundaries) noexcept : text_size_(text_size), boundaries_(boundaries)
    {}

    bool Wants(size_t start, size_t end) noexcept
    {
        return end == text_size_ || NextBoundary(start) < end;
    }
    void emplace_back(uint32_t start, uint32_t length, SyntaxTokenType type)
    {
        if (Wants(start, size_t{ start } + length)) {
            tokens_.emplace_back(start, length, type);
        }
    }
    // i から、コメントや文字列が始まりうる文字の手前の空白まで語をまとめて飛ばす。飛ばす区間にコメントも文字列も無いので、
    // 空白はトークンの外にある。区切りをまたぐ語は分類が要るので、区切りの手前の空白までしか飛ばさない。
    // 飛ばせないと分かった所は探し直さない (探し直すと、空白の無い 16MB の JSON の配列のパースが 69ms から 30 秒になる)。
    template <LexerConfig Cfg>
    size_t SkipWords(std::string_view text, size_t i)
    {
        if (i < searched_until_) {
            return i;
        }
        if (token_start_ < i) {
            token_start_ = FindTokenStart<Cfg>(text, i);
        }
        searched_until_ = std::min(token_start_, NextBoundary(i));
        size_t space = searched_until_;
        while (space > i && !IsMarkupSpace(text[space - 1])) {
            --space;
        }
        return space > i ? space - 1 : i;
    }
    std::vector<SyntaxToken> Take() noexcept
    {
        return std::move(tokens_);
    }

private:
    // pos より後ろの最初の区切り。無ければ text の大きさ。
    size_t NextBoundary(size_t pos) noexcept
    {
        while (next_ < boundaries_.size() && boundaries_[next_] <= pos) {
            ++next_;
        }
        return next_ < boundaries_.size() ? boundaries_[next_] : text_size_;
    }
    // すぐ先に見つかることが多いので、CharSet の先頭の注意書きのとおり、ベクトル化した検索を呼ぶ前に数バイト見る。
    template <LexerConfig Cfg>
    static size_t FindTokenStart(std::string_view text, size_t pos) noexcept
    {
        const size_t nearby = std::min(pos + 16, text.size());
        const size_t found = SkipWhile(text, pos, nearby, [](char c) static noexcept { return !kTokenStartSet<Cfg>.contains(c); });
        return found < nearby ? found : std::min(text.find_first_of(kTokenStarts<Cfg>, found), text.size());
    }

    size_t text_size_ = 0;
    std::span<const size_t> boundaries_;
    size_t next_ = 0;
    size_t token_start_ = 0;
    size_t searched_until_ = 0;
    std::vector<SyntaxToken> tokens_;
};

// シェルの単語を区切る文字 (空白とメタ文字) か。
constexpr bool IsShellWordBoundary(char c) noexcept
{
    static constexpr CharSet kBoundaries{ " \t\n;&|()<>" };
    return kBoundaries.contains(c);
}

// LexerConfig を NTTP として渡すことで `if constexpr (Cfg.X)` で死分岐をコンパイル時に消し、
// 各言語の lexer は不要なチェックを含まない最小コードにインスタンス化される。
// [begin, text.size()) を走査して tokens に追記する。埋め込み言語の範囲は呼び出し側が text を範囲末尾で切って渡す。
template <LexerConfig Cfg, class Tokens>
void TokenizeRange(
    std::string_view text,
    size_t begin,
    kw::KeywordTable keywords,
    kw::KeywordTable types,
    Tokens& tokens)
{
    // 行頭判定が必要な言語のみフラグを保持する。それ以外では at_line_start の維持コストを払わない。
    constexpr bool kNeedAtLineStart = Cfg.preprocessor || Cfg.double_colon_comment || Cfg.rem_comment || Cfg.ruby_block_comment;

    size_t i = begin;
    // 行頭判定の状態フラグ。pos i において、現在行の開始から i までが空白のみなら true。
    // 反復の開始時点で位置 i の at-line-start 状態を表す。i を進めた後に更新する。
    [[maybe_unused]] bool at_line_start = true;

    // [start, i) を確定する。スキャン済みトークンは非空白で終わる (改行は未消費) ため行頭状態も解除する。
    const auto emit_from = [&](size_t start, SyntaxTokenType type) {
        EmitRange(tokens, start, i, type);
        at_line_start = false;
    };

    // 現在位置からスキャン済みの終端 token_end までを type のトークンとして確定する。
    const auto emit_until = [&](size_t token_end, SyntaxTokenType type) {
        const size_t start = i;
        i = token_end;
        emit_from(start, type);
    };

    const auto step_plain = [&]([[maybe_unused]] char c) {
        // 行頭判定を読む言語のみフラグを更新する。それ以外は per-char ストアを丸ごと省略。
        if constexpr (kNeedAtLineStart) {
            if (c == '\n') {
                at_line_start = true;
            }
            else if (!KeepsLineStart<Cfg.rem_comment>(c)) {
                at_line_start = false;
            }
        }
        i++;
    };

    constexpr bool kCrossing = std::is_same_v<Tokens, CrossingTokens>;

    while (i < text.size()) {
        if constexpr (kCrossing) {
            if (const size_t skipped = tokens.template SkipWords<Cfg>(text, i); skipped != i) {
                i = skipped;
                if constexpr (kNeedAtLineStart) {
                    size_t line = i;
                    while (line > 0 && KeepsLineStart<Cfg.rem_comment>(text[line - 1])) {
                        --line;
                    }
                    at_line_start = line == 0 || text[line - 1] == '\n';
                }
            }
        }

        const char c = text[i];

        if (kPlainChars<Cfg>.contains(c)) {
            step_plain(c);
            continue;
        }

        // 行コメント: //
        if constexpr (Cfg.line_comment_slash) {
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // アングルブロックコメント: <# #>（PowerShell）
        if constexpr (Cfg.angle_block_comment) {
            if (c == '<' && i + 1 < text.size() && text[i + 1] == '#') {
                emit_until(ScanBlockComment(text, i, '#', '>'), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 長括弧: --[[ ]] コメントと [[ ]] 文字列（Lua）
        if constexpr (Cfg.lua_long_bracket) {
            const bool comment = c == '-' && i + 2 < text.size() && text[i + 1] == '-' && text[i + 2] == '[';
            if (comment || c == '[') {
                if (const size_t e = ScanLuaLongBracket(text, comment ? i + 2 : i); e != npos) {
                    emit_until(e, comment ? SyntaxTokenType::Comment : SyntaxTokenType::String);
                    continue;
                }
            }
        }

        if constexpr (Cfg.dash_dash_comment) {
            if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        if constexpr (Cfg.hash_comment) {
            if (c == '#' && (!Cfg.hash_comment_word_start || i == 0 || IsShellWordBoundary(text[i - 1])) &&
                (!Cfg.hash_attribute_bracket || i + 1 >= text.size() || text[i + 1] != '[')) {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // ブロックコメント: /* */
        if constexpr (Cfg.block_comment) {
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                emit_until(ScanBlockComment(text, i, '*', '/'), SyntaxTokenType::Comment);
                continue;
            }
        }

        // プリプロセッサ: 行頭の#（C/C++）
        if constexpr (Cfg.preprocessor) {
            if (c == '#' && at_line_start) {
                emit_until(SkipToLogicalEol(text, i), SyntaxTokenType::Preprocessor);
                continue;
            }
        }

        // ダブルコロンコメント: 行頭の::（cmd）
        if constexpr (Cfg.double_colon_comment) {
            if (c == ':' && i + 1 < text.size() && text[i + 1] == ':' && at_line_start) {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // REMコメント: 行頭のREM（cmd）
        if constexpr (Cfg.rem_comment) {
            if (at_line_start && ascii_util::istarts_with(text.substr(i), "rem") &&
                (i + 3 >= text.size() || !IsIdentChar(text[i + 3]))) {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // ブロックコメント: 行頭の =begin（Ruby）
        if constexpr (Cfg.ruby_block_comment) {
            if (c == '=' && at_line_start && text.substr(i).starts_with("=begin") &&
                (i + 6 >= text.size() || !IsIdentChar(text[i + 6]))) {
                emit_until(ScanRubyBlockComment(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 逐語的文字列: @"..." / @$"..."（C#）
        if constexpr (Cfg.verbatim_at_string) {
            if (c == '@' && i + 1 < text.size()) {
                const size_t quote = text[i + 1] == '$' ? i + 2 : i + 1;
                if (quote < text.size() && text[quote] == '"') {
                    emit_until(ScanDoubledQuoteString(text, quote, '"', true), SyntaxTokenType::String);
                    continue;
                }
            }
        }

        // 複数行文字列の行: \\ から行末まで（Zig）
        if constexpr (Cfg.backslash_line_string) {
            if (c == '\\' && i + 1 < text.size() && text[i + 1] == '\\') {
                emit_until(SkipToEol(text, i), SyntaxTokenType::String);
                continue;
            }
        }

        // 組み込み関数: @import 等（Zig）
        if constexpr (Cfg.at_builtin) {
            if (c == '@' && i + 1 < text.size() && IsIdentStart(text[i + 1])) {
                emit_until(SkipWhile(text, i + 1, IsIdentChar), SyntaxTokenType::Function);
                continue;
            }
        }

        // トリプルクォート文字列
        if constexpr (Cfg.triple_quote != TripleQuote::None) {
            if ((c == '"' || c == '\'') && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
                const size_t e = Cfg.triple_quote == TripleQuote::CountedRaw && c == '"'
                                     ? ScanCountedRawQuote(text, i)
                                     : ScanTripleQuote(text, i, c, Cfg.triple_quote == TripleQuote::Escaped);
                emit_until(e, SyntaxTokenType::String);
                continue;
            }
        }

        // 文字列リテラル
        if (c == '"' || (c == '\'' && !Cfg.skip_single_quote)) {
            // 生文字列 R"(...)" の R は識別子として後の分岐で消費されるため、ここでは通常の文字列として扱う。
            emit_until(ScanString(text, i, c, false), SyntaxTokenType::String);
            continue;
        }

        // バッククォートテンプレートリテラル（JS）
        if constexpr (Cfg.backtick_string) {
            if (c == '`') {
                emit_until(ScanString(text, i, '`', true, !Cfg.raw_backtick), SyntaxTokenType::String);
                continue;
            }
        }

        // 数値
        if (IsAsciiDigit(c) || (c == '.' && i + 1 < text.size() && IsAsciiDigit(text[i + 1]))) {
            emit_until(ScanNumber(text, i), SyntaxTokenType::Number);
            continue;
        }

        // 識別子とキーワード
        if (IsIdentStart(c)) {
            const size_t start = i;
            i = SkipWhile(text, i, IsIdentChar);

            const std::string_view word(text.data() + start, i - start);

            if constexpr (Cfg.dollar_variable) {
                if (start > 0 && text[start - 1] == '$') {
                    continue;
                }
            }

            if constexpr (Cfg.raw_string) {
                // R"..." の接頭辞は String トークンに含める。word は識別子全体なので、RENDER" の R では生文字列にしない。
                constexpr std::string_view kRawPrefixes[]{ "R", "LR", "uR", "UR", "u8R" };
                if (i < text.size() && text[i] == '"' && std::ranges::contains(kRawPrefixes, word)) {
                    const size_t paren = FindRawStringParen(text, i);
                    if (paren != npos) {
                        i = ScanCppRawString(text, i, paren);
                    }
                    else {
                        i = ScanString(text, i, '"', false);
                    }
                    emit_from(start, SyntaxTokenType::String);
                    continue;
                }
            }

            // 結果を使わない語ではキーワードの表を引かない。
            if constexpr (kCrossing) {
                if (!tokens.Wants(start, i)) {
                    emit_from(start, SyntaxTokenType::Plain);
                    continue;
                }
            }

            const auto in_table = [word](kw::KeywordTable table) {
                return Cfg.case_insensitive ? table.contains_ignore_case(word) : table.contains(word);
            };
            SyntaxTokenType tt = SyntaxTokenType::Plain;
            if (in_table(keywords)) {
                tt = SyntaxTokenType::Keyword;
            }
            else if (in_table(types)) {
                tt = SyntaxTokenType::Type;
            }
            else if (IsFollowedByParen(text, i)) {
                tt = SyntaxTokenType::Function;
            }

            emit_from(start, tt);
            continue;
        }

        // その他: Plain なのでトークンにしない
        step_plain(c);
    }
}

inline constexpr LexerConfig CPP_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .preprocessor = true,
    .raw_string = true,
};
inline constexpr LexerConfig PYTHON_LEXER_CONFIG{
    .hash_comment = true,
    .triple_quote = TripleQuote::Escaped,
};
inline constexpr LexerConfig JS_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .backtick_string = true,
};
inline constexpr LexerConfig GO_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .backtick_string = true,
    .raw_backtick = true,
};
inline constexpr LexerConfig RUST_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .skip_single_quote = true,
};
inline constexpr LexerConfig BASH_LEXER_CONFIG{
    .hash_comment = true,
    .hash_comment_word_start = true,
    .backtick_string = true,
};
inline constexpr LexerConfig PWSH_LEXER_CONFIG{
    .hash_comment = true,
    .angle_block_comment = true,
    .case_insensitive = true,
};
inline constexpr LexerConfig CMD_LEXER_CONFIG{
    .double_colon_comment = true,
    .rem_comment = true,
    .case_insensitive = true,
    .skip_single_quote = true,
};
inline constexpr LexerConfig JSON_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .skip_single_quote = true,
};
inline constexpr LexerConfig JAVA_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .triple_quote = TripleQuote::Escaped,
};
inline constexpr LexerConfig KOTLIN_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .triple_quote = TripleQuote::Raw,
};
inline constexpr LexerConfig CSHARP_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .preprocessor = true,
    .triple_quote = TripleQuote::CountedRaw,
    .verbatim_at_string = true,
};
inline constexpr LexerConfig SWIFT_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .preprocessor = true,
    .triple_quote = TripleQuote::Escaped,
    .skip_single_quote = true,
};
inline constexpr LexerConfig PHP_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .hash_comment = true,
    .case_insensitive = true,
    .hash_attribute_bracket = true,
    .dollar_variable = true,
};
inline constexpr LexerConfig RUBY_LEXER_CONFIG{
    .hash_comment = true,
    .hash_comment_word_start = true, // 正規表現 /\A#+/ 等の # をコメントにしない
    .backtick_string = true,
    .ruby_block_comment = true,
};
inline constexpr LexerConfig ZIG_LEXER_CONFIG{
    .line_comment_slash = true,
    .backslash_line_string = true,
    .at_builtin = true,
};
inline constexpr LexerConfig LUA_LEXER_CONFIG{
    .dash_dash_comment = true,
    .lua_long_bracket = true,
};
inline constexpr LexerConfig SQL_LEXER_CONFIG{
    .block_comment = true,
    .backtick_string = true,
    .case_insensitive = true,
    .raw_backtick = true,
    .dash_dash_comment = true,
};

// ============================================================
// 行や宣言の構造で色が決まる言語の専用 lexer
// ============================================================

// 行や入れ子の状態を持つ lexer は、begin とそこでの状態 (SyntaxLexState) を受け取って途中から解析できる。区切ったコードの
// 続きのノードは前のノードを読まずに解析するので (TokenizeCodeNode)、継ぎ目での状態をパーサが先に求めて持たせる。
// trace はそのときだけ渡り、ふだんの解析 (null) では何も残さない。状態の 0 は、どの lexer でもコードの先頭の状態にする。

// [pos, end) の先頭のスペース・タブを飛ばした位置。
constexpr size_t SkipBlanks(std::string_view text, size_t pos, size_t end) noexcept
{
    return SkipWhile(text, pos, end, IsBlank);
}

void TokenizeShellRange(std::string_view text, size_t begin, size_t end, std::vector<SyntaxToken>& tokens)
{
    TokenizeRange<BASH_LEXER_CONFIG>(text.substr(0, end), begin, kw::BASH_KEYWORDS, kw::BASH_TYPES, tokens);
}

// 行末の '\' でつないだ論理行 [begin, end) の 2 行目から後ろは、行の途中から続ける状態 rest で始め直せる。head は begin での状態。
// '\' で終わったままテキストが尽きた行は続きが次の窓にあるので、最後の行の頭から解析し直させる。
void NoteContinuationLines(SyntaxStateTrace* trace, std::string_view text, size_t begin, size_t end, SyntaxLexState head, SyntaxLexState rest)
{
    if (!trace || end == begin) {
        return;
    }
    if (const size_t first_eol = SkipToEol(text, begin); first_eol < end) {
        trace->SetRange(first_eol + 1, std::min(end + 1, text.size()), rest);
    }
    if (end == text.size()) {
        const size_t newline = text.rfind('\n', end - 1);
        const size_t last_line = newline != npos && newline >= begin ? newline + 1 : begin;
        if (const size_t last = TrimEnd(text, last_line, end); last > last_line && text[last - 1] == '\\') {
            trace->SetUnsettled(last_line, last_line == begin ? head : rest, text.size());
        }
    }
}

// 値全体が数値リテラル（符号・基数接頭辞・小数・指数・'_' 区切り・inf / nan）か。
constexpr bool IsNumberWord(std::string_view w) noexcept
{
    if (!w.empty() && (w[0] == '+' || w[0] == '-')) {
        w.remove_prefix(1);
    }
    if (ascii_util::iequal(w, "inf") || ascii_util::iequal(w, "nan") ||
        ascii_util::iequal(w, ".inf") || ascii_util::iequal(w, ".nan")) {
        return true;
    }
    const bool lead_dot = w.size() >= 2 && w[0] == '.' && IsAsciiDigit(w[1]);
    if (w.empty() || !(IsAsciiDigit(w[0]) || lead_dot)) {
        return false;
    }
    static constexpr CharSet kRadixLetters{ "xXoObB" };
    if (w.size() > 2 && w[0] == '0' && kRadixLetters.contains(w[1])) {
        return SkipChars(w, 2, "0123456789abcdefABCDEF_") == w.size();
    }
    size_t i = SkipChars(w, 0, "0123456789_");
    if (i < w.size() && w[i] == '.') {
        i = SkipChars(w, i + 1, "0123456789_");
    }
    if (i < w.size() && (w[i] == 'e' || w[i] == 'E')) {
        i++;
        if (i < w.size() && (w[i] == '+' || w[i] == '-')) {
            i++;
        }
        const size_t digits_end = SkipChars(w, i, "0123456789");
        if (digits_end == i) {
            return false;
        }
        i = digits_end;
    }
    return i == w.size();
}

// 日付・時刻（1979-05-27 / 07:32:00 / 1979-05-27T07:32:00Z 等）か。
constexpr bool IsDateTimeWord(std::string_view w) noexcept
{
    const auto digits = [&](size_t count) {
        return w.size() >= count && std::ranges::all_of(w.substr(0, count), IsAsciiDigit<char>);
    };
    const bool date = w.size() >= 10 && digits(4) && w[4] == '-';
    const bool time = w.size() >= 5 && digits(2) && w[2] == ':';
    return (date || time) && w.find_first_not_of("0123456789-:.TZtz+ ") == npos;
}

// 設定ファイルの値をスカラー全体で数値・定数と判定する。部分一致では色を付けない (例: 1.2.3、18-alpine)。
SyntaxTokenType ClassifyScalar(std::string_view w) noexcept
{
    if (IsNumberWord(w) || IsDateTimeWord(w)) {
        return SyntaxTokenType::Number;
    }
    static constexpr ascii_util::DocLowercaseLiteral kConstants[]{ "true", "false", "null", "yes", "no", "on", "off", "~" };
    const bool constant = std::ranges::any_of(kConstants, [&](const auto& k) { return ascii_util::iequal(w, k); });
    return constant ? SyntaxTokenType::Keyword : SyntaxTokenType::Plain;
}

// ---------- diff ----------

SyntaxTokenType DiffLineType(std::string_view line) noexcept
{
    static constexpr std::string_view kHeaders[]{
        "diff ",
        "index ",
        "--- ",
        "+++ ",
        "new file mode",
        "deleted file mode",
        "old mode",
        "new mode",
        "similarity index",
        "rename from",
        "rename to",
        "Binary files",
        "Index: ",
        "===",
    };
    if (std::ranges::any_of(kHeaders, [&](std::string_view h) { return line.starts_with(h); })) {
        return SyntaxTokenType::Keyword;
    }
    if (line.starts_with("@@")) {
        return SyntaxTokenType::Type;
    }
    switch (line.empty() ? '\0' : line[0]) {
    case '+':
    case '>':
        return SyntaxTokenType::Inserted;
    case '-':
    case '<':
        return SyntaxTokenType::Deleted;
    case '\\':
        return SyntaxTokenType::Comment; // "\ No newline at end of file"
    default:
        return SyntaxTokenType::Plain;
    }
}

// 状態はハンクの中かどうかと、直前の行が旧ファイルのヘッダかどうか。
// 続きのノードは自分のテキストしか読めないので、ノードの末尾 2 行にある "--- " はヘッダでも先読みできず、削除行の色になる。
void TokenizeDiff(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    constexpr SyntaxLexState kInHunk = 1;
    constexpr SyntaxLexState kAfterOldHeader = 2;
    const auto starts_with_at = [text](size_t pos, std::string_view prefix) {
        return pos < text.size() && text.substr(pos).starts_with(prefix);
    };
    // ハンク内では "-- " を消した行も "--- " で始まるので、次のファイルのヘッダは
    // "--- " "+++ " "@@" の 3 行が並ぶときだけとみなす。
    bool in_hunk = (state & kInHunk) != 0;
    bool after_old_header = (state & kAfterOldHeader) != 0;
    const auto packed = [&]() -> SyntaxLexState { return (in_hunk ? kInHunk : 0) | (after_old_header ? kAfterOldHeader : 0); };
    for (size_t pos = begin; pos < text.size();) {
        if (trace && trace->Wants(pos)) {
            trace->Set(pos, packed());
        }
        const size_t eol = SkipToEol(text, pos);
        const auto line = text.substr(pos, eol - pos);
        auto type = DiffLineType(line);
        const bool old_header_candidate = line.starts_with("--- ");
        if (in_hunk && old_header_candidate) {
            const bool header = starts_with_at(eol + 1, "+++ ") && starts_with_at(SkipToEol(text, eol + 1) + 1, "@@");
            type = header ? SyntaxTokenType::Keyword : SyntaxTokenType::Deleted;
        }
        else if (in_hunk && line.starts_with("+++ ") && !after_old_header) {
            type = SyntaxTokenType::Inserted;
        }
        after_old_header = old_header_candidate && type == SyntaxTokenType::Keyword;
        in_hunk = in_hunk || type == SyntaxTokenType::Type;
        EmitRange(tokens, pos, eol, type);
        pos = eol + 1;
    }
    if (trace) {
        trace->Set(text.size(), packed());
    }
}

// ---------- YAML ----------

constexpr bool IsYamlMappingColon(std::string_view text, size_t pos, size_t eol) noexcept
{
    return pos < eol && text[pos] == ':' && (pos + 1 == eol || IsBlank(text[pos + 1]));
}

// "..." は \ エスケープ、'...' は '' の二重化だけを持つ。
constexpr size_t ScanYamlQuoted(std::string_view text, size_t pos) noexcept
{
    return text[pos] == '"' ? ScanString(text, pos, '"', false) : ScanDoubledQuoteString(text, pos, '\'', false);
}

// 行頭のキーを Type として出し、':' の次の位置を返す。キーでなければ pos のまま。
size_t ScanYamlKey(std::string_view text, size_t pos, size_t eol, std::vector<SyntaxToken>& tokens)
{
    if (pos >= eol) {
        return pos;
    }
    const char c = text[pos];
    if (c == '"' || c == '\'') {
        const size_t key_end = ScanYamlQuoted(text, pos);
        const size_t colon = SkipBlanks(text, key_end, eol);
        if (!IsYamlMappingColon(text, colon, eol)) {
            return pos;
        }
        EmitRange(tokens, pos, key_end, SyntaxTokenType::Type);
        return colon + 1;
    }
    static constexpr CharSet kIndicators{ "[]{},#&*!|>%@`" };
    if (kIndicators.contains(c)) {
        return pos;
    }
    for (size_t k = pos + 1; k < eol; k++) {
        if (IsYamlMappingColon(text, k, eol)) {
            EmitRange(tokens, pos, TrimEnd(text, pos, k), SyntaxTokenType::Type);
            return k + 1;
        }
        if (text[k] == '#' && IsBlank(text[k - 1])) {
            return pos;
        }
    }
    return pos;
}

// キーの後（またはシーケンス要素）の値を行末まで読む。ブロックスカラー（| / >）が始まるなら true。
bool ScanYamlValue(std::string_view text, size_t i, size_t eol, std::vector<SyntaxToken>& tokens)
{
    static constexpr CharSet kFlowIndicators{ ",[]{}" };
    int flow_depth = 0;
    for (bool first = true;; first = false) {
        i = SkipBlanks(text, i, eol);
        if (i >= eol) {
            return false;
        }
        const char c = text[i];
        if (c == '#') {
            EmitRange(tokens, i, eol, SyntaxTokenType::Comment);
            return false;
        }
        if (c == '"' || c == '\'') {
            i = EmitRange(tokens, i, ScanYamlQuoted(text, i), SyntaxTokenType::String);
            continue;
        }
        // アンカー &a・エイリアス *a・タグ !!str
        if (c == '&' || c == '*' || c == '!') {
            const size_t e = SkipWhile(text, i + 1, eol, [&](char ch) { return !IsBlank(ch) && !kFlowIndicators.contains(ch); });
            i = EmitRange(tokens, i, e, SyntaxTokenType::Preprocessor);
            continue;
        }
        if (first && (c == '|' || c == '>')) {
            const size_t e = SkipBlanks(text, std::min(SkipChars(text, i + 1, "+-0123456789"), eol), eol);
            if (e == eol || text[e] == '#') {
                EmitRange(tokens, e, eol, SyntaxTokenType::Comment);
                return true;
            }
        }
        if (c == '[' || c == '{') {
            flow_depth++;
            i++;
            continue;
        }
        if (c == ']' || c == '}') {
            flow_depth = std::max(flow_depth - 1, 0);
            i++;
            continue;
        }
        if (c == ',') {
            i++;
            continue;
        }
        // プレーンスカラー: フロー外では行末かコメントまでが 1 つの値。
        size_t e = i + 1;
        while (e < eol && !(text[e] == '#' && IsBlank(text[e - 1])) &&
               !(flow_depth > 0 && (kFlowIndicators.contains(text[e]) || IsYamlMappingColon(text, e, eol)))) {
            e++;
        }
        const size_t word_end = TrimEnd(text, i, e);
        if (flow_depth > 0 && IsYamlMappingColon(text, e, eol)) {
            EmitRange(tokens, i, word_end, SyntaxTokenType::Type);
            i = e + 1;
            continue;
        }
        EmitRange(tokens, i, word_end, ClassifyScalar(text.substr(i, word_end - i)));
        i = e;
    }
}

// YAML の状態はブロックスカラーの親の桁 + 1 で、本文の外は 0。254 桁より深い親は 254 桁として扱う。
constexpr SyntaxLexState YamlBlockScalarState(size_t parent_indent) noexcept
{
    return static_cast<SyntaxLexState>(std::min<size_t>(parent_indent, 254) + 1);
}

// インデントが parent_indent より深い行（と空行）をブロックスカラーの本文として String にする。返り値は本文直後の行頭。
size_t ScanYamlBlockScalar(std::string_view text, size_t pos, size_t parent_indent, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    while (pos < text.size()) {
        if (trace && trace->Wants(pos)) {
            trace->Set(pos, YamlBlockScalarState(parent_indent));
        }
        const size_t eol = SkipToEol(text, pos);
        const size_t first = SkipBlanks(text, pos, eol);
        const size_t end = TrimEnd(text, first, eol);
        if (first < end) {
            if (first - pos <= parent_indent) {
                return pos;
            }
            EmitRange(tokens, first, end, SyntaxTokenType::String);
        }
        pos = eol + 1;
    }
    return pos;
}

void TokenizeYaml(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    size_t pos = begin;
    // 本文の途中でテキストが尽きたら、末尾の状態も本文の中。
    SyntaxLexState tail = 0;
    const auto scan_block_scalar = [&](size_t parent_indent) {
        pos = ScanYamlBlockScalar(text, pos, parent_indent, tokens, trace);
        tail = pos >= text.size() ? YamlBlockScalarState(parent_indent) : 0;
    };
    if (state != 0) {
        scan_block_scalar(state - 1);
    }
    while (pos < text.size()) {
        if (trace && trace->Wants(pos)) {
            trace->Set(pos, 0);
        }
        tail = 0;
        const size_t eol = SkipToEol(text, pos);
        const auto line = text.substr(pos, eol - pos);
        // 引用符の中の行末 '\' で次の行へ読み進めないよう、行末で切ったテキストを走査する。
        const auto until_eol = text.substr(0, eol);
        const size_t indent_end = SkipBlanks(text, pos, eol);
        // ブロックスカラーの本文はこの桁より深い行。行内で最後に現れた "-" かキーの桁が親になる。
        size_t indent = indent_end - pos;
        size_t i = indent_end;
        if ((line.starts_with("---") || line.starts_with("...")) && (line.size() == 3 || IsBlank(line[3]))) {
            i = EmitRange(tokens, pos, pos + 3, SyntaxTokenType::Keyword);
        }
        else if (line.starts_with('%')) {
            i = EmitRange(tokens, pos, eol, SyntaxTokenType::Preprocessor);
        }
        else {
            while (i < eol && text[i] == '-' && (i + 1 == eol || IsBlank(text[i + 1]))) {
                indent = i - pos;
                i = SkipBlanks(text, i + 1, eol);
            }
            const size_t key_start = i;
            i = ScanYamlKey(until_eol, i, eol, tokens);
            if (i != key_start) {
                indent = key_start - pos;
            }
        }
        const bool block_scalar = ScanYamlValue(until_eol, i, eol, tokens);
        pos = eol + 1;
        if (block_scalar) {
            scan_block_scalar(indent);
        }
    }
    if (trace) {
        trace->Set(text.size(), tail);
    }
}

// ---------- TOML ----------

constexpr bool IsTomlBareKeyChar(char c) noexcept
{
    return ascii_util::IsAsciiWordChar(c) || c == '-';
}

// pos からキー（bare / 引用符付き / ドット区切り）が続いて '=' に達するならその位置。キーでなければ npos。
// '\n' はどの分岐でも受け付けないので、行末を先に求めなくても行内で止まる。
constexpr size_t FindTomlKeyEquals(std::string_view text, size_t pos) noexcept
{
    const size_t end = text.size();
    while (true) {
        pos = SkipBlanks(text, pos, end);
        if (pos >= end) {
            return npos;
        }
        const char c = text[pos];
        if (c == '"' || c == '\'') {
            const size_t start = pos;
            pos = ScanString(text, pos, c, false, c == '"');
            if (pos == start + 1 || text[pos - 1] != c) {
                return npos;
            }
        }
        else if (IsTomlBareKeyChar(c)) {
            pos = SkipWhile(text, pos, end, IsTomlBareKeyChar);
        }
        else {
            return npos;
        }
        pos = SkipBlanks(text, pos, end);
        if (pos < end && text[pos] == '.') {
            pos++;
            continue;
        }
        return pos < end && text[pos] == '=' ? pos : npos;
    }
}

// 括弧の深さごとの 1 ビットのフラグ。64 段より深い分は覚えずに false とする。
struct DepthFlags {
    static constexpr int kMaxDepth = 64;
    uint64_t bits = 0;

    [[nodiscard]] constexpr bool test(int level) const noexcept
    {
        return level > 0 && level <= kMaxDepth && ((bits >> (level - 1)) & 1) != 0;
    }

    constexpr void set(int level, bool on) noexcept
    {
        if (level > 0 && level <= kMaxDepth) {
            const uint64_t bit = uint64_t{ 1 } << (level - 1);
            bits = on ? (bits | bit) : (bits & ~bit);
        }
    }
};

// 括弧の深さと、深さごとのフラグを状態に詰める。持つのは内側の kPackedLevels 段までで、それより外側の段は忘れる
// (抜けた先を深さ 0・フラグなしとして読む)。SyntaxLexState の残りは 3 bit。
constexpr int kPackedLevels = 16;
constexpr int kPackedDepthBits = kPackedLevels + 5;
static_assert(kPackedDepthBits + 3 <= kSyntaxLexStateBits);

constexpr SyntaxLexState PackDepth(int depth, DepthFlags flags) noexcept
{
    const int kept = std::min(depth, kPackedLevels);
    const int dropped = depth - kept;
    const uint64_t bits = dropped < DepthFlags::kMaxDepth ? flags.bits >> dropped : 0;
    return static_cast<SyntaxLexState>((bits & ((uint64_t{ 1 } << kept) - 1)) | (static_cast<uint64_t>(kept) << kPackedLevels));
}

constexpr int UnpackDepth(SyntaxLexState state) noexcept
{
    return static_cast<int>((state >> kPackedLevels) & 0x1F);
}

constexpr DepthFlags UnpackDepthFlags(SyntaxLexState state) noexcept
{
    return { state & ((SyntaxLexState{ 1 } << kPackedLevels) - 1) };
}

constexpr SyntaxLexState kTomlNoKey = SyntaxLexState{ 1 } << kPackedDepthBits;
constexpr SyntaxLexState kTomlMidLine = kTomlNoKey << 1;

// 1 文字ずつ進む lexer が、状態を残すために次に解析を区切る位置。ループの中で毎回確かめると、状態を残さないふだんの解析まで
// 1 割遅くなるので (16KB の TOML で 28.7 → 32.8µs)、区切りまでを内側のループで進め、その外で状態を残す。
size_t NextStateStop(const SyntaxStateTrace* trace, size_t text_size) noexcept
{
    return trace ? std::min(trace->next_wanted(), text_size) : text_size;
}

void TokenizeToml(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    // 括弧の深さごとに '{' (インラインテーブル) か '[' (配列) かを覚える。キーはテーブル直下でのみ現れる。
    DepthFlags inline_tables = UnpackDepthFlags(state);
    int depth = UnpackDepth(state);
    bool expect_key = (state & kTomlNoKey) == 0;
    bool line_start = (state & kTomlMidLine) == 0;
    size_t i = begin;
    for (const size_t n = text.size();;) {
        const size_t stop = NextStateStop(trace, n);
        while (i < stop) {
            const char c = text[i];
            if (c == '\n') {
                i++;
                line_start = true;
                expect_key = expect_key || depth == 0;
                continue;
            }
            if (IsBlank(c)) {
                i++;
                continue;
            }
            if (c == '#') {
                i = EmitRange(tokens, i, SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
            const bool in_inline_table = inline_tables.test(depth);
            if (expect_key && (depth == 0 || in_inline_table)) {
                expect_key = false;
                if (depth == 0 && line_start && c == '[') {
                    const size_t eol = SkipToEol(text, i);
                    size_t e = text.substr(0, eol).find(']', i);
                    e = e < eol ? e + 1 : eol;
                    if (e < eol && text[e] == ']') {
                        e++;
                    }
                    i = EmitRange(tokens, i, e, SyntaxTokenType::Keyword);
                    line_start = false;
                    continue;
                }
                if (const size_t eq = FindTomlKeyEquals(text, i); eq != npos) {
                    EmitRange(tokens, i, TrimEnd(text, i, eq), SyntaxTokenType::Type);
                    i = eq + 1;
                    line_start = false;
                    continue;
                }
            }
            line_start = false;
            if (c == '"' || c == '\'') {
                const bool triple = i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c;
                i = EmitRange(tokens, i, triple ? ScanTripleQuote(text, i, c, c == '"') : ScanString(text, i, c, false, c == '"'), SyntaxTokenType::String);
                continue;
            }
            if (c == '[' || c == '{') {
                depth++;
                inline_tables.set(depth, c == '{');
                expect_key = c == '{';
                i++;
                continue;
            }
            if (c == ']' || c == '}') {
                depth = std::max(depth - 1, 0);
                i++;
                continue;
            }
            if (c == ',') {
                expect_key = in_inline_table;
                i++;
                continue;
            }
            static constexpr CharSet kScalarPunct{ "+-.:" };
            const size_t e = SkipWhile(text, i, [](char ch) static noexcept { return IsIdentChar(ch) || kScalarPunct.contains(ch); });
            if (e == i) {
                i++;
                continue;
            }
            i = EmitRange(tokens, i, e, ClassifyScalar(text.substr(i, e - i)));
        }
        if (trace) {
            trace->Set(i, PackDepth(depth, inline_tables) | (expect_key ? 0 : kTomlNoKey) | (line_start ? 0 : kTomlMidLine));
        }
        if (i >= n) {
            break;
        }
    }
}

// ---------- INI ----------

void TokenizeIni(std::string_view text, size_t begin, std::vector<SyntaxToken>& tokens)
{
    for (size_t pos = begin; pos < text.size();) {
        const size_t eol = SkipToEol(text, pos);
        const size_t i = SkipBlanks(text, pos, eol);
        const size_t end = TrimEnd(text, i, eol);
        pos = eol + 1;
        if (i == end) {
            continue;
        }
        const char c = text[i];
        if (c == ';' || c == '#') {
            EmitRange(tokens, i, end, SyntaxTokenType::Comment);
            continue;
        }
        // 探索は行内で止める (区切りの無い行が続く入力で文書末尾まで読まない)
        const auto line = text.substr(0, end);
        if (c == '[') {
            const size_t close = line.find(']', i);
            EmitRange(tokens, i, close < end ? close + 1 : end, SyntaxTokenType::Keyword);
            continue;
        }
        const size_t sep = line.find_first_of("=:", i);
        if (sep >= end) {
            continue;
        }
        EmitRange(tokens, i, TrimEnd(text, i, sep), SyntaxTokenType::Type);
        const size_t v = SkipBlanks(text, sep + 1, end);
        if (v < end && (text[v] == '"' || text[v] == '\'')) {
            EmitRange(tokens, v, std::min(ScanString(text, v, text[v], false, false), end), SyntaxTokenType::String);
        }
        else {
            EmitRange(tokens, v, end, ClassifyScalar(text.substr(v, end - v)));
        }
    }
}

// ---------- CSS / SCSS ----------

constexpr bool IsDashedIdentChar(char c) noexcept
{
    return IsIdentChar(c) || c == '-';
}

constexpr size_t SkipCssIdent(std::string_view text, size_t pos) noexcept
{
    return SkipWhile(text, pos, IsDashedIdentChar);
}

// 数値と単位（10px / 1.5em / 50% / -2）の終端。
constexpr size_t ScanCssNumber(std::string_view text, size_t pos) noexcept
{
    return SkipWhile(text, SkipChars(text, pos + 1, "0123456789."), [](char c) static noexcept { return IsAsciiAlpha(c) || c == '%'; });
}

// セレクタの 1 トークンを読み、次の位置を返す。タグ名は Keyword、.class / #id は Function、疑似クラスと属性名は Type。
size_t ScanCssSelectorToken(std::string_view text, size_t i, std::vector<SyntaxToken>& tokens)
{
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    // .class / #id と、SCSS の親参照に続く接尾辞 &--modifier
    if ((c == '.' || c == '#' || c == '&') && IsDashedIdentChar(next)) {
        return EmitRange(tokens, i, SkipCssIdent(text, i + 1), SyntaxTokenType::Function);
    }
    if (c == ':') {
        const size_t name = next == ':' ? i + 2 : i + 1;
        const size_t e = SkipCssIdent(text, name);
        if (e > name) {
            EmitRange(tokens, i, e, SyntaxTokenType::Type);
        }
        return std::max(e, i + 1);
    }
    if (c == '[') {
        const size_t name = SkipBlanks(text, i + 1, text.size());
        const size_t e = SkipCssIdent(text, name);
        EmitRange(tokens, name, e, SyntaxTokenType::Type);
        return e;
    }
    if (IsAsciiDigit(c)) {
        return EmitRange(tokens, i, ScanCssNumber(text, i), SyntaxTokenType::Number);
    }
    if (IsIdentStart(c) || (c == '-' && IsIdentStart(next))) {
        return EmitRange(tokens, i, SkipCssIdent(text, i), SyntaxTokenType::Keyword);
    }
    return i + 1;
}

// 宣言の値かアットルールの引数の 1 トークンを読み、次の位置を返す。
template <bool kScss>
size_t ScanCssValueToken(std::string_view text, size_t i, bool at_rule, std::vector<SyntaxToken>& tokens)
{
    const size_t n = text.size();
    const char c = text[i];
    const char next = i + 1 < n ? text[i + 1] : '\0';
    const bool signed_number = (c == '-' || c == '+') &&
                               (IsAsciiDigit(next) || (next == '.' && i + 2 < n && IsAsciiDigit(text[i + 2])));
    if (IsAsciiDigit(c) || (c == '.' && IsAsciiDigit(next)) || signed_number) {
        return EmitRange(tokens, i, ScanCssNumber(text, i), SyntaxTokenType::Number);
    }
    // 色 #fff
    if (c == '#' && IsDashedIdentChar(next)) {
        return EmitRange(tokens, i, SkipCssIdent(text, i + 1), SyntaxTokenType::Number);
    }
    // !important、@media 等のアットルール名、Less の @var
    if ((c == '!' || c == '@') && IsIdentStart(next)) {
        return EmitRange(tokens, i, SkipCssIdent(text, i + 1), c == '!' || at_rule ? SyntaxTokenType::Keyword : SyntaxTokenType::Type);
    }
    if (kScss && c == '$' && IsIdentStart(next)) {
        return EmitRange(tokens, i, SkipCssIdent(text, i + 1), SyntaxTokenType::Type);
    }
    if (IsIdentStart(c) || (c == '-' && (IsIdentStart(next) || next == '-'))) {
        const size_t e = SkipCssIdent(text, i);
        if (e < n && text[e] == '(') {
            EmitRange(tokens, i, e, SyntaxTokenType::Function);
            // url(...) の引用符なし引数は // や ; を含みうるので、まとめて String にする。
            if (ascii_util::iequal(text.substr(i, e - i), "url")) {
                const size_t arg = SkipBlanks(text, e + 1, n);
                if (arg < n && text[arg] != '"' && text[arg] != '\'') {
                    const size_t close = std::min(text.find_first_of(")\n", arg), n);
                    EmitRange(tokens, arg, close, SyntaxTokenType::String);
                    return close;
                }
            }
            return e;
        }
        // @media (max-width: 600px) の特性名
        if (const size_t k = SkipBlanks(text, e, n); at_rule && k < n && text[k] == ':') {
            EmitRange(tokens, i, e, SyntaxTokenType::Type);
        }
        return e;
    }
    return i + 1;
}

// CSS の状態はブロックの深さ (255 で頭打ち)・文の頭かどうか・文の途中ならそのモード。HTML の lexer がこの上のビットを使う。
constexpr int kCssStateBits = 11;

// [begin, text.size()) を CSS として走査する。HTML の <style> 内もここを通す。
template <bool kScss>
void TokenizeCssRange(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    enum class Mode : uint8_t {
        Selector,
        Property,
        Value,
        AtRule,
    };
    constexpr SyntaxLexState kDepthMask = 0xFF;
    constexpr SyntaxLexState kMidStatement = 0x100;
    constexpr int kModeShift = 9;
    const size_t n = text.size();
    int depth = static_cast<int>(state & kDepthMask);
    bool stmt_start = (state & kMidStatement) == 0;
    Mode mode = static_cast<Mode>((state >> kModeShift) & 3);
    // 文の頭ではモードを次のトークンで決め直すので、持ち越さない。
    const auto packed = [](int at_depth, bool at_stmt_start, Mode in_mode) static noexcept -> SyntaxLexState {
        const auto all_depth = static_cast<SyntaxLexState>(at_depth);
        const SyntaxLexState kept_depth = all_depth < kDepthMask ? all_depth : kDepthMask;
        return kept_depth | (at_stmt_start ? 0 : kMidStatement | (SyntaxLexState{ std::to_underlying(in_mode) } << kModeShift));
    };
    size_t i = begin;
    for (;;) {
        const size_t stop = NextStateStop(trace, n);
        while (i < stop) {
            const char c = text[i];
            const char next = i + 1 < n ? text[i + 1] : '\0';
            if (c == '/' && next == '*') {
                i = EmitRange(tokens, i, ScanBlockComment(text, i, '*', '/'), SyntaxTokenType::Comment);
                continue;
            }
            if constexpr (kScss) {
                if (c == '/' && next == '/') {
                    i = EmitRange(tokens, i, SkipToEol(text, i), SyntaxTokenType::Comment);
                    continue;
                }
                // 補間 #{...} の括弧をブロックと数えない。
                if (c == '#' && next == '{') {
                    const size_t close = text.find('}', i);
                    i = close == npos ? n : close + 1;
                    continue;
                }
            }
            if (IsMarkupSpace(c)) {
                i++;
                continue;
            }
            if (c == '"' || c == '\'') {
                i = EmitRange(tokens, i, ScanString(text, i, c, false), SyntaxTokenType::String);
                continue;
            }
            if (c == '{' || c == '}' || c == ';') {
                if (c == '{') {
                    depth++;
                }
                else if (c == '}') {
                    depth = std::max(depth - 1, 0);
                }
                stmt_start = true;
                i++;
                continue;
            }
            if (stmt_start) {
                stmt_start = false;
                if (c == '@') {
                    mode = Mode::AtRule;
                }
                else if (kScss && c == '$') {
                    mode = Mode::Property;
                }
                else if (depth == 0) {
                    mode = Mode::Selector;
                }
                else {
                    // ブロック内は、文末より先に '{' が来ればネストしたルールのセレクタ、そうでなければ宣言。
                    // 続きのノードは自分のテキストしか読めないので、文がノードの末尾をまたぐと宣言として読む。
                    const size_t p = text.find_first_of(";{}", i);
                    if (trace && p == npos) {
                        trace->SetUnsettled(i, packed(depth, true, mode), n);
                    }
                    mode = p != npos && text[p] == '{' ? Mode::Selector : Mode::Property;
                }
            }
            switch (mode) {
            case Mode::Selector:
                i = ScanCssSelectorToken(text, i, tokens);
                break;
            case Mode::Property:
                if (c == ':') {
                    mode = Mode::Value;
                    i++;
                }
                else if (IsDashedIdentChar(c) || (kScss && c == '$')) {
                    i = EmitRange(tokens, i, SkipCssIdent(text, i + 1), SyntaxTokenType::Type);
                }
                else {
                    i++;
                }
                break;
            case Mode::Value:
            case Mode::AtRule:
                i = ScanCssValueToken<kScss>(text, i, mode == Mode::AtRule, tokens);
                break;
            }
        }
        if (trace) {
            trace->Set(i, packed(depth, stmt_start, mode));
        }
        if (i >= n) {
            break;
        }
    }
}

// ---------- HTML / XML ----------

// 文字参照 &name; / &#123; / &#x1F; の終端（';' の次）。参照でなければ npos。
constexpr size_t ScanHtmlEntity(std::string_view text, size_t pos) noexcept
{
    constexpr size_t kMaxLength = 32;
    const size_t limit = std::min(text.size(), pos + kMaxLength);
    size_t i = pos + 1;
    if (i < limit && text[i] == '#') {
        i++;
    }
    const size_t name = i;
    i = SkipWhile(text, i, limit, IsAsciiAlnum<char>);
    return i > name && i < limit && text[i] == ';' ? i + 1 : npos;
}

constexpr bool IsMarkupNameChar(char c) noexcept
{
    return IsIdentChar(c) || c == '-' || c == ':' || c == '.';
}

// 終了タグ </name を ASCII 大文字小文字無視で探し、その '<' の位置を返す。無ければ text.size()。
constexpr size_t FindClosingTag(std::string_view text, size_t pos, ascii_util::DocLowercaseLiteral name) noexcept
{
    for (size_t lt = text.find("</", pos); lt != npos; lt = text.find("</", lt + 2)) {
        const size_t after = lt + 2 + name.value.size();
        if (ascii_util::istarts_with(text.substr(lt + 2), name) && (after >= text.size() || !IsMarkupNameChar(text[after]))) {
            return lt;
        }
    }
    return text.size();
}

// マークアップの状態。タグの属性の途中 (閉じると中身が続く <script> / <style> の開きタグは別に持つ) か、
// <script> / <style> の中身か。<style> の中身では下位のビットに CSS の状態を重ねる。
// 属性の '=' と値の間は持たないので、そこから始めると引用符の無い値を属性名として色付けする。
enum class MarkupMode : uint8_t {
    Text,
    Tag,
    ScriptTag,
    StyleTag,
    Script,
    Style,
};
static_assert(kCssStateBits + 3 <= kSyntaxLexStateBits);

constexpr SyntaxLexState MarkupState(MarkupMode mode) noexcept
{
    return SyntaxLexState{ std::to_underlying(mode) } << kCssStateBits;
}

// タグ名は Keyword、属性名は Type、属性値は String、文字参照は Number。
// HTML では <script> / <style> の中身を JS / CSS として色付けする。
template <bool kHtml>
void TokenizeMarkup(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    const size_t n = text.size();
    size_t i = begin;
    // 1 周でタグの外 → 属性 → 中身と進んで Text に戻る。Text 以外で周に入るのは、途中から始めるときだけ。
    auto mode = static_cast<MarkupMode>(state >> kCssStateBits);
    SyntaxLexState css_state = state & ((SyntaxLexState{ 1 } << kCssStateBits) - 1);
    // テキストの末尾での状態。<style> の中身のまま尽きたときは CSS の lexer が残す。
    SyntaxLexState tail = state;
    bool style_open = false;
    while (i < n) {
        if (mode == MarkupMode::Text) {
            tail = MarkupState(MarkupMode::Text);
            const size_t lt = text.find('<', i);
            const size_t text_end = lt == npos ? n : lt;
            // '<' まではどこからでも始め直せる。その直後のタグ名から始めるとタグとして読めない。
            if (trace && trace->Wants(text_end)) {
                trace->SetRange(i, std::min(text_end + 1, n), MarkupState(MarkupMode::Text));
            }
            // タグ間のテキストだけを探す (& の無い文書でタグごとに末尾まで読まない)
            const auto between = text.substr(0, text_end);
            for (size_t amp = between.find('&', i); amp != npos; amp = between.find('&', amp + 1)) {
                if (const size_t e = ScanHtmlEntity(between, amp); e != npos) {
                    EmitRange(tokens, amp, e, SyntaxTokenType::Number);
                    amp = e - 1;
                }
            }
            if (lt == npos) {
                break;
            }
            const auto rest = text.substr(lt);
            const auto emit_until = [&](size_t open_length, std::string_view close, SyntaxTokenType type) {
                const size_t p = text.find(close, lt + open_length);
                i = p == npos ? n : p + close.size();
                EmitRange(tokens, lt, i, type);
            };
            if (rest.starts_with("<!--")) {
                emit_until(4, "-->", SyntaxTokenType::Comment);
                continue;
            }
            if (rest.starts_with("<![CDATA[")) {
                emit_until(9, "]]>", SyntaxTokenType::String);
                continue;
            }
            if (rest.starts_with("<!") || rest.starts_with("<?")) {
                emit_until(2, ">", SyntaxTokenType::Preprocessor);
                continue;
            }
            const bool closing = rest.starts_with("</");
            const size_t name_start = lt + (closing ? 2 : 1);
            const size_t name_end = SkipWhile(text, name_start, IsMarkupNameChar);
            if (name_end == name_start || !IsIdentStart(text[name_start])) {
                i = lt + 1;
                continue;
            }
            i = EmitRange(tokens, name_start, name_end, SyntaxTokenType::Keyword);
            mode = MarkupMode::Tag;
            if constexpr (kHtml) {
                const auto tag_name = text.substr(name_start, name_end - name_start);
                if (!closing && ascii_util::iequal(tag_name, "script")) {
                    mode = MarkupMode::ScriptTag;
                }
                else if (!closing && ascii_util::iequal(tag_name, "style")) {
                    mode = MarkupMode::StyleTag;
                }
            }
        }
        if (mode < MarkupMode::Script) {
            size_t j = i;
            bool closed = false;
            bool self_closing = false;
            size_t stop = NextStateStop(trace, n);
            while (j < n) {
                const char c = text[j];
                if (IsMarkupSpace(c)) {
                    j++;
                    continue;
                }
                if (j >= stop) [[unlikely]] {
                    trace->Set(j, MarkupState(mode));
                    stop = NextStateStop(trace, n);
                }
                if (c == '>') {
                    j++;
                    closed = true;
                    break;
                }
                // '>' の閉じ忘れ。次のタグとして読み直す。
                if (c == '<') {
                    closed = true;
                    break;
                }
                if (c == '/' && j + 1 < n && text[j + 1] == '>') {
                    j += 2;
                    closed = true;
                    self_closing = true;
                    break;
                }
                if (c == '/') {
                    j++;
                    continue;
                }
                if (c == '"' || c == '\'') {
                    const size_t e = ScanString(text, j, c, true, false);
                    j = EmitRange(tokens, j, e, SyntaxTokenType::String);
                    continue;
                }
                if (c == '=') {
                    j = SkipWhile(text, j + 1, IsMarkupSpace);
                    const size_t e = j < n && (text[j] == '"' || text[j] == '\'')
                                         ? ScanString(text, j, text[j], true, false)
                                         : SkipWhile(text, j, [](char ch) static noexcept { return !IsMarkupSpace(ch) && ch != '>' && ch != '<'; });
                    j = EmitRange(tokens, j, e, SyntaxTokenType::String);
                    continue;
                }
                static constexpr CharSet kAttrNameEnd{ " \t\n=>/<\"'" };
                const size_t e = SkipWhile(text, j, [](char ch) static noexcept { return !kAttrNameEnd.contains(ch); });
                j = EmitRange(tokens, j, e, SyntaxTokenType::Type);
            }
            i = j;
            if (!closed) {
                tail = MarkupState(mode);
                break;
            }
            tail = MarkupState(MarkupMode::Text);
            mode = self_closing || mode == MarkupMode::Tag ? MarkupMode::Text
                   : mode == MarkupMode::ScriptTag         ? MarkupMode::Script
                                                           : MarkupMode::Style;
            css_state = 0;
            if (mode == MarkupMode::Text) {
                continue;
            }
        }
        if (mode == MarkupMode::Script) {
            const size_t close = FindClosingTag(text, i, "script");
            // 中身はどのトークンの外からでも JS として始め直せる。
            if (trace && close > i && trace->Wants(close - 1)) {
                trace->SetRange(i, close, MarkupState(mode));
            }
            TokenizeRange<JS_LEXER_CONFIG>(text.substr(0, close), i, kw::JS_KEYWORDS, kw::JS_TYPES, tokens);
            tail = MarkupState(close == n ? mode : MarkupMode::Text);
            i = close;
        }
        else {
            const size_t close = FindClosingTag(text, i, "style");
            if (trace) {
                trace->outer = MarkupState(mode);
            }
            TokenizeCssRange<false>(text.substr(0, close), i, css_state, tokens, trace);
            if (trace) {
                trace->outer = 0;
            }
            style_open = close == n;
            tail = MarkupState(MarkupMode::Text);
            i = close;
        }
        mode = MarkupMode::Text;
    }
    if (trace && !style_open) {
        trace->Set(n, tail);
    }
}

// ---------- Dockerfile ----------

// 行頭の命令を Keyword とし、引数は継続行も含めてシェルとして色付けする。
// 状態は継続行の途中かどうか。途中から始めた行は、論理行の終わりまでシェルとして読む。
void TokenizeDockerfile(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    constexpr SyntaxLexState kShellRest = 1;
    size_t pos = begin;
    if (state == kShellRest) {
        const size_t end = SkipToLogicalEol(text, pos);
        TokenizeShellRange(text, pos, end, tokens);
        NoteContinuationLines(trace, text, pos, end, state, kShellRest);
        pos = end + 1;
    }
    while (pos < text.size()) {
        if (trace && trace->Wants(pos)) {
            trace->Set(pos, 0);
        }
        const size_t i = SkipBlanks(text, pos, text.size());
        if (i < text.size() && text[i] == '#') {
            const size_t eol = SkipToEol(text, i);
            EmitRange(tokens, i, eol, SyntaxTokenType::Comment);
            pos = eol + 1;
            continue;
        }
        const size_t end = SkipToLogicalEol(text, i);
        const size_t j = SkipWhile(text, i, end, IsIdentChar);
        const auto word = text.substr(i, j - i);
        size_t rest = i;
        if (kw::DOCKER_KEYWORDS.contains_ignore_case(word)) {
            rest = EmitRange(tokens, i, j, SyntaxTokenType::Keyword);
            // FROM image AS stage の AS
            if (ascii_util::iequal(word, "from")) {
                for (size_t k = SkipBlanks(text, j, end); k < end; k = SkipBlanks(text, k, end)) {
                    const size_t e = SkipWhile(text, k, end, [](char ch) static noexcept { return !IsMarkupSpace(ch); });
                    if (ascii_util::iequal(text.substr(k, e - k), "as")) {
                        TokenizeShellRange(text, rest, k, tokens);
                        rest = EmitRange(tokens, k, e, SyntaxTokenType::Keyword);
                        break;
                    }
                    k = std::max(e, k + 1);
                }
            }
        }
        TokenizeShellRange(text, rest, end, tokens);
        NoteContinuationLines(trace, text, pos, end, 0, kShellRest);
        pos = end + 1;
    }
    if (trace) {
        trace->Set(text.size(), 0);
    }
}

// ---------- Makefile ----------

// $(...) / ${...} / $@ 等の変数参照の終端。参照でなければ pos + 1。
constexpr size_t ScanMakeVarRef(std::string_view text, size_t pos, size_t end) noexcept
{
    if (pos + 1 >= end) {
        return pos + 1;
    }
    const char open = text[pos + 1];
    if (open == '(' || open == '{') {
        const char close = open == '(' ? ')' : '}';
        int depth = 0;
        for (size_t i = pos + 1; i < end; i++) {
            if (text[i] == open) {
                depth++;
            }
            else if (text[i] == close && --depth == 0) {
                return i + 1;
            }
        }
        return end;
    }
    const auto u = static_cast<unsigned char>(open);
    return u > ' ' && u < 0x80 ? pos + 2 : pos + 1;
}

// [begin, end) の変数参照を Type、'#' 以降を Comment、残りを type として出す。
void TokenizeMakeText(std::string_view text, size_t begin, size_t end, SyntaxTokenType type, std::vector<SyntaxToken>& tokens)
{
    size_t seg = begin;
    size_t i = begin;
    while (i < end) {
        const char c = text[i];
        if (c == '#') {
            EmitRange(tokens, seg, TrimEnd(text, seg, i), type);
            EmitRange(tokens, i, end, SyntaxTokenType::Comment);
            return;
        }
        if (c == '$') {
            if (i + 1 < end && text[i + 1] == '$') {
                i += 2;
                continue;
            }
            const size_t e = ScanMakeVarRef(text, i, end);
            if (e > i + 1) {
                EmitRange(tokens, seg, TrimEnd(text, seg, i), type);
                seg = EmitRange(tokens, i, e, SyntaxTokenType::Type);
            }
            i = e;
            continue;
        }
        i++;
    }
    EmitRange(tokens, seg, TrimEnd(text, seg, end), type);
}

// レシピ行はシェルとして色付けし、make の変数参照だけを Type で上書きする。
void TokenizeMakeRecipe(std::string_view text, size_t begin, size_t end, std::vector<SyntaxToken>& tokens)
{
    size_t seg = begin;
    const auto line = text.substr(0, end);
    for (size_t i = line.find('$', begin); i != npos; i = line.find('$', i)) {
        if (i + 1 < end && text[i + 1] == '$') {
            i += 2;
            continue;
        }
        const size_t e = ScanMakeVarRef(text, i, end);
        if (e > i + 1) {
            TokenizeShellRange(text, seg, i, tokens);
            seg = EmitRange(tokens, i, e, SyntaxTokenType::Type);
        }
        i = e;
    }
    TokenizeShellRange(text, seg, end, tokens);
}

// 代入演算子（= := ::= ?= += !=）の先頭かルールの ':' の位置。どちらも無ければ npos。
constexpr size_t FindMakeOperator(std::string_view text, size_t pos, size_t end) noexcept
{
    while (pos < end) {
        const char c = text[pos];
        if (c == '#') {
            return npos;
        }
        if (c == '$') {
            pos = ScanMakeVarRef(text, pos, end);
            continue;
        }
        if (c == ':' || c == '=') {
            return pos;
        }
        if ((c == '?' || c == '+' || c == '!') && pos + 1 < end && text[pos + 1] == '=') {
            return pos;
        }
        pos++;
    }
    return npos;
}

// 論理行 [pos, end) を色付けする。レシピの行なら true。in_rule は次の行がレシピになりうるか。
bool TokenizeMakeLine(std::string_view text, size_t pos, size_t end, bool& in_rule, std::vector<SyntaxToken>& tokens)
{
    size_t i = SkipBlanks(text, pos, end);
    // md4c はコードブロック行頭のタブをスペースに展開して渡すので、タブではなく
    // 「ルール行に続くインデント行」をレシピとみなす。
    if (in_rule && i > pos) {
        TokenizeMakeRecipe(text, i, end, tokens);
        return true;
    }
    const size_t j = SkipWhile(text, i, end, IsDashedIdentChar);
    const auto word = text.substr(i, j - i);
    if (kw::MAKE_KEYWORDS.contains(word) && (j == end || IsBlank(text[j]) || text[j] == '(')) {
        EmitRange(tokens, i, j, SyntaxTokenType::Keyword);
        i = SkipBlanks(text, j, end);
        // export / override / private の後には代入が続く。それ以外の指令の引数は条件式やファイル名。
        // 条件分岐はレシピの途中にも書けるので in_rule は変えない。
        if (word != "export" && word != "override" && word != "private") {
            TokenizeMakeText(text, i, end, SyntaxTokenType::Plain, tokens);
            return false;
        }
    }
    const size_t op = FindMakeOperator(text, i, end);
    if (op == npos) {
        // 空行とコメント行はレシピを終わらせない。
        in_rule = in_rule && (i == end || text[i] == '#');
        TokenizeMakeText(text, i, end, SyntaxTokenType::Plain, tokens);
    }
    else if (text[op] == ':' && !text.substr(op).starts_with(":=") && !text.substr(op).starts_with("::=")) {
        in_rule = true;
        // .PHONY 等の特殊ターゲットは Keyword
        const bool special = text[i] == '.' && i + 1 < end && IsAsciiAlpha(text[i + 1]);
        TokenizeMakeText(text, i, op, special ? SyntaxTokenType::Keyword : SyntaxTokenType::Function, tokens);
        const size_t prerequisites = op + 1 < end && text[op + 1] == ':' ? op + 2 : op + 1;
        TokenizeMakeText(text, prerequisites, end, SyntaxTokenType::Plain, tokens);
    }
    else {
        in_rule = false;
        TokenizeMakeText(text, i, op, SyntaxTokenType::Type, tokens);
        TokenizeMakeText(text, text.find('=', op) + 1, end, SyntaxTokenType::Plain, tokens);
    }
    return false;
}

// 状態は次の行がレシピになりうるかと、継続行の途中ならその残りの読み方。残りはレシピならシェル、ほかは変数参照と
// コメントだけを色付けする (ターゲットや代入の名前が 2 行目より後ろまで続く行は、その分を色付けしない)。
void TokenizeMakefile(std::string_view text, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens, SyntaxStateTrace* trace)
{
    constexpr SyntaxLexState kInRule = 1;
    constexpr SyntaxLexState kRecipeRest = 2;
    constexpr SyntaxLexState kTextRest = 4;
    bool in_rule = (state & kInRule) != 0;
    size_t pos = begin;
    if (const SyntaxLexState rest = state & (kRecipeRest | kTextRest); rest != 0) {
        const size_t end = SkipToLogicalEol(text, pos);
        if (rest == kRecipeRest) {
            TokenizeMakeRecipe(text, pos, end, tokens);
        }
        else {
            TokenizeMakeText(text, pos, end, SyntaxTokenType::Plain, tokens);
        }
        NoteContinuationLines(trace, text, pos, end, state, state);
        pos = end + 1;
    }
    while (pos < text.size()) {
        const SyntaxLexState head = in_rule ? kInRule : 0;
        if (trace && trace->Wants(pos)) {
            trace->Set(pos, head);
        }
        const size_t end = SkipToLogicalEol(text, pos);
        const bool recipe = TokenizeMakeLine(text, pos, end, in_rule, tokens);
        NoteContinuationLines(trace, text, pos, end, head, (in_rule ? kInRule : 0) | (recipe ? kRecipeRest : kTextRest));
        pos = end + 1;
    }
    if (trace) {
        trace->Set(text.size(), in_rule ? kInRule : 0);
    }
}

// ---------- Haskell / Lisp 共通 ----------

// UTF-8 の 1 コードポイント分進めた位置（後続バイトを読み飛ばす）。pos < text.size() であること。
constexpr size_t NextCodePoint(std::string_view text, size_t pos) noexcept
{
    return SkipWhile(text, pos + 1, [](char c) static noexcept { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; });
}

// {- -} や #| |# の入れ子を数えて、最外の閉じの直後を返す。未終端なら text.size()。
constexpr size_t ScanNestedComment(std::string_view text, size_t pos, std::string_view open, std::string_view close) noexcept
{
    const char firsts[]{ open[0], close[0] };
    int depth = 0;
    size_t i = pos;
    // 開き・閉じの先頭文字の候補まで find で飛ぶ
    while ((i = text.find_first_of(std::string_view{ firsts, 2 }, i)) != npos) {
        const auto rest = text.substr(i);
        if (rest.starts_with(open)) {
            depth++;
            i += open.size();
        }
        else if (rest.starts_with(close)) {
            i += close.size();
            if (--depth == 0) {
                return i;
            }
        }
        else {
            i++;
        }
    }
    return text.size();
}

// ---------- Haskell ----------

constexpr bool IsHaskellSymbolChar(char c) noexcept
{
    static constexpr CharSet kSymbols{ "!#$%&*+./<=>?@\\^|-~:" };
    return kSymbols.contains(c);
}

// 文字リテラル 'a' / '\n' / '\'' の終端。' が識別子の一部 (foldl') や型レベルの昇格 ('True) なら npos。
constexpr size_t ScanHaskellChar(std::string_view text, size_t pos) noexcept
{
    constexpr size_t kMaxEscapeLength = 12; // '\x10FFFF' 等
    if (pos + 1 >= text.size() || text[pos + 1] == '\n' || text[pos + 1] == '\'') {
        return npos;
    }
    if (text[pos + 1] == '\\') {
        // バックスラッシュ直後の 1 文字はエスケープの一部 ('\'' の 2 個目の ' を閉じと見ない)
        const auto window = text.substr(0, std::min(text.size(), pos + kMaxEscapeLength + 1));
        const size_t close = pos + 3 <= window.size() ? window.find('\'', pos + 3) : npos;
        if (close == npos || text.substr(pos, close - pos).contains('\n')) {
            return npos;
        }
        return close + 1;
    }
    const size_t after = NextCodePoint(text, pos + 1);
    return after < text.size() && text[after] == '\'' ? after + 1 : npos;
}

// 大文字始まりの識別子は型・コンストラクタ、行頭の小文字始まりは定義名 (関数の型注釈と本体) とする。
void TokenizeHaskell(std::string_view text, size_t begin, std::vector<SyntaxToken>& tokens)
{
    const size_t n = text.size();
    size_t i = begin;
    while (i < n) {
        const char c = text[i];
        const char next = i + 1 < n ? text[i + 1] : '\0';
        const bool col0 = i == 0 || text[i - 1] == '\n';
        if (IsMarkupSpace(c)) {
            i++;
            continue;
        }
        if (c == '{' && next == '-') {
            if (i + 2 < n && text[i + 2] == '#') {
                const size_t p = text.find("#-}", i + 3);
                i = EmitRange(tokens, i, p == npos ? n : p + 3, SyntaxTokenType::Preprocessor);
            }
            else {
                i = EmitRange(tokens, i, ScanNestedComment(text, i, "{-", "-}"), SyntaxTokenType::Comment);
            }
            continue;
        }
        // 記号と隣り合う -- は演算子 (-->, |-- 等) の一部でコメントではない
        if (c == '-' && next == '-') {
            const size_t dashes_end = SkipChars(text, i, "-");
            if ((dashes_end < n && IsHaskellSymbolChar(text[dashes_end])) || (i > 0 && IsHaskellSymbolChar(text[i - 1]))) {
                i = dashes_end;
                continue;
            }
            i = EmitRange(tokens, i, SkipToEol(text, i), SyntaxTokenType::Comment);
            continue;
        }
        // CPP 拡張の #if や shebang
        if (col0 && c == '#') {
            i = EmitRange(tokens, i, SkipToLogicalEol(text, i), SyntaxTokenType::Preprocessor);
            continue;
        }
        if (c == '"') {
            i = EmitRange(tokens, i, ScanString(text, i, '"', false), SyntaxTokenType::String);
            continue;
        }
        if (c == '\'') {
            const size_t e = ScanHaskellChar(text, i);
            i = e != npos ? EmitRange(tokens, i, e, SyntaxTokenType::String) : i + 1;
            continue;
        }
        if (IsAsciiDigit(c)) {
            i = EmitRange(tokens, i, ScanNumber(text, i), SyntaxTokenType::Number);
            continue;
        }
        if (IsIdentStart(c)) {
            const size_t e = SkipWhile(text, i + 1, [](char ch) static noexcept { return IsIdentChar(ch) || ch == '\''; });
            SyntaxTokenType type = SyntaxTokenType::Plain;
            if (IsAsciiUpper(c)) {
                type = SyntaxTokenType::Type;
            }
            else if (kw::HASKELL_KEYWORDS.contains(text.substr(i, e - i))) {
                type = SyntaxTokenType::Keyword;
            }
            else if (col0 && c != '_') {
                type = SyntaxTokenType::Function;
            }
            i = EmitRange(tokens, i, e, type);
            continue;
        }
        i++;
    }
}

// ---------- Lisp 系 (Common Lisp / Emacs Lisp / Scheme・Racket / Clojure) ----------

struct LispDialect {
    kw::KeywordTable special;      // 呼び出し位置で Keyword にする特殊形式・マクロ
    kw::KeywordTable constants;    // nil / t / true 等
    bool case_insensitive = false; // Common Lisp のシンボルは大文字小文字を区別しない
    bool cond_clauses = true;      // (cond (test body) ...) の節を呼び出しと見ない。Clojure の cond は節を持たない
    bool elisp_char = false;       // ?a / ?\n
    bool clojure_char = false;     // \a / \newline
};

constexpr bool IsLispDelimiter(char c) noexcept
{
    static constexpr CharSet kDelimiters{ " \t\n()[]{}\"';`," };
    return kDelimiters.contains(c);
}

constexpr size_t SkipLispSymbol(std::string_view text, size_t pos) noexcept
{
    return SkipWhile(text, pos, [](char c) static noexcept { return !IsLispDelimiter(c); });
}

// 文字リテラル (#\a / \newline / ?\C-x) の本体の終端。英字で始まれば名前付き文字として英数字と - を続けて読む。
constexpr size_t ScanLispCharBody(std::string_view text, size_t pos) noexcept
{
    if (pos >= text.size()) {
        return text.size();
    }
    const size_t e = NextCodePoint(text, pos);
    if (!IsAsciiAlpha(text[pos])) {
        return e;
    }
    return SkipWhile(text, e, [](char c) static noexcept { return IsAsciiAlnum(c) || c == '-'; });
}

// #_ / #; が読み飛ばす次の 1 データの終端。リストは括弧の対応を数える。
constexpr size_t ScanLispDatum(std::string_view text, size_t pos) noexcept
{
    const size_t n = text.size();
    static constexpr CharSet kOpens{ "([{" };
    static constexpr CharSet kCloses{ ")]}" };
    static constexpr CharSet kPrefixes{ " \t\n'`#@^~," };
    pos = SkipChars(text, pos, kPrefixes);
    if (pos >= n) {
        return n;
    }
    if (text[pos] == '"') {
        return ScanString(text, pos, '"', true);
    }
    if (!kOpens.contains(text[pos])) {
        return SkipLispSymbol(text, pos);
    }
    int depth = 0;
    for (size_t k = pos; k < n;) {
        const char ch = text[k];
        if (ch == '"') {
            k = ScanString(text, k, '"', true);
            continue;
        }
        if (ch == ';') {
            k = SkipToEol(text, k);
            continue;
        }
        // \( や #\( の括弧は数えない
        if (ch == '\\') {
            k = k + 1 < n ? NextCodePoint(text, k + 1) : n;
            continue;
        }
        if (kOpens.contains(ch)) {
            depth++;
        }
        else if (kCloses.contains(ch) && --depth == 0) {
            return k + 1;
        }
        k++;
    }
    return n;
}

// 符号・小数・分数 (1/2)・指数・Clojure の 0x1F / 36rZ / 1N / 1.5M。末尾が + や - の 1+ 等は関数名。
constexpr bool IsLispNumber(std::string_view w) noexcept
{
    const size_t i = !w.empty() && (w[0] == '+' || w[0] == '-') ? 1 : 0;
    if (i >= w.size() || !(IsAsciiDigit(w[i]) || (w[i] == '.' && i + 1 < w.size() && IsAsciiDigit(w[i + 1])))) {
        return false;
    }
    const char last = w.back();
    return (IsAsciiDigit(last) || last == '.' || last == 'N' || last == 'M') &&
           w.find_first_not_of("0123456789abcdefABCDEFxXoObB._/+-eENMrR", i) == npos;
}

bool LispContains(const LispDialect& d, kw::KeywordTable table, std::string_view sym) noexcept
{
    return d.case_insensitive ? table.contains_ignore_case(sym) : table.contains(sym);
}

// # で始まるディスパッチ記法を読み、次の位置を返す。
size_t ScanLispDispatch(std::string_view text, size_t i, std::vector<SyntaxToken>& tokens)
{
    const size_t n = text.size();
    const char next = i + 1 < n ? text[i + 1] : '\0';
    const bool col0 = i == 0 || text[i - 1] == '\n';
    size_t e = npos;
    SyntaxTokenType type = SyntaxTokenType::Plain;
    if (next == '|') {
        e = ScanNestedComment(text, i, "#|", "|#");
        type = SyntaxTokenType::Comment;
    }
    else if (next == ';' || next == '_') {
        // データ単位のコメント: Scheme の #; と Clojure の #_
        e = ScanLispDatum(text, i + 2);
        type = SyntaxTokenType::Comment;
    }
    else if (next == '\\') {
        e = ScanLispCharBody(text, i + 2);
        type = SyntaxTokenType::String;
    }
    else if (next == '\'') {
        e = SkipLispSymbol(text, i + 2);
        type = SyntaxTokenType::Function;
    }
    else if (next == '"') {
        // Clojure の正規表現 #"..."
        e = ScanString(text, i + 1, '"', true);
        type = SyntaxTokenType::String;
    }
    else if (next == ':') {
        e = SkipLispSymbol(text, i + 2);
        type = SyntaxTokenType::Type;
    }
    else if (col0 && next == '!') {
        e = SkipToEol(text, i);
        type = SyntaxTokenType::Comment;
    }
    else if (col0 && text.substr(i).starts_with("#lang")) {
        e = SkipToEol(text, i);
        type = SyntaxTokenType::Preprocessor;
    }
    else if (IsAsciiAlpha(next)) {
        e = SkipLispSymbol(text, i + 1);
        const auto word = text.substr(i + 1, e - i - 1);
        static constexpr ascii_util::DocLowercaseLiteral kBooleans[]{ "t", "f", "true", "false" };
        static constexpr CharSet kRadixLetters{ "xXbBoOeEiIdD" };
        if (std::ranges::any_of(kBooleans, [&](const auto& b) { return ascii_util::iequal(word, b); })) {
            type = SyntaxTokenType::Keyword;
        }
        else if (word.size() > 1 && kRadixLetters.contains(word[0]) &&
                 word.find_first_not_of("0123456789abcdefABCDEF._/+-", 1) == npos) {
            type = SyntaxTokenType::Number; // #x1F / #b101 / #e1.5
        }
        else {
            type = SyntaxTokenType::Preprocessor; // #inst / #rx 等のタグ・リーダー
        }
    }
    if (e == npos) {
        return i + 1; // #( #{ #[ はそのまま括弧として読ませる
    }
    EmitRange(tokens, i, e, type);
    return std::max(e, i + 1);
}

// リストの先頭 (呼び出し位置) のシンボルを Function、特殊形式を Keyword とする。
// 定義名 (defun foo の foo) は Function、引数リストと let の束縛リストは呼び出しと見ない。
constexpr SyntaxLexState kLispCallHead = SyntaxLexState{ 1 } << kPackedDepthBits;
constexpr SyntaxLexState kLispDefineName = kLispCallHead << 1;
constexpr SyntaxLexState kLispPlainListNext = kLispCallHead << 2;

constexpr SyntaxLexState LispState(int depth, DepthFlags plain_children, bool call_head, bool define_name, bool plain_list_next) noexcept
{
    return PackDepth(depth, plain_children) | (call_head ? kLispCallHead : 0) | (define_name ? kLispDefineName : 0) |
           (plain_list_next ? kLispPlainListNext : 0);
}

void TokenizeLisp(std::string_view text, const LispDialect& d, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens,
                  SyntaxStateTrace* trace)
{
    const size_t n = text.size();
    // 深さごとに「直下のリストは呼び出しではない」を覚える (let の束縛リスト、cond の節の親)
    DepthFlags plain_children = UnpackDepthFlags(state);
    int depth = UnpackDepth(state);
    bool call_head = (state & kLispCallHead) != 0;            // 次のシンボルが呼び出し位置
    bool define_name = (state & kLispDefineName) != 0;        // 次のシンボルが定義名
    bool plain_list_next = (state & kLispPlainListNext) != 0; // 次の ( は引数リストか束縛リスト
    size_t i = begin;
    for (;;) {
        const size_t stop = NextStateStop(trace, n);
        while (i < stop) {
            const char c = text[i];
            if (IsMarkupSpace(c) || c == ',') {
                i++;
                continue;
            }
            if (c == ';') {
                i = EmitRange(tokens, i, SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
            if (c == '"') {
                i = EmitRange(tokens, i, ScanString(text, i, '"', true), SyntaxTokenType::String);
                call_head = define_name = false;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') {
                const bool quoted = i > 0 && (text[i - 1] == '\'' || text[i - 1] == '`');
                const bool plain = quoted || plain_children.test(depth) || plain_list_next;
                depth++;
                // 束縛リストの (x 1) も引数リストの (max-level 3) / ((x integer)) も呼び出しではない
                plain_children.set(depth, c == '(' && plain_list_next);
                call_head = c == '(' && !plain;
                define_name = plain_list_next = false;
                i++;
                continue;
            }
            if (c == ')' || c == ']' || c == '}') {
                depth = std::max(depth - 1, 0);
                call_head = define_name = plain_list_next = false;
                i++;
                continue;
            }
            if (c == '\'' || c == '`') {
                call_head = false;
                i++;
                continue;
            }
            if (c == '#') {
                i = ScanLispDispatch(text, i, tokens);
                call_head = false;
                continue;
            }
            if (d.clojure_char && c == '\\') {
                i = EmitRange(tokens, i, ScanLispCharBody(text, i + 1), SyntaxTokenType::String);
                call_head = false;
                continue;
            }
            if (d.elisp_char && c == '?' && (i == 0 || IsLispDelimiter(text[i - 1]))) {
                const size_t body = i + 1;
                const size_t e = body < n && text[body] == '\\' ? ScanLispCharBody(text, body + 1)
                                 : body < n                     ? NextCodePoint(text, body)
                                                                : n;
                i = EmitRange(tokens, i, e, SyntaxTokenType::String);
                call_head = false;
                continue;
            }
            const size_t e = SkipLispSymbol(text, i);
            const auto sym = text.substr(i, e - i);
            SyntaxTokenType type = SyntaxTokenType::Plain;
            // キーワード :key (Clojure の (:require ...) のような呼び出し位置も含む) と &optional 等
            if (sym.starts_with(':') || sym.starts_with('&')) {
                type = SyntaxTokenType::Type;
            }
            else if (define_name) {
                type = SyntaxTokenType::Function;
                define_name = false;
                plain_list_next = true;
            }
            else if (call_head) {
                type = LispContains(d, d.special, sym) ? SyntaxTokenType::Keyword
                       : IsLispNumber(sym)             ? SyntaxTokenType::Number
                                                       : SyntaxTokenType::Function;
                define_name = LispContains(d, kw::LISP_DEFINERS, sym);
                plain_list_next = LispContains(d, kw::LISP_PLAIN_LIST_FORMS, sym);
                if (d.cond_clauses && ascii_util::iequal(sym, "cond")) {
                    plain_children.set(depth, true);
                }
            }
            else if (LispContains(d, d.constants, sym)) {
                type = SyntaxTokenType::Keyword;
            }
            else if (IsLispNumber(sym)) {
                type = SyntaxTokenType::Number;
            }
            call_head = false;
            EmitRange(tokens, i, e, type);
            i = std::max(e, i + 1);
        }
        if (trace) {
            trace->Set(i, LispState(depth, plain_children, call_head, define_name, plain_list_next));
        }
        if (i >= n) {
            break;
        }
    }
}

inline constexpr LispDialect COMMON_LISP_DIALECT{
    .special = kw::COMMON_LISP_SPECIAL,
    .constants = kw::LISP_NIL_T,
    .case_insensitive = true,
};
inline constexpr LispDialect EMACS_LISP_DIALECT{
    .special = kw::EMACS_LISP_SPECIAL,
    .constants = kw::LISP_NIL_T,
    .elisp_char = true,
};
inline constexpr LispDialect SCHEME_DIALECT{
    .special = kw::SCHEME_SPECIAL,
    .constants = kw::SCHEME_CONSTANTS,
};
inline constexpr LispDialect CLOJURE_DIALECT{
    .special = kw::CLOJURE_SPECIAL,
    .constants = kw::CLOJURE_CONSTANTS,
    .cond_clauses = false,
    .clojure_char = true,
};

// ---------- Vim script ----------

// lua << EOF / let x =<< trim END で始まる、後続行を本文とするヒアドキュメント。
struct VimHeredoc {
    enum class Kind : uint8_t {
        None,
        Lua,   // Lua として色付けする
        Text,  // let の本文 (文字列データ)
        Plain, // python3 / perl / ruby。色付けしない
    };
    Kind kind = Kind::None;
    bool trim = false;
    std::string_view marker;
};

// 変数名・関数名の終端。スコープ g: s: a: 等と autoload の foo#bar を含める。scoped はスコープ付きか。
struct VimName {
    size_t end;
    bool scoped;
};

constexpr VimName ScanVimName(std::string_view text, size_t pos) noexcept
{
    static constexpr CharSet kScopes{ "gsablwtv" };
    size_t i = pos;
    bool scoped = false;
    if (pos + 2 < text.size() && kScopes.contains(text[pos]) && text[pos + 1] == ':' &&
        (IsIdentStart(text[pos + 2]) || IsAsciiDigit(text[pos + 2]))) {
        i = pos + 2;
        scoped = true;
    }
    return { SkipWhile(text, i, [](char c) static noexcept { return IsIdentChar(c) || c == '#'; }), scoped };
}

// <CR> / <leader> / <C-w> / <C-]> 等のキー表記の終端。キー表記でなければ npos。
constexpr size_t ScanVimKeyNotation(std::string_view text, size_t pos) noexcept
{
    constexpr size_t kMaxLength = 32;
    static constexpr CharSet kEnds{ " \t<>" };
    if (pos + 1 >= text.size() || !IsAsciiAlpha(text[pos + 1])) {
        return npos;
    }
    const size_t limit = std::min(text.size(), pos + kMaxLength);
    const size_t i = SkipWhile(text, pos + 2, limit, [](char c) static noexcept { return !kEnds.contains(c); });
    return i < limit && text[i] == '>' ? i + 1 : npos;
}

// autocmd のグループ・イベント (大文字始まり) とパターン、++once 等を読み飛ばす。
// イベントが見つかれば続くコマンドの位置を返し、見つからなければ npos。
constexpr size_t SkipVimAutocmdHead(std::string_view text, size_t pos) noexcept
{
    const auto skip_word = [&](size_t p) {
        return SkipBlanks(text, SkipWhile(text, p, [](char c) static noexcept { return !IsBlank(c); }), text.size());
    };
    size_t i = SkipBlanks(text, pos, text.size());
    bool event = false;
    while (i < text.size() && IsAsciiUpper(text[i])) {
        i = skip_word(i);
        event = true;
    }
    if (!event) {
        return npos;
    }
    i = skip_word(i);
    while (text.substr(i).starts_with("++")) {
        i = skip_word(i);
    }
    return i;
}

// <<[ trim][ eval] MARKER の解析。<< が無ければ Kind::None。
// pos はコマンドの直後 (let は =<< の <)。行の途中のシフト演算子や文字列中の "<<" で始めると
// 文書の残りが本文になるので、空白の後にすぐ << が来るときだけ受け付ける。
constexpr VimHeredoc ParseVimHeredoc(std::string_view text, size_t pos, VimHeredoc::Kind kind) noexcept
{
    const size_t lt = SkipBlanks(text, pos, text.size());
    if (!text.substr(lt).starts_with("<<")) {
        return {};
    }
    VimHeredoc heredoc{ .kind = kind };
    size_t m = SkipBlanks(text, lt + 2, text.size());
    for (const std::string_view option : { std::string_view{ "trim" }, std::string_view{ "eval" } }) {
        if (text.substr(m).starts_with(option) && (m + option.size() == text.size() || IsBlank(text[m + option.size()]))) {
            heredoc.trim = heredoc.trim || option == "trim";
            m = SkipBlanks(text, m + option.size(), text.size());
        }
    }
    const size_t marker_end = TrimEnd(text, m, text.size());
    heredoc.marker = marker_end > m ? text.substr(m, marker_end - m) : std::string_view{ "." };
    return heredoc;
}

// 行頭 (と | の後) をコマンド位置として読み、コマンド名を Keyword、引数を式として色付けする。
// text は行末で切ってある。
VimHeredoc TokenizeVimLine(std::string_view text, size_t i, bool command_pos, bool vim9, std::vector<SyntaxToken>& tokens)
{
    const size_t eol = text.size();
    while (i < eol) {
        if (command_pos) {
            i = SkipWhile(text, i, eol, [](char c) static noexcept { return c == ':' || IsBlank(c); });
            if (i >= eol) {
                break;
            }
            if (text[i] == (vim9 ? '#' : '"')) {
                EmitRange(tokens, i, eol, SyntaxTokenType::Comment);
                break;
            }
            const size_t j = SkipWhile(text, i, eol, IsAsciiAlnum<char>);
            command_pos = false;
            const auto word = text.substr(i, j - i);
            const bool is_map = kw::VIM_MAP_COMMANDS.contains(word);
            if (!is_map && !kw::VIM_COMMANDS.contains(word)) {
                // Plug 'x' 等のユーザー定義コマンド
                static constexpr CharSet kNotCommandEnds{ "(.:" };
                if (IsAsciiUpper(text[i]) && (j == eol || !kNotCommandEnds.contains(text[j]))) {
                    i = EmitRange(tokens, i, j, SyntaxTokenType::Function);
                }
                continue;
            }
            EmitRange(tokens, i, j, SyntaxTokenType::Keyword);
            i = j < eol && text[j] == '!' ? j + 1 : j;
            // マッピングの右辺は Vim のキー列なので、キー表記だけを色付けする (" もコメントではない)
            if (is_map) {
                for (size_t k = text.find('<', i); k < eol; k = text.find('<', k + 1)) {
                    if (const size_t e = ScanVimKeyNotation(text, k); e != npos) {
                        EmitRange(tokens, k, e, SyntaxTokenType::Preprocessor);
                        k = e - 1;
                    }
                }
                break;
            }
            static constexpr std::string_view kFunctionCommands[]{ "fu", "fun", "func", "function", "def" };
            if (std::ranges::contains(kFunctionCommands, word)) {
                const size_t name = SkipBlanks(text, i, eol);
                const size_t e = SkipWhile(text, name, eol, [](char c) static noexcept { return !IsBlank(c) && c != '('; });
                i = EmitRange(tokens, name, e, SyntaxTokenType::Function);
                continue;
            }
            if (word == "au" || word == "autocmd") {
                if (const size_t cmd = SkipVimAutocmdHead(text, i); cmd != npos) {
                    i = cmd;
                    command_pos = true;
                }
                continue;
            }
            if (word == "lua") {
                const auto heredoc = ParseVimHeredoc(text, i, VimHeredoc::Kind::Lua);
                if (heredoc.kind == VimHeredoc::Kind::None) {
                    TokenizeRange<LUA_LEXER_CONFIG>(text, i, kw::LUA_KEYWORDS, kw::LUA_TYPES, tokens);
                }
                return heredoc;
            }
            static constexpr std::string_view kScriptCommands[]{ "perl", "py3", "python3", "ruby" };
            if (std::ranges::contains(kScriptCommands, word)) {
                return ParseVimHeredoc(text, i, VimHeredoc::Kind::Plain);
            }
            static constexpr std::string_view kLetCommands[]{ "let", "var", "const", "cons", "final" };
            // 代入演算子は 1 つなので、前に = がある =<< は右辺の中 (文字列など) で本文の始まりではない。
            const size_t op = std::ranges::contains(kLetCommands, word) ? text.find("=<<", i) : npos;
            if (op != npos && !text.substr(i, op - i).contains('=')) {
                const auto heredoc = ParseVimHeredoc(text, op + 1, VimHeredoc::Kind::Text);
                TokenizeVimLine(text.substr(0, op), i, false, vim9, tokens);
                return heredoc;
            }
            continue;
        }
        const char c = text[i];
        const char next = i + 1 < eol ? text[i + 1] : '\0';
        if (IsBlank(c)) {
            i++;
            continue;
        }
        // || は論理和、単独の | はコマンドの区切り
        if (c == '|') {
            if (next == '|') {
                i += 2;
                continue;
            }
            i++;
            command_pos = true;
            continue;
        }
        // 旧来の Vim script では、同じ行に閉じの " が無い " は行末コメント
        if (c == '"') {
            const size_t e = ScanString(text, i, '"', false);
            if (vim9 || (e > i + 1 && text[e - 1] == '"')) {
                i = EmitRange(tokens, i, e, SyntaxTokenType::String);
                continue;
            }
            EmitRange(tokens, i, eol, SyntaxTokenType::Comment);
            break;
        }
        if (vim9 && c == '#' && (i == 0 || IsBlank(text[i - 1]))) {
            EmitRange(tokens, i, eol, SyntaxTokenType::Comment);
            break;
        }
        if (c == '\'') {
            i = EmitRange(tokens, i, ScanDoubledQuoteString(text, i, '\'', false), SyntaxTokenType::String);
            continue;
        }
        if (c == '<') {
            const size_t e = ScanVimKeyNotation(text, i);
            i = e != npos ? EmitRange(tokens, i, e, SyntaxTokenType::Preprocessor) : i + 1;
            continue;
        }
        // オプション &tabstop / &l:sw、環境変数 $HOME、レジスタ @a
        if ((c == '&' && IsAsciiAlpha(next)) || (c == '$' && IsIdentStart(next))) {
            const size_t e = SkipWhile(text, i + 1, eol, [](char ch) static noexcept { return IsIdentChar(ch) || ch == ':'; });
            i = EmitRange(tokens, i, e, SyntaxTokenType::Type);
            continue;
        }
        if (c == '@' && static_cast<unsigned char>(next) > ' ' && static_cast<unsigned char>(next) < 0x80) {
            i = EmitRange(tokens, i, i + 2, SyntaxTokenType::Type);
            continue;
        }
        if (IsAsciiDigit(c)) {
            i = EmitRange(tokens, i, ScanNumber(text, i), SyntaxTokenType::Number);
            continue;
        }
        if (IsIdentStart(c)) {
            const auto [e, scoped] = ScanVimName(text, i);
            const auto word = text.substr(i, e - i);
            SyntaxTokenType type = SyntaxTokenType::Plain;
            if (e < eol && text[e] == '(') {
                type = SyntaxTokenType::Function;
            }
            else if (scoped) {
                type = SyntaxTokenType::Type;
            }
            else if (kw::VIM_EXPR_KEYWORDS.contains(word)) {
                type = SyntaxTokenType::Keyword;
            }
            else if (vim9 && kw::VIM_TYPES.contains(word)) {
                type = SyntaxTokenType::Type;
            }
            i = EmitRange(tokens, i, e, type);
            continue;
        }
        i++;
    }
    return {};
}

// 先頭のコマンドが vim9script なら Vim9 script とみなす。
constexpr bool StartsWithVim9Script(std::string_view text) noexcept
{
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = SkipToEol(text, pos);
        const size_t i = SkipBlanks(text, pos, eol);
        if (i < eol && text[i] != '"' && text[i] != '#') {
            return text.substr(i, eol - i).starts_with("vim9script");
        }
        pos = eol + 1;
    }
    return false;
}

// ヒアドキュメントの終端の行は長さが決まらないので、状態には 16 bit のハッシュで持つ。途中から始めた本文は
// ハッシュの合う最初の行で終わる (終端でない行が偶然合うのは 6 万行に 1 行程度)。
SyntaxLexState VimMarkerHash(std::string_view marker) noexcept
{
    return static_cast<SyntaxLexState>(RapidHash(marker) & 0xFFFF);
}

// 状態は、途中から始めること・Vim9 script かどうか・ヒアドキュメントの本文の中ならその種類と trim と終端の行のハッシュ。
void TokenizeVim(std::string_view text, bool vim9_language, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens,
                 SyntaxStateTrace* trace)
{
    constexpr SyntaxLexState kResumed = 1;
    constexpr SyntaxLexState kVim9 = 2;
    constexpr int kKindShift = 2;
    constexpr SyntaxLexState kTrim = 1 << 4;
    constexpr int kMarkerShift = 5;
    static_assert(kMarkerShift + 16 <= kSyntaxLexStateBits);
    const size_t n = text.size();
    // Vim9 script かどうかは先頭のコマンドで決まるので、途中から始めるときは状態から取る。
    const bool vim9 = vim9_language || ((state & kResumed) != 0 ? (state & kVim9) != 0 : StartsWithVim9Script(text));
    const SyntaxLexState line_state = kResumed | (vim9 ? kVim9 : 0);
    size_t pos = begin;
    SyntaxLexState tail = line_state;
    // pos からの本文を終端の行 (ends が真) の手前まで色付けし、pos を終端の次の行へ進める。
    const auto scan_heredoc = [&](VimHeredoc::Kind kind, bool trim, SyntaxLexState heredoc_state, auto&& ends) {
        pos = std::min(pos, n);
        // 終端マーカーの行を探し、本文をまとめて色付けする (Lua の複数行コメント等が行をまたぐため)
        size_t body_end = n;
        size_t resume = n;
        tail = heredoc_state;
        for (size_t p = pos; p < n;) {
            const size_t line_end = SkipToEol(text, p);
            const size_t first = trim ? SkipBlanks(text, p, line_end) : p;
            if (ends(text.substr(first, TrimEnd(text, first, line_end) - first))) {
                body_end = p;
                resume = line_end + 1;
                tail = line_state;
                break;
            }
            p = line_end + 1;
        }
        // 終端の行の頭から始めても、本文が空のヒアドキュメントとして同じ結果になる。
        if (trace) {
            trace->SetRange(pos, std::min(resume, n), heredoc_state);
        }
        if (kind == VimHeredoc::Kind::Lua) {
            TokenizeRange<LUA_LEXER_CONFIG>(text.substr(0, body_end), pos, kw::LUA_KEYWORDS, kw::LUA_TYPES, tokens);
        }
        else if (kind == VimHeredoc::Kind::Text) {
            for (size_t p = pos; p < body_end;) {
                const size_t line_end = std::min(SkipToEol(text, p), body_end);
                const size_t first = SkipBlanks(text, p, line_end);
                EmitRange(tokens, first, TrimEnd(text, first, line_end), SyntaxTokenType::String);
                p = line_end + 1;
            }
        }
        pos = resume;
    };
    if (const auto kind = static_cast<VimHeredoc::Kind>((state >> kKindShift) & 3); kind != VimHeredoc::Kind::None) {
        const SyntaxLexState hash = (state >> kMarkerShift) & 0xFFFF;
        scan_heredoc(kind, (state & kTrim) != 0, state, [hash](std::string_view line) { return VimMarkerHash(line) == hash; });
    }
    while (pos < n) {
        if (trace && trace->Wants(pos)) {
            trace->Set(pos, line_state);
        }
        tail = line_state;
        const size_t eol = SkipToEol(text, pos);
        const size_t i = SkipBlanks(text, pos, eol);
        // 行頭の \ は前の行の続き。コマンド位置ではない
        const bool continuation = i < eol && text[i] == '\\';
        const auto heredoc = TokenizeVimLine(text.substr(0, eol), continuation ? i + 1 : i, !continuation, vim9, tokens);
        pos = eol + 1;
        if (heredoc.kind != VimHeredoc::Kind::None) {
            const SyntaxLexState heredoc_state = line_state | (SyntaxLexState{ std::to_underlying(heredoc.kind) } << kKindShift) |
                                                 (heredoc.trim ? kTrim : 0) | (VimMarkerHash(heredoc.marker) << kMarkerShift);
            scan_heredoc(heredoc.kind, heredoc.trim, heredoc_state, [&heredoc](std::string_view line) { return line == heredoc.marker; });
        }
    }
    if (trace) {
        trace->Set(n, tail);
    }
}

} // namespace

SyntaxLanguage DetectLanguage(std::string_view info_string) noexcept
{
    const auto lang = info_string.substr(0, info_string.find_first_of(" \t"));
    if (lang.empty()) {
        return SyntaxLanguage::None;
    }

    struct Alias {
        ascii_util::DocLowercaseLiteral name;
        SyntaxLanguage language;
    };
    static constexpr Alias kAliases[]{
        { "c",             SyntaxLanguage::Cpp        },
        { "h",             SyntaxLanguage::Cpp        },
        { "cc",            SyntaxLanguage::Cpp        },
        { "cpp",           SyntaxLanguage::Cpp        },
        { "c++",           SyntaxLanguage::Cpp        },
        { "cxx",           SyntaxLanguage::Cpp        },
        { "hpp",           SyntaxLanguage::Cpp        },
        { "hxx",           SyntaxLanguage::Cpp        },
        { "js",            SyntaxLanguage::JavaScript },
        { "jsx",           SyntaxLanguage::JavaScript },
        { "javascript",    SyntaxLanguage::JavaScript },
        { "ts",            SyntaxLanguage::TypeScript },
        { "tsx",           SyntaxLanguage::TypeScript },
        { "typescript",    SyntaxLanguage::TypeScript },
        { "py",            SyntaxLanguage::Python     },
        { "python",        SyntaxLanguage::Python     },
        { "go",            SyntaxLanguage::Go         },
        { "golang",        SyntaxLanguage::Go         },
        { "rs",            SyntaxLanguage::Rust       },
        { "rust",          SyntaxLanguage::Rust       },
        { "sh",            SyntaxLanguage::Bash       },
        { "zsh",           SyntaxLanguage::Bash       },
        { "bash",          SyntaxLanguage::Bash       },
        { "shell",         SyntaxLanguage::Bash       },
        { "json",          SyntaxLanguage::Json       },
        { "jsonc",         SyntaxLanguage::Json       },
        { "json5",         SyntaxLanguage::Json       },
        { "cmd",           SyntaxLanguage::Cmd        },
        { "bat",           SyntaxLanguage::Cmd        },
        { "batch",         SyntaxLanguage::Cmd        },
        { "dosbatch",      SyntaxLanguage::Cmd        },
        { "ps1",           SyntaxLanguage::PowerShell },
        { "pwsh",          SyntaxLanguage::PowerShell },
        { "powershell",    SyntaxLanguage::PowerShell },
        { "mermaid",       SyntaxLanguage::Mermaid    },
        { "java",          SyntaxLanguage::Java       },
        { "cs",            SyntaxLanguage::CSharp     },
        { "c#",            SyntaxLanguage::CSharp     },
        { "csharp",        SyntaxLanguage::CSharp     },
        { "kt",            SyntaxLanguage::Kotlin     },
        { "kts",           SyntaxLanguage::Kotlin     },
        { "kotlin",        SyntaxLanguage::Kotlin     },
        { "swift",         SyntaxLanguage::Swift      },
        { "php",           SyntaxLanguage::Php        },
        { "rb",            SyntaxLanguage::Ruby       },
        { "ruby",          SyntaxLanguage::Ruby       },
        { "sql",           SyntaxLanguage::Sql        },
        { "mysql",         SyntaxLanguage::Sql        },
        { "sqlite",        SyntaxLanguage::Sql        },
        { "postgres",      SyntaxLanguage::Sql        },
        { "postgresql",    SyntaxLanguage::Sql        },
        { "psql",          SyntaxLanguage::Sql        },
        { "plsql",         SyntaxLanguage::Sql        },
        { "tsql",          SyntaxLanguage::Sql        },
        { "yml",           SyntaxLanguage::Yaml       },
        { "yaml",          SyntaxLanguage::Yaml       },
        { "toml",          SyntaxLanguage::Toml       },
        { "ini",           SyntaxLanguage::Ini        },
        { "cfg",           SyntaxLanguage::Ini        },
        { "conf",          SyntaxLanguage::Ini        },
        { "dosini",        SyntaxLanguage::Ini        },
        { "properties",    SyntaxLanguage::Ini        },
        { "editorconfig",  SyntaxLanguage::Ini        },
        { "gitconfig",     SyntaxLanguage::Ini        },
        { "htm",           SyntaxLanguage::Html       },
        { "html",          SyntaxLanguage::Html       },
        { "xhtml",         SyntaxLanguage::Html       },
        { "xml",           SyntaxLanguage::Xml        },
        { "svg",           SyntaxLanguage::Xml        },
        { "xsl",           SyntaxLanguage::Xml        },
        { "xslt",          SyntaxLanguage::Xml        },
        { "xaml",          SyntaxLanguage::Xml        },
        { "plist",         SyntaxLanguage::Xml        },
        { "rss",           SyntaxLanguage::Xml        },
        { "css",           SyntaxLanguage::Css        },
        { "scss",          SyntaxLanguage::Scss       },
        { "less",          SyntaxLanguage::Scss       },
        { "diff",          SyntaxLanguage::Diff       },
        { "patch",         SyntaxLanguage::Diff       },
        { "docker",        SyntaxLanguage::Dockerfile },
        { "dockerfile",    SyntaxLanguage::Dockerfile },
        { "containerfile", SyntaxLanguage::Dockerfile },
        { "mk",            SyntaxLanguage::Makefile   },
        { "mak",           SyntaxLanguage::Makefile   },
        { "make",          SyntaxLanguage::Makefile   },
        { "makefile",      SyntaxLanguage::Makefile   },
        { "math",          SyntaxLanguage::LatexMath  },
        { "zig",           SyntaxLanguage::Zig        },
        { "hs",            SyntaxLanguage::Haskell    },
        { "haskell",       SyntaxLanguage::Haskell    },
        { "clj",           SyntaxLanguage::Clojure    },
        { "cljs",          SyntaxLanguage::Clojure    },
        { "cljc",          SyntaxLanguage::Clojure    },
        { "edn",           SyntaxLanguage::Clojure    },
        { "clojure",       SyntaxLanguage::Clojure    },
        { "el",            SyntaxLanguage::EmacsLisp  },
        { "elisp",         SyntaxLanguage::EmacsLisp  },
        { "emacs-lisp",    SyntaxLanguage::EmacsLisp  },
        { "cl",            SyntaxLanguage::CommonLisp },
        { "lisp",          SyntaxLanguage::CommonLisp },
        { "common-lisp",   SyntaxLanguage::CommonLisp },
        { "commonlisp",    SyntaxLanguage::CommonLisp },
        { "scm",           SyntaxLanguage::Scheme     },
        { "scheme",        SyntaxLanguage::Scheme     },
        { "rkt",           SyntaxLanguage::Scheme     },
        { "racket",        SyntaxLanguage::Scheme     },
        { "lua",           SyntaxLanguage::Lua        },
        { "vim",           SyntaxLanguage::Vim        },
        { "viml",          SyntaxLanguage::Vim        },
        { "vimscript",     SyntaxLanguage::Vim        },
        { "vim9",          SyntaxLanguage::Vim9       },
        { "vim9script",    SyntaxLanguage::Vim9       },
    };
    // コードブロック 1 個につき 1 回しか呼ばれないので線形探索で十分 (iequal は長さ不一致で即 false)。
    const auto it = std::ranges::find_if(kAliases, [&](const Alias& a) noexcept {
        return ascii_util::iequal(lang, a.name);
    });
    return it != std::end(kAliases) ? it->language : SyntaxLanguage::None;
}

namespace {

template <LexerConfig Cfg, class Tokens>
bool Lex(std::string_view text, size_t begin, kw::KeywordTable keywords, kw::KeywordTable types, Tokens& tokens)
{
    TokenizeRange<Cfg>(text, begin, keywords, types, tokens);
    return true;
}

// TokenizeRange で解析する言語なら解析して true を返す。状態はトークンの中にしか無いので、トークンの外ならどこからでも始められる。
template <class Tokens>
bool TokenizeByLexerConfig(std::string_view text, size_t begin, SyntaxLanguage language, Tokens& tokens)
{
    switch (language) {
    case SyntaxLanguage::Cpp:
        return Lex<CPP_LEXER_CONFIG>(text, begin, kw::CPP_KEYWORDS, kw::CPP_TYPES, tokens);
    case SyntaxLanguage::Python:
        return Lex<PYTHON_LEXER_CONFIG>(text, begin, kw::PYTHON_KEYWORDS, kw::PYTHON_TYPES, tokens);
    case SyntaxLanguage::JavaScript:
        return Lex<JS_LEXER_CONFIG>(text, begin, kw::JS_KEYWORDS, kw::JS_TYPES, tokens);
    case SyntaxLanguage::Go:
        return Lex<GO_LEXER_CONFIG>(text, begin, kw::GO_KEYWORDS, kw::GO_TYPES, tokens);
    case SyntaxLanguage::Rust:
        return Lex<RUST_LEXER_CONFIG>(text, begin, kw::RUST_KEYWORDS, kw::RUST_TYPES, tokens);
    case SyntaxLanguage::TypeScript:
        return Lex<JS_LEXER_CONFIG>(text, begin, kw::TS_KEYWORDS, kw::TS_TYPES, tokens);
    case SyntaxLanguage::Bash:
        return Lex<BASH_LEXER_CONFIG>(text, begin, kw::BASH_KEYWORDS, kw::BASH_TYPES, tokens);
    case SyntaxLanguage::PowerShell:
        return Lex<PWSH_LEXER_CONFIG>(text, begin, kw::PWSH_KEYWORDS, kw::PWSH_TYPES, tokens);
    case SyntaxLanguage::Cmd:
        return Lex<CMD_LEXER_CONFIG>(text, begin, kw::CMD_KEYWORDS, kw::CMD_TYPES, tokens);
    case SyntaxLanguage::Json:
        return Lex<JSON_LEXER_CONFIG>(text, begin, kw::JSON_KEYWORDS, kw::KeywordTable{}, tokens);
    case SyntaxLanguage::Java:
        return Lex<JAVA_LEXER_CONFIG>(text, begin, kw::JAVA_KEYWORDS, kw::JAVA_TYPES, tokens);
    case SyntaxLanguage::CSharp:
        return Lex<CSHARP_LEXER_CONFIG>(text, begin, kw::CSHARP_KEYWORDS, kw::CSHARP_TYPES, tokens);
    case SyntaxLanguage::Kotlin:
        return Lex<KOTLIN_LEXER_CONFIG>(text, begin, kw::KOTLIN_KEYWORDS, kw::KOTLIN_TYPES, tokens);
    case SyntaxLanguage::Swift:
        return Lex<SWIFT_LEXER_CONFIG>(text, begin, kw::SWIFT_KEYWORDS, kw::SWIFT_TYPES, tokens);
    case SyntaxLanguage::Php:
        return Lex<PHP_LEXER_CONFIG>(text, begin, kw::PHP_KEYWORDS, kw::PHP_TYPES, tokens);
    case SyntaxLanguage::Ruby:
        return Lex<RUBY_LEXER_CONFIG>(text, begin, kw::RUBY_KEYWORDS, kw::RUBY_TYPES, tokens);
    case SyntaxLanguage::Sql:
        return Lex<SQL_LEXER_CONFIG>(text, begin, kw::SQL_KEYWORDS, kw::SQL_TYPES, tokens);
    case SyntaxLanguage::Zig:
        return Lex<ZIG_LEXER_CONFIG>(text, begin, kw::ZIG_KEYWORDS, kw::ZIG_TYPES, tokens);
    case SyntaxLanguage::Lua:
        return Lex<LUA_LEXER_CONFIG>(text, begin, kw::LUA_KEYWORDS, kw::LUA_TYPES, tokens);
    default:
        return false;
    }
}

// 状態を持たない専用 lexer の解析だった。トークンの頭からなら、どこからでも状態なしで始め直せる。
void MarkStateless(SyntaxStateTrace* trace) noexcept
{
    if (trace) {
        trace->SetStateless();
    }
}

// 専用 lexer の言語を begin から解析する。
void TokenizeStateful(std::string_view text, SyntaxLanguage language, size_t begin, SyntaxLexState state, std::vector<SyntaxToken>& tokens,
                      SyntaxStateTrace* trace)
{
    switch (language) {
    case SyntaxLanguage::Yaml:
        return TokenizeYaml(text, begin, state, tokens, trace);
    case SyntaxLanguage::Toml:
        return TokenizeToml(text, begin, state, tokens, trace);
    case SyntaxLanguage::Ini:
        MarkStateless(trace);
        return TokenizeIni(text, begin, tokens);
    case SyntaxLanguage::Html:
        return TokenizeMarkup<true>(text, begin, state, tokens, trace);
    case SyntaxLanguage::Xml:
        return TokenizeMarkup<false>(text, begin, state, tokens, trace);
    case SyntaxLanguage::Css:
        return TokenizeCssRange<false>(text, begin, state, tokens, trace);
    case SyntaxLanguage::Scss:
        return TokenizeCssRange<true>(text, begin, state, tokens, trace);
    case SyntaxLanguage::Diff:
        return TokenizeDiff(text, begin, state, tokens, trace);
    case SyntaxLanguage::Dockerfile:
        return TokenizeDockerfile(text, begin, state, tokens, trace);
    case SyntaxLanguage::Makefile:
        return TokenizeMakefile(text, begin, state, tokens, trace);
    case SyntaxLanguage::Haskell:
        MarkStateless(trace);
        return TokenizeHaskell(text, begin, tokens);
    case SyntaxLanguage::Clojure:
        return TokenizeLisp(text, CLOJURE_DIALECT, begin, state, tokens, trace);
    case SyntaxLanguage::EmacsLisp:
        return TokenizeLisp(text, EMACS_LISP_DIALECT, begin, state, tokens, trace);
    case SyntaxLanguage::CommonLisp:
        return TokenizeLisp(text, COMMON_LISP_DIALECT, begin, state, tokens, trace);
    case SyntaxLanguage::Scheme:
        return TokenizeLisp(text, SCHEME_DIALECT, begin, state, tokens, trace);
    case SyntaxLanguage::Vim:
        return TokenizeVim(text, false, begin, state, tokens, trace);
    case SyntaxLanguage::Vim9:
        return TokenizeVim(text, true, begin, state, tokens, trace);
    default:
        MarkStateless(trace);
        return;
    }
}

} // namespace

void SyntaxStateTrace::Reset(SyntaxLexState entry, size_t text_size, std::span<const size_t> wanted)
{
    assert(std::ranges::is_sorted(wanted));
    wanted_.assign_range(wanted);
    points_.clear();
    next_wanted_ = wanted_.empty() ? kNone : wanted_.front();
    text_size_ = text_size;
    entry_ = { 0, entry };
    settled_end_ = { text_size, 0 };
    unsettled_ = false;
    stateless_ = false;
    outer = 0;
}

void SyntaxStateTrace::Set(size_t pos, SyntaxLexState state)
{
    const Point point{ pos, state | outer };
    while (points_.size() < wanted_.size() && wanted_[points_.size()] <= pos) {
        points_.push_back(point);
    }
    next_wanted_ = points_.size() < wanted_.size() ? wanted_[points_.size()] : kNone;
    if (pos == text_size_ && !unsettled_) {
        settled_end_ = point;
    }
}

void SyntaxStateTrace::SetRange(size_t begin, size_t end, SyntaxLexState state)
{
    while (points_.size() < wanted_.size() && wanted_[points_.size()] < end) {
        points_.push_back({ std::max(begin, wanted_[points_.size()]), state | outer });
    }
    next_wanted_ = points_.size() < wanted_.size() ? wanted_[points_.size()] : kNone;
    if (end == text_size_) {
        Set(end, state);
    }
}

void SyntaxStateTrace::SetUnsettled(size_t pos, SyntaxLexState state, size_t scanned_size) noexcept
{
    if (scanned_size == text_size_ && !unsettled_) {
        settled_end_ = { pos, state | outer };
        unsettled_ = true;
    }
}

SyntaxStateTrace::Point SyntaxStateTrace::For(size_t wanted) const noexcept
{
    if (stateless_) {
        return { wanted, 0 };
    }
    // lexer は末尾でも Set を呼ぶので、解析が届かずに残る位置は無いはず。
    const auto index = static_cast<size_t>(std::ranges::lower_bound(wanted_, wanted) - wanted_.begin());
    assert(index < points_.size() && wanted_[index] == wanted);
    return index < points_.size() ? points_[index] : Point{ wanted, 0 };
}

SyntaxStateTrace::Point SyntaxStateTrace::RestartFor(size_t token_start) const noexcept
{
    if (stateless_) {
        return { token_start, 0 };
    }
    const auto after = std::ranges::upper_bound(points_, token_start, {}, &Point::pos);
    return after != points_.begin() ? *std::prev(after) : entry_;
}

void AppendTokens(std::string_view text, SyntaxLanguage language, std::vector<SyntaxToken>& tokens, size_t begin, SyntaxLexState state)
{
    if (!TokenizeByLexerConfig(text, begin, language, tokens)) {
        TokenizeStateful(text, language, begin, state, tokens, nullptr);
    }
}

std::vector<SyntaxToken> TokensCrossing(std::string_view text, SyntaxLanguage language, std::span<const size_t> boundaries,
                                        SyntaxLexState state, SyntaxStateTrace* trace)
{
    CrossingTokens crossing(text.size(), boundaries);
    if (TokenizeByLexerConfig(text, 0, language, crossing)) {
        if (trace) {
            trace->Reset(0, text.size(), {});
            trace->SetStateless();
        }
        return crossing.Take();
    }
    if (trace) {
        // 末尾に届いたトークンは最後の行にあることが多いので、そこから解析し直せるように最後の行の頭での状態も残す。
        std::vector<size_t> wanted(boundaries.begin(), boundaries.end());
        if (const size_t newline = text.rfind('\n'); newline != npos) {
            wanted.insert(std::ranges::lower_bound(wanted, newline + 1), newline + 1);
        }
        const auto [first, last] = std::ranges::unique(wanted);
        wanted.erase(first, last);
        trace->Reset(state, text.size(), wanted);
    }
    std::vector<SyntaxToken> tokens;
    tokens.reserve(text.size() / 32);
    TokenizeStateful(text, language, 0, state, tokens, trace);
    return tokens;
}

std::vector<SyntaxToken> Tokenize(std::string_view text, SyntaxLanguage language)
{
    std::vector<SyntaxToken> tokens;
    if (text.empty() || language == SyntaxLanguage::None || IsDiagramLanguage(language)) {
        return tokens;
    }
    tokens.reserve(text.size() / 32);
    AppendTokens(text, language, tokens);
    return tokens;
}
