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
using ascii_util::IsAsciiHexDigit;

// 識別子先頭文字: ASCII 英字 + '_' に加え、CJK 等の非 ASCII (>= U+0080) も許可する。
// ascii_util の純粋 ASCII ヘルパに乗らないので syntax 固有として残す。
bool IsIdentStart(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c >= 0x80;
}

bool IsIdentChar(wchar_t c)
{
    return IsIdentStart(c) || IsAsciiDigit(c);
}

void EmitToken(std::pmr::vector<SyntaxToken>& tokens, uint32_t start, uint32_t length, SyntaxTokenType type)
{
    if (length > 0) {
        tokens.emplace_back(start, length, type);
    }
}

// posから始まる文字列リテラルをスキャン（posは開始引用符を指す）。
// 閉じ引用符の次の位置を返す（未終端の場合はテキストの末尾）。
size_t ScanString(std::wstring_view text, size_t pos, wchar_t quote, bool allow_multiline, bool handle_escape = true)
{
    size_t i = pos + 1;
    while (i < text.size()) {
        if (handle_escape && text[i] == L'\\') {
            i += 2;
            if (i > text.size()) {
                i = text.size();
            }
        }
        else if (text[i] == quote) {
            return i + 1;
        }
        else if (!allow_multiline && text[i] == L'\n') {
            return i; // 未終端
        }
        else {
            i++;
        }
    }
    return i;
}

// Pythonのトリプルクォート文字列をスキャン。
size_t ScanTripleQuote(std::wstring_view text, size_t pos, wchar_t quote)
{
    // posはトリプルクォートの最初の引用符を指す
    size_t i = pos + 3;
    while (i + 2 < text.size()) {
        if (text[i] == L'\\') {
            i += 2;
        }
        else if (text[i] == quote && text[i + 1] == quote && text[i + 2] == quote) {
            return i + 3;
        }
        else {
            i++;
        }
    }
    return text.size(); // 未終端
}

// posから始まる数値リテラルをスキャン。
size_t ScanNumber(std::wstring_view text, size_t pos)
{
    size_t i = pos;

    // 0x, 0b, 0oプレフィックスの処理
    if (i + 1 < text.size() && text[i] == L'0') {
        wchar_t next = text[i + 1];
        if (next == L'x' || next == L'X') {
            i += 2;
            while (i < text.size() && (IsAsciiHexDigit(text[i]) || text[i] == L'\'')) {
                i++;
            }
            // サフィックス
            while (i < text.size() && (text[i] == L'u' || text[i] == L'U' || text[i] == L'l' || text[i] == L'L')) {
                i++;
            }
            return i;
        }
        if (next == L'b' || next == L'B') {
            i += 2;
            while (i < text.size() && (text[i] == L'0' || text[i] == L'1' || text[i] == L'\'')) {
                i++;
            }
            return i;
        }
        if (next == L'o' || next == L'O') {
            i += 2;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'7') {
                i++;
            }
            return i;
        }
    }

    // 整数 / 浮動小数点
    while (i < text.size() && (IsAsciiDigit(text[i]) || text[i] == L'\'')) {
        i++;
    }

    // 小数点
    if (i < text.size() && text[i] == L'.') {
        i++;
        while (i < text.size() && (IsAsciiDigit(text[i]) || text[i] == L'\'')) {
            i++;
        }
    }

    // 指数部
    if (i < text.size() && (text[i] == L'e' || text[i] == L'E')) {
        i++;
        if (i < text.size() && (text[i] == L'+' || text[i] == L'-')) {
            i++;
        }
        while (i < text.size() && IsAsciiDigit(text[i])) {
            i++;
        }
    }

    // サフィックス (f, F, l, L, u, U 等)
    while (i < text.size() && (text[i] == L'f' || text[i] == L'F' ||
                               text[i] == L'l' || text[i] == L'L' ||
                               text[i] == L'u' || text[i] == L'U' ||
                               text[i] == L'n')) {
        i++;
    } // 'n'はJS BigInt用

    return i;
}

// [start, end)の識別子の後に'('が続くか確認（空白をスキップ）。
bool IsFollowedByParen(std::wstring_view text, size_t end)
{
    size_t i = end;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) {
        i++;
    }
    return i < text.size() && text[i] == L'(';
}

// posから始まるブロックコメントをスキャン（posは開始ペアの最初の文字を指す）。
// 閉じペアの次の位置を返す。未終端の場合はtext.size()を返す。
size_t ScanBlockComment(std::wstring_view text, size_t pos, wchar_t close1, wchar_t close2)
{
    size_t i = pos + 2;
    while (i + 1 < text.size()) {
        if (text[i] == close1 && text[i + 1] == close2) {
            return i + 2;
        }
        i++;
    }
    return text.size();
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
    std::wstring_view text,
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
    [[maybe_unused]] std::pmr::wstring ci_buf; // case_insensitive 用の再利用バッファ
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
        const wchar_t c = text[i];

        // 1. 行コメント: //
        if constexpr (Cfg.line_comment_slash) {
            if (c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') {
                flush_plain();
                const size_t start = i;
                while (i < text.size() && text[i] != L'\n') {
                    i++;
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                // i は '\n' (まだ未消費) または末尾を指す。'\n' を含む場合でも本体は非空白で終わるので false。
                at_line_start = false;
                continue;
            }
        }

        // 1b. アングルブロックコメント: <# #>（PowerShell）
        if constexpr (Cfg.angle_block_comment) {
            if (c == L'<' && i + 1 < text.size() && text[i + 1] == L'#') {
                flush_plain();
                const size_t start = i;
                i = ScanBlockComment(text, i, L'#', L'>');
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                // 終端 #> は非空白。複数行コメントでも i 直前の文字は '>' で確定。
                at_line_start = false;
                continue;
            }
        }

        if constexpr (Cfg.hash_comment && !Cfg.preprocessor) {
            if (c == L'#') {
                flush_plain();
                const size_t start = i;
                while (i < text.size() && text[i] != L'\n') {
                    i++;
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 2. ブロックコメント: /* */
        if constexpr (Cfg.block_comment) {
            if (c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
                flush_plain();
                const size_t start = i;
                i = ScanBlockComment(text, i, L'*', L'/');
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 3. プリプロセッサ: 行頭の#（C/C++）
        if constexpr (Cfg.preprocessor) {
            if (c == L'#' && at_line_start) {
                flush_plain();
                const size_t start = i;
                while (i < text.size()) {
                    if (text[i] == L'\n') {
                        // 行継続の確認
                        if (i > 0 && text[i - 1] == L'\\') {
                            i++;
                            continue;
                        }
                        break;
                    }
                    i++;
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Preprocessor);
                at_line_start = false;
                continue;
            }
        }

        // 3b. ダブルコロンコメント: 行頭の::（cmd）
        if constexpr (Cfg.double_colon_comment) {
            if (c == L':' && i + 1 < text.size() && text[i + 1] == L':' && at_line_start) {
                flush_plain();
                const size_t start = i;
                while (i < text.size() && text[i] != L'\n') {
                    i++;
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 3c. REMコメント: 行頭のREM（cmd）
        if constexpr (Cfg.rem_comment) {
            if ((c == L'r' || c == L'R') && at_line_start &&
                i + 2 < text.size() &&
                (text[i + 1] == L'e' || text[i + 1] == L'E') &&
                (text[i + 2] == L'm' || text[i + 2] == L'M') &&
                (i + 3 >= text.size() || !IsIdentChar(text[i + 3]))) {
                flush_plain();
                const size_t start = i;
                while (i < text.size() && text[i] != L'\n') {
                    i++;
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
                at_line_start = false;
                continue;
            }
        }

        // 4. トリプルクォート文字列（Python）
        if constexpr (Cfg.triple_quote) {
            if ((c == L'"' || c == L'\'') && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
                flush_plain();
                const size_t start = i;
                i = ScanTripleQuote(text, i, c);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
                at_line_start = false;
                continue;
            }
        }

        // 5. 文字列リテラル
        if (c == L'"' || (c == L'\'' && !Cfg.skip_single_quote)) {
            // C++生文字列の確認: R"(...)"。preprocessor 言語 (C/C++) でのみ意味があるが、
            // 元コードと挙動を揃えるため Cfg に依存しない判定にする。
            if (c == L'"' && i > 0 && text[i - 1] == L'R' && (i < 2 || !IsIdentChar(text[i - 2]))) {
                // 調整: Rは既にプレーンバッファにあるので除去する。
                if (in_plain) {
                    if (static_cast<uint32_t>(i - 1) > plain_start) {
                        EmitToken(tokens, plain_start, static_cast<uint32_t>(i - 1) - plain_start, SyntaxTokenType::Plain);
                    }
                    in_plain = false;
                }
                const size_t start = i - 1;
                // デリミタを検索: R"DELIM( ... )DELIM"
                const size_t paren = text.find(L'(', i + 1);
                if (paren != std::wstring_view::npos) {
                    // delim / end_marker を pmr::wstring で確保しないよう view と手書き走査で済ます。
                    // end_marker は ')' + delim + '"' の固定構造なので、'(' 以降から ')' を探して
                    // delim の wmemcmp で一致確認するだけで O(1) ヒープ確保で完結する。
                    const std::wstring_view delim = text.substr(i + 1, paren - i - 1);
                    const size_t end_marker_len = 1 + delim.size() + 1; // ')' + delim + '"'
                    size_t end_pos = std::wstring_view::npos;
                    if (paren + 1 + end_marker_len <= text.size()) {
                        for (size_t k = paren + 1; k + end_marker_len <= text.size(); k++) {
                            if (text[k] != L')' || text[k + 1 + delim.size()] != L'"') {
                                continue;
                            }
                            if (delim.empty() || std::char_traits<wchar_t>::compare(text.data() + k + 1, delim.data(), delim.size()) == 0) {
                                end_pos = k;
                                break;
                            }
                        }
                    }
                    if (end_pos != std::wstring_view::npos) {
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
            if (c == L'`') {
                flush_plain();
                const size_t start = i;
                i = ScanString(text, i, L'`', true, !Cfg.raw_backtick);
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
                at_line_start = false;
                continue;
            }
        }

        // 7. 数値
        if (IsAsciiDigit(c) || (c == L'.' && i + 1 < text.size() && IsAsciiDigit(text[i + 1]))) {
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

            const std::wstring_view word(text.data() + start, i - start);
            std::wstring_view lookup_word = word;
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
            if (c == L'\n') {
                at_line_start = true;
            }
            else if (c != L' ' && c != L'\t') {
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

SyntaxLanguage DetectLanguage(std::wstring_view info_string)
{
    // info string の最初の空白/タブまでを言語識別子として抽出。残りは追加情報。
    const auto lang = info_string.substr(0, info_string.find_first_of(L" \t"));
    if (lang.empty()) {
        return SyntaxLanguage::None;
    }

    struct Alias {
        ascii_util::LowercaseAsciiLiteral name;
        SyntaxLanguage language;
    };
    static constexpr Alias kAliases[]{
        { L"c",          SyntaxLanguage::Cpp        },
        { L"cpp",        SyntaxLanguage::Cpp        },
        { L"c++",        SyntaxLanguage::Cpp        },
        { L"cxx",        SyntaxLanguage::Cpp        },
        { L"h",          SyntaxLanguage::Cpp        },
        { L"hpp",        SyntaxLanguage::Cpp        },
        { L"cc",         SyntaxLanguage::Cpp        },
        { L"hxx",        SyntaxLanguage::Cpp        },
        { L"python",     SyntaxLanguage::Python     },
        { L"py",         SyntaxLanguage::Python     },
        { L"javascript", SyntaxLanguage::JavaScript },
        { L"js",         SyntaxLanguage::JavaScript },
        { L"jsx",        SyntaxLanguage::JavaScript },
        { L"typescript", SyntaxLanguage::TypeScript },
        { L"ts",         SyntaxLanguage::TypeScript },
        { L"tsx",        SyntaxLanguage::TypeScript },
        { L"mermaid",    SyntaxLanguage::Mermaid    },
        { L"go",         SyntaxLanguage::Go         },
        { L"golang",     SyntaxLanguage::Go         },
        { L"rust",       SyntaxLanguage::Rust       },
        { L"rs",         SyntaxLanguage::Rust       },
        { L"bash",       SyntaxLanguage::Bash       },
        { L"sh",         SyntaxLanguage::Bash       },
        { L"zsh",        SyntaxLanguage::Bash       },
        { L"shell",      SyntaxLanguage::Bash       },
        { L"powershell", SyntaxLanguage::PowerShell },
        { L"pwsh",       SyntaxLanguage::PowerShell },
        { L"ps1",        SyntaxLanguage::PowerShell },
        { L"cmd",        SyntaxLanguage::Cmd        },
        { L"bat",        SyntaxLanguage::Cmd        },
        { L"batch",      SyntaxLanguage::Cmd        },
        { L"dosbatch",   SyntaxLanguage::Cmd        },
        { L"json",       SyntaxLanguage::Json       },
        { L"jsonc",      SyntaxLanguage::Json       },
        { L"json5",      SyntaxLanguage::Json       },
    };

    for (const auto& [alias, language] : kAliases) {
        if (ascii_util::iequal(lang, alias)) {
            return language;
        }
    }
    return SyntaxLanguage::None;
}

std::pmr::vector<SyntaxToken> Tokenize(std::wstring_view text, SyntaxLanguage language)
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
