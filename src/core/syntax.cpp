#include "syntax.h"
#include <algorithm>
#include <array>
#include <span>

using namespace std::literals;

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

// ---- キーワードテーブル ----
// ソート済み配列 + std::ranges::binary_search でヒープ確保を完全に排除。
// MakeSorted は consteval なのでソート順はコンパイル時に確定する。

using KeywordTable = std::span<const std::wstring_view>;

template<size_t N>
consteval std::array<std::wstring_view, N> MakeSorted(std::array<std::wstring_view, N> arr)
{
    std::ranges::sort(arr);
    return arr;
}

static constexpr auto CPP_KEYWORDS = MakeSorted(std::array{
    L"auto"sv, L"break"sv, L"case"sv, L"catch"sv, L"class"sv, L"co_await"sv,
    L"co_return"sv, L"co_yield"sv, L"concept"sv, L"const"sv, L"const_cast"sv,
    L"consteval"sv, L"constexpr"sv, L"constinit"sv, L"continue"sv, L"decltype"sv,
    L"default"sv, L"delete"sv, L"do"sv, L"dynamic_cast"sv, L"else"sv, L"enum"sv,
    L"explicit"sv, L"export"sv, L"extern"sv, L"false"sv, L"final"sv, L"for"sv,
    L"friend"sv, L"goto"sv, L"if"sv, L"import"sv, L"inline"sv, L"module"sv,
    L"mutable"sv, L"namespace"sv, L"new"sv, L"noexcept"sv, L"nullptr"sv,
    L"operator"sv, L"override"sv, L"private"sv, L"protected"sv, L"public"sv,
    L"register"sv, L"reinterpret_cast"sv, L"requires"sv, L"return"sv,
    L"sizeof"sv, L"static"sv, L"static_assert"sv, L"static_cast"sv,
    L"struct"sv, L"switch"sv, L"template"sv, L"this"sv, L"throw"sv, L"true"sv,
    L"try"sv, L"typedef"sv, L"typeid"sv, L"typename"sv, L"union"sv, L"using"sv,
    L"virtual"sv, L"void"sv, L"volatile"sv, L"while"sv,
});

static constexpr auto CPP_TYPES = MakeSorted(std::array{
    L"BOOL"sv, L"DWORD"sv, L"HANDLE"sv, L"HINSTANCE"sv, L"HRESULT"sv, L"HWND"sv,
    L"LPARAM"sv, L"LRESULT"sv, L"POINT"sv, L"RECT"sv, L"SIZE"sv, L"UINT"sv, L"WPARAM"sv,
    L"array"sv, L"bool"sv, L"char"sv, L"char16_t"sv, L"char32_t"sv, L"char8_t"sv,
    L"double"sv, L"float"sv, L"int"sv, L"int16_t"sv, L"int32_t"sv, L"int64_t"sv,
    L"int8_t"sv, L"long"sv, L"map"sv, L"optional"sv, L"pair"sv, L"ptrdiff_t"sv,
    L"set"sv, L"shared_ptr"sv, L"short"sv, L"signed"sv, L"size_t"sv, L"span"sv,
    L"string"sv, L"string_view"sv, L"tuple"sv, L"uint16_t"sv, L"uint32_t"sv,
    L"uint64_t"sv, L"uint8_t"sv, L"unique_ptr"sv, L"unordered_map"sv, L"unordered_set"sv,
    L"unsigned"sv, L"variant"sv, L"vector"sv, L"wchar_t"sv, L"weak_ptr"sv,
    L"wstring"sv, L"wstring_view"sv,
});

static constexpr auto PYTHON_KEYWORDS = MakeSorted(std::array{
    L"False"sv, L"None"sv, L"True"sv,
    L"and"sv, L"as"sv, L"assert"sv, L"async"sv, L"await"sv, L"break"sv, L"class"sv,
    L"continue"sv, L"def"sv, L"del"sv, L"elif"sv, L"else"sv, L"except"sv, L"finally"sv,
    L"for"sv, L"from"sv, L"global"sv, L"if"sv, L"import"sv, L"in"sv, L"is"sv,
    L"lambda"sv, L"nonlocal"sv, L"not"sv, L"or"sv, L"pass"sv, L"raise"sv,
    L"return"sv, L"try"sv, L"while"sv, L"with"sv, L"yield"sv,
});

static constexpr auto PYTHON_TYPES = MakeSorted(std::array{
    L"Exception"sv, L"IOError"sv, L"IndexError"sv, L"KeyError"sv, L"OSError"sv,
    L"RuntimeError"sv, L"StopIteration"sv, L"TypeError"sv, L"ValueError"sv,
    L"bool"sv, L"bytearray"sv, L"bytes"sv, L"classmethod"sv, L"complex"sv,
    L"dict"sv, L"float"sv, L"frozenset"sv, L"int"sv, L"list"sv, L"memoryview"sv,
    L"object"sv, L"property"sv, L"range"sv, L"set"sv, L"staticmethod"sv,
    L"str"sv, L"tuple"sv, L"type"sv,
});

static constexpr auto JS_KEYWORDS = MakeSorted(std::array{
    L"as"sv, L"async"sv, L"await"sv, L"break"sv, L"case"sv, L"catch"sv, L"class"sv,
    L"const"sv, L"continue"sv, L"debugger"sv, L"default"sv, L"delete"sv, L"do"sv,
    L"else"sv, L"export"sv, L"extends"sv, L"finally"sv, L"for"sv, L"from"sv,
    L"function"sv, L"if"sv, L"import"sv, L"in"sv, L"instanceof"sv, L"let"sv,
    L"new"sv, L"of"sv, L"return"sv, L"static"sv, L"super"sv, L"switch"sv, L"this"sv,
    L"throw"sv, L"try"sv, L"typeof"sv, L"var"sv, L"void"sv, L"while"sv, L"with"sv,
    L"yield"sv,
});

static constexpr auto JS_TYPES = MakeSorted(std::array{
    L"Array"sv, L"BigInt"sv, L"Boolean"sv, L"Date"sv, L"Error"sv, L"Function"sv,
    L"Infinity"sv, L"JSON"sv, L"Map"sv, L"Math"sv, L"NaN"sv, L"Number"sv,
    L"Object"sv, L"Promise"sv, L"Proxy"sv, L"Reflect"sv, L"RegExp"sv, L"Set"sv,
    L"String"sv, L"Symbol"sv, L"WeakMap"sv, L"WeakSet"sv,
    L"console"sv, L"document"sv, L"false"sv, L"globalThis"sv, L"null"sv, L"true"sv,
    L"undefined"sv, L"window"sv,
});

static constexpr auto GO_KEYWORDS = MakeSorted(std::array{
    L"break"sv, L"case"sv, L"chan"sv, L"const"sv, L"continue"sv, L"default"sv,
    L"defer"sv, L"else"sv, L"fallthrough"sv, L"for"sv, L"func"sv, L"go"sv,
    L"goto"sv, L"if"sv, L"import"sv, L"interface"sv, L"map"sv, L"package"sv,
    L"range"sv, L"return"sv, L"select"sv, L"struct"sv, L"switch"sv, L"type"sv,
    L"var"sv,
});

static constexpr auto GO_TYPES = MakeSorted(std::array{
    L"any"sv, L"bool"sv, L"byte"sv, L"comparable"sv, L"complex128"sv, L"complex64"sv,
    L"error"sv, L"false"sv, L"float32"sv, L"float64"sv, L"int"sv, L"int16"sv,
    L"int32"sv, L"int64"sv, L"int8"sv, L"iota"sv, L"nil"sv, L"rune"sv, L"string"sv,
    L"true"sv, L"uint"sv, L"uint16"sv, L"uint32"sv, L"uint64"sv, L"uint8"sv,
    L"uintptr"sv,
});

static constexpr auto RUST_KEYWORDS = MakeSorted(std::array{
    L"Self"sv,
    L"as"sv, L"async"sv, L"await"sv, L"break"sv, L"const"sv, L"continue"sv,
    L"crate"sv, L"dyn"sv, L"else"sv, L"enum"sv, L"extern"sv, L"false"sv, L"fn"sv,
    L"for"sv, L"if"sv, L"impl"sv, L"in"sv, L"let"sv, L"loop"sv, L"macro_rules"sv,
    L"match"sv, L"mod"sv, L"move"sv, L"mut"sv, L"pub"sv, L"ref"sv, L"return"sv,
    L"self"sv, L"static"sv, L"struct"sv, L"super"sv, L"trait"sv, L"true"sv,
    L"type"sv, L"unsafe"sv, L"use"sv, L"where"sv, L"while"sv, L"yield"sv,
});

static constexpr auto RUST_TYPES = MakeSorted(std::array{
    L"Arc"sv, L"BTreeMap"sv, L"BTreeSet"sv, L"Box"sv, L"Cell"sv, L"Cow"sv,
    L"Err"sv, L"HashMap"sv, L"HashSet"sv, L"LinkedList"sv, L"None"sv, L"Ok"sv,
    L"Option"sv, L"PhantomData"sv, L"Pin"sv, L"Rc"sv, L"RefCell"sv, L"Result"sv,
    L"Some"sv, L"String"sv, L"Vec"sv, L"VecDeque"sv,
    L"bool"sv, L"char"sv, L"f32"sv, L"f64"sv, L"i128"sv, L"i16"sv, L"i32"sv,
    L"i64"sv, L"i8"sv, L"isize"sv, L"str"sv, L"u128"sv, L"u16"sv, L"u32"sv,
    L"u64"sv, L"u8"sv, L"usize"sv,
});

// TypeScript = JS + TS固有キーワード（マージ済み）
static constexpr auto TS_KEYWORDS = MakeSorted(std::array{
    L"abstract"sv, L"as"sv, L"asserts"sv, L"async"sv, L"await"sv, L"break"sv,
    L"case"sv, L"catch"sv, L"class"sv, L"const"sv, L"continue"sv, L"debugger"sv,
    L"declare"sv, L"default"sv, L"delete"sv, L"do"sv, L"else"sv, L"enum"sv,
    L"export"sv, L"extends"sv, L"finally"sv, L"for"sv, L"from"sv, L"function"sv,
    L"if"sv, L"implements"sv, L"import"sv, L"in"sv, L"infer"sv, L"instanceof"sv,
    L"interface"sv, L"is"sv, L"keyof"sv, L"let"sv, L"module"sv, L"namespace"sv,
    L"new"sv, L"of"sv, L"override"sv, L"readonly"sv, L"return"sv, L"satisfies"sv,
    L"static"sv, L"super"sv, L"switch"sv, L"this"sv, L"throw"sv, L"try"sv,
    L"type"sv, L"typeof"sv, L"var"sv, L"void"sv, L"while"sv, L"with"sv, L"yield"sv,
});

static constexpr auto TS_TYPES = MakeSorted(std::array{
    L"Array"sv, L"Awaited"sv, L"BigInt"sv, L"Boolean"sv, L"Capitalize"sv,
    L"ConstructorParameters"sv, L"Date"sv, L"Error"sv, L"Exclude"sv, L"Extract"sv,
    L"Function"sv, L"Infinity"sv, L"InstanceType"sv, L"JSON"sv, L"Lowercase"sv,
    L"Map"sv, L"Math"sv, L"NaN"sv, L"NonNullable"sv, L"Number"sv, L"Object"sv,
    L"Omit"sv, L"Parameters"sv, L"Partial"sv, L"Pick"sv, L"Promise"sv, L"Proxy"sv,
    L"Readonly"sv, L"Record"sv, L"Reflect"sv, L"RegExp"sv, L"Required"sv,
    L"ReturnType"sv, L"Set"sv, L"String"sv, L"Symbol"sv, L"ThisType"sv,
    L"Uncapitalize"sv, L"Uppercase"sv, L"WeakMap"sv, L"WeakSet"sv,
    L"any"sv, L"bigint"sv, L"boolean"sv, L"console"sv, L"document"sv, L"false"sv,
    L"globalThis"sv, L"never"sv, L"null"sv, L"number"sv, L"object"sv, L"string"sv,
    L"symbol"sv, L"true"sv, L"undefined"sv, L"unknown"sv, L"window"sv,
});

static constexpr auto BASH_KEYWORDS = MakeSorted(std::array{
    L"break"sv, L"case"sv, L"continue"sv, L"declare"sv, L"do"sv, L"done"sv,
    L"elif"sv, L"else"sv, L"esac"sv, L"eval"sv, L"exec"sv, L"exit"sv, L"export"sv,
    L"fi"sv, L"for"sv, L"function"sv, L"if"sv, L"in"sv, L"local"sv, L"readonly"sv,
    L"return"sv, L"select"sv, L"set"sv, L"shift"sv, L"source"sv, L"then"sv,
    L"time"sv, L"trap"sv, L"typeset"sv, L"unset"sv, L"until"sv, L"while"sv,
});

static constexpr auto BASH_TYPES = MakeSorted(std::array{
    L"alias"sv, L"builtin"sv, L"cd"sv, L"command"sv, L"echo"sv, L"false"sv,
    L"getopts"sv, L"let"sv, L"mapfile"sv, L"printf"sv, L"pwd"sv, L"read"sv,
    L"readarray"sv, L"test"sv, L"true"sv, L"type"sv, L"unalias"sv, L"which"sv,
});

// PowerShell（大文字小文字を区別しないマッチングのためキーワードは小文字で格納）
static constexpr auto PWSH_KEYWORDS = MakeSorted(std::array{
    L"begin"sv, L"break"sv, L"catch"sv, L"class"sv, L"continue"sv, L"data"sv,
    L"do"sv, L"dynamicparam"sv, L"else"sv, L"elseif"sv, L"end"sv, L"enum"sv,
    L"exit"sv, L"filter"sv, L"finally"sv, L"for"sv, L"foreach"sv, L"from"sv,
    L"function"sv, L"hidden"sv, L"if"sv, L"in"sv, L"inlinescript"sv, L"param"sv,
    L"process"sv, L"return"sv, L"static"sv, L"switch"sv, L"throw"sv, L"trap"sv,
    L"try"sv, L"until"sv, L"using"sv, L"while"sv, L"workflow"sv,
});

static constexpr auto PWSH_TYPES = MakeSorted(std::array{
    L"array"sv, L"bool"sv, L"byte"sv, L"char"sv, L"datetime"sv, L"decimal"sv,
    L"double"sv, L"false"sv, L"float"sv, L"hashtable"sv, L"int"sv, L"long"sv,
    L"null"sv, L"regex"sv, L"scriptblock"sv, L"string"sv, L"timespan"sv,
    L"true"sv, L"void"sv, L"xml"sv,
});

// Cmd（大文字小文字を区別しないマッチングのためキーワードは小文字で格納）
static constexpr auto CMD_KEYWORDS = MakeSorted(std::array{
    L"call"sv, L"defined"sv, L"do"sv, L"echo"sv, L"else"sv, L"endlocal"sv,
    L"equ"sv, L"errorlevel"sv, L"exist"sv, L"exit"sv, L"for"sv, L"geq"sv,
    L"goto"sv, L"gtr"sv, L"if"sv, L"in"sv, L"leq"sv, L"lss"sv, L"neq"sv,
    L"not"sv, L"off"sv, L"on"sv, L"pause"sv, L"rem"sv, L"set"sv, L"setlocal"sv,
});

static constexpr auto CMD_TYPES = MakeSorted(std::array{
    L"attrib"sv, L"cls"sv, L"color"sv, L"copy"sv, L"del"sv, L"dir"sv, L"find"sv,
    L"findstr"sv, L"ipconfig"sv, L"md"sv, L"mkdir"sv, L"more"sv, L"move"sv,
    L"net"sv, L"netsh"sv, L"ping"sv, L"rd"sv, L"reg"sv, L"ren"sv, L"rename"sv,
    L"rmdir"sv, L"robocopy"sv, L"sc"sv, L"sort"sv, L"start"sv, L"taskkill"sv,
    L"tasklist"sv, L"title"sv, L"type"sv, L"ver"sv, L"xcopy"sv,
});

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
        language == SyntaxLanguage::Mermaid || idx >= std::size(LANGUAGE_DEFS)) {
        return {};
    }
    const auto& def = LANGUAGE_DEFS[idx];
    return TokenizeGeneric(text, def.keywords, def.types, def.config);
}
