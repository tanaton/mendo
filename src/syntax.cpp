#include "syntax.h"
#include "document_utils.h"
#include <algorithm>
#include <unordered_set>
#include <memory_resource>

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
        if (text[i] == L'\n') { return true; }
        if (text[i] != L' ' && text[i] != L'\t') { return false; }
        if (i == 0) { return true; }
    }
}

// ---- キーワードテーブル ----

using KeywordSet = std::pmr::unordered_set<std::wstring_view>;

KeywordSet MergeKeywords(const KeywordSet& base, std::initializer_list<std::wstring_view> extra)
{
    KeywordSet result = base;
    result.insert(extra);
    return result;
}

const KeywordSet& CppKeywords()
{
    static const KeywordSet s = {
        L"auto", L"break", L"case", L"catch", L"class", L"const", L"consteval",
        L"constexpr", L"constinit", L"continue", L"co_await", L"co_return", L"co_yield",
        L"decltype", L"default", L"delete", L"do", L"else", L"enum", L"explicit",
        L"export", L"extern", L"false", L"for", L"friend", L"goto", L"if", L"inline",
        L"mutable", L"namespace", L"new", L"noexcept", L"nullptr", L"operator",
        L"private", L"protected", L"public", L"register", L"return",
        L"sizeof", L"static", L"static_assert", L"static_cast", L"dynamic_cast",
        L"reinterpret_cast", L"const_cast",
        L"struct", L"switch", L"template", L"this", L"throw", L"true", L"try",
        L"typedef", L"typeid", L"typename", L"union", L"using", L"virtual",
        L"void", L"volatile", L"while", L"override", L"final",
        L"concept", L"requires", L"module", L"import"
    };
    return s;
}

const KeywordSet& CppTypes()
{
    static const KeywordSet s = {
        L"int", L"long", L"short", L"char", L"float", L"double", L"bool",
        L"unsigned", L"signed", L"size_t", L"ptrdiff_t",
        L"uint8_t", L"uint16_t", L"uint32_t", L"uint64_t",
        L"int8_t", L"int16_t", L"int32_t", L"int64_t",
        L"wchar_t", L"char8_t", L"char16_t", L"char32_t",
        L"string", L"wstring", L"string_view", L"wstring_view",
        L"vector", L"map", L"unordered_map", L"set", L"unordered_set",
        L"array", L"pair", L"tuple", L"optional", L"variant", L"span",
        L"unique_ptr", L"shared_ptr", L"weak_ptr",
        L"HRESULT", L"BOOL", L"DWORD", L"UINT", L"LPARAM", L"WPARAM", L"HWND",
        L"LRESULT", L"HANDLE", L"HINSTANCE", L"RECT", L"POINT", L"SIZE"
    };
    return s;
}

const KeywordSet& PythonKeywords()
{
    static const KeywordSet s = {
        L"and", L"as", L"assert", L"async", L"await", L"break", L"class",
        L"continue", L"def", L"del", L"elif", L"else", L"except", L"finally",
        L"for", L"from", L"global", L"if", L"import", L"in", L"is", L"lambda",
        L"nonlocal", L"not", L"or", L"pass", L"raise", L"return", L"try",
        L"while", L"with", L"yield", L"True", L"False", L"None"
    };
    return s;
}

const KeywordSet& PythonTypes()
{
    static const KeywordSet s = {
        L"int", L"float", L"str", L"bool", L"list", L"dict", L"set", L"tuple",
        L"bytes", L"bytearray", L"object", L"type", L"range", L"complex",
        L"frozenset", L"memoryview", L"property", L"classmethod", L"staticmethod",
        L"Exception", L"ValueError", L"TypeError", L"KeyError", L"IndexError",
        L"RuntimeError", L"StopIteration", L"OSError", L"IOError"
    };
    return s;
}

const KeywordSet& JsKeywords()
{
    static const KeywordSet s = {
        L"break", L"case", L"catch", L"class", L"const", L"continue",
        L"debugger", L"default", L"delete", L"do", L"else", L"export",
        L"extends", L"finally", L"for", L"function", L"if", L"import",
        L"in", L"instanceof", L"let", L"new", L"of", L"return", L"static",
        L"super", L"switch", L"this", L"throw", L"try", L"typeof", L"var",
        L"void", L"while", L"with", L"yield", L"async", L"await", L"from", L"as"
    };
    return s;
}

const KeywordSet& JsTypes()
{
    static const KeywordSet s = {
        L"Array", L"Boolean", L"Date", L"Error", L"Function", L"Map",
        L"Number", L"Object", L"Promise", L"RegExp", L"Set", L"String",
        L"Symbol", L"BigInt", L"WeakMap", L"WeakSet", L"Proxy", L"Reflect",
        L"undefined", L"null", L"true", L"false", L"NaN", L"Infinity",
        L"console", L"document", L"window", L"globalThis", L"JSON", L"Math"
    };
    return s;
}

// ---- Go ----

const KeywordSet& GoKeywords()
{
    static const KeywordSet s = {
        L"break", L"case", L"chan", L"const", L"continue", L"default", L"defer",
        L"else", L"fallthrough", L"for", L"func", L"go", L"goto", L"if", L"import",
        L"interface", L"map", L"package", L"range", L"return", L"select", L"struct",
        L"switch", L"type", L"var"
    };
    return s;
}

const KeywordSet& GoTypes()
{
    static const KeywordSet s = {
        L"bool", L"byte", L"complex64", L"complex128", L"error",
        L"float32", L"float64", L"int", L"int8", L"int16", L"int32", L"int64",
        L"rune", L"string", L"uint", L"uint8", L"uint16", L"uint32", L"uint64",
        L"uintptr", L"any", L"comparable",
        L"true", L"false", L"nil", L"iota"
    };
    return s;
}

// ---- Rust ----

const KeywordSet& RustKeywords()
{
    static const KeywordSet s = {
        L"as", L"async", L"await", L"break", L"const", L"continue", L"crate",
        L"dyn", L"else", L"enum", L"extern", L"false", L"fn", L"for", L"if",
        L"impl", L"in", L"let", L"loop", L"match", L"mod", L"move", L"mut",
        L"pub", L"ref", L"return", L"self", L"Self", L"static", L"struct",
        L"super", L"trait", L"true", L"type", L"unsafe", L"use", L"where",
        L"while", L"yield", L"macro_rules"
    };
    return s;
}

const KeywordSet& RustTypes()
{
    static const KeywordSet s = {
        L"bool", L"char", L"f32", L"f64", L"i8", L"i16", L"i32", L"i64", L"i128",
        L"isize", L"str", L"u8", L"u16", L"u32", L"u64", L"u128", L"usize",
        L"String", L"Vec", L"Box", L"Rc", L"Arc", L"Cell", L"RefCell",
        L"Option", L"Result", L"Some", L"None", L"Ok", L"Err",
        L"HashMap", L"HashSet", L"BTreeMap", L"BTreeSet", L"VecDeque",
        L"LinkedList", L"Cow", L"Pin", L"PhantomData"
    };
    return s;
}

// ---- TypeScript（JSのスーパーセット） ----

const KeywordSet& TsKeywords()
{
    static const KeywordSet s = MergeKeywords(JsKeywords(), {
        L"abstract", L"declare", L"enum", L"implements", L"infer",
        L"interface", L"is", L"keyof", L"namespace", L"override",
        L"readonly", L"satisfies", L"type", L"module", L"asserts"
        });
    return s;
}

const KeywordSet& TsTypes()
{
    static const KeywordSet s = MergeKeywords(JsTypes(), {
        L"any", L"unknown", L"never", L"number", L"string", L"boolean",
        L"symbol", L"bigint", L"object",
        L"Record", L"Partial", L"Required", L"Readonly", L"Pick", L"Omit",
        L"Exclude", L"Extract", L"NonNullable", L"ReturnType", L"Parameters",
        L"InstanceType", L"Awaited", L"Uppercase", L"Lowercase",
        L"Capitalize", L"Uncapitalize", L"ThisType", L"ConstructorParameters"
        });
    return s;
}

// ---- Bash ----

const KeywordSet& BashKeywords()
{
    static const KeywordSet s = {
        L"if", L"then", L"else", L"elif", L"fi", L"case", L"esac",
        L"for", L"while", L"until", L"do", L"done", L"in", L"function",
        L"select", L"time", L"return", L"exit", L"break", L"continue",
        L"declare", L"local", L"export", L"readonly", L"typeset", L"unset",
        L"shift", L"source", L"eval", L"exec", L"trap", L"set"
    };
    return s;
}

const KeywordSet& BashTypes()
{
    static const KeywordSet s = {
        L"echo", L"printf", L"read", L"test", L"true", L"false",
        L"cd", L"pwd", L"alias", L"unalias", L"type", L"which",
        L"command", L"builtin", L"let", L"getopts", L"mapfile", L"readarray"
    };
    return s;
}

// ---- PowerShell（大文字小文字を区別しないマッチングのためキーワードは小文字で格納） ----

const KeywordSet& PwshKeywords()
{
    static const KeywordSet s = {
        L"begin", L"break", L"catch", L"class", L"continue", L"data",
        L"do", L"dynamicparam", L"else", L"elseif", L"end", L"enum",
        L"exit", L"filter", L"finally", L"for", L"foreach", L"from",
        L"function", L"hidden", L"if", L"in", L"inlinescript", L"param",
        L"process", L"return", L"static", L"switch", L"throw", L"trap",
        L"try", L"until", L"using", L"while", L"workflow"
    };
    return s;
}

const KeywordSet& PwshTypes()
{
    static const KeywordSet s = {
        L"int", L"long", L"float", L"double", L"decimal", L"bool",
        L"byte", L"string", L"char", L"array", L"hashtable", L"xml",
        L"datetime", L"timespan", L"regex", L"scriptblock", L"void",
        L"null", L"true", L"false"
    };
    return s;
}

// ---- Cmd（大文字小文字を区別しないマッチングのためキーワードは小文字で格納） ----

const KeywordSet& CmdKeywords()
{
    static const KeywordSet s = {
        L"if", L"else", L"for", L"do", L"goto", L"call", L"set",
        L"setlocal", L"endlocal", L"echo", L"pause", L"exit", L"rem",
        L"not", L"exist", L"defined", L"equ", L"neq", L"lss", L"leq",
        L"gtr", L"geq", L"errorlevel", L"off", L"on", L"in"
    };
    return s;
}

const KeywordSet& CmdTypes()
{
    static const KeywordSet s = {
        L"dir", L"copy", L"move", L"del", L"ren", L"rename",
        L"mkdir", L"md", L"rmdir", L"rd", L"type", L"find", L"findstr",
        L"sort", L"more", L"cls", L"title", L"color", L"start",
        L"taskkill", L"tasklist", L"reg", L"sc", L"net", L"netsh",
        L"ping", L"ipconfig", L"ver", L"attrib", L"xcopy", L"robocopy"
    };
    return s;
}

// ---- レキサーヘルパー ----

void EmitToken(std::pmr::vector<SyntaxToken>& tokens, uint32_t start, uint32_t length, SyntaxTokenType type)
{
    if (length > 0) {
        tokens.push_back({ start, length, type });
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
            if (i > text.size()) { i = text.size(); }
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
            while (i < text.size() && (IsHexDigit(text[i]) || text[i] == L'\'')) { i++; }
            // サフィックス
            while (i < text.size() && (text[i] == L'u' || text[i] == L'U' || text[i] == L'l' || text[i] == L'L')) { i++; }
            return i;
        }
        if (next == L'b' || next == L'B') {
            i += 2;
            while (i < text.size() && (text[i] == L'0' || text[i] == L'1' || text[i] == L'\'')) { i++; }
            return i;
        }
        if (next == L'o' || next == L'O') {
            i += 2;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'7') { i++; }
            return i;
        }
    }

    // 整数 / 浮動小数点
    while (i < text.size() && (IsDigit(text[i]) || text[i] == L'\'')) { i++; }

    // 小数点
    if (i < text.size() && text[i] == L'.') {
        i++;
        while (i < text.size() && (IsDigit(text[i]) || text[i] == L'\'')) { i++; }
    }

    // 指数部
    if (i < text.size() && (text[i] == L'e' || text[i] == L'E')) {
        i++;
        if (i < text.size() && (text[i] == L'+' || text[i] == L'-')) { i++; }
        while (i < text.size() && IsDigit(text[i])) { i++; }
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
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) { i++; }
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
    const KeywordSet& keywords,
    const KeywordSet& types,
    const LexerConfig& cfg
)
{
    std::pmr::vector<SyntaxToken> tokens;
    tokens.reserve(text.size() / 4);
    size_t i = 0;
    uint32_t plain_start = 0;
    bool in_plain = false;
    std::pmr::wstring ci_buf; // case_insensitive用の再利用バッファ

    auto flush_plain = [&]() {
        if (in_plain && static_cast<uint32_t>(i) > plain_start) {
            EmitToken(tokens, plain_start, static_cast<uint32_t>(i) - plain_start, SyntaxTokenType::Plain);
            in_plain = false;
        }
    };

    auto start_plain = [&]() {
        if (!in_plain) {
            plain_start = static_cast<uint32_t>(i);
            in_plain = true;
        }
    };

    while (i < text.size()) {
        wchar_t c = text[i];

        // 1. 行コメント: //
        if (cfg.line_comment_slash && c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') { i++; }
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 1b. アングルブロックコメント: <# #>（PowerShell）
        if (cfg.angle_block_comment && c == L'<' && i + 1 < text.size() && text[i + 1] == L'#') {
            flush_plain();
            size_t start = i;
            i = ScanBlockComment(text, i, L'#', L'>');
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        if (cfg.hash_comment && c == L'#' && !cfg.preprocessor) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') { i++; }
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 2. ブロックコメント: /* */
        if (cfg.block_comment && c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
            flush_plain();
            size_t start = i;
            i = ScanBlockComment(text, i, L'*', L'/');
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 3. プリプロセッサ: 行頭の#（C/C++）
        if (cfg.preprocessor && c == L'#' && IsAtLineStart(text, i)) {
            flush_plain();
            size_t start = i;
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
        if (cfg.double_colon_comment && c == L':' && i + 1 < text.size() && text[i + 1] == L':' &&
            IsAtLineStart(text, i)) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') { i++; }
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
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') { i++; }
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 4. トリプルクォート文字列（Python）
        if (cfg.triple_quote && (c == L'"' || c == L'\'') &&
            i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
            flush_plain();
            size_t start = i;
            i = ScanTripleQuote(text, i, c);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 5. 文字列リテラル
        if (c == L'"' || (c == L'\'' && !cfg.skip_single_quote)) {
            // C++生文字列の確認: R"(...)"
            if (c == L'"' && i > 0 && text[i - 1] == L'R' &&
                (i < 2 || !IsIdentChar(text[i - 2]))) {
                // 調整: Rは既にプレーンバッファにあるので除去する。
                if (in_plain) {
                    if (static_cast<uint32_t>(i - 1) > plain_start) {
                        EmitToken(tokens, plain_start, static_cast<uint32_t>(i - 1) - plain_start, SyntaxTokenType::Plain);
                    }
                    in_plain = false;
                }
                size_t start = i - 1;
                // デリミタを検索: R"DELIM( ... )DELIM"
                size_t paren = text.find(L'(', i + 1);
                if (paren != std::wstring_view::npos) {
                    std::pmr::wstring delim{ text.substr(i + 1, paren - i - 1) };
                    std::pmr::wstring end_marker = L")" + delim + L"\"";
                    size_t end_pos = text.find(std::wstring_view{ end_marker }, paren + 1);
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
            size_t start = i;
            i = ScanString(text, i, c, false);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 6. バッククォートテンプレートリテラル（JS）
        if (cfg.backtick_string && c == L'`') {
            flush_plain();
            size_t start = i;
            i = ScanString(text, i, L'`', true, !cfg.raw_backtick);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 7. 数値
        if (IsDigit(c) || (c == L'.' && i + 1 < text.size() && IsDigit(text[i + 1]))) {
            flush_plain();
            size_t start = i;
            i = ScanNumber(text, i);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Number);
            continue;
        }

        // 8. 識別子とキーワード
        if (IsIdentStart(c)) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && IsIdentChar(text[i])) { i++; }

            std::wstring_view word(text.data() + start, i - start);
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
            if (keywords.count(lookup_word)) {
                tt = SyntaxTokenType::Keyword;
            }
            else if (types.count(lookup_word)) {
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
    const KeywordSet& (*keywords)();
    const KeywordSet& (*types)();
    LexerConfig config;
};

// プレースホルダーエントリ（None, Mermaid）用の空キーワードセット。
const KeywordSet& EmptyKeywords()
{
    static const KeywordSet s;
    return s;
}

// SyntaxLanguage列挙値でインデックス。
// None=0, Cpp=1, Python=2, JavaScript=3, Mermaid=4,
// Go=5, Rust=6, TypeScript=7, Bash=8, PowerShell=9, Cmd=10
static const LanguageDef LANGUAGE_DEFS[] = {
    // なし
    {&EmptyKeywords, &EmptyKeywords, {}},
    // C++
    {&CppKeywords, &CppTypes, {
        .line_comment_slash = true, .block_comment = true,
        .preprocessor = true,
    }},
    // Python
    {&PythonKeywords, &PythonTypes, {
        .hash_comment = true, .triple_quote = true,
    }},
    // JavaScript
    {&JsKeywords, &JsTypes, {
        .line_comment_slash = true, .block_comment = true,
        .backtick_string = true,
    }},
    // Mermaid（トークン化しない）
    {&EmptyKeywords, &EmptyKeywords, {}},
    // Go
    {&GoKeywords, &GoTypes, {
        .line_comment_slash = true, .block_comment = true,
        .backtick_string = true, .raw_backtick = true,
    }},
    // Rust
    {&RustKeywords, &RustTypes, {
        .line_comment_slash = true, .block_comment = true,
        .skip_single_quote = true,
    }},
    // TypeScript
    {&TsKeywords, &TsTypes, {
        .line_comment_slash = true, .block_comment = true,
        .backtick_string = true,
    }},
    // Bash
    {&BashKeywords, &BashTypes, {
        .hash_comment = true, .backtick_string = true,
    }},
    // PowerShell
    {&PwshKeywords, &PwshTypes, {
        .hash_comment = true, .angle_block_comment = true,
        .case_insensitive = true,
    }},
    // Cmd
    {&CmdKeywords, &CmdTypes, {
        .double_colon_comment = true, .rem_comment = true,
        .case_insensitive = true, .skip_single_quote = true,
    }},
};

static_assert(std::size(LANGUAGE_DEFS) == static_cast<size_t>(SyntaxLanguage::Cmd) + 1,
    "LANGUAGE_DEFS must cover all SyntaxLanguage values");

} // namespace

// ---- 公開API ----

SyntaxLanguage DetectLanguage(std::wstring_view info_string)
{
    if (info_string.empty()) { return SyntaxLanguage::None; }

    // 最初の単語を抽出して小文字に変換
    std::pmr::wstring lang;
    for (wchar_t c : info_string) {
        if (c == L' ' || c == L'\t') { break; }
        lang += c;
    }
    lang = ToLowerAscii(lang);

    if (lang == L"c" || lang == L"cpp" || lang == L"c++" || lang == L"cxx" ||
        lang == L"h" || lang == L"hpp" || lang == L"cc" || lang == L"hxx") {
        return SyntaxLanguage::Cpp;
    }
    if (lang == L"python" || lang == L"py") {
        return SyntaxLanguage::Python;
    }
    if (lang == L"javascript" || lang == L"js" || lang == L"jsx") {
        return SyntaxLanguage::JavaScript;
    }
    if (lang == L"typescript" || lang == L"ts" || lang == L"tsx") {
        return SyntaxLanguage::TypeScript;
    }
    if (lang == L"mermaid") {
        return SyntaxLanguage::Mermaid;
    }
    if (lang == L"go" || lang == L"golang") {
        return SyntaxLanguage::Go;
    }
    if (lang == L"rust" || lang == L"rs") {
        return SyntaxLanguage::Rust;
    }
    if (lang == L"bash" || lang == L"sh" || lang == L"zsh" || lang == L"shell") {
        return SyntaxLanguage::Bash;
    }
    if (lang == L"powershell" || lang == L"pwsh" || lang == L"ps1") {
        return SyntaxLanguage::PowerShell;
    }
    if (lang == L"cmd" || lang == L"bat" || lang == L"batch" || lang == L"dosbatch") {
        return SyntaxLanguage::Cmd;
    }

    return SyntaxLanguage::None;
}

std::pmr::vector<SyntaxToken> Tokenize(std::wstring_view text, SyntaxLanguage language)
{
    auto idx = static_cast<size_t>(language);
    if (text.empty() || language == SyntaxLanguage::None ||
        language == SyntaxLanguage::Mermaid || idx >= std::size(LANGUAGE_DEFS)) {
        return {};
    }
    const auto& def = LANGUAGE_DEFS[idx];
    return TokenizeGeneric(text, def.keywords(), def.types(), def.config);
}
