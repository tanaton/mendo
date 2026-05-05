#include "syntax.h"
#include "syntax_keywords.h"
#include "ascii_util.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>

using namespace std::literals;
using syntax_keywords::BASH_KEYWORDS;
using syntax_keywords::BASH_TYPES;
using syntax_keywords::CMD_KEYWORDS;
using syntax_keywords::CMD_TYPES;
using syntax_keywords::CPP_KEYWORDS;
using syntax_keywords::CPP_TYPES;
using syntax_keywords::GO_KEYWORDS;
using syntax_keywords::GO_TYPES;
using syntax_keywords::JS_KEYWORDS;
using syntax_keywords::JS_TYPES;
using syntax_keywords::JSON_KEYWORDS;
using syntax_keywords::KeywordTable;
using syntax_keywords::PWSH_KEYWORDS;
using syntax_keywords::PWSH_TYPES;
using syntax_keywords::PYTHON_KEYWORDS;
using syntax_keywords::PYTHON_TYPES;
using syntax_keywords::RUST_KEYWORDS;
using syntax_keywords::RUST_TYPES;
using syntax_keywords::TS_KEYWORDS;
using syntax_keywords::TS_TYPES;

namespace {

using ascii_util::IsAsciiDigit;

// 識別子先頭文字: ASCII 英字 + '_' に加え、CJK 等の非 ASCII (>= U+0080) も許可する。
// ascii_util の純粋 ASCII ヘルパに乗らないので syntax 固有として残す。
constexpr bool IsIdentStart(mendo::doc_char c) noexcept
{
    return (c >= MENDO_LIT('a') && c <= MENDO_LIT('z')) || (c >= MENDO_LIT('A') && c <= MENDO_LIT('Z')) || c == MENDO_LIT('_') || c >= 0x80;
}

constexpr bool IsIdentChar(mendo::doc_char c) noexcept
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
constexpr size_t SkipToEol(mendo::doc_string_view text, size_t pos) noexcept
{
    const auto p = text.find(MENDO_LIT('\n'), pos);
    return p == mendo::doc_string_view::npos ? text.size() : p;
}

// pos から chars に含まれない最初の文字位置を返す（無ければ text.size()）。
constexpr size_t SkipChars(mendo::doc_string_view text, size_t pos, mendo::doc_string_view chars) noexcept
{
    const auto p = text.find_first_not_of(chars, pos);
    return p == mendo::doc_string_view::npos ? text.size() : p;
}

// posから始まる文字列リテラルをスキャン（posは開始引用符を指す）。
// 閉じ引用符の次の位置を返す（未終端の場合はテキストの末尾）。
constexpr size_t ScanString(mendo::doc_string_view text, size_t pos, mendo::doc_char quote, bool allow_multiline, bool handle_escape = true) noexcept
{
    size_t i = pos + 1;
    while (i < text.size()) {
        if (handle_escape && text[i] == MENDO_LIT('\\')) {
            i += 2;
            if (i > text.size()) {
                i = text.size();
            }
        }
        else if (text[i] == quote) {
            return i + 1;
        }
        else if (!allow_multiline && text[i] == MENDO_LIT('\n')) {
            return i; // 未終端
        }
        else {
            i++;
        }
    }
    return i;
}

// Pythonのトリプルクォート文字列をスキャン（pos はトリプルクォートの最初の引用符を指す）。
constexpr size_t ScanTripleQuote(mendo::doc_string_view text, size_t pos, mendo::doc_char quote) noexcept
{
    size_t i = pos + 3;
    const mendo::doc_char delims_buf[]{ MENDO_LIT('\\'), quote };
    const mendo::doc_string_view delims(delims_buf, 2);
    while (i + 2 < text.size()) {
        const auto p = text.find_first_of(delims, i);
        if (p == mendo::doc_string_view::npos || p + 2 >= text.size()) {
            return text.size();
        }
        if (text[p] == MENDO_LIT('\\')) {
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
constexpr size_t ScanNumber(mendo::doc_string_view text, size_t pos) noexcept
{
    constexpr auto kHexDigits = MENDO_LIT("0123456789abcdefABCDEF'");
    constexpr auto kBinDigits = MENDO_LIT("01'");
    constexpr auto kOctDigits = MENDO_LIT("01234567");
    constexpr auto kDecDigitsSep = MENDO_LIT("0123456789'");
    constexpr auto kDecDigits = MENDO_LIT("0123456789");
    constexpr auto kIntSuffix = MENDO_LIT("uUlL");
    constexpr auto kNumSuffix = MENDO_LIT("fFlLuUn"); // 'n' は JS BigInt 用

    size_t i = pos;

    // 0x, 0b, 0o プレフィックスの処理
    if (i + 1 < text.size() && text[i] == MENDO_LIT('0')) {
        const mendo::doc_char next = text[i + 1];
        if (next == MENDO_LIT('x') || next == MENDO_LIT('X')) {
            i = SkipChars(text, i + 2, kHexDigits);
            return SkipChars(text, i, kIntSuffix);
        }
        if (next == MENDO_LIT('b') || next == MENDO_LIT('B')) {
            return SkipChars(text, i + 2, kBinDigits);
        }
        if (next == MENDO_LIT('o') || next == MENDO_LIT('O')) {
            return SkipChars(text, i + 2, kOctDigits);
        }
    }

    // 整数 / 浮動小数点
    i = SkipChars(text, i, kDecDigitsSep);

    // 小数点
    if (i < text.size() && text[i] == MENDO_LIT('.')) {
        i = SkipChars(text, i + 1, kDecDigitsSep);
    }

    // 指数部
    if (i < text.size() && (text[i] == MENDO_LIT('e') || text[i] == MENDO_LIT('E'))) {
        i++;
        if (i < text.size() && (text[i] == MENDO_LIT('+') || text[i] == MENDO_LIT('-'))) {
            i++;
        }
        i = SkipChars(text, i, kDecDigits);
    }

    // サフィックス (f, F, l, L, u, U, n)
    return SkipChars(text, i, kNumSuffix);
}

// [start, end)の識別子の後に'('が続くか確認（空白をスキップ）。
constexpr bool IsFollowedByParen(mendo::doc_string_view text, size_t end) noexcept
{
    const auto i = text.find_first_not_of(MENDO_LIT(" \t"), end);
    return i != mendo::doc_string_view::npos && text[i] == MENDO_LIT('(');
}

// posから始まるブロックコメントをスキャン（posは開始ペアの最初の文字を指す）。
// 閉じペアの次の位置を返す。未終端の場合はtext.size()を返す。
constexpr size_t ScanBlockComment(mendo::doc_string_view text, size_t pos, mendo::doc_char close1, mendo::doc_char close2) noexcept
{
    // close1 はソース中で比較的レアな文字（'*' や '#'）なので、find で間引いてから close2 を確認する。
    size_t i = pos + 2;
    while (true) {
        const auto p = text.find(close1, i);
        if (p == mendo::doc_string_view::npos || p + 1 >= text.size()) {
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
    mendo::doc_string_view text,
    KeywordTable keywords,
    KeywordTable types)
{
    // 行頭判定が必要な言語のみフラグを保持する。それ以外では at_line_start の維持コストを払わない。
    constexpr bool kNeedAtLineStart = Cfg.preprocessor || Cfg.double_colon_comment || Cfg.rem_comment;

    std::pmr::vector<SyntaxToken> tokens;
    tokens.reserve(text.size() / 8);
    size_t i = 0;
    uint32_t plain_start = 0;
    bool in_plain = false;
    // 行頭判定の状態フラグ。pos i において、現在行の開始から i までが空白のみなら true。
    // 反復の開始時点で位置 i の at-line-start 状態を表す。i を進めた後に更新する。
    [[maybe_unused]] bool at_line_start = true;
    [[maybe_unused]] mendo::doc_string ci_buf; // case_insensitive 用の再利用バッファ
    if constexpr (Cfg.case_insensitive) {
        // 典型的なキーワード最長（PowerShell の `ForEach-Object` 等）を事前確保
        ci_buf.reserve(64);
    }

    const auto flush_plain = [&]() {
        if (in_plain && static_cast<uint32_t>(i) > plain_start) {
            EmitToken(tokens, plain_start, static_cast<uint32_t>(i) - plain_start, SyntaxTokenType::Plain);
            in_plain = false;
        }
    };

    const auto start_plain = [&]() {
        if (!in_plain) {
            plain_start = static_cast<uint32_t>(i);
            in_plain = true;
        }
    };

    while (i < text.size()) {
        const mendo::doc_char c = text[i];

        // 1. 行コメント: //
        if constexpr (Cfg.line_comment_slash) {
            if (c == MENDO_LIT('/') && i + 1 < text.size() && text[i + 1] == MENDO_LIT('/')) {
                flush_plain();
                const size_t start = i;
                i = SkipToEol(text, i);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                // i は '\n' (まだ未消費) または末尾を指す。'\n' を含む場合でも本体は非空白で終わるので false。
                at_line_start = false;
                continue;
            }
        }

        // 1b. アングルブロックコメント: <# #>（PowerShell）
        if constexpr (Cfg.angle_block_comment) {
            if (c == MENDO_LIT('<') && i + 1 < text.size() && text[i + 1] == MENDO_LIT('#')) {
                flush_plain();
                const size_t start = i;
                i = ScanBlockComment(text, i, MENDO_LIT('#'), MENDO_LIT('>'));
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                // 終端 #> は非空白。複数行コメントでも i 直前の文字は '>' で確定。
                at_line_start = false;
                continue;
            }
        }

        if constexpr (Cfg.hash_comment && !Cfg.preprocessor) {
            if (c == MENDO_LIT('#')) {
                flush_plain();
                const size_t start = i;
                i = SkipToEol(text, i);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 2. ブロックコメント: /* */
        if constexpr (Cfg.block_comment) {
            if (c == MENDO_LIT('/') && i + 1 < text.size() && text[i + 1] == MENDO_LIT('*')) {
                flush_plain();
                const size_t start = i;
                i = ScanBlockComment(text, i, MENDO_LIT('*'), MENDO_LIT('/'));
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 3. プリプロセッサ: 行頭の#（C/C++）
        if constexpr (Cfg.preprocessor) {
            if (c == MENDO_LIT('#') && at_line_start) {
                flush_plain();
                const size_t start = i;
                // 改行までジャンプし、直前が '\' なら行継続として次の改行を再探索する。
                while (i < text.size()) {
                    const auto p = text.find(MENDO_LIT('\n'), i);
                    if (p == mendo::doc_string_view::npos) {
                        i = text.size();
                        break;
                    }
                    if (p > 0 && text[p - 1] == MENDO_LIT('\\')) {
                        i = p + 1;
                        continue;
                    }
                    i = p;
                    break;
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Preprocessor);
                at_line_start = false;
                continue;
            }
        }

        // 3b. ダブルコロンコメント: 行頭の::（cmd）
        if constexpr (Cfg.double_colon_comment) {
            if (c == MENDO_LIT(':') && i + 1 < text.size() && text[i + 1] == MENDO_LIT(':') && at_line_start) {
                flush_plain();
                const size_t start = i;
                i = SkipToEol(text, i);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 3c. REMコメント: 行頭のREM（cmd）
        if constexpr (Cfg.rem_comment) {
            if ((c == MENDO_LIT('r') || c == MENDO_LIT('R')) && at_line_start &&
                i + 2 < text.size() &&
                (text[i + 1] == MENDO_LIT('e') || text[i + 1] == MENDO_LIT('E')) &&
                (text[i + 2] == MENDO_LIT('m') || text[i + 2] == MENDO_LIT('M')) &&
                (i + 3 >= text.size() || !IsIdentChar(text[i + 3]))) {
                flush_plain();
                const size_t start = i;
                i = SkipToEol(text, i);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 4. トリプルクォート文字列（Python）
        if constexpr (Cfg.triple_quote) {
            if ((c == MENDO_LIT('"') || c == MENDO_LIT('\'')) && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
                flush_plain();
                const size_t start = i;
                i = ScanTripleQuote(text, i, c);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
                at_line_start = false;
                continue;
            }
        }

        // 5. 文字列リテラル
        if (c == MENDO_LIT('"') || (c == MENDO_LIT('\'') && !Cfg.skip_single_quote)) {
            // C++生文字列の確認: R"(...)"。preprocessor 言語 (C/C++) でのみ意味があるが、
            // 元コードと挙動を揃えるため Cfg に依存しない判定にする。
            if (c == MENDO_LIT('"') && i > 0 && text[i - 1] == MENDO_LIT('R') && (i < 2 || !IsIdentChar(text[i - 2]))) {
                // 調整: Rは既にプレーンバッファにあるので除去する。
                if (in_plain) {
                    if (static_cast<uint32_t>(i - 1) > plain_start) {
                        EmitToken(tokens, plain_start, static_cast<uint32_t>(i - 1) - plain_start, SyntaxTokenType::Plain);
                    }
                    in_plain = false;
                }
                const size_t start = i - 1;
                // デリミタを検索: R"DELIM( ... )DELIM"
                const size_t paren = text.find(MENDO_LIT('('), i + 1);
                if (paren != mendo::doc_string_view::npos) {
                    // end_marker = ')' + delim + '"'。delim は view のまま比較してヒープ確保しない。
                    const mendo::doc_string_view delim = text.substr(i + 1, paren - i - 1);
                    const size_t end_marker_len = 1 + delim.size() + 1; // ')' + delim + '"'
                    size_t end_pos = mendo::doc_string_view::npos;
                    // 生文字列の本文中で ')' は通常レアなので、find で間引いてから delim と '"' を確認する。
                    size_t k = paren + 1;
                    while (k + end_marker_len <= text.size()) {
                        const auto found = text.find(MENDO_LIT(')'), k);
                        if (found == mendo::doc_string_view::npos || found + end_marker_len > text.size()) {
                            break;
                        }
                        if (text[found + 1 + delim.size()] != MENDO_LIT('"')) {
                            k = found + 1;
                            continue;
                        }
                        if (delim.empty() || text.compare(found + 1, delim.size(), delim) == 0) {
                            end_pos = found;
                            break;
                        }
                        k = found + 1;
                    }
                    if (end_pos != mendo::doc_string_view::npos) {
                        i = end_pos + end_marker_len;
                    }
                    else {
                        i = text.size();
                    }
                }
                else {
                    i = ScanString(text, i, c, false);
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
                at_line_start = false;
                continue;
            }
            flush_plain();
            const size_t start = i;
            i = ScanString(text, i, c, false);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            at_line_start = false;
            continue;
        }

        // 6. バッククォートテンプレートリテラル（JS）
        if constexpr (Cfg.backtick_string) {
            if (c == MENDO_LIT('`')) {
                flush_plain();
                const size_t start = i;
                i = ScanString(text, i, MENDO_LIT('`'), true, !Cfg.raw_backtick);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
                at_line_start = false;
                continue;
            }
        }

        // 7. 数値
        if (IsAsciiDigit(c) || (c == MENDO_LIT('.') && i + 1 < text.size() && IsAsciiDigit(text[i + 1]))) {
            flush_plain();
            const size_t start = i;
            i = ScanNumber(text, i);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Number);
            at_line_start = false;
            continue;
        }

        // 8. 識別子とキーワード
        if (IsIdentStart(c)) {
            flush_plain();
            const size_t start = i;
            while (i < text.size() && IsIdentChar(text[i])) {
                i++;
            }

            const mendo::doc_string_view word(text.data() + start, i - start);
            mendo::doc_string_view lookup_word = word;
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

            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), tt);
            at_line_start = false;
            continue;
        }

        // 9. その他: プレーンとして蓄積
        start_plain();
        // 行頭判定を読む言語のみフラグを更新する。それ以外は per-char ストアを丸ごと省略。
        if constexpr (kNeedAtLineStart) {
            if (c == MENDO_LIT('\n')) {
                at_line_start = true;
            }
            else if (c != MENDO_LIT(' ') && c != MENDO_LIT('\t')) {
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

SyntaxLanguage DetectLanguage(mendo::doc_string_view info_string) noexcept
{
    // info string の最初の空白/タブまでを言語識別子として抽出。残りは追加情報。
    const auto lang = info_string.substr(0, info_string.find_first_of(MENDO_LIT(" \t")));
    if (lang.empty()) {
        return SyntaxLanguage::None;
    }

    struct Alias {
        ascii_util::DocLowercaseLiteral name;
        SyntaxLanguage language;
    };
    static constexpr Alias kAliases[]{
        { MENDO_LIT("c"),          SyntaxLanguage::Cpp        },
        { MENDO_LIT("cpp"),        SyntaxLanguage::Cpp        },
        { MENDO_LIT("c++"),        SyntaxLanguage::Cpp        },
        { MENDO_LIT("cxx"),        SyntaxLanguage::Cpp        },
        { MENDO_LIT("h"),          SyntaxLanguage::Cpp        },
        { MENDO_LIT("hpp"),        SyntaxLanguage::Cpp        },
        { MENDO_LIT("cc"),         SyntaxLanguage::Cpp        },
        { MENDO_LIT("hxx"),        SyntaxLanguage::Cpp        },
        { MENDO_LIT("python"),     SyntaxLanguage::Python     },
        { MENDO_LIT("py"),         SyntaxLanguage::Python     },
        { MENDO_LIT("javascript"), SyntaxLanguage::JavaScript },
        { MENDO_LIT("js"),         SyntaxLanguage::JavaScript },
        { MENDO_LIT("jsx"),        SyntaxLanguage::JavaScript },
        { MENDO_LIT("typescript"), SyntaxLanguage::TypeScript },
        { MENDO_LIT("ts"),         SyntaxLanguage::TypeScript },
        { MENDO_LIT("tsx"),        SyntaxLanguage::TypeScript },
        { MENDO_LIT("mermaid"),    SyntaxLanguage::Mermaid    },
        { MENDO_LIT("go"),         SyntaxLanguage::Go         },
        { MENDO_LIT("golang"),     SyntaxLanguage::Go         },
        { MENDO_LIT("rust"),       SyntaxLanguage::Rust       },
        { MENDO_LIT("rs"),         SyntaxLanguage::Rust       },
        { MENDO_LIT("bash"),       SyntaxLanguage::Bash       },
        { MENDO_LIT("sh"),         SyntaxLanguage::Bash       },
        { MENDO_LIT("zsh"),        SyntaxLanguage::Bash       },
        { MENDO_LIT("shell"),      SyntaxLanguage::Bash       },
        { MENDO_LIT("powershell"), SyntaxLanguage::PowerShell },
        { MENDO_LIT("pwsh"),       SyntaxLanguage::PowerShell },
        { MENDO_LIT("ps1"),        SyntaxLanguage::PowerShell },
        { MENDO_LIT("cmd"),        SyntaxLanguage::Cmd        },
        { MENDO_LIT("bat"),        SyntaxLanguage::Cmd        },
        { MENDO_LIT("batch"),      SyntaxLanguage::Cmd        },
        { MENDO_LIT("dosbatch"),   SyntaxLanguage::Cmd        },
        { MENDO_LIT("json"),       SyntaxLanguage::Json       },
        { MENDO_LIT("jsonc"),      SyntaxLanguage::Json       },
        { MENDO_LIT("json5"),      SyntaxLanguage::Json       },
    };

    for (const auto& [alias, language] : kAliases) {
        if (ascii_util::iequal(lang, alias)) {
            return language;
        }
    }
    return SyntaxLanguage::None;
}

std::pmr::vector<SyntaxToken> Tokenize(mendo::doc_string_view text, SyntaxLanguage language)
{
    if (text.empty() || language == SyntaxLanguage::None || IsDiagramLanguage(language)) {
        return {};
    }
    switch (language) {
    case SyntaxLanguage::Cpp:
        return TokenizeImpl<CPP_LEXER_CONFIG>(text, CPP_KEYWORDS, CPP_TYPES);
    case SyntaxLanguage::Python:
        return TokenizeImpl<PYTHON_LEXER_CONFIG>(text, PYTHON_KEYWORDS, PYTHON_TYPES);
    case SyntaxLanguage::JavaScript:
        return TokenizeImpl<JS_LEXER_CONFIG>(text, JS_KEYWORDS, JS_TYPES);
    case SyntaxLanguage::Go:
        return TokenizeImpl<GO_LEXER_CONFIG>(text, GO_KEYWORDS, GO_TYPES);
    case SyntaxLanguage::Rust:
        return TokenizeImpl<RUST_LEXER_CONFIG>(text, RUST_KEYWORDS, RUST_TYPES);
    case SyntaxLanguage::TypeScript:
        return TokenizeImpl<TS_LEXER_CONFIG>(text, TS_KEYWORDS, TS_TYPES);
    case SyntaxLanguage::Bash:
        return TokenizeImpl<BASH_LEXER_CONFIG>(text, BASH_KEYWORDS, BASH_TYPES);
    case SyntaxLanguage::PowerShell:
        return TokenizeImpl<PWSH_LEXER_CONFIG>(text, PWSH_KEYWORDS, PWSH_TYPES);
    case SyntaxLanguage::Cmd:
        return TokenizeImpl<CMD_LEXER_CONFIG>(text, CMD_KEYWORDS, CMD_TYPES);
    case SyntaxLanguage::Json:
        return TokenizeImpl<JSON_LEXER_CONFIG>(text, JSON_KEYWORDS, KeywordTable{});
    case SyntaxLanguage::None:
    case SyntaxLanguage::Mermaid:
    case SyntaxLanguage::LatexMath:
        return {};
    }
    std::unreachable();
}
