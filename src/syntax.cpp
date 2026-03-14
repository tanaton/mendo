#include "syntax.h"
#include <algorithm>
#include <unordered_set>

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

bool IsAtLineStart(const std::wstring& text, size_t pos) {
    if (pos == 0) return true;
    for (size_t i = pos - 1; ; i--) {
        if (text[i] == L'\n') return true;
        if (text[i] != L' ' && text[i] != L'\t') return false;
        if (i == 0) return true;
    }
}

std::wstring ToLower(const std::wstring& s) {
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

// ---- Lexer helpers ----

void EmitToken(std::vector<SyntaxToken>& tokens, uint32_t start, uint32_t length, SyntaxTokenType type) {
    if (length > 0) {
        tokens.push_back({start, length, type});
    }
}

// Scan a string literal starting at pos (pos points to the opening quote).
// Returns the position after the closing quote (or end of text if unterminated).
size_t ScanString(const std::wstring& text, size_t pos, wchar_t quote, bool allow_multiline) {
    size_t i = pos + 1;
    while (i < text.size()) {
        if (text[i] == L'\\') {
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
size_t ScanTripleQuote(const std::wstring& text, size_t pos, wchar_t quote) {
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
size_t ScanNumber(const std::wstring& text, size_t pos) {
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
bool IsFollowedByParen(const std::wstring& text, size_t end) {
    size_t i = end;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) i++;
    return i < text.size() && text[i] == L'(';
}

// ---- Generic tokenizer ----

std::vector<SyntaxToken> TokenizeGeneric(
    const std::wstring& text,
    const KeywordSet& keywords,
    const KeywordSet& types,
    bool has_line_comment_slash,    // //
    bool has_block_comment,         // /* */
    bool has_hash_comment,          // #
    bool has_preprocessor,          // # at line start
    bool has_triple_quote,          // """ '''
    bool has_backtick_string        // ` (JS template literals)
) {
    std::vector<SyntaxToken> tokens;
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

        // 1. Line comments: // or #
        if (has_line_comment_slash && c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') i++;
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        if (has_hash_comment && c == L'#' && !has_preprocessor) {
            flush_plain();
            size_t start = i;
            while (i < text.size() && text[i] != L'\n') i++;
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 2. Block comments: /* */
        if (has_block_comment && c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
            flush_plain();
            size_t start = i;
            i += 2;
            while (i + 1 < text.size()) {
                if (text[i] == L'*' && text[i + 1] == L'/') {
                    i += 2;
                    break;
                }
                i++;
            }
            if (i >= text.size()) i = text.size();
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::Comment);
            continue;
        }

        // 3. Preprocessor: # at line start (C/C++)
        if (has_preprocessor && c == L'#' && IsAtLineStart(text, i)) {
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

        // 4. Triple-quoted strings (Python)
        if (has_triple_quote && (c == L'"' || c == L'\'') &&
            i + 2 < text.size() && text[i + 1] == c && text[i + 2] == c) {
            flush_plain();
            size_t start = i;
            i = ScanTripleQuote(text, i, c);
            EmitToken(tokens, static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxTokenType::String);
            continue;
        }

        // 5. String literals
        if (c == L'"' || c == L'\'') {
            // Check for C++ raw string: R"(...)"
            if (c == L'"' && i > 0 && text[i - 1] == L'R') {
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
                if (paren != std::wstring::npos) {
                    std::wstring delim = text.substr(i + 1, paren - i - 1);
                    std::wstring end_marker = L")" + delim + L"\"";
                    size_t end_pos = text.find(end_marker, paren + 1);
                    if (end_pos != std::wstring::npos) {
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
        if (has_backtick_string && c == L'`') {
            flush_plain();
            size_t start = i;
            i = ScanString(text, i, L'`', true);
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

            SyntaxTokenType tt = SyntaxTokenType::Plain;
            if (keywords.count(word)) {
                tt = SyntaxTokenType::Keyword;
            } else if (types.count(word)) {
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

SyntaxLanguage DetectLanguage(const std::wstring& info_string) {
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
    if (lang == L"javascript" || lang == L"js" || lang == L"typescript" || lang == L"ts" ||
        lang == L"jsx" || lang == L"tsx") {
        return SyntaxLanguage::JavaScript;
    }

    return SyntaxLanguage::None;
}

std::vector<SyntaxToken> Tokenize(const std::wstring& text, SyntaxLanguage language) {
    if (text.empty() || language == SyntaxLanguage::None) {
        return {};
    }

    switch (language) {
        case SyntaxLanguage::Cpp:
            return TokenizeGeneric(text, CppKeywords(), CppTypes(),
                /*line_comment_slash=*/true, /*block_comment=*/true,
                /*hash_comment=*/false, /*preprocessor=*/true,
                /*triple_quote=*/false, /*backtick_string=*/false);

        case SyntaxLanguage::Python:
            return TokenizeGeneric(text, PythonKeywords(), PythonTypes(),
                /*line_comment_slash=*/false, /*block_comment=*/false,
                /*hash_comment=*/true, /*preprocessor=*/false,
                /*triple_quote=*/true, /*backtick_string=*/false);

        case SyntaxLanguage::JavaScript:
            return TokenizeGeneric(text, JsKeywords(), JsTypes(),
                /*line_comment_slash=*/true, /*block_comment=*/true,
                /*hash_comment=*/false, /*preprocessor=*/false,
                /*triple_quote=*/false, /*backtick_string=*/true);

        default:
            return {};
    }
}
