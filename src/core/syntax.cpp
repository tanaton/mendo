#include "syntax.h"
#include "syntax_keywords.h"
#include "ascii_util.h"
#include <algorithm>
#include <type_traits>
#include <utility>

namespace kw = syntax_keywords;

namespace {

using ascii_util::IsAsciiDigit;

// 識別子先頭文字: ASCII 英字 + '_' に加え、CJK 等の非 ASCII (UTF-8 の 0x80 以上の byte) も許可する。
// char は signed のため unsigned 比較を明示しないと非 ASCII が負値で弾かれる。
constexpr bool IsIdentStart(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<std::make_unsigned_t<char>>(c) >= 0x80;
}

constexpr bool IsIdentChar(char c) noexcept
{
    return IsIdentStart(c) || IsAsciiDigit(c);
}

constexpr void EmitToken(std::pmr::vector<SyntaxToken>& tokens, uint32_t start, uint32_t length, SyntaxTokenType type)
{
    if (length > 0) {
        tokens.emplace_back(start, length, type);
    }
}

// pos から最初の '\n' まで（または末尾まで）一気に進める。返り値は '\n' の位置（未消費）または text.size()。
constexpr size_t SkipToEol(std::string_view text, size_t pos) noexcept
{
    const auto p = text.find('\n', pos);
    return p == std::string_view::npos ? text.size() : p;
}

// pos から chars に含まれない最初の文字位置を返す（無ければ text.size()）。
constexpr size_t SkipChars(std::string_view text, size_t pos, std::string_view chars) noexcept
{
    const auto p = text.find_first_not_of(chars, pos);
    return p == std::string_view::npos ? text.size() : p;
}

// posから始まる文字列リテラルをスキャン（posは開始引用符を指す）。
// 閉じ引用符の次の位置を返す（未終端の場合はテキストの末尾）。
constexpr size_t ScanString(std::string_view text, size_t pos, char quote, bool allow_multiline, bool handle_escape = true) noexcept
{
    size_t i = pos + 1;
    while (i < text.size()) {
        if (handle_escape && text[i] == '\\') {
            i += 2;
            if (i > text.size()) {
                i = text.size();
            }
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

// C++ 生文字列 R"DELIM(...)DELIM" をスキャン。quote_pos は開きの '"'、paren は '(' の位置。
// 終端 )DELIM" を見つけたらその直後位置を返し、未終端なら text.size() を返す。
constexpr size_t ScanCppRawString(std::string_view text, size_t quote_pos, size_t paren) noexcept
{
    const std::string_view delim = text.substr(quote_pos + 1, paren - quote_pos - 1);
    const size_t end_marker_len = 1 + delim.size() + 1; // ')' + delim + '"'
    size_t k = paren + 1;
    while (k + end_marker_len <= text.size()) {
        const size_t found = text.find(')', k);
        if (found == std::string_view::npos || found + end_marker_len > text.size()) {
            break;
        }
        if (text[found + 1 + delim.size()] != '"') {
            k = found + 1;
            continue;
        }
        if (delim.empty() || text.compare(found + 1, delim.size(), delim) == 0) {
            return found + end_marker_len;
        }
        k = found + 1;
    }
    return text.size();
}

// Pythonのトリプルクォート文字列をスキャン（pos はトリプルクォートの最初の引用符を指す）。
constexpr size_t ScanTripleQuote(std::string_view text, size_t pos, char quote) noexcept
{
    size_t i = pos + 3;
    const char delims_buf[]{ '\\', quote };
    const std::string_view delims(delims_buf, 2);
    while (i + 2 < text.size()) {
        const auto p = text.find_first_of(delims, i);
        if (p == std::string_view::npos || p + 2 >= text.size()) {
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

// posから始まる数値リテラルをスキャン。
constexpr size_t ScanNumber(std::string_view text, size_t pos) noexcept
{
    constexpr auto kHexDigits = "0123456789abcdefABCDEF'";
    constexpr auto kBinDigits = "01'";
    constexpr auto kOctDigits = "01234567";
    constexpr auto kDecDigitsSep = "0123456789'";
    constexpr auto kDecDigits = "0123456789";
    constexpr auto kIntSuffix = "uUlL";
    constexpr auto kNumSuffix = "fFlLuUn"; // 'n' は JS BigInt 用

    size_t i = pos;

    // 0x, 0b, 0o プレフィックスの処理
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

    // 整数 / 浮動小数点
    i = SkipChars(text, i, kDecDigitsSep);

    // 小数点
    if (i < text.size() && text[i] == '.') {
        i = SkipChars(text, i + 1, kDecDigitsSep);
    }

    // 指数部
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        i = SkipChars(text, i, kDecDigits);
    }

    // サフィックス (f, F, l, L, u, U, n)
    return SkipChars(text, i, kNumSuffix);
}

// [start, end)の識別子の後に'('が続くか確認（空白をスキップ）。
constexpr bool IsFollowedByParen(std::string_view text, size_t end) noexcept
{
    const auto i = text.find_first_not_of(" \t", end);
    return i != std::string_view::npos && text[i] == '(';
}

// 行頭 '#' から改行までをスキャンする。直前が '\' の改行は行継続として読み進める。
// 返り値は終端の '\n' の位置（未消費）または text.size()。
constexpr size_t ScanPreprocessorLine(std::string_view text, size_t pos) noexcept
{
    while (pos < text.size()) {
        const size_t p = SkipToEol(text, pos);
        if (p == text.size() || p == 0 || text[p - 1] != '\\') {
            return p;
        }
        pos = p + 1;
    }
    return pos;
}

// posから始まるブロックコメントをスキャン（posは開始ペアの最初の文字を指す）。
// 閉じペアの次の位置を返す。未終端の場合はtext.size()を返す。
constexpr size_t ScanBlockComment(std::string_view text, size_t pos, char close1, char close2) noexcept
{
    // close1 はソース中で比較的レアな文字（'*' や '#'）なので、find で間引いてから close2 を確認する。
    size_t i = pos + 2;
    while (true) {
        const auto p = text.find(close1, i);
        if (p == std::string_view::npos || p + 1 >= text.size()) {
            return text.size();
        }
        if (text[p + 1] == close2) {
            return p + 2;
        }
        i = p + 1;
    }
}

struct LexerConfig {
    bool line_comment_slash = false;   // //
    bool block_comment = false;        // /* */
    bool hash_comment = false;         // #
    bool preprocessor = false;         // 行頭の#
    bool triple_quote = false;         // """ '''
    bool raw_string = false;           // C++ R"(...)"
    bool backtick_string = false;      // `
    bool angle_block_comment = false;  // <# #>
    bool double_colon_comment = false; // ::
    bool rem_comment = false;          // REM
    bool case_insensitive = false;     // 大文字小文字を区別しないキーワードマッチング
    bool skip_single_quote = false;    // 'を文字列デリミタとして扱わない
    bool raw_backtick = false;         // エスケープなしのバッククォート文字列（Go）
};

// LexerConfig を NTTP として渡すことで `if constexpr (Cfg.X)` で死分岐をコンパイル時に消し、
// 各言語の lexer は不要なチェックを含まない最小コードにインスタンス化される。
template <LexerConfig Cfg>
std::pmr::vector<SyntaxToken> TokenizeImpl(
    std::string_view text,
    kw::KeywordTable keywords,
    kw::KeywordTable types)
{
    // 行頭判定が必要な言語のみフラグを保持する。それ以外では at_line_start の維持コストを払わない。
    constexpr bool kNeedAtLineStart = Cfg.preprocessor || Cfg.double_colon_comment || Cfg.rem_comment;

    std::pmr::vector<SyntaxToken> tokens;
    tokens.reserve(text.size() / 16);
    size_t i = 0;
    // 各トークン分岐は flush_plain() → スキャン → emit_from() の順なので、
    // 未確定の Plain 区間は常に [直前トークン末尾, i) になる。
    uint32_t plain_start = 0;
    // 行頭判定の状態フラグ。pos i において、現在行の開始から i までが空白のみなら true。
    // 反復の開始時点で位置 i の at-line-start 状態を表す。i を進めた後に更新する。
    [[maybe_unused]] bool at_line_start = true;
    [[maybe_unused]] std::pmr::string ci_buf; // case_insensitive 用の再利用バッファ
    if constexpr (Cfg.case_insensitive) {
        // 典型的なキーワード最長（PowerShell の `ForEach-Object` 等）を事前確保
        ci_buf.reserve(64);
    }

    const auto flush_plain = [&]() {
        EmitToken(tokens, plain_start, static_cast<uint32_t>(i) - plain_start, SyntaxTokenType::Plain);
    };

    // [start, i) を確定する。スキャン済みトークンは非空白で終わる (改行は未消費) ため行頭状態も解除する。
    const auto emit_from = [&](size_t start, SyntaxTokenType type) {
        EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), type);
        plain_start = static_cast<uint32_t>(i);
        at_line_start = false;
    };

    // 現在位置からスキャン済みの終端 token_end までを type のトークンとして確定する。
    const auto emit_until = [&](size_t token_end, SyntaxTokenType type) {
        flush_plain();
        const size_t start = i;
        i = token_end;
        emit_from(start, type);
    };

    while (i < text.size()) {
        const char c = text[i];

        // 1. 行コメント: //
        if constexpr (Cfg.line_comment_slash) {
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 1b. アングルブロックコメント: <# #>（PowerShell）
        if constexpr (Cfg.angle_block_comment) {
            if (c == '<' && i + 1 < text.size() && text[i + 1] == '#') {
                emit_until(ScanBlockComment(text, i, '#', '>'), SyntaxTokenType::Comment);
                continue;
            }
        }

        if constexpr (Cfg.hash_comment && !Cfg.preprocessor) {
            if (c == '#') {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 2. ブロックコメント: /* */
        if constexpr (Cfg.block_comment) {
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                emit_until(ScanBlockComment(text, i, '*', '/'), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 3. プリプロセッサ: 行頭の#（C/C++）
        if constexpr (Cfg.preprocessor) {
            if (c == '#' && at_line_start) {
                emit_until(ScanPreprocessorLine(text, i), SyntaxTokenType::Preprocessor);
                continue;
            }
        }

        // 3b. ダブルコロンコメント: 行頭の::（cmd）
        if constexpr (Cfg.double_colon_comment) {
            if (c == ':' && i + 1 < text.size() && text[i + 1] == ':' && at_line_start) {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 3c. REMコメント: 行頭のREM（cmd）
        if constexpr (Cfg.rem_comment) {
            if (at_line_start && ascii_util::istarts_with(text.substr(i), "rem") &&
                (i + 3 >= text.size() || !IsIdentChar(text[i + 3]))) {
                emit_until(SkipToEol(text, i), SyntaxTokenType::Comment);
                continue;
            }
        }

        // 4. トリプルクォート文字列（Python）
        if constexpr (Cfg.triple_quote) {
            if ((c == '"' || c == '\'') && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
                emit_until(ScanTripleQuote(text, i, c), SyntaxTokenType::String);
                continue;
            }
        }

        // 5. 文字列リテラル
        if (c == '"' || (c == '\'' && !Cfg.skip_single_quote)) {
            // 生文字列 R"(...)" の R は識別子として step 8 で消費されるため、ここでは通常の文字列として扱う。
            emit_until(ScanString(text, i, c, false), SyntaxTokenType::String);
            continue;
        }

        // 6. バッククォートテンプレートリテラル（JS）
        if constexpr (Cfg.backtick_string) {
            if (c == '`') {
                emit_until(ScanString(text, i, '`', true, !Cfg.raw_backtick), SyntaxTokenType::String);
                continue;
            }
        }

        // 7. 数値
        if (IsAsciiDigit(c) || (c == '.' && i + 1 < text.size() && IsAsciiDigit(text[i + 1]))) {
            emit_until(ScanNumber(text, i), SyntaxTokenType::Number);
            continue;
        }

        // 8. 識別子とキーワード
        if (IsIdentStart(c)) {
            flush_plain();
            const size_t start = i;
            while (i < text.size() && IsIdentChar(text[i])) {
                i++;
            }

            const std::string_view word(text.data() + start, i - start);

            if constexpr (Cfg.raw_string) {
                // R"..." の R はプレフィックスとして String トークンに含める。
                // word == "R" なら直前が非識別子文字であることも保証される。
                if (word == "R" && i < text.size() && text[i] == '"') {
                    const size_t paren = text.find('(', i + 1);
                    if (paren != std::string_view::npos) {
                        i = ScanCppRawString(text, i, paren);
                    }
                    else {
                        i = ScanString(text, i, '"', false);
                    }
                    emit_from(start, SyntaxTokenType::String);
                    continue;
                }
            }

            std::string_view lookup_word = word;
            if constexpr (Cfg.case_insensitive) {
                if (ascii_util::HasAsciiUpper(word.data(), word.size())) {
                    ci_buf.resize(word.size());
                    ascii_util::AsciiToLowerOnly(word.data(), ci_buf.data(), word.size());
                    lookup_word = ci_buf;
                }
            }

            SyntaxTokenType tt = SyntaxTokenType::Plain;
            if (keywords.contains(lookup_word)) {
                tt = SyntaxTokenType::Keyword;
            }
            else if (types.contains(lookup_word)) {
                tt = SyntaxTokenType::Type;
            }
            else if (IsFollowedByParen(text, i)) {
                tt = SyntaxTokenType::Function;
            }

            emit_from(start, tt);
            continue;
        }

        // 9. その他: プレーンとして蓄積
        // 行頭判定を読む言語のみフラグを更新する。それ以外は per-char ストアを丸ごと省略。
        if constexpr (kNeedAtLineStart) {
            if (c == '\n') {
                at_line_start = true;
            }
            else if (c != ' ' && c != '\t') {
                at_line_start = false;
            }
        }
        i++;
    }

    flush_plain();
    return tokens;
}

inline constexpr LexerConfig CPP_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .preprocessor = true,
    .raw_string = true,
};
inline constexpr LexerConfig PYTHON_LEXER_CONFIG{
    .hash_comment = true,
    .triple_quote = true,
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
inline constexpr LexerConfig TS_LEXER_CONFIG{
    .line_comment_slash = true,
    .block_comment = true,
    .backtick_string = true,
};
inline constexpr LexerConfig BASH_LEXER_CONFIG{
    .hash_comment = true,
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
        { "c", SyntaxLanguage::Cpp },
        { "h", SyntaxLanguage::Cpp },
        { "cc", SyntaxLanguage::Cpp },
        { "cpp", SyntaxLanguage::Cpp },
        { "c++", SyntaxLanguage::Cpp },
        { "cxx", SyntaxLanguage::Cpp },
        { "hpp", SyntaxLanguage::Cpp },
        { "hxx", SyntaxLanguage::Cpp },
        { "js", SyntaxLanguage::JavaScript },
        { "jsx", SyntaxLanguage::JavaScript },
        { "javascript", SyntaxLanguage::JavaScript },
        { "ts", SyntaxLanguage::TypeScript },
        { "tsx", SyntaxLanguage::TypeScript },
        { "typescript", SyntaxLanguage::TypeScript },
        { "py", SyntaxLanguage::Python },
        { "python", SyntaxLanguage::Python },
        { "go", SyntaxLanguage::Go },
        { "golang", SyntaxLanguage::Go },
        { "rs", SyntaxLanguage::Rust },
        { "rust", SyntaxLanguage::Rust },
        { "sh", SyntaxLanguage::Bash },
        { "zsh", SyntaxLanguage::Bash },
        { "bash", SyntaxLanguage::Bash },
        { "shell", SyntaxLanguage::Bash },
        { "json", SyntaxLanguage::Json },
        { "jsonc", SyntaxLanguage::Json },
        { "json5", SyntaxLanguage::Json },
        { "cmd", SyntaxLanguage::Cmd },
        { "bat", SyntaxLanguage::Cmd },
        { "batch", SyntaxLanguage::Cmd },
        { "dosbatch", SyntaxLanguage::Cmd },
        { "ps1", SyntaxLanguage::PowerShell },
        { "pwsh", SyntaxLanguage::PowerShell },
        { "powershell", SyntaxLanguage::PowerShell },
        { "mermaid", SyntaxLanguage::Mermaid },
    };
    // コードブロック 1 個につき 1 回しか呼ばれないので線形探索で十分 (iequal は長さ不一致で即 false)。
    const auto it = std::ranges::find_if(kAliases, [&](const Alias& a) noexcept {
        return ascii_util::iequal(lang, a.name);
    });
    return it != std::end(kAliases) ? it->language : SyntaxLanguage::None;
}

std::pmr::vector<SyntaxToken> Tokenize(std::string_view text, SyntaxLanguage language)
{
    if (text.empty()) {
        return {};
    }
    switch (language) {
    case SyntaxLanguage::Cpp:
        return TokenizeImpl<CPP_LEXER_CONFIG>(text, kw::CPP_KEYWORDS, kw::CPP_TYPES);
    case SyntaxLanguage::Python:
        return TokenizeImpl<PYTHON_LEXER_CONFIG>(text, kw::PYTHON_KEYWORDS, kw::PYTHON_TYPES);
    case SyntaxLanguage::JavaScript:
        return TokenizeImpl<JS_LEXER_CONFIG>(text, kw::JS_KEYWORDS, kw::JS_TYPES);
    case SyntaxLanguage::Go:
        return TokenizeImpl<GO_LEXER_CONFIG>(text, kw::GO_KEYWORDS, kw::GO_TYPES);
    case SyntaxLanguage::Rust:
        return TokenizeImpl<RUST_LEXER_CONFIG>(text, kw::RUST_KEYWORDS, kw::RUST_TYPES);
    case SyntaxLanguage::TypeScript:
        return TokenizeImpl<TS_LEXER_CONFIG>(text, kw::TS_KEYWORDS, kw::TS_TYPES);
    case SyntaxLanguage::Bash:
        return TokenizeImpl<BASH_LEXER_CONFIG>(text, kw::BASH_KEYWORDS, kw::BASH_TYPES);
    case SyntaxLanguage::PowerShell:
        return TokenizeImpl<PWSH_LEXER_CONFIG>(text, kw::PWSH_KEYWORDS, kw::PWSH_TYPES);
    case SyntaxLanguage::Cmd:
        return TokenizeImpl<CMD_LEXER_CONFIG>(text, kw::CMD_KEYWORDS, kw::CMD_TYPES);
    case SyntaxLanguage::Json:
        return TokenizeImpl<JSON_LEXER_CONFIG>(text, kw::JSON_KEYWORDS, kw::KeywordTable{});
    case SyntaxLanguage::None:
    case SyntaxLanguage::Mermaid:
    case SyntaxLanguage::LatexMath:
        return {};
    }
    std::unreachable();
}
