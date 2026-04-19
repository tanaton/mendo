#include "syntax.h"
#include "syntax_keywords.h"
#include <algorithm>
#include <array>
#include <span>

using namespace std::literals;
using syntax_keywords::KeywordTable;
using syntax_keywords::CPP_KEYWORDS;
using syntax_keywords::CPP_TYPES;
using syntax_keywords::PYTHON_KEYWORDS;
using syntax_keywords::PYTHON_TYPES;
using syntax_keywords::JS_KEYWORDS;
using syntax_keywords::JS_TYPES;
using syntax_keywords::GO_KEYWORDS;
using syntax_keywords::GO_TYPES;
using syntax_keywords::RUST_KEYWORDS;
using syntax_keywords::RUST_TYPES;
using syntax_keywords::TS_KEYWORDS;
using syntax_keywords::TS_TYPES;
using syntax_keywords::BASH_KEYWORDS;
using syntax_keywords::BASH_TYPES;
using syntax_keywords::PWSH_KEYWORDS;
using syntax_keywords::PWSH_TYPES;
using syntax_keywords::CMD_KEYWORDS;
using syntax_keywords::CMD_TYPES;

namespace {

// ---- ヘルパー関数 ----

bool IsIdentStart(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c >= 0x80;
}

bool IsIdentChar(wchar_t c)
{
    return IsIdentStart(c) || (c >= L'0' && c <= L'9');
}

bool IsDigit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

bool IsHexDigit(wchar_t c)
{
    return IsDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

bool IsWhitespace(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r';
}

bool IsAtLineStart(std::wstring_view text, size_t pos)
{
    if (pos == 0) { return true; }
    for (size_t i = pos - 1; ; i--) {
        if (text[i] == L'\n') {
            return true;
        }
        if (text[i] != L' ' && text[i] != L'\t') {
            return false;
        }
        if (i == 0) {
            return true;
        }
    }
}

// キーワード・型名テーブルは syntax_keywords.h にある（各言語の KEYWORDS/TYPES）。

// ---- レキサーヘルパー ----

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
            while (i < text.size() && (IsHexDigit(text[i]) || text[i] == L'\'')) {
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
    while (i < text.size() && (IsDigit(text[i]) || text[i] == L'\'')) {
        i++;
    }

    // 小数点
    if (i < text.size() && text[i] == L'.') {
        i++;
        while (i < text.size() && (IsDigit(text[i]) || text[i] == L'\'')) {
            i++;
        }
    }

    // 指数部
    if (i < text.size() && (text[i] == L'e' || text[i] == L'E')) {
        i++;
        if (i < text.size() && (text[i] == L'+' || text[i] == L'-')) {
            i++;
        }
        while (i < text.size() && IsDigit(text[i])) {
            i++;
        }
    }

    // サフィックス (f, F, l, L, u, U 等)
    while (i < text.size() && (text[i] == L'f' || text[i] == L'F' ||
        text[i] == L'l' || text[i] == L'L' ||
        text[i] == L'u' || text[i] == L'U' ||
        text[i] == L'n')) {
        i++;
    }  // 'n'はJS BigInt用

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

// ---- 汎用トークナイザ ----

struct LexerConfig {
    bool line_comment_slash = false;    // //
    bool block_comment = false;         // /* */
    bool hash_comment = false;          // #
    bool preprocessor = false;          // 行頭の#
    bool triple_quote = false;          // """ '''
    bool backtick_string = false;       // `
    bool angle_block_comment = false;   // <# #>
    bool double_colon_comment = false;  // ::
    bool rem_comment = false;           // REM
    bool case_insensitive = false;      // 大文字小文字を区別しないキーワードマッチング
    bool skip_single_quote = false;     // 'を文字列デリミタとして扱わない
    bool raw_backtick = false;          // エスケープなしのバッククォート文字列（Go）
};

std::pmr::vector<SyntaxToken> TokenizeGeneric(
    std::wstring_view text,
    KeywordTable keywords,
    KeywordTable types,
    const LexerConfig& cfg
)
{
    std::pmr::vector<SyntaxToken> tokens;
    tokens.reserve(text.size() / 4);
    size_t i = 0;
    uint32_t plain_start = 0;
    bool in_plain = false;
    std::wstring ci_buf; // case_insensitive用の再利用バッファ

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
        if (cfg.line_comment_slash && c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') {
            flush_plain();
            const size_t start = i;
            while (i < text.size() && text[i] != L'\n') {
                i++;
            }
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 1b. アングルブロックコメント: <# #>（PowerShell）
        if (cfg.angle_block_comment && c == L'<' && i + 1 < text.size() && text[i + 1] == L'#') {
            flush_plain();
            const size_t start = i;
            i = ScanBlockComment(text, i, L'#', L'>');
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        if (cfg.hash_comment && c == L'#' && !cfg.preprocessor) {
            flush_plain();
            const size_t start = i;
            while (i < text.size() && text[i] != L'\n') {
                i++;
            }
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 2. ブロックコメント: /* */
        if (cfg.block_comment && c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
            flush_plain();
            const size_t start = i;
            i = ScanBlockComment(text, i, L'*', L'/');
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 3. プリプロセッサ: 行頭の#（C/C++）
        if (cfg.preprocessor && c == L'#' && IsAtLineStart(text, i)) {
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
            continue;
        }

        // 3b. ダブルコロンコメント: 行頭の::（cmd）
        if (cfg.double_colon_comment && c == L':' && i + 1 < text.size() && text[i + 1] == L':' && IsAtLineStart(text, i)) {
            flush_plain();
            const size_t start = i;
            while (i < text.size() && text[i] != L'\n') {
                i++;
            }
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 3c. REMコメント: 行頭のREM（cmd）
        if (cfg.rem_comment && (c == L'r' || c == L'R') && IsAtLineStart(text, i) &&
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
            continue;
        }

        // 4. トリプルクォート文字列（Python）
        if (cfg.triple_quote && (c == L'"' || c == L'\'') && i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
            flush_plain();
            const size_t start = i;
            i = ScanTripleQuote(text, i, c);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 5. 文字列リテラル
        if (c == L'"' || (c == L'\'' && !cfg.skip_single_quote)) {
            // C++生文字列の確認: R"(...)"
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
                    const std::wstring delim{ text.substr(i + 1, paren - i - 1) };
                    const std::wstring end_marker = L")" + delim + L"\"";
                    const size_t end_pos = text.find(std::wstring_view{ end_marker }, paren + 1);
                    if (end_pos != std::wstring_view::npos) {
                        i = end_pos + end_marker.size();
                    }
                    else {
                        i = text.size();
                    }
                }
                else {
                    i = ScanString(text, i, c, false);
                }
                EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
                continue;
            }
            flush_plain();
            const size_t start = i;
            i = ScanString(text, i, c, false);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 6. バッククォートテンプレートリテラル（JS）
        if (cfg.backtick_string && c == L'`') {
            flush_plain();
            const size_t start = i;
            i = ScanString(text, i, L'`', true, !cfg.raw_backtick);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 7. 数値
        if (IsDigit(c) || (c == L'.' && i + 1 < text.size() && IsDigit(text[i + 1]))) {
            flush_plain();
            const size_t start = i;
            i = ScanNumber(text, i);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Number);
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
            if (cfg.case_insensitive) {
                // 再利用バッファで小文字化（毎回のメモリ確保を回避）
                ci_buf.clear();
                if (ci_buf.capacity() < word.size()) {
                    ci_buf.reserve(word.size());
                }
                bool has_upper = false;
                for (wchar_t ch : word) {
                    if (ch >= L'A' && ch <= L'Z') {
                        has_upper = true;
                        ci_buf += static_cast<wchar_t>(ch - L'A' + L'a');
                    }
                    else {
                        ci_buf += ch;
                    }
                }
                if (has_upper) {
                    lookup_word = ci_buf;
                }
            }

            SyntaxTokenType tt = SyntaxTokenType::Plain;
            if (std::ranges::binary_search(keywords, lookup_word)) {
                tt = SyntaxTokenType::Keyword;
            }
            else if (std::ranges::binary_search(types, lookup_word)) {
                tt = SyntaxTokenType::Type;
            }
            else if (IsFollowedByParen(text, i)) {
                tt = SyntaxTokenType::Function;
            }

            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), tt);
            continue;
        }

        // 9. その他: プレーンとして蓄積
        start_plain();
        i++;
    }

    flush_plain();
    return tokens;
}

// ---- 言語定義テーブル ----

struct LanguageDef {
    KeywordTable keywords;
    KeywordTable types;
    LexerConfig config;
};

// SyntaxLanguage列挙値でインデックス。
// None=0, Cpp=1, Python=2, JavaScript=3, Mermaid=4,
// Go=5, Rust=6, TypeScript=7, Bash=8, PowerShell=9, Cmd=10
static const LanguageDef LANGUAGE_DEFS[] = {
    // なし
    {{}, {}, {}},
    // C++
    {CPP_KEYWORDS, CPP_TYPES, {
        .line_comment_slash = true,
        .block_comment = true,
        .preprocessor = true,
    }},
    // Python
    {PYTHON_KEYWORDS, PYTHON_TYPES, {
        .hash_comment = true,
        .triple_quote = true,
    }},
    // JavaScript
    {JS_KEYWORDS, JS_TYPES, {
        .line_comment_slash = true,
        .block_comment = true,
        .backtick_string = true,
    }},
    // Mermaid（トークン化しない）
    {{}, {}, {}},
    // Go
    {GO_KEYWORDS, GO_TYPES, {
        .line_comment_slash = true,
        .block_comment = true,
        .backtick_string = true,
        .raw_backtick = true,
    }},
    // Rust
    {RUST_KEYWORDS, RUST_TYPES, {
        .line_comment_slash = true,
        .block_comment = true,
        .skip_single_quote = true,
    }},
    // TypeScript
    {TS_KEYWORDS, TS_TYPES, {
        .line_comment_slash = true,
        .block_comment = true,
        .backtick_string = true,
    }},
    // Bash
    {BASH_KEYWORDS, BASH_TYPES, {
        .hash_comment = true,
        .backtick_string = true,
    }},
    // PowerShell
    {PWSH_KEYWORDS, PWSH_TYPES, {
        .hash_comment = true,
        .angle_block_comment = true,
        .case_insensitive = true,
    }},
    // Cmd
    {CMD_KEYWORDS, CMD_TYPES, {
        .double_colon_comment = true,
        .rem_comment = true,
        .case_insensitive = true,
        .skip_single_quote = true,
    }},
};

static_assert(std::size(LANGUAGE_DEFS) == static_cast<size_t>(SyntaxLanguage::Cmd) + 1, "LANGUAGE_DEFS must cover all SyntaxLanguage values");

} // namespace

// ---- 公開API ----

SyntaxLanguage DetectLanguage(std::string_view info_string)
{
    if (info_string.empty()) {
        return SyntaxLanguage::None;
    }

    // 最初の単語を抽出してASCII小文字に変換（スタックバッファで割り当て回避）
    char lang_buf[32];
    size_t lang_len = 0;
    for (char c : info_string) {
        if (c == ' ' || c == '\t') {
            break;
        }
        if (lang_len >= sizeof(lang_buf) - 1) {
            break;
        }
        lang_buf[lang_len++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view lang(lang_buf, lang_len);

    if (lang == "c" || lang == "cpp" || lang == "c++" || lang == "cxx" ||
        lang == "h" || lang == "hpp" || lang == "cc" || lang == "hxx") {
        return SyntaxLanguage::Cpp;
    }
    if (lang == "python" || lang == "py") {
        return SyntaxLanguage::Python;
    }
    if (lang == "javascript" || lang == "js" || lang == "jsx") {
        return SyntaxLanguage::JavaScript;
    }
    if (lang == "typescript" || lang == "ts" || lang == "tsx") {
        return SyntaxLanguage::TypeScript;
    }
    if (lang == "mermaid") {
        return SyntaxLanguage::Mermaid;
    }
    if (lang == "go" || lang == "golang") {
        return SyntaxLanguage::Go;
    }
    if (lang == "rust" || lang == "rs") {
        return SyntaxLanguage::Rust;
    }
    if (lang == "bash" || lang == "sh" || lang == "zsh" || lang == "shell") {
        return SyntaxLanguage::Bash;
    }
    if (lang == "powershell" || lang == "pwsh" || lang == "ps1") {
        return SyntaxLanguage::PowerShell;
    }
    if (lang == "cmd" || lang == "bat" || lang == "batch" || lang == "dosbatch") {
        return SyntaxLanguage::Cmd;
    }

    return SyntaxLanguage::None;
}

SyntaxLanguage DetectLanguage(std::wstring_view info_string)
{
    if (info_string.empty()) {
        return SyntaxLanguage::None;
    }
    // 言語名は全てASCIIなのでnarrowに変換してstring_view版に委譲
    char buf[32];
    size_t len = 0;
    for (wchar_t c : info_string) {
        if (c == L' ' || c == L'\t') {
            break;
        }
        if (len >= sizeof(buf) - 1 || c > 0x7F) {
            break;
        }
        buf[len++] = static_cast<char>(c);
    }
    return DetectLanguage(std::string_view(buf, len));
}

std::pmr::vector<SyntaxToken> Tokenize(std::wstring_view text, SyntaxLanguage language)
{
    const auto idx = static_cast<size_t>(language);
    if (text.empty() || language == SyntaxLanguage::None ||
        IsDiagramLanguage(language) || idx >= std::size(LANGUAGE_DEFS)) {
        return {};
    }
    const auto& def = LANGUAGE_DEFS[idx];
    return TokenizeGeneric(text, def.keywords, def.types, def.config);
}
