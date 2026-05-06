#include <gtest/gtest.h>
#include <memory_resource>
#include "syntax.h"
#include "parser.h"
#include <numeric>

// ============================================================
// DetectLanguage テスト
// ============================================================

TEST(Syntax, DetectLanguageCpp)
{
    EXPECT_EQ(DetectLanguage("cpp"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageC)
{
    EXPECT_EQ(DetectLanguage("c"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageCPlusPlus)
{
    EXPECT_EQ(DetectLanguage("c++"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageCxx)
{
    EXPECT_EQ(DetectLanguage("cxx"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageH)
{
    EXPECT_EQ(DetectLanguage("h"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageHpp)
{
    EXPECT_EQ(DetectLanguage("hpp"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguagePython)
{
    EXPECT_EQ(DetectLanguage("python"), SyntaxLanguage::Python);
}

TEST(Syntax, DetectLanguagePy)
{
    EXPECT_EQ(DetectLanguage("py"), SyntaxLanguage::Python);
}

TEST(Syntax, DetectLanguageJavaScript)
{
    EXPECT_EQ(DetectLanguage("javascript"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageJs)
{
    EXPECT_EQ(DetectLanguage("js"), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageTs)
{
    EXPECT_EQ(DetectLanguage("typescript"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage("ts"), SyntaxLanguage::TypeScript);
}

TEST(Syntax, DetectLanguageJsx)
{
    EXPECT_EQ(DetectLanguage("jsx"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage("tsx"), SyntaxLanguage::TypeScript);
}

TEST(Syntax, DetectLanguageUnknown)
{
    EXPECT_EQ(DetectLanguage("java"), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage("ruby"), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage("swift"), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageEmpty)
{
    EXPECT_EQ(DetectLanguage(""), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageCaseInsensitive)
{
    EXPECT_EQ(DetectLanguage("CPP"), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage("Python"), SyntaxLanguage::Python);
    EXPECT_EQ(DetectLanguage("JavaScript"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage("JS"), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage("TypeScript"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage("GO"), SyntaxLanguage::Go);
    EXPECT_EQ(DetectLanguage("RUST"), SyntaxLanguage::Rust);
    EXPECT_EQ(DetectLanguage("BASH"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage("PowerShell"), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage("CMD"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage("JSON"), SyntaxLanguage::Json);
    EXPECT_EQ(DetectLanguage("JsonC"), SyntaxLanguage::Json);
}

TEST(Syntax, DetectLanguageWithExtraInfo)
{
    // md4cは言語の後に追加テキストを含むinfo文字列を提供する場合がある
    EXPECT_EQ(DetectLanguage("cpp some-extra"), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage("python\ttab-separated"), SyntaxLanguage::Python);
}

// ============================================================
// Tokenize - 基本テスト
// ============================================================

TEST(Syntax, EmptyTextReturnsEmpty)
{
    auto tokens = Tokenize("", SyntaxLanguage::Cpp);
    EXPECT_TRUE(tokens.empty());
}

TEST(Syntax, NoneLanguageReturnsEmpty)
{
    auto tokens = Tokenize("int main() {}", SyntaxLanguage::None);
    EXPECT_TRUE(tokens.empty());
}

// ヘルパー: トークンがテキスト全体を連続的にカバーしていることを確認
void AssertTokensCoverText(const std::pmr::vector<SyntaxToken>& tokens, size_t text_length)
{
    if (text_length == 0) {
        EXPECT_TRUE(tokens.empty());
        return;
    }
    ASSERT_FALSE(tokens.empty());

    // 最初のトークンは0から開始
    EXPECT_EQ(tokens[0].start, 0u);

    // トークンは連続している
    for (size_t i = 1; i < tokens.size(); i++) {
        EXPECT_EQ(tokens[i].start, tokens[i - 1].start + tokens[i - 1].length)
            << "トークン " << (i - 1) << " と " << i << " の間にギャップあり";
    }

    // 合計長が一致
    uint32_t total = 0;
    for (const auto& t : tokens)
        total += t.length;
    EXPECT_EQ(total, static_cast<uint32_t>(text_length));
}

// ヘルパー: 指定された種類の最初のトークンを検索
const SyntaxToken* FindToken(const std::pmr::vector<SyntaxToken>& tokens, SyntaxTokenType type)
{
    for (const auto& t : tokens) {
        if (t.type == type) {
            return &t;
        }
    }
    return nullptr;
}

// ヘルパー: 指定された種類のトークン数をカウント
int CountTokens(const std::pmr::vector<SyntaxToken>& tokens, SyntaxTokenType type)
{
    int count = 0;
    for (const auto& t : tokens) {
        if (t.type == type)
            count++;
    }
    return count;
}

// ヘルパー: トークンのテキストを取得
std::string GetTokenText(const std::string& text, const SyntaxToken& token)
{
    return text.substr(token.start, token.length);
}

TEST(Syntax, TokensCoverEntireTextCpp)
{
    std::string code = "int main() { return 0; }";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, TokensCoverEntireTextPython)
{
    std::string code = "def hello():\n    print('world')";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, TokensCoverEntireTextJs)
{
    std::string code = "const f = () => { return 42; };";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// C/C++ トークン化
// ============================================================

TEST(Syntax, CppKeywords)
{
    std::string code = "if else while for return";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    // すべての識別子はキーワードであるべき
    for (const auto& t : tokens) {
        if (t.type != SyntaxTokenType::Plain) {
            EXPECT_EQ(t.type, SyntaxTokenType::Keyword) << "offset=" << t.start;
        }
    }
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 5);
}

TEST(Syntax, CppTypes)
{
    std::string code = "int float double bool";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, CppSingleLineComment)
{
    std::string code = "x = 1; // comment\ny = 2;";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "// comment");
}

TEST(Syntax, CppMultiLineComment)
{
    std::string code = "/* multi\nline\ncomment */";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, CppStringDouble)
{
    std::string code = "x = \"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "\"hello world\"");
}

TEST(Syntax, CppStringSingle)
{
    std::string code = "c = 'x'";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "'x'");
}

TEST(Syntax, CppStringEscape)
{
    std::string code = "s = \"hello\\\"world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "\"hello\\\"world\"");
}

TEST(Syntax, CppNumberInteger)
{
    std::string code = "x = 42";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "42");
}

TEST(Syntax, CppNumberHex)
{
    std::string code = "x = 0xFF";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "0xFF");
}

TEST(Syntax, CppNumberFloat)
{
    std::string code = "x = 3.14f";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "3.14f");
}

TEST(Syntax, CppNumberBinary)
{
    std::string code = "x = 0b1010";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "0b1010");
}

TEST(Syntax, CppPreprocessorInclude)
{
    std::string code = "#include <stdio.h>";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Preprocessor);
}

TEST(Syntax, CppPreprocessorDefine)
{
    std::string code = "#define MAX 100";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Preprocessor);
}

TEST(Syntax, CppPreprocessorNotAtLineStart)
{
    // コードの後の#はプリプロセッサではないべき
    std::string code = "x = a #";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Preprocessor), 0);
}

TEST(Syntax, CppFunctionCall)
{
    std::string code = "foo(42)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), "foo");
}

TEST(Syntax, CppFunctionCallWithSpace)
{
    std::string code = "bar (x)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), "bar");
}

TEST(Syntax, CppKeywordNotFunction)
{
    // (の後に続くキーワードは関数ではなくキーワードのままであるべき
    std::string code = "if (x)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), "if");
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Function), 0);
}

TEST(Syntax, CppComplexCode)
{
    std::string code = "#include <iostream>\n\nint main() {\n    // Hello\n    std::cout << \"Hello\" << 42;\n    return 0;\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Preprocessor), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 1);     // int
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 1); // main
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // // Hello
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);   // "Hello"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 1);   // 42
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 1);  // return
}

// ============================================================
// Python トークン化
// ============================================================

TEST(Syntax, PythonKeywords)
{
    std::string code = "if else while for return def class";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, PythonTypes)
{
    std::string code = "int float str bool list dict";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, PythonComment)
{
    std::string code = "x = 1  # comment\ny = 2";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "# comment");
}

TEST(Syntax, PythonTripleQuoteDouble)
{
    std::string code = "s = \"\"\"hello\nworld\"\"\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "\"\"\"hello\nworld\"\"\"");
}

TEST(Syntax, PythonTripleQuoteSingle)
{
    std::string code = "s = '''docstring'''";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "'''docstring'''");
}

TEST(Syntax, PythonDefFunction)
{
    std::string code = "def foo():";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), "def");
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), "foo");
}

TEST(Syntax, PythonTrueFalseNone)
{
    std::string code = "x = True\ny = False\nz = None";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 3);
}

TEST(Syntax, PythonFString)
{
    std::string code = "f\"hello {name}\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    // 'f'はプレーン、その後が文字列
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PythonComplexCode)
{
    std::string code = "def greet(name: str) -> str:\n    # Greeting\n    return f\"Hello, {name}!\"\n\nprint(greet(\"World\"))";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);  // def, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);     // str, str
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // # Greeting
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2); // greet, print
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

// ============================================================
// JavaScript トークン化
// ============================================================

TEST(Syntax, JsKeywords)
{
    std::string code = "if else while for return const let var function";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, JsTypes)
{
    std::string code = "Array Map Set Promise";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, JsSingleLineComment)
{
    std::string code = "// comment\nx = 1";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "// comment");
}

TEST(Syntax, JsMultiLineComment)
{
    std::string code = "/* block\ncomment */";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, JsTemplateLiteral)
{
    std::string code = "`hello ${name}`";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "`hello ${name}`");
}

TEST(Syntax, JsTemplateLiteralMultiLine)
{
    std::string code = "`line1\nline2\nline3`";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::String);
}

TEST(Syntax, JsArrowFunction)
{
    std::string code = "const f = () => 42";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), "const");
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "42");
}

TEST(Syntax, JsTrueFalseNull)
{
    std::string code = "true false null undefined";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, JsComplexCode)
{
    std::string code = "async function fetchData(url) {\n  // Fetch data\n  const resp = await fetch(url);\n  return resp.json();\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 4);  // async, function, const, await, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // // Fetch data
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2); // fetchData, fetch
}

// ============================================================
// エッジケース
// ============================================================

TEST(Syntax, OnlyWhitespace)
{
    std::string code = "   \n\t  \n  ";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    for (const auto& t : tokens) {
        EXPECT_EQ(t.type, SyntaxTokenType::Plain);
    }
}

TEST(Syntax, OnlyOperators)
{
    std::string code = "+ - * / = == != < > <= >=";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, UnterminatedString)
{
    // 閉じられていない文字列が無限ループを引き起こさないべき
    std::string code = "x = \"unterminated\ny = 1";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, UnterminatedBlockComment)
{
    std::string code = "/* never closed";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, NumberAtEndOfText)
{
    std::string code = "x = 123";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "123");
}

TEST(Syntax, DotNotANumber)
{
    // 単独のドットは数値として扱われないべき
    std::string code = "a.b";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 0);
}

TEST(Syntax, MultipleLinesOfCode)
{
    std::string code =
        "int x = 10;\n"
        "float y = 3.14f;\n"
        "// comment\n"
        "if (x > 0) {\n"
        "    return y;\n"
        "}";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);   // int, float
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 2); // 10, 3.14f
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2); // if, return
}

// ============================================================
// パーサー統合: 言語抽出
// ============================================================

TEST(Syntax, ParserExtractsLanguageCpp)
{
    auto nodes = ParseMarkdown("```cpp\nint x = 1;\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

TEST(Syntax, ParserExtractsLanguagePython)
{
    auto nodes = ParseMarkdown("```python\ndef foo(): pass\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Python);
}

TEST(Syntax, ParserExtractsLanguageJs)
{
    auto nodes = ParseMarkdown("```js\nconst x = 1;\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::JavaScript);
}

TEST(Syntax, ParserNoLanguage)
{
    auto nodes = ParseMarkdown("```\nplain code\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::None);
}

TEST(Syntax, ParserExtractsLanguageRust)
{
    auto nodes = ParseMarkdown("```rust\nfn main() {}\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Rust);
}

TEST(Syntax, ParserUnknownLanguage)
{
    auto nodes = ParseMarkdown("```java\nclass Main {}\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::None);
}

TEST(Syntax, ParserCaseInsensitiveLanguage)
{
    auto nodes = ParseMarkdown("```CPP\nint x;\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

// ---- 追加エッジケース ----

// Mermaid検出
TEST(Syntax, DetectLanguageMermaid)
{
    EXPECT_EQ(DetectLanguage("mermaid"), SyntaxLanguage::Mermaid);
}

// C++ 生文字列
TEST(Syntax, CppRawString)
{
    auto tokens = Tokenize("R\"(hello)\"", SyntaxLanguage::Cpp);
    // 生文字列を単一の文字列トークンとして検出すべき
    bool has_string = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::String) {
            has_string = true;
        }
    }
    EXPECT_TRUE(has_string);
}

// C++ 8進数
TEST(Syntax, CppNumberOctal)
{
    auto tokens = Tokenize("0o77", SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number)
            has_number = true;
    }
    EXPECT_TRUE(has_number);
}

// C++ 数値サフィックス
TEST(Syntax, CppNumberWithSuffix)
{
    auto tokens = Tokenize("42ULL", SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) {
            has_number = true;
            // "42ULMENDO_LIT("全体が単一の数値トークンであるべき
            EXPECT_EQ(t.length, 5u);
        }
    }
    EXPECT_TRUE(has_number);
}

// Python デコレータ
TEST(Syntax, PythonDecorator)
{
    auto tokens = Tokenize("@staticmethod\ndef foo():\n    pass", SyntaxLanguage::Python);
    // "@"は特別に処理されないが、"def"と"pass"はキーワードであるべき
    bool has_def = false;
    bool has_pass = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Keyword) {
            std::string_view word("@staticmethod\ndef foo():\n    pass" + t.start, t.length);
            if (word == "def") {
                has_def = true;
            }
            if (word == "pass") {
                has_pass = true;
            }
        }
    }
    EXPECT_TRUE(has_def);
    EXPECT_TRUE(has_pass);
}

// JavaScript BigInt
TEST(Syntax, JsBigIntNumber)
{
    auto tokens = Tokenize("42n", SyntaxLanguage::JavaScript);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) {
            has_number = true;
            EXPECT_EQ(t.length, 3u); // "42n"
        }
    }
    EXPECT_TRUE(has_number);
}

// 空のコードブロック
TEST(Syntax, TokenizeEmptyCpp)
{
    auto tokens = Tokenize("", SyntaxLanguage::Cpp);
    EXPECT_TRUE(tokens.empty());
}

// 単一文字
TEST(Syntax, TokenizeSingleKeyword)
{
    auto tokens = Tokenize("if", SyntaxLanguage::Cpp);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Keyword);
}

// C++ テキスト末尾の行コメント（改行なし）
TEST(Syntax, CppCommentEol)
{
    auto tokens = Tokenize("int x; // comment", SyntaxLanguage::Cpp);
    bool has_comment = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Comment)
            has_comment = true;
    }
    EXPECT_TRUE(has_comment);
}

// ドットで始まる浮動小数点数
TEST(Syntax, NumberStartsWithDot)
{
    auto tokens = Tokenize(".5f", SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number)
            has_number = true;
    }
    EXPECT_TRUE(has_number);
}

// C++ 行継続付きプリプロセッサ
TEST(Syntax, CppPreprocessorContinuation)
{
    auto tokens = Tokenize("#define FOO \\\n    bar", SyntaxLanguage::Cpp);
    // 行継続をまたぐ単一のプリプロセッサトークンであるべき
    bool has_prep = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Preprocessor) {
            has_prep = true;
        }
    }
    EXPECT_TRUE(has_prep);
}

// Tsx拡張子の検出
TEST(Syntax, DetectLanguageTsx)
{
    EXPECT_EQ(DetectLanguage("tsx"), SyntaxLanguage::TypeScript);
}

// 不明な拡張子の検出
TEST(Syntax, DetectLanguageRuby)
{
    EXPECT_EQ(DetectLanguage("ruby"), SyntaxLanguage::None);
}

// ============================================================
// 追加の言語拡張子
// ============================================================

TEST(Syntax, DetectLanguageCc)
{
    EXPECT_EQ(DetectLanguage("cc"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageHxx)
{
    EXPECT_EQ(DetectLanguage("hxx"), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageMermaidCaseInsensitive)
{
    EXPECT_EQ(DetectLanguage("Mermaid"), SyntaxLanguage::Mermaid);
    EXPECT_EQ(DetectLanguage("MERMAID"), SyntaxLanguage::Mermaid);
}

// ============================================================
// Mermaidトークン化は空を返す（キーワードテーブルなし）
// ============================================================

TEST(Syntax, MermaidLanguageReturnsEmpty)
{
    // Mermaidは現在の実装ではトークナイザーを持たない
    auto tokens = Tokenize("graph TD; A-->B;", SyntaxLanguage::Mermaid);
    EXPECT_TRUE(tokens.empty());
}

// ============================================================
// 数値のエッジケース
// ============================================================

TEST(Syntax, CppNumberExponent)
{
    std::string code = "1.5e10";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "1.5e10");
}

TEST(Syntax, CppNumberExponentNegative)
{
    std::string code = "2.0e-3";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "2.0e-3");
}

TEST(Syntax, CppNumberDigitSeparator)
{
    std::string code = "1'000'000";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "1'000'000");
}

TEST(Syntax, CppHexDigitSeparator)
{
    std::string code = "0xFF'FF";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), "0xFF'FF");
}

// ============================================================
// C++ デリミタ付き生文字列
// ============================================================

TEST(Syntax, CppRawStringWithDelimiter)
{
    std::string code = "R\"delim(hello \"world\")delim\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    // Rは別の識別子として出力され、その後に生文字列が続く
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    // R"delim(...)delim"全体がキャプチャされる（Rはプレーン + 文字列本体）
    EXPECT_GT(str->length, 10u);
}

// ============================================================
// Python 閉じられていないトリプルクォート
// ============================================================

TEST(Syntax, PythonUnterminatedTripleQuote)
{
    std::string code = "s = \"\"\"never closed";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PythonTripleQuoteWithBackslashEscape)
{
    // バックスラッシュエスケープ経路: `\"` をスキップしてから本物の `"""` で終端する。
    std::string code = "\"\"\"abc\\\"\"\"def\"\"\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->length, code.size());
}

// ============================================================
// C++ モダンキーワード
// ============================================================

TEST(Syntax, CppModernKeywords)
{
    std::string code = "constexpr consteval constinit concept requires co_await co_return co_yield";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CppCastKeywords)
{
    std::string code = "static_cast dynamic_cast reinterpret_cast const_cast";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 4);
}

// ============================================================
// C++ STL型
// ============================================================

TEST(Syntax, CppStlTypes)
{
    std::string code = "vector map optional variant span unique_ptr shared_ptr";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 7);
}

TEST(Syntax, CppWin32Types)
{
    std::string code = "HRESULT BOOL DWORD HWND LRESULT";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

// ============================================================
// Python 例外型
// ============================================================

TEST(Syntax, PythonExceptionTypes)
{
    std::string code = "ValueError TypeError KeyError IndexError RuntimeError";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

// ============================================================
// JavaScript グローバル
// ============================================================

TEST(Syntax, JsGlobalTypes)
{
    std::string code = "console document window JSON Math";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, JsAsyncAwait)
{
    std::string code = "async await";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 2);
}

// ============================================================
// バグ #16: 生文字列R"の検出がRで終わる識別子で
// トリガーされないべき（例: RENDER"hello"）
// ============================================================

TEST(Syntax, CppRawStringNotTriggeredByIdentifierEndingR)
{
    std::string code = "RENDER\"hello\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    // RENDERは単一の識別子トークン（PlainまたはFunction）であるべき
    // "hello"は文字列トークンであるべき
    // 両者は重複してはならない
    bool found_render = false;
    bool found_string = false;
    for (const auto& t : tokens) {
        std::string text = GetTokenText(code, t);
        if (text == "RENDER") {
            found_render = true;
            // 文字列であってはならない
            EXPECT_NE(t.type, SyntaxTokenType::String);
        }
        if (text == "\"hello\"") {
            found_string = true;
            EXPECT_EQ(t.type, SyntaxTokenType::String);
        }
    }
    EXPECT_TRUE(found_render) << "RENDERが独立したトークンとして見つかるべき";
    EXPECT_TRUE(found_string) << "\"hello\"が文字列トークンとして見つかるべき";
}

TEST(Syntax, CppRawStringStandaloneRStillWorks)
{
    // 単独のR"(...)"は依然として生文字列として認識されるべき
    std::string code = "R\"(hello)\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_GT(str->length, 5u);
}

TEST(Syntax, CppRawStringAfterSpaceR)
{
    // "x R\"(test)\"" — スペースの後のRは動作すべき
    std::string code = "x R\"(test)\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

// ============================================================
// バグ #22: 閉じられていないブロックコメントは最後の文字を含むべき
// ============================================================

TEST(Syntax, UnterminatedBlockCommentCoversAllText)
{
    std::string code = "/* unterminated comment";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, UnterminatedBlockCommentEndsWithStar)
{
    // エッジケース: コメントが*で終わるが/がない
    std::string code = "/* test *";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, TerminatedBlockCommentStillWorks)
{
    std::string code = "/* ok */ x";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "/* ok */");
}

// ============================================================
// Go トークン化
// ============================================================

TEST(Syntax, DetectLanguageGo)
{
    EXPECT_EQ(DetectLanguage("go"), SyntaxLanguage::Go);
    EXPECT_EQ(DetectLanguage("golang"), SyntaxLanguage::Go);
}

TEST(Syntax, GoKeywords)
{
    std::string code = "if else for return func defer go";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, GoTypes)
{
    std::string code = "int float64 string bool error";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, GoLineComment)
{
    std::string code = "x := 1 // comment\ny := 2";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "// comment");
}

TEST(Syntax, GoBlockComment)
{
    std::string code = "/* multi\nline */";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, GoBacktickRawString)
{
    std::string code = "`raw\\nstring`";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "`raw\\nstring`");
}

TEST(Syntax, GoBacktickRawStringWithBackslash)
{
    // Goの生文字列はバックスラッシュをエスケープとして扱わないので、`c:\`は有効
    std::string code = "`c:\\`";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "`c:\\`");
}

TEST(Syntax, GoBacktickRawStringTrailingBackslash)
{
    // 生文字列末尾のバックスラッシュが閉じバッククォートをスキップしないことを確認
    std::string code = "s := `path\\` + x";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "`path\\`");
}

TEST(Syntax, GoNilTrueFalse)
{
    std::string code = "nil true false iota";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, GoComplexCode)
{
    std::string code = "package main\n\nimport \"fmt\"\n\nfunc main() {\n    // Hello\n    fmt.Println(\"Hello\")\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);  // package, import, func
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // // Hello
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 2);   // "fmt", "Hello"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 1); // main
}

TEST(Syntax, TokensCoverEntireTextGo)
{
    std::string code = "func hello(name string) error {\n    return nil\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Rust トークン化
// ============================================================

TEST(Syntax, DetectLanguageRust)
{
    EXPECT_EQ(DetectLanguage("rust"), SyntaxLanguage::Rust);
    EXPECT_EQ(DetectLanguage("rs"), SyntaxLanguage::Rust);
}

TEST(Syntax, RustKeywords)
{
    std::string code = "fn let mut if else match return";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, RustTypes)
{
    std::string code = "i32 u64 f64 bool String Vec Option Result";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 8);
}

TEST(Syntax, RustLineComment)
{
    std::string code = "let x = 1; // comment";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, RustBlockComment)
{
    std::string code = "/* block\ncomment */";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, RustStringDouble)
{
    std::string code = "let s = \"hello\";";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "\"hello\"");
}

TEST(Syntax, RustSingleQuoteNotString)
{
    // Rustではシングルクォートはライフタイム('a)と文字リテラル('x')に使用される。
    // ライフタイムの問題を避けるためシングルクォート文字列はスキップする。
    std::string code = "fn foo<'a>(x: &'a str) {}";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    // 'aは行の残りを飲み込む文字列トークンを生成してはならない
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 1); // fn
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 1);    // str
}

TEST(Syntax, RustSomeNoneOkErr)
{
    std::string code = "Some None Ok Err";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, RustAsyncAwait)
{
    std::string code = "async await";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 2);
}

TEST(Syntax, RustComplexCode)
{
    std::string code = "use std::io;\n\nfn main() -> Result<(), Box<dyn std::error::Error>> {\n    let x: i32 = 42;\n    // comment\n    println!(\"Hello {}\", x);\n    Ok(())\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // use, fn, let
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);    // Result, i32
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1); // // comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);  // "Hello {}"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 1);  // 42
}

TEST(Syntax, TokensCoverEntireTextRust)
{
    std::string code = "struct Point { x: f64, y: f64 }";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// TypeScript トークン化
// ============================================================

TEST(Syntax, DetectLanguageTypeScript)
{
    EXPECT_EQ(DetectLanguage("typescript"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage("ts"), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage("tsx"), SyntaxLanguage::TypeScript);
}

TEST(Syntax, TsKeywordsInclJsKeywords)
{
    std::string code = "if else while for return const let var function";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, TsSpecificKeywords)
{
    std::string code = "interface type enum namespace declare abstract readonly";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, TsSpecificTypes)
{
    // voidはキーワード（JSから継承）なので、型には含まれない
    std::string code = "any unknown never number string boolean";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, TsUtilityTypes)
{
    std::string code = "Record Partial Required Readonly Pick Omit";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, TsTemplateLiteral)
{
    std::string code = "`hello ${name}`";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, TsComplexCode)
{
    std::string code = "interface User {\n  name: string;\n  age: number;\n}\n\nconst greet = (user: User): string => {\n  return `Hello, ${user.name}`;\n};";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // interface, const, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 3);    // string, number, string
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);  // template literal
}

TEST(Syntax, TokensCoverEntireTextTs)
{
    std::string code = "type Props = { value: number; onChange: (v: number) => void; };";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Bash トークン化
// ============================================================

TEST(Syntax, DetectLanguageBash)
{
    EXPECT_EQ(DetectLanguage("bash"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage("sh"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage("zsh"), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage("shell"), SyntaxLanguage::Bash);
}

TEST(Syntax, BashKeywords)
{
    std::string code = "if then else elif fi for while do done";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, BashBuiltins)
{
    std::string code = "echo printf read cd pwd";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, BashHashComment)
{
    std::string code = "x=1  # comment\ny=2";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "# comment");
}

TEST(Syntax, BashString)
{
    std::string code = "echo \"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), "\"hello world\"");
}

TEST(Syntax, BashBacktick)
{
    std::string code = "result=`ls -la`";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, BashComplexCode)
{
    std::string code = "#!/bin/bash\n# Script\nfor f in *.txt; do\n    echo \"$f\"\ndone";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // for, in, do, done
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, TokensCoverEntireTextBash)
{
    std::string code = "if [ -f \"$1\" ]; then\n    echo \"exists\"\nfi";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// PowerShell トークン化
// ============================================================

TEST(Syntax, DetectLanguagePowerShell)
{
    EXPECT_EQ(DetectLanguage("powershell"), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage("pwsh"), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage("ps1"), SyntaxLanguage::PowerShell);
}

TEST(Syntax, PwshKeywords)
{
    std::string code = "if else foreach while function return";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 6);
}

TEST(Syntax, PwshKeywordsCaseInsensitive)
{
    std::string code = "If Else ForEach WHILE Function RETURN";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 6);
}

TEST(Syntax, PwshTypes)
{
    std::string code = "int string bool array hashtable";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, PwshHashComment)
{
    std::string code = "$x = 1  # comment\n$y = 2";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, PwshAngleBlockComment)
{
    std::string code = "<# block\ncomment #>";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, PwshAngleBlockCommentUnterminated)
{
    std::string code = "<# never closed";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, PwshString)
{
    std::string code = "\"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PwshComplexCode)
{
    std::string code = "<# Script #>\nfunction Get-Item {\n    param([string]$Path)\n    # Do work\n    return $Path\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 2); // <# #> and # comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2); // function, param, return
}

TEST(Syntax, TokensCoverEntireTextPwsh)
{
    std::string code = "if ($x -eq 1) { Write-Host \"hello\" }";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Cmd トークン化
// ============================================================

TEST(Syntax, DetectLanguageCmd)
{
    EXPECT_EQ(DetectLanguage("cmd"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage("bat"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage("batch"), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage("dosbatch"), SyntaxLanguage::Cmd);
}

TEST(Syntax, CmdKeywords)
{
    std::string code = "if else for do goto call set echo";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CmdKeywordsCaseInsensitive)
{
    std::string code = "IF ELSE FOR DO GOTO CALL SET ECHO";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CmdRemComment)
{
    std::string code = "REM this is a comment\nset x=1";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), "REM this is a comment");
}

TEST(Syntax, CmdRemCommentCaseInsensitive)
{
    std::string code = "rem comment here";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, CmdRemNotAtLineStart)
{
    // 行の途中のREMはコメントではなくキーワードであるべき
    std::string code = "echo REM";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    // "echo"はキーワード、" "はプレーン、"REM"はコメントであってはならない
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 0);
}

TEST(Syntax, CmdDoubleColonComment)
{
    std::string code = ":: this is a comment\nset x=1";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), ":: this is a comment");
}

TEST(Syntax, CmdDoubleColonNotAtLineStart)
{
    // 行頭でない::はコメントとして扱われないべき
    std::string code = "x::y";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 0);
}

TEST(Syntax, CmdString)
{
    std::string code = "echo \"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, CmdTypes)
{
    std::string code = "dir copy move del mkdir";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, CmdComplexCode)
{
    std::string code = "@echo off\nREM Build script\nfor %%f in (*.cpp) do (\n    echo Building %%f\n)\npause";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1); // REM
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // echo, for, do, echo, pause
}

TEST(Syntax, TokensCoverEntireTextCmd)
{
    std::string code = "if exist \"file.txt\" (\n    del \"file.txt\"\n)";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// JSON / JSONC
// ============================================================

TEST(Syntax, DetectLanguageJson)
{
    EXPECT_EQ(DetectLanguage("json"), SyntaxLanguage::Json);
    EXPECT_EQ(DetectLanguage("jsonc"), SyntaxLanguage::Json);
    EXPECT_EQ(DetectLanguage("json5"), SyntaxLanguage::Json);
}

TEST(Syntax, JsonLiteralsAsKeywords)
{
    std::string code = "[true, false, null]";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 3);
}

TEST(Syntax, JsonString)
{
    std::string code = "\"hello world\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, JsonStringWithEscapes)
{
    std::string code = "\"line1\\nline2\\t\\\"quoted\\\"\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, JsonSingleQuoteIsNotString)
{
    // JSON は二重引用符のみ。シングルクォートは文字列として扱わない（プレーン）。
    std::string code = "'not a string'";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 0);
}

TEST(Syntax, JsonNumbers)
{
    // 負号は分離されるが、数値部はトークン化される。
    std::string code = "[0, 1, -2, 3.14, 1e10, 1.5e-3]";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 6);
}

TEST(Syntax, JsonObject)
{
    std::string code = "{\"name\": \"alice\", \"age\": 30, \"active\": true}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 4);
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 1);
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 1);
}

TEST(Syntax, JsonNestedStructure)
{
    std::string code = "{\"items\": [{\"id\": 1}, {\"id\": 2}], \"count\": 2}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 3);
}

TEST(Syntax, JsoncLineComment)
{
    // JSONC: // 形式のコメントを許容。
    std::string code = "{\n  // comment\n  \"key\": 1\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 1);
}

TEST(Syntax, JsoncBlockComment)
{
    // JSONC: /* */ 形式のコメントを許容。
    std::string code = "{\n  /* block\n     comment */\n  \"key\": 1\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 1);
}

TEST(Syntax, TokensCoverEntireTextJson)
{
    std::string code = "{\"a\": [1, 2, null], \"b\": {\"c\": false}}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// パーサー統合: 新言語
// ============================================================

TEST(Syntax, ParserExtractsLanguageGo)
{
    auto nodes = ParseMarkdown("```go\nfunc main() {}\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Go);
}

TEST(Syntax, ParserExtractsLanguageTs)
{
    auto nodes = ParseMarkdown("```typescript\nconst x: number = 1;\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::TypeScript);
}

TEST(Syntax, ParserExtractsLanguageBash)
{
    auto nodes = ParseMarkdown("```bash\necho hello\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Bash);
}

TEST(Syntax, ParserExtractsLanguagePwsh)
{
    auto nodes = ParseMarkdown("```powershell\nWrite-Host hello\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::PowerShell);
}

TEST(Syntax, ParserExtractsLanguageCmd)
{
    auto nodes = ParseMarkdown("```cmd\necho hello\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cmd);
}

TEST(Syntax, ParserExtractsLanguageJson)
{
    auto nodes = ParseMarkdown("```json\n{\"k\": 1}\n```").nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Json);
}
