#include "syntax.h"
#include <algorithm>
#include <unordered_set>
#include <memory_resource>

namespace {

// ---- Helper functions ----

bool IsIdentStart(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c >= 0x80;
}

bool IsIdentChar(wchar_t c) {
    return IsIdentStart(c) || (c >= L'0' && c <= L'9');
}

bool IsDigit(wchar_t c) {
    return c >= L'0' && c <= L'9';
}

bool IsHexDigit(wchar_t c) {
    return IsDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

bool IsWhitespace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r';
}

bool IsAtLineStart(std::wstring_view text, size_t pos) {
    if (pos == 0) return true;
    for (size_t i = pos - 1; ; i--) {
        if (text[i] == L'\n') return true;
        if (text[i] != L' ' && text[i] != L'\t') return false;
        if (i == 0) return true;
    }
}

std::wstring ToLower(std::wstring_view s) {
    std::wstring result;
    result.reserve(s.size());
    for (wchar_t c : s) {
        if (c >= L'A' && c <= L'Z')
            result += static_cast<wchar_t>(c - L'A' + L'a');
        else
            result += c;
    }
    return result;
}

// ---- Keyword tables ----

using KeywordSet = std::unordered_set<std::wstring_view>;

KeywordSet MergeKeywords(const KeywordSet& base, std::initializer_list<std::wstring_view> extra) {
    KeywordSet result = base;
    result.insert(extra);
    return result;
}

const KeywordSet& CppKeywords() {
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

const KeywordSet& CppTypes() {
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

const KeywordSet& PythonKeywords() {
    static const KeywordSet s = {
        L"and", L"as", L"assert", L"async", L"await", L"break", L"class",
        L"continue", L"def", L"del", L"elif", L"else", L"except", L"finally",
        L"for", L"from", L"global", L"if", L"import", L"in", L"is", L"lambda",
        L"nonlocal", L"not", L"or", L"pass", L"raise", L"return", L"try",
        L"while", L"with", L"yield", L"True", L"False", L"None"
    };
    return s;
}

const KeywordSet& PythonTypes() {
    static const KeywordSet s = {
        L"int", L"float", L"str", L"bool", L"list", L"dict", L"set", L"tuple",
        L"bytes", L"bytearray", L"object", L"type", L"range", L"complex",
        L"frozenset", L"memoryview", L"property", L"classmethod", L"staticmethod",
        L"Exception", L"ValueError", L"TypeError", L"KeyError", L"IndexError",
        L"RuntimeError", L"StopIteration", L"OSError", L"IOError"
    };
    return s;
}

const KeywordSet& JsKeywords() {
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

const KeywordSet& JsTypes() {
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

const KeywordSet& GoKeywords() {
    static const KeywordSet s = {
        L"break", L"case", L"chan", L"const", L"continue", L"default", L"defer",
        L"else", L"fallthrough", L"for", L"func", L"go", L"goto", L"if", L"import",
        L"interface", L"map", L"package", L"range", L"return", L"select", L"struct",
        L"switch", L"type", L"var"
    };
    return s;
}

const KeywordSet& GoTypes() {
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

const KeywordSet& RustKeywords() {
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

const KeywordSet& RustTypes() {
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

// ---- TypeScript (JS superset) ----

const KeywordSet& TsKeywords() {
    static const KeywordSet s = MergeKeywords(JsKeywords(), {
        L"abstract", L"declare", L"enum", L"implements", L"infer",
        L"interface", L"is", L"keyof", L"namespace", L"override",
        L"readonly", L"satisfies", L"type", L"module", L"asserts"
    });
    return s;
}

const KeywordSet& TsTypes() {
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

const KeywordSet& BashKeywords() {
    static const KeywordSet s = {
        L"if", L"then", L"else", L"elif", L"fi", L"case", L"esac",
        L"for", L"while", L"until", L"do", L"done", L"in", L"function",
        L"select", L"time", L"return", L"exit", L"break", L"continue",
        L"declare", L"local", L"export", L"readonly", L"typeset", L"unset",
        L"shift", L"source", L"eval", L"exec", L"trap", L"set"
    };
    return s;
}

const KeywordSet& BashTypes() {
    static const KeywordSet s = {
        L"echo", L"printf", L"read", L"test", L"true", L"false",
        L"cd", L"pwd", L"alias", L"unalias", L"type", L"which",
        L"command", L"builtin", L"let", L"getopts", L"mapfile", L"readarray"
    };
    return s;
}

// ---- PowerShell (keywords stored in lowercase for case-insensitive matching) ----

const KeywordSet& PwshKeywords() {
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

const KeywordSet& PwshTypes() {
    static const KeywordSet s = {
        L"int", L"long", L"float", L"double", L"decimal", L"bool",
        L"byte", L"string", L"char", L"array", L"hashtable", L"xml",
        L"datetime", L"timespan", L"regex", L"scriptblock", L"void",
        L"null", L"true", L"false"
    };
    return s;
}

// ---- Cmd (keywords stored in lowercase for case-insensitive matching) ----

const KeywordSet& CmdKeywords() {
    static const KeywordSet s = {
        L"if", L"else", L"for", L"do", L"goto", L"call", L"set",
        L"setlocal", L"endlocal", L"echo", L"pause", L"exit", L"rem",
        L"not", L"exist", L"defined", L"equ", L"neq", L"lss", L"leq",
        L"gtr", L"geq", L"errorlevel", L"off", L"on", L"in"
    };
    return s;
}

const KeywordSet& CmdTypes() {
    static const KeywordSet s = {
        L"dir", L"copy", L"move", L"del", L"ren", L"rename",
        L"mkdir", L"md", L"rmdir", L"rd", L"type", L"find", L"findstr",
        L"sort", L"more", L"cls", L"title", L"color", L"start",
        L"taskkill", L"tasklist", L"reg", L"sc", L"net", L"netsh",
        L"ping", L"ipconfig", L"ver", L"attrib", L"xcopy", L"robocopy"
    };
    return s;
}

// ---- Lexer helpers ----

void EmitToken(std::pmr::vector<SyntaxToken>& tokens, uint32_t start, uint32_t length, SyntaxTokenType type) {
    if (length > 0) {
        tokens.push_back({start, length, type});
    }
}

// Scan a string literal starting at pos (pos points to the opening quote).
// Returns the position after the closing quote (or end of text if unterminated).
size_t ScanString(std::wstring_view text, size_t pos, wchar_t quote, bool allow_multiline, bool handle_escape = true) {
    size_t i = pos + 1;
    while (i < text.size()) {
        if (handle_escape && text[i] == L'\\') {
            i += 2;
            if (i > text.size()) i = text.size();
        } else if (text[i] == quote) {
            return i + 1;
        } else if (!allow_multiline && text[i] == L'\n') {
            return i; // unterminated
        } else {
            i++;
        }
    }
    return i;
}

// Scan a Python triple-quoted string.
size_t ScanTripleQuote(std::wstring_view text, size_t pos, wchar_t quote) {
    // pos points to the first quote of the triple
    size_t i = pos + 3;
    while (i + 2 < text.size()) {
        if (text[i] == L'\\') {
            i += 2;
        } else if (text[i] == quote && text[i + 1] == quote && text[i + 2] == quote) {
            return i + 3;
        } else {
            i++;
        }
    }
    return text.size(); // unterminated
}

// Scan a number literal starting at pos.
size_t ScanNumber(std::wstring_view text, size_t pos) {
    size_t i = pos;

    // Handle 0x, 0b, 0o prefixes
    if (i + 1 < text.size() && text[i] == L'0') {
        wchar_t next = text[i + 1];
        if (next == L'x' || next == L'X') {
            i += 2;
            while (i < text.size() && (IsHexDigit(text[i]) || text[i] == L'\'')) i++;
            // Suffixes
            while (i < text.size() && (text[i] == L'u' || text[i] == L'U' || text[i] == L'l' || text[i] == L'L')) i++;
            return i;
        }
        if (next == L'b' || next == L'B') {
            i += 2;
            while (i < text.size() && (text[i] == L'0' || text[i] == L'1' || text[i] == L'\'')) i++;
            return i;
        }
        if (next == L'o' || next == L'O') {
            i += 2;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'7') i++;
            return i;
        }
    }

    // Integer / floating point
    while (i < text.size() && (IsDigit(text[i]) || text[i] == L'\'')) i++;

    // Decimal point
    if (i < text.size() && text[i] == L'.') {
        i++;
        while (i < text.size() && (IsDigit(text[i]) || text[i] == L'\'')) i++;
    }

    // Exponent
    if (i < text.size() && (text[i] == L'e' || text[i] == L'E')) {
        i++;
        if (i < text.size() && (text[i] == L'+' || text[i] == L'-')) i++;
        while (i < text.size() && IsDigit(text[i])) i++;
    }

    // Suffixes (f, F, l, L, u, U, etc.)
    while (i < text.size() && (text[i] == L'f' || text[i] == L'F' ||
                                text[i] == L'l' || text[i] == L'L' ||
                                text[i] == L'u' || text[i] == L'U' ||
                                text[i] == L'n')) i++;  // 'n' for JS BigInt

    return i;
}

// Check if identifier at [start, end) is followed by '(' (skipping whitespace).
bool IsFollowedByParen(std::wstring_view text, size_t end) {
    size_t i = end;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) i++;
    return i < text.size() && text[i] == L'(';
}

// Scan a block comment starting at pos (pos points to the first char of the opening pair).
// Returns the position after the closing pair, or text.size() if unterminated.
size_t ScanBlockComment(std::wstring_view text, size_t pos, wchar_t close1, wchar_t close2) {
    size_t i = pos + 2;
    while (i + 1 < text.size()) {
        if (text[i] == close1 && text[i + 1] == close2) {
            return i + 2;
        }
        i++;
    }
    return text.size();
}

// ---- Generic tokenizer ----

struct LexerConfig {
    bool line_comment_slash = false;    // //
    bool block_comment = false;         // /* */
    bool hash_comment = false;          // #
    bool preprocessor = false;          // # at line start
    bool triple_quote = false;          // """ '''
    bool backtick_string = false;       // `
    bool angle_block_comment = false;   // <# #>
    bool double_colon_comment = false;  // ::
    bool rem_comment = false;           // REM
    bool case_insensitive = false;      // case-insensitive keyword matching
    bool skip_single_quote = false;     // don't treat ' as string delimiter
    bool raw_backtick = false;          // backtick strings without escape (Go)
};

std::pmr::vector<SyntaxToken> TokenizeGeneric(
    std::wstring_view text,
    const KeywordSet& keywords,
    const KeywordSet& types,
    const LexerConfig& cfg
) {
    std::pmr::vector<SyntaxToken> tokens;
    tokens.reserve(text.size() / 4);
    size_t i = 0;
    uint32_t plain_start = 0;
    bool in_plain = false;

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

        // 1. Line comments: //
        if (cfg.line_comment_slash && c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') i++;
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 1b. Angle block comments: <# #> (PowerShell)
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
            while (i < text.size() && text[i] != L'\n') i++;
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 2. Block comments: /* */
        if (cfg.block_comment && c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
            flush_plain();
            size_t start = i;
            i = ScanBlockComment(text, i, L'*', L'/');
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 3. Preprocessor: # at line start (C/C++)
        if (cfg.preprocessor && c == L'#' && IsAtLineStart(text, i)) {
            flush_plain();
            size_t start = i;
            while (i < text.size()) {
                if (text[i] == L'\n') {
                    // Check for line continuation
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

        // 3b. Double-colon comments: :: at line start (cmd)
        if (cfg.double_colon_comment && c == L':' && i + 1 < text.size() && text[i + 1] == L':' &&
            IsAtLineStart(text, i)) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') i++;
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 3c. REM comments: REM at line start (cmd)
        if (cfg.rem_comment && (c == L'r' || c == L'R') && IsAtLineStart(text, i) &&
            i + 2 < text.size() &&
            (text[i + 1] == L'e' || text[i + 1] == L'E') &&
            (text[i + 2] == L'm' || text[i + 2] == L'M') &&
            (i + 3 >= text.size() || !IsIdentChar(text[i + 3]))) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') i++;
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 4. Triple-quoted strings (Python)
        if (cfg.triple_quote && (c == L'"' || c == L'\'') &&
            i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
            flush_plain();
            size_t start = i;
            i = ScanTripleQuote(text, i, c);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 5. String literals
        if (c == L'"' || (c == L'\'' && !cfg.skip_single_quote)) {
            // Check for C++ raw string: R"(...)"
            if (c == L'"' && i > 0 && text[i - 1] == L'R' &&
                (i < 2 || !IsIdentChar(text[i - 2]))) {
                // Adjust: the R is already in the plain buffer. Remove it.
                if (in_plain) {
                    if (static_cast<uint32_t>(i - 1) > plain_start) {
                        EmitToken(tokens, plain_start, static_cast<uint32_t>(i - 1) - plain_start, SyntaxTokenType::Plain);
                    }
                    in_plain = false;
                }
                size_t start = i - 1;
                // Find the delimiter: R"DELIM( ... )DELIM"
                size_t paren = text.find(L'(', i + 1);
                if (paren != std::wstring_view::npos) {
                    std::wstring delim{text.substr(i + 1, paren - i - 1)};
                    std::wstring end_marker = L")" + delim + L"\"";
                    size_t end_pos = text.find(std::wstring_view{end_marker}, paren + 1);
                    if (end_pos != std::wstring_view::npos) {
                        i = end_pos + end_marker.size();
                    } else {
                        i = text.size();
                    }
                } else {
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

        // 6. Backtick template literals (JS)
        if (cfg.backtick_string && c == L'`') {
            flush_plain();
            size_t start = i;
            i = ScanString(text, i, L'`', true, !cfg.raw_backtick);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 7. Numbers
        if (IsDigit(c) || (c == L'.' && i + 1 < text.size() && IsDigit(text[i + 1]))) {
            flush_plain();
            size_t start = i;
            i = ScanNumber(text, i);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Number);
            continue;
        }

        // 8. Identifiers and keywords
        if (IsIdentStart(c)) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && IsIdentChar(text[i])) i++;

            std::wstring_view word(text.data() + start, i - start);
            std::wstring word_lower;
            std::wstring_view lookup_word = word;
            if (cfg.case_insensitive) {
                bool has_upper = false;
                for (wchar_t ch : word) {
                    if (ch >= L'A' && ch <= L'Z') { has_upper = true; break; }
                }
                if (has_upper) {
                    word_lower = ToLower(word);
                    lookup_word = word_lower;
                }
            }

            SyntaxTokenType tt = SyntaxTokenType::Plain;
            if (keywords.count(lookup_word)) {
                tt = SyntaxTokenType::Keyword;
            } else if (types.count(lookup_word)) {
                tt = SyntaxTokenType::Type;
            } else if (IsFollowedByParen(text, i)) {
                tt = SyntaxTokenType::Function;
            }

            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), tt);
            continue;
        }

        // 9. Everything else: accumulate as plain
        start_plain();
        i++;
    }

    flush_plain();
    return tokens;
}

} // namespace

// ---- Public API ----

SyntaxLanguage DetectLanguage(std::wstring_view info_string) {
    if (info_string.empty()) return SyntaxLanguage::None;

    // Extract first word and lowercase it
    std::wstring lang;
    for (wchar_t c : info_string) {
        if (c == L' ' || c == L'\t') break;
        lang += c;
    }
    lang = ToLower(lang);

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

std::pmr::vector<SyntaxToken> Tokenize(std::wstring_view text, SyntaxLanguage language) {
    if (text.empty() || language == SyntaxLanguage::None) {
        return {};
    }

    switch (language) {
        case SyntaxLanguage::Cpp:
            return TokenizeGeneric(text, CppKeywords(), CppTypes(), {
                .line_comment_slash = true, .block_comment = true,
                .preprocessor = true,
            });

        case SyntaxLanguage::Python:
            return TokenizeGeneric(text, PythonKeywords(), PythonTypes(), {
                .hash_comment = true, .triple_quote = true,
            });

        case SyntaxLanguage::JavaScript:
            return TokenizeGeneric(text, JsKeywords(), JsTypes(), {
                .line_comment_slash = true, .block_comment = true,
                .backtick_string = true,
            });

        case SyntaxLanguage::Go:
            return TokenizeGeneric(text, GoKeywords(), GoTypes(), {
                .line_comment_slash = true, .block_comment = true,
                .backtick_string = true, .raw_backtick = true,
            });

        case SyntaxLanguage::Rust:
            return TokenizeGeneric(text, RustKeywords(), RustTypes(), {
                .line_comment_slash = true, .block_comment = true,
                .skip_single_quote = true,
            });

        case SyntaxLanguage::TypeScript:
            return TokenizeGeneric(text, TsKeywords(), TsTypes(), {
                .line_comment_slash = true, .block_comment = true,
                .backtick_string = true,
            });

        case SyntaxLanguage::Bash:
            return TokenizeGeneric(text, BashKeywords(), BashTypes(), {
                .hash_comment = true, .backtick_string = true,
            });

        case SyntaxLanguage::PowerShell:
            return TokenizeGeneric(text, PwshKeywords(), PwshTypes(), {
                .hash_comment = true, .angle_block_comment = true,
                .case_insensitive = true,
            });

        case SyntaxLanguage::Cmd:
            return TokenizeGeneric(text, CmdKeywords(), CmdTypes(), {
                .double_colon_comment = true, .rem_comment = true,
                .case_insensitive = true, .skip_single_quote = true,
            });

        default:
            return {};
    }
}
