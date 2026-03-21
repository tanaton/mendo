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
    EXPECT_EQ(DetectLanguage(L"typescript"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(L"ts"), SyntaxLanguage::TypeScript);
}

TEST(Syntax, DetectLanguageJsx) {
    EXPECT_EQ(DetectLanguage(L"jsx"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(L"tsx"), SyntaxLanguage::TypeScript);
}

TEST(Syntax, DetectLanguageUnknown) {
    EXPECT_EQ(DetectLanguage(L"java"), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage(L"ruby"), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage(L"swift"), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageEmpty) {
    EXPECT_EQ(DetectLanguage(L""), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageCaseInsensitive) {
    EXPECT_EQ(DetectLanguage(L"CPP"), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage(L"Python"), SyntaxLanguage::Python);
    EXPECT_EQ(DetectLanguage(L"JavaScript"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(L"JS"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(L"TypeScript"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(L"GO"), SyntaxLanguage::Go);
    EXPECT_EQ(DetectLanguage(L"RUST"), SyntaxLanguage::Rust);
    EXPECT_EQ(DetectLanguage(L"BASH"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(L"PowerShell"), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage(L"CMD"), SyntaxLanguage::Cmd);
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

TEST(Syntax, ParserExtractsLanguageRust) {
    auto nodes = ParseMarkdown("```rust\nfn main() {}\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Rust);
}

TEST(Syntax, ParserUnknownLanguage) {
    auto nodes = ParseMarkdown("```java\nclass Main {}\n```");
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
    EXPECT_EQ(DetectLanguage(L"tsx"), SyntaxLanguage::TypeScript);
}

// Detect unknown extensions
TEST(Syntax, DetectLanguageRuby) {
    EXPECT_EQ(DetectLanguage(L"ruby"), SyntaxLanguage::None);
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

// ============================================================
// Go tokenization
// ============================================================

TEST(Syntax, DetectLanguageGo) {
    EXPECT_EQ(DetectLanguage(L"go"), SyntaxLanguage::Go);
    EXPECT_EQ(DetectLanguage(L"golang"), SyntaxLanguage::Go);
}

TEST(Syntax, GoKeywords) {
    std::wstring code = L"if else for return func defer go";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, GoTypes) {
    std::wstring code = L"int float64 string bool error";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, GoLineComment) {
    std::wstring code = L"x := 1 // comment\ny := 2";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"// comment");
}

TEST(Syntax, GoBlockComment) {
    std::wstring code = L"/* multi\nline */";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, GoBacktickRawString) {
    std::wstring code = L"`raw\\nstring`";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"`raw\\nstring`");
}

TEST(Syntax, GoNilTrueFalse) {
    std::wstring code = L"nil true false iota";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, GoComplexCode) {
    std::wstring code = L"package main\n\nimport \"fmt\"\n\nfunc main() {\n    // Hello\n    fmt.Println(\"Hello\")\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // package, import, func
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // Hello
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 2);     // "fmt", "Hello"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 1);   // main
}

TEST(Syntax, TokensCoverEntireTextGo) {
    std::wstring code = L"func hello(name string) error {\n    return nil\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Rust tokenization
// ============================================================

TEST(Syntax, DetectLanguageRust) {
    EXPECT_EQ(DetectLanguage(L"rust"), SyntaxLanguage::Rust);
    EXPECT_EQ(DetectLanguage(L"rs"), SyntaxLanguage::Rust);
}

TEST(Syntax, RustKeywords) {
    std::wstring code = L"fn let mut if else match return";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, RustTypes) {
    std::wstring code = L"i32 u64 f64 bool String Vec Option Result";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 8);
}

TEST(Syntax, RustLineComment) {
    std::wstring code = L"let x = 1; // comment";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, RustBlockComment) {
    std::wstring code = L"/* block\ncomment */";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, RustStringDouble) {
    std::wstring code = L"let s = \"hello\";";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"\"hello\"");
}

TEST(Syntax, RustSingleQuoteNotString) {
    // In Rust, single quotes are used for lifetimes ('a) and char literals ('x').
    // We skip single-quote strings to avoid lifetime issues.
    std::wstring code = L"fn foo<'a>(x: &'a str) {}";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    // 'a should NOT create a string token that swallows the rest of the line
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 1); // fn
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 1);    // str
}

TEST(Syntax, RustSomeNoneOkErr) {
    std::wstring code = L"Some None Ok Err";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, RustAsyncAwait) {
    std::wstring code = L"async await";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 2);
}

TEST(Syntax, RustComplexCode) {
    std::wstring code = L"use std::io;\n\nfn main() -> Result<(), Box<dyn std::error::Error>> {\n    let x: i32 = 42;\n    // comment\n    println!(\"Hello {}\", x);\n    Ok(())\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // use, fn, let
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);       // Result, i32
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);     // "Hello {}"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 1);     // 42
}

TEST(Syntax, TokensCoverEntireTextRust) {
    std::wstring code = L"struct Point { x: f64, y: f64 }";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// TypeScript tokenization
// ============================================================

TEST(Syntax, DetectLanguageTypeScript) {
    EXPECT_EQ(DetectLanguage(L"typescript"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(L"ts"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(L"tsx"), SyntaxLanguage::TypeScript);
}

TEST(Syntax, TsKeywordsInclJsKeywords) {
    std::wstring code = L"if else while for return const let var function";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, TsSpecificKeywords) {
    std::wstring code = L"interface type enum namespace declare abstract readonly";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, TsSpecificTypes) {
    // void is a keyword (inherited from JS), so it won't be in types
    std::wstring code = L"any unknown never number string boolean";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, TsUtilityTypes) {
    std::wstring code = L"Record Partial Required Readonly Pick Omit";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, TsTemplateLiteral) {
    std::wstring code = L"`hello ${name}`";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, TsComplexCode) {
    std::wstring code = L"interface User {\n  name: string;\n  age: number;\n}\n\nconst greet = (user: User): string => {\n  return `Hello, ${user.name}`;\n};";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // interface, const, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 3);       // string, number, string
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);     // template literal
}

TEST(Syntax, TokensCoverEntireTextTs) {
    std::wstring code = L"type Props = { value: number; onChange: (v: number) => void; };";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Bash tokenization
// ============================================================

TEST(Syntax, DetectLanguageBash) {
    EXPECT_EQ(DetectLanguage(L"bash"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(L"sh"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(L"zsh"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(L"shell"), SyntaxLanguage::Bash);
}

TEST(Syntax, BashKeywords) {
    std::wstring code = L"if then else elif fi for while do done";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, BashBuiltins) {
    std::wstring code = L"echo printf read cd pwd";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, BashHashComment) {
    std::wstring code = L"x=1  # comment\ny=2";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"# comment");
}

TEST(Syntax, BashString) {
    std::wstring code = L"echo \"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), L"\"hello world\"");
}

TEST(Syntax, BashBacktick) {
    std::wstring code = L"result=`ls -la`";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, BashComplexCode) {
    std::wstring code = L"#!/bin/bash\n# Script\nfor f in *.txt; do\n    echo \"$f\"\ndone";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // for, in, do, done
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, TokensCoverEntireTextBash) {
    std::wstring code = L"if [ -f \"$1\" ]; then\n    echo \"exists\"\nfi";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// PowerShell tokenization
// ============================================================

TEST(Syntax, DetectLanguagePowerShell) {
    EXPECT_EQ(DetectLanguage(L"powershell"), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage(L"pwsh"), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage(L"ps1"), SyntaxLanguage::PowerShell);
}

TEST(Syntax, PwshKeywords) {
    std::wstring code = L"if else foreach while function return";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 6);
}

TEST(Syntax, PwshKeywordsCaseInsensitive) {
    std::wstring code = L"If Else ForEach WHILE Function RETURN";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 6);
}

TEST(Syntax, PwshTypes) {
    std::wstring code = L"int string bool array hashtable";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, PwshHashComment) {
    std::wstring code = L"$x = 1  # comment\n$y = 2";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, PwshAngleBlockComment) {
    std::wstring code = L"<# block\ncomment #>";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, PwshAngleBlockCommentUnterminated) {
    std::wstring code = L"<# never closed";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, PwshString) {
    std::wstring code = L"\"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PwshComplexCode) {
    std::wstring code = L"<# Script #>\nfunction Get-Item {\n    param([string]$Path)\n    # Do work\n    return $Path\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 2);   // <# #> and # comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);   // function, param, return
}

TEST(Syntax, TokensCoverEntireTextPwsh) {
    std::wstring code = L"if ($x -eq 1) { Write-Host \"hello\" }";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Cmd tokenization
// ============================================================

TEST(Syntax, DetectLanguageCmd) {
    EXPECT_EQ(DetectLanguage(L"cmd"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(L"bat"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(L"batch"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(L"dosbatch"), SyntaxLanguage::Cmd);
}

TEST(Syntax, CmdKeywords) {
    std::wstring code = L"if else for do goto call set echo";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CmdKeywordsCaseInsensitive) {
    std::wstring code = L"IF ELSE FOR DO GOTO CALL SET ECHO";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CmdRemComment) {
    std::wstring code = L"REM this is a comment\nset x=1";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L"REM this is a comment");
}

TEST(Syntax, CmdRemCommentCaseInsensitive) {
    std::wstring code = L"rem comment here";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, CmdRemNotAtLineStart) {
    // REM in the middle of a line should be a keyword, not a comment
    std::wstring code = L"echo REM";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    // "echo" is keyword, " " is plain, "REM" should not be a comment
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 0);
}

TEST(Syntax, CmdDoubleColonComment) {
    std::wstring code = L":: this is a comment\nset x=1";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), L":: this is a comment");
}

TEST(Syntax, CmdDoubleColonNotAtLineStart) {
    // :: not at line start should not be treated as comment
    std::wstring code = L"x::y";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 0);
}

TEST(Syntax, CmdString) {
    std::wstring code = L"echo \"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, CmdTypes) {
    std::wstring code = L"dir copy move del mkdir";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, CmdComplexCode) {
    std::wstring code = L"@echo off\nREM Build script\nfor %%f in (*.cpp) do (\n    echo Building %%f\n)\npause";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);   // REM
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // echo, for, do, echo, pause
}

TEST(Syntax, TokensCoverEntireTextCmd) {
    std::wstring code = L"if exist \"file.txt\" (\n    del \"file.txt\"\n)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Parser integration: new languages
// ============================================================

TEST(Syntax, ParserExtractsLanguageGo) {
    auto nodes = ParseMarkdown("```go\nfunc main() {}\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Go);
}

TEST(Syntax, ParserExtractsLanguageTs) {
    auto nodes = ParseMarkdown("```typescript\nconst x: number = 1;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::TypeScript);
}

TEST(Syntax, ParserExtractsLanguageBash) {
    auto nodes = ParseMarkdown("```bash\necho hello\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Bash);
}

TEST(Syntax, ParserExtractsLanguagePwsh) {
    auto nodes = ParseMarkdown("```powershell\nWrite-Host hello\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::PowerShell);
}

TEST(Syntax, ParserExtractsLanguageCmd) {
    auto nodes = ParseMarkdown("```cmd\necho hello\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cmd);
}
