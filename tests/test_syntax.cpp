#include <gtest/gtest.h>
#include "syntax.h"
#include "parser.h"
#include <numeric>

// ============================================================
// DetectLanguage tests
// ============================================================

TEST(Syntax, DetectLanguageCpp) {
    EXPECT_EQ(DetectLanguage(L"cpp"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageC) {
    EXPECT_EQ(DetectLanguage(L"c"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageCPlusPlus) {
    EXPECT_EQ(DetectLanguage(L"c++"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageCxx) {
    EXPECT_EQ(DetectLanguage(L"cxx"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageH) {
    EXPECT_EQ(DetectLanguage(L"h"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageHpp) {
    EXPECT_EQ(DetectLanguage(L"hpp"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguagePython) {
    EXPECT_EQ(DetectLanguage(L"python"), SyntaxLanguage::Python);
}

TEST(Syntax, DetectLanguagePy) {
    EXPECT_EQ(DetectLanguage(L"py"), SyntaxLanguage::Python);
}

TEST(Syntax, DetectLanguageJavaScript) {
    EXPECT_EQ(DetectLanguage(L"javascript"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageJs) {
    EXPECT_EQ(DetectLanguage(L"js"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageTs) {
    EXPECT_EQ(DetectLanguage(L"typescript"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(L"ts"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageJsx) {
    EXPECT_EQ(DetectLanguage(L"jsx"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(L"tsx"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageUnknown) {
    EXPECT_EQ(DetectLanguage(L"rust"), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage(L"go"), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage(L"java"), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageEmpty) {
    EXPECT_EQ(DetectLanguage(L""), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageCaseInsensitive) {
    EXPECT_EQ(DetectLanguage(L"CPP"), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage(L"Python"), SyntaxLanguage::Python);
    EXPECT_EQ(DetectLanguage(L"JavaScript"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(L"JS"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageWithExtraInfo) {
    // md4c may provide info string with extra text after language
    EXPECT_EQ(DetectLanguage(L"cpp some-extra"), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage(L"python\ttab-separated"), SyntaxLanguage::Python);
}

// ============================================================
// Tokenize - basic tests
// ============================================================

TEST(Syntax, EmptyTextReturnsEmpty) {
    auto tokens = Tokenize(L"", SyntaxLanguage::Cpp);
    EXPECT_TRUE(tokens.empty());
}

TEST(Syntax, NoneLanguageReturnsEmpty) {
    auto tokens = Tokenize(L"int main() {}", SyntaxLanguage::None);
    EXPECT_TRUE(tokens.empty());
}

// Helper: check that tokens cover the entire text contiguously
void AssertTokensCoverText(const std::vector<SyntaxToken>& tokens, size_t text_length) {
    if (text_length == 0) {
        EXPECT_TRUE(tokens.empty());
        return;
    }
    ASSERT_FALSE(tokens.empty());

    // First token starts at 0
    EXPECT_EQ(tokens[0].start, 0u);

    // Tokens are contiguous
    for (size_t i = 1; i < tokens.size(); i++) {
        EXPECT_EQ(tokens[i].start, tokens[i - 1].start + tokens[i - 1].length)
            << "Gap between token " << (i - 1) << " and " << i;
    }

    // Total length matches
    uint32_t total = 0;
    for (const auto& t : tokens) total += t.length;
    EXPECT_EQ(total, static_cast<uint32_t>(text_length));
}

// Helper: find first token of a given type
const SyntaxToken* FindToken(const std::vector<SyntaxToken>& tokens, SyntaxTokenType type) {
    for (const auto& t : tokens) {
        if (t.type == type) return &t;
    }
    return nullptr;
}

// Helper: count tokens of a given type
int CountTokens(const std::vector<SyntaxToken>& tokens, SyntaxTokenType type) {
    int count = 0;
    for (const auto& t : tokens) {
        if (t.type == type) count++;
    }
    return count;
}

// Helper: get the text for a token
std::wstring GetTokenText(const std::wstring& text, const SyntaxToken& token) {
    return text.substr(token.start, token.length);
}

TEST(Syntax, TokensCoverEntireTextCpp) {
    std::wstring code = L"int main() { return 0; }";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, TokensCoverEntireTextPython) {
    std::wstring code = L"def hello():\n    print('world')";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, TokensCoverEntireTextJs) {
    std::wstring code = L"const f = () => { return 42; };";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// C/C++ tokenization
// ============================================================

TEST(Syntax, CppKeywords) {
    std::wstring code = L"if else while for return";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    // All identifiers should be keywords
    for (const auto& t : tokens) {
        if (t.type != SyntaxTokenType::Plain) {
            EXPECT_EQ(t.type, SyntaxTokenType::Keyword) << "offset=" << t.start;
        }
    }
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 5);
}

TEST(Syntax, CppTypes) {
    std::wstring code = L"int float double bool";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, CppSingleLineComment) {
    std::wstring code = L"x = 1; // comment\ny = 2;";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"// comment");
}

TEST(Syntax, CppMultiLineComment) {
    std::wstring code = L"/* multi\nline\ncomment */";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, CppStringDouble) {
    std::wstring code = L"x = \"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"\"hello world\"");
}

TEST(Syntax, CppStringSingle) {
    std::wstring code = L"c = 'x'";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"'x'");
}

TEST(Syntax, CppStringEscape) {
    std::wstring code = L"s = \"hello\\\"world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"\"hello\\\"world\"");
}

TEST(Syntax, CppNumberInteger) {
    std::wstring code = L"x = 42";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"42");
}

TEST(Syntax, CppNumberHex) {
    std::wstring code = L"x = 0xFF";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"0xFF");
}

TEST(Syntax, CppNumberFloat) {
    std::wstring code = L"x = 3.14f";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"3.14f");
}

TEST(Syntax, CppNumberBinary) {
    std::wstring code = L"x = 0b1010";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"0b1010");
}

TEST(Syntax, CppPreprocessorInclude) {
    std::wstring code = L"#include <stdio.h>";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Preprocessor);
}

TEST(Syntax, CppPreprocessorDefine) {
    std::wstring code = L"#define MAX 100";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Preprocessor);
}

TEST(Syntax, CppPreprocessorNotAtLineStart) {
    // # after code should not be preprocessor
    std::wstring code = L"x = a #";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Preprocessor), 0);
}

TEST(Syntax, CppFunctionCall) {
    std::wstring code = L"foo(42)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), L"foo");
}

TEST(Syntax, CppFunctionCallWithSpace) {
    std::wstring code = L"bar (x)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), L"bar");
}

TEST(Syntax, CppKeywordNotFunction) {
    // Keywords followed by ( should still be keywords, not functions
    std::wstring code = L"if (x)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), L"if");
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Function), 0);
}

TEST(Syntax, CppComplexCode) {
    std::wstring code = L"#include <iostream>\n\nint main() {\n    // Hello\n    std::cout << \"Hello\" << 42;\n    return 0;\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Preprocessor), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 1);       // int
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 1);   // main
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // Hello
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);     // "Hello"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 1);     // 42
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 1);    // return
}

// ============================================================
// Python tokenization
// ============================================================

TEST(Syntax, PythonKeywords) {
    std::wstring code = L"if else while for return def class";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, PythonTypes) {
    std::wstring code = L"int float str bool list dict";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, PythonComment) {
    std::wstring code = L"x = 1  # comment\ny = 2";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"# comment");
}

TEST(Syntax, PythonTripleQuoteDouble) {
    std::wstring code = L"s = \"\"\"hello\nworld\"\"\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"\"\"\"hello\nworld\"\"\"");
}

TEST(Syntax, PythonTripleQuoteSingle) {
    std::wstring code = L"s = '''docstring'''";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"'''docstring'''");
}

TEST(Syntax, PythonDefFunction) {
    std::wstring code = L"def foo():";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), L"def");
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), L"foo");
}

TEST(Syntax, PythonTrueFalseNone) {
    std::wstring code = L"x = True\ny = False\nz = None";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 3);
}

TEST(Syntax, PythonFString) {
    std::wstring code = L"f\"hello {name}\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    // 'f' is plain, then the string
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PythonComplexCode) {
    std::wstring code = L"def greet(name: str) -> str:\n    # Greeting\n    return f\"Hello, {name}!\"\n\nprint(greet(\"World\"))";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);   // def, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);       // str, str
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // # Greeting
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2);   // greet, print
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

// ============================================================
// JavaScript tokenization
// ============================================================

TEST(Syntax, JsKeywords) {
    std::wstring code = L"if else while for return const let var function";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, JsTypes) {
    std::wstring code = L"Array Map Set Promise";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, JsSingleLineComment) {
    std::wstring code = L"// comment\nx = 1";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"// comment");
}

TEST(Syntax, JsMultiLineComment) {
    std::wstring code = L"/* block\ncomment */";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, JsTemplateLiteral) {
    std::wstring code = L"`hello ${name}`";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"`hello ${name}`");
}

TEST(Syntax, JsTemplateLiteralMultiLine) {
    std::wstring code = L"`line1\nline2\nline3`";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::String);
}

TEST(Syntax, JsArrowFunction) {
    std::wstring code = L"const f = () => 42";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), L"const");
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"42");
}

TEST(Syntax, JsTrueFalseNull) {
    std::wstring code = L"true false null undefined";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, JsComplexCode) {
    std::wstring code = L"async function fetchData(url) {\n  // Fetch data\n  const resp = await fetch(url);\n  return resp.json();\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 4);   // async, function, const, await, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // Fetch data
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2);   // fetchData, fetch
}

// ============================================================
// Edge cases
// ============================================================

TEST(Syntax, OnlyWhitespace) {
    std::wstring code = L"   \n\t  \n  ";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    for (const auto& t : tokens) {
        EXPECT_EQ(t.type, SyntaxTokenType::Plain);
    }
}

TEST(Syntax, OnlyOperators) {
    std::wstring code = L"+ - * / = == != < > <= >=";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, UnterminatedString) {
    // Unterminated string should not cause infinite loop
    std::wstring code = L"x = \"unterminated\ny = 1";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, UnterminatedBlockComment) {
    std::wstring code = L"/* never closed";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, NumberAtEndOfText) {
    std::wstring code = L"x = 123";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"123");
}

TEST(Syntax, DotNotANumber) {
    // A lone dot should not be treated as a number
    std::wstring code = L"a.b";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 0);
}

TEST(Syntax, MultipleLinesOfCode) {
    std::wstring code =
        L"int x = 10;\n"
        L"float y = 3.14f;\n"
        L"// comment\n"
        L"if (x > 0) {\n"
        L"    return y;\n"
        L"}";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);      // int, float
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 2);     // 10, 3.14f
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);    // if, return
}

// ============================================================
// Parser integration: language extraction
// ============================================================

TEST(Syntax, ParserExtractsLanguageCpp) {
    auto nodes = ParseMarkdown("```cpp\nint x = 1;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

TEST(Syntax, ParserExtractsLanguagePython) {
    auto nodes = ParseMarkdown("```python\ndef foo(): pass\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Python);
}

TEST(Syntax, ParserExtractsLanguageJs) {
    auto nodes = ParseMarkdown("```js\nconst x = 1;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::JavaScript);
}

TEST(Syntax, ParserNoLanguage) {
    auto nodes = ParseMarkdown("```\nplain code\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::None);
}

TEST(Syntax, ParserUnknownLanguage) {
    auto nodes = ParseMarkdown("```rust\nfn main() {}\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::None);
}

TEST(Syntax, ParserCaseInsensitiveLanguage) {
    auto nodes = ParseMarkdown("```CPP\nint x;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

// ---- Additional edge cases ----

// Mermaid detection
TEST(Syntax, DetectLanguageMermaid) {
    EXPECT_EQ(DetectLanguage(L"mermaid"), SyntaxLanguage::Mermaid);
}

// C++ raw strings
TEST(Syntax, CppRawString) {
    auto tokens = Tokenize(L"R\"(hello)\"", SyntaxLanguage::Cpp);
    // Should detect the raw string as a single string token
    bool has_string = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::String) {
            has_string = true;
        }
    }
    EXPECT_TRUE(has_string);
}

// C++ octal number
TEST(Syntax, CppNumberOctal) {
    auto tokens = Tokenize(L"0o77", SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) has_number = true;
    }
    EXPECT_TRUE(has_number);
}

// C++ number suffix
TEST(Syntax, CppNumberWithSuffix) {
    auto tokens = Tokenize(L"42ULL", SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) {
            has_number = true;
            // The entire "42ULL" should be a single number token
            EXPECT_EQ(t.length, 5u);
        }
    }
    EXPECT_TRUE(has_number);
}

// Python decorator
TEST(Syntax, PythonDecorator) {
    auto tokens = Tokenize(L"@staticmethod\ndef foo():\n    pass", SyntaxLanguage::Python);
    // "@" is not specifically handled, but "def" and "pass" should still be keywords
    bool has_def = false;
    bool has_pass = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Keyword) {
            std::wstring_view word(L"@staticmethod\ndef foo():\n    pass" + t.start, t.length);
            if (word == L"def") has_def = true;
            if (word == L"pass") has_pass = true;
        }
    }
    EXPECT_TRUE(has_def);
    EXPECT_TRUE(has_pass);
}

// JavaScript BigInt
TEST(Syntax, JsBigIntNumber) {
    auto tokens = Tokenize(L"42n", SyntaxLanguage::JavaScript);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) {
            has_number = true;
            EXPECT_EQ(t.length, 3u); // "42n"
        }
    }
    EXPECT_TRUE(has_number);
}

// Empty code block
TEST(Syntax, TokenizeEmptyCpp) {
    auto tokens = Tokenize(L"", SyntaxLanguage::Cpp);
    EXPECT_TRUE(tokens.empty());
}

// Single character
TEST(Syntax, TokenizeSingleKeyword) {
    auto tokens = Tokenize(L"if", SyntaxLanguage::Cpp);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Keyword);
}

// C++ line comment at end of text (no newline)
TEST(Syntax, CppCommentEol) {
    auto tokens = Tokenize(L"int x; // comment", SyntaxLanguage::Cpp);
    bool has_comment = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Comment) has_comment = true;
    }
    EXPECT_TRUE(has_comment);
}

// Float that starts with dot
TEST(Syntax, NumberStartsWithDot) {
    auto tokens = Tokenize(L".5f", SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) has_number = true;
    }
    EXPECT_TRUE(has_number);
}

// C++ preprocessor with line continuation
TEST(Syntax, CppPreprocessorContinuation) {
    auto tokens = Tokenize(L"#define FOO \\\n    bar", SyntaxLanguage::Cpp);
    // Should be a single preprocessor token spanning the continuation
    bool has_prep = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Preprocessor) {
            has_prep = true;
        }
    }
    EXPECT_TRUE(has_prep);
}

// Detect Tsx extension
TEST(Syntax, DetectLanguageTsx) {
    EXPECT_EQ(DetectLanguage(L"tsx"), SyntaxLanguage::JavaScript);
}

// Detect unknown extensions
TEST(Syntax, DetectLanguageRuby) {
    EXPECT_EQ(DetectLanguage(L"ruby"), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageGo) {
    EXPECT_EQ(DetectLanguage(L"go"), SyntaxLanguage::None);
}

// ============================================================
// Additional language extensions
// ============================================================

TEST(Syntax, DetectLanguageCc) {
    EXPECT_EQ(DetectLanguage(L"cc"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageHxx) {
    EXPECT_EQ(DetectLanguage(L"hxx"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageMermaidCaseInsensitive) {
    EXPECT_EQ(DetectLanguage(L"Mermaid"), SyntaxLanguage::Mermaid);
    EXPECT_EQ(DetectLanguage(L"MERMAID"), SyntaxLanguage::Mermaid);
}

// ============================================================
// Mermaid tokenization returns empty (no keyword tables)
// ============================================================

TEST(Syntax, MermaidLanguageReturnsEmpty) {
    // Mermaid has no tokenizer in the current implementation
    auto tokens = Tokenize(L"graph TD; A-->B;", SyntaxLanguage::Mermaid);
    EXPECT_TRUE(tokens.empty());
}

// ============================================================
// Number edge cases
// ============================================================

TEST(Syntax, CppNumberExponent) {
    std::wstring code = L"1.5e10";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"1.5e10");
}

TEST(Syntax, CppNumberExponentNegative) {
    std::wstring code = L"2.0e-3";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"2.0e-3");
}

TEST(Syntax, CppNumberDigitSeparator) {
    std::wstring code = L"1'000'000";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"1'000'000");
}

TEST(Syntax, CppHexDigitSeparator) {
    std::wstring code = L"0xFF'FF";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), L"0xFF'FF");
}

// ============================================================
// C++ raw string with delimiter
// ============================================================

TEST(Syntax, CppRawStringWithDelimiter) {
    std::wstring code = LR"(R"delim(hello "world")delim")";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    // R is emitted as a separate identifier, then the raw string follows
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    // Entire R"delim(...)delim" captured (R as plain + string body)
    EXPECT_GT(str->length, 10u);
}

// ============================================================
// Python unterminated triple-quote
// ============================================================

TEST(Syntax, PythonUnterminatedTripleQuote) {
    std::wstring code = L"s = \"\"\"never closed";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

// ============================================================
// C++ modern keywords
// ============================================================

TEST(Syntax, CppModernKeywords) {
    std::wstring code = L"constexpr consteval constinit concept requires co_await co_return co_yield";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CppCastKeywords) {
    std::wstring code = L"static_cast dynamic_cast reinterpret_cast const_cast";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 4);
}

// ============================================================
// C++ STL types
// ============================================================

TEST(Syntax, CppStlTypes) {
    std::wstring code = L"vector map optional variant span unique_ptr shared_ptr";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 7);
}

TEST(Syntax, CppWin32Types) {
    std::wstring code = L"HRESULT BOOL DWORD HWND LRESULT";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

// ============================================================
// Python exception types
// ============================================================

TEST(Syntax, PythonExceptionTypes) {
    std::wstring code = L"ValueError TypeError KeyError IndexError RuntimeError";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

// ============================================================
// JavaScript globals
// ============================================================

TEST(Syntax, JsGlobalTypes) {
    std::wstring code = L"console document window JSON Math";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, JsAsyncAwait) {
    std::wstring code = L"async await";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 2);
}

// ============================================================
// Bug #16: Raw string R" detection should not trigger on
// identifiers ending with R (e.g. RENDER"hello")
// ============================================================

TEST(Syntax, CppRawStringNotTriggeredByIdentifierEndingR) {
    std::wstring code = L"RENDER\"hello\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    // RENDER should be a single identifier token (Plain or Function)
    // "hello" should be a string token
    // They should NOT overlap
    bool found_render = false;
    bool found_string = false;
    for (const auto& t : tokens) {
        std::wstring text = GetTokenText(code, t);
        if (text == L"RENDER") {
            found_render = true;
            // Should NOT be a string
            EXPECT_NE(t.type, SyntaxTokenType::String);
        }
        if (text == L"\"hello\"") {
            found_string = true;
            EXPECT_EQ(t.type, SyntaxTokenType::String);
        }
    }
    EXPECT_TRUE(found_render) << "Should find RENDER as a separate token";
    EXPECT_TRUE(found_string) << "Should find \"hello\" as a string token";
}

TEST(Syntax, CppRawStringStandaloneRStillWorks) {
    // Standalone R"(...)" should still be recognized as raw string
    std::wstring code = L"R\"(hello)\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_GT(str->length, 5u);
}

TEST(Syntax, CppRawStringAfterSpaceR) {
    // "x R\"(test)\"" — R preceded by space should work
    std::wstring code = L"x R\"(test)\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

// ============================================================
// Bug #22: Unterminated block comment should include last char
// ============================================================

TEST(Syntax, UnterminatedBlockCommentCoversAllText) {
    std::wstring code = L"/* unterminated comment";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, UnterminatedBlockCommentEndsWithStar) {
    // Edge case: comment ends with * but no /
    std::wstring code = L"/* test *";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, TerminatedBlockCommentStillWorks) {
    std::wstring code = L"/* ok */ x";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"/* ok */");
}
