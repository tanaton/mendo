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
    EXPECT_EQ(DetectLanguage(MENDO_LIT("cpp")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageC)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("c")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageCPlusPlus)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("c++")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageCxx)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("cxx")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageH)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("h")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageHpp)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("hpp")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguagePython)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("python")), SyntaxLanguage::Python);
}

TEST(Syntax, DetectLanguagePy)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("py")), SyntaxLanguage::Python);
}

TEST(Syntax, DetectLanguageJavaScript)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("javascript")), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageJs)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("js")), SyntaxLanguage::JavaScript);
}

TEST(Syntax, DetectLanguageTs)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("typescript")), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("ts")), SyntaxLanguage::TypeScript);
}

TEST(Syntax, DetectLanguageJsx)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("jsx")), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("tsx")), SyntaxLanguage::TypeScript);
}

TEST(Syntax, DetectLanguageUnknown)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("java")), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("ruby")), SyntaxLanguage::None);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("swift")), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageEmpty)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("")), SyntaxLanguage::None);
}

TEST(Syntax, DetectLanguageCaseInsensitive)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("CPP")), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("Python")), SyntaxLanguage::Python);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("JavaScript")), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("JS")), SyntaxLanguage::JavaScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("TypeScript")), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("GO")), SyntaxLanguage::Go);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("RUST")), SyntaxLanguage::Rust);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("BASH")), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("PowerShell")), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("CMD")), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("JSON")), SyntaxLanguage::Json);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("JsonC")), SyntaxLanguage::Json);
}

TEST(Syntax, DetectLanguageWithExtraInfo)
{
    // md4cは言語の後に追加テキストを含むinfo文字列を提供する場合がある
    EXPECT_EQ(DetectLanguage(MENDO_LIT("cpp some-extra")), SyntaxLanguage::Cpp);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("python\ttab-separated")), SyntaxLanguage::Python);
}

// ============================================================
// Tokenize - 基本テスト
// ============================================================

TEST(Syntax, EmptyTextReturnsEmpty)
{
    auto tokens = Tokenize(MENDO_LIT(""), SyntaxLanguage::Cpp);
    EXPECT_TRUE(tokens.empty());
}

TEST(Syntax, NoneLanguageReturnsEmpty)
{
    auto tokens = Tokenize(MENDO_LIT("int main() {}"), SyntaxLanguage::None);
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
    for (const auto& t : tokens) total += t.length;
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
        if (t.type == type) count++;
    }
    return count;
}

// ヘルパー: トークンのテキストを取得
mendo::doc_string_std GetTokenText(const mendo::doc_string_std& text, const SyntaxToken& token)
{
    return text.substr(token.start, token.length);
}

TEST(Syntax, TokensCoverEntireTextCpp)
{
    mendo::doc_string_std code = MENDO_LIT("int main() { return 0; }");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, TokensCoverEntireTextPython)
{
    mendo::doc_string_std code = MENDO_LIT("def hello():\n    print('world')");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, TokensCoverEntireTextJs)
{
    mendo::doc_string_std code = MENDO_LIT("const f = () => { return 42; };");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// C/C++ トークン化
// ============================================================

TEST(Syntax, CppKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else while for return");
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
    mendo::doc_string_std code = MENDO_LIT("int float double bool");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, CppSingleLineComment)
{
    mendo::doc_string_std code = MENDO_LIT("x = 1; // comment\ny = 2;");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("// comment"));
}

TEST(Syntax, CppMultiLineComment)
{
    mendo::doc_string_std code = MENDO_LIT("/* multi\nline\ncomment */");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, CppStringDouble)
{
    mendo::doc_string_std code = MENDO_LIT("x = \"hello world\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("\"hello world\""));
}

TEST(Syntax, CppStringSingle)
{
    mendo::doc_string_std code = MENDO_LIT("c = 'x'");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("'x'"));
}

TEST(Syntax, CppStringEscape)
{
    mendo::doc_string_std code = MENDO_LIT("s = \"hello\\\"world\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("\"hello\\\"world\""));
}

TEST(Syntax, CppNumberInteger)
{
    mendo::doc_string_std code = MENDO_LIT("x = 42");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("42"));
}

TEST(Syntax, CppNumberHex)
{
    mendo::doc_string_std code = MENDO_LIT("x = 0xFF");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("0xFF"));
}

TEST(Syntax, CppNumberFloat)
{
    mendo::doc_string_std code = MENDO_LIT("x = 3.14f");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("3.14f"));
}

TEST(Syntax, CppNumberBinary)
{
    mendo::doc_string_std code = MENDO_LIT("x = 0b1010");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("0b1010"));
}

TEST(Syntax, CppPreprocessorInclude)
{
    mendo::doc_string_std code = MENDO_LIT("#include <stdio.h>");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Preprocessor);
}

TEST(Syntax, CppPreprocessorDefine)
{
    mendo::doc_string_std code = MENDO_LIT("#define MAX 100");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Preprocessor);
}

TEST(Syntax, CppPreprocessorNotAtLineStart)
{
    // コードの後の#はプリプロセッサではないべき
    mendo::doc_string_std code = MENDO_LIT("x = a #");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Preprocessor), 0);
}

TEST(Syntax, CppFunctionCall)
{
    mendo::doc_string_std code = MENDO_LIT("foo(42)");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), MENDO_LIT("foo"));
}

TEST(Syntax, CppFunctionCallWithSpace)
{
    mendo::doc_string_std code = MENDO_LIT("bar (x)");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), MENDO_LIT("bar"));
}

TEST(Syntax, CppKeywordNotFunction)
{
    // (の後に続くキーワードは関数ではなくキーワードのままであるべき
    mendo::doc_string_std code = MENDO_LIT("if (x)");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), MENDO_LIT("if"));
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Function), 0);
}

TEST(Syntax, CppComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("#include <iostream>\n\nint main() {\n    // Hello\n    std::cout << \"Hello\" << 42;\n    return 0;\n}");
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
// Python トークン化
// ============================================================

TEST(Syntax, PythonKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else while for return def class");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, PythonTypes)
{
    mendo::doc_string_std code = MENDO_LIT("int float str bool list dict");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, PythonComment)
{
    mendo::doc_string_std code = MENDO_LIT("x = 1  # comment\ny = 2");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("# comment"));
}

TEST(Syntax, PythonTripleQuoteDouble)
{
    mendo::doc_string_std code = MENDO_LIT("s = \"\"\"hello\nworld\"\"\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("\"\"\"hello\nworld\"\"\""));
}

TEST(Syntax, PythonTripleQuoteSingle)
{
    mendo::doc_string_std code = MENDO_LIT("s = '''docstring'''");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("'''docstring'''"));
}

TEST(Syntax, PythonDefFunction)
{
    mendo::doc_string_std code = MENDO_LIT("def foo():");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), MENDO_LIT("def"));
    auto* func = FindToken(tokens, SyntaxTokenType::Function);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(GetTokenText(code, *func), MENDO_LIT("foo"));
}

TEST(Syntax, PythonTrueFalseNone)
{
    mendo::doc_string_std code = MENDO_LIT("x = True\ny = False\nz = None");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 3);
}

TEST(Syntax, PythonFString)
{
    mendo::doc_string_std code = MENDO_LIT("f\"hello {name}\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    // 'f'はプレーン、その後が文字列
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PythonComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("def greet(name: str) -> str:\n    # Greeting\n    return f\"Hello, {name}!\"\n\nprint(greet(\"World\"))");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);   // def, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);       // str, str
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // # Greeting
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2);   // greet, print
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

// ============================================================
// JavaScript トークン化
// ============================================================

TEST(Syntax, JsKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else while for return const let var function");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, JsTypes)
{
    mendo::doc_string_std code = MENDO_LIT("Array Map Set Promise");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, JsSingleLineComment)
{
    mendo::doc_string_std code = MENDO_LIT("// comment\nx = 1");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("// comment"));
}

TEST(Syntax, JsMultiLineComment)
{
    mendo::doc_string_std code = MENDO_LIT("/* block\ncomment */");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, JsTemplateLiteral)
{
    mendo::doc_string_std code = MENDO_LIT("`hello ${name}`");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("`hello ${name}`"));
}

TEST(Syntax, JsTemplateLiteralMultiLine)
{
    mendo::doc_string_std code = MENDO_LIT("`line1\nline2\nline3`");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::String);
}

TEST(Syntax, JsArrowFunction)
{
    mendo::doc_string_std code = MENDO_LIT("const f = () => 42");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    auto* kw = FindToken(tokens, SyntaxTokenType::Keyword);
    ASSERT_NE(kw, nullptr);
    EXPECT_EQ(GetTokenText(code, *kw), MENDO_LIT("const"));
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("42"));
}

TEST(Syntax, JsTrueFalseNull)
{
    mendo::doc_string_std code = MENDO_LIT("true false null undefined");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, JsComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("async function fetchData(url) {\n  // Fetch data\n  const resp = await fetch(url);\n  return resp.json();\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 4);   // async, function, const, await, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // Fetch data
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2);   // fetchData, fetch
}

// ============================================================
// エッジケース
// ============================================================

TEST(Syntax, OnlyWhitespace)
{
    mendo::doc_string_std code = MENDO_LIT("   \n\t  \n  ");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    for (const auto& t : tokens) {
        EXPECT_EQ(t.type, SyntaxTokenType::Plain);
    }
}

TEST(Syntax, OnlyOperators)
{
    mendo::doc_string_std code = MENDO_LIT("+ - * / = == != < > <= >=");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, UnterminatedString)
{
    // 閉じられていない文字列が無限ループを引き起こさないべき
    mendo::doc_string_std code = MENDO_LIT("x = \"unterminated\ny = 1");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
}

TEST(Syntax, UnterminatedBlockComment)
{
    mendo::doc_string_std code = MENDO_LIT("/* never closed");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, NumberAtEndOfText)
{
    mendo::doc_string_std code = MENDO_LIT("x = 123");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("123"));
}

TEST(Syntax, DotNotANumber)
{
    // 単独のドットは数値として扱われないべき
    mendo::doc_string_std code = MENDO_LIT("a.b");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 0);
}

TEST(Syntax, MultipleLinesOfCode)
{
    mendo::doc_string_std code =
        MENDO_LIT("int x = 10;\n")
        MENDO_LIT("float y = 3.14f;\n")
        MENDO_LIT("// comment\n")
        MENDO_LIT("if (x > 0) {\n")
        MENDO_LIT("    return y;\n")
        MENDO_LIT("}");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);      // int, float
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 2);     // 10, 3.14f
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);    // if, return
}

// ============================================================
// パーサー統合: 言語抽出
// ============================================================

TEST(Syntax, ParserExtractsLanguageCpp)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```cpp\nint x = 1;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

TEST(Syntax, ParserExtractsLanguagePython)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```python\ndef foo(): pass\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Python);
}

TEST(Syntax, ParserExtractsLanguageJs)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```js\nconst x = 1;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::JavaScript);
}

TEST(Syntax, ParserNoLanguage)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```\nplain code\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::None);
}

TEST(Syntax, ParserExtractsLanguageRust)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```rust\nfn main() {}\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Rust);
}

TEST(Syntax, ParserUnknownLanguage)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```java\nclass Main {}\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::None);
}

TEST(Syntax, ParserCaseInsensitiveLanguage)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```CPP\nint x;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

// ---- 追加エッジケース ----

// Mermaid検出
TEST(Syntax, DetectLanguageMermaid)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("mermaid")), SyntaxLanguage::Mermaid);
}

// C++ 生文字列
TEST(Syntax, CppRawString)
{
    auto tokens = Tokenize(MENDO_LIT("R\"(hello)\""), SyntaxLanguage::Cpp);
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
    auto tokens = Tokenize(MENDO_LIT("0o77"), SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) has_number = true;
    }
    EXPECT_TRUE(has_number);
}

// C++ 数値サフィックス
TEST(Syntax, CppNumberWithSuffix)
{
    auto tokens = Tokenize(MENDO_LIT("42ULL"), SyntaxLanguage::Cpp);
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
    auto tokens = Tokenize(MENDO_LIT("@staticmethod\ndef foo():\n    pass"), SyntaxLanguage::Python);
    // "@"は特別に処理されないが、"def"と"pass"はキーワードであるべき
    bool has_def = false;
    bool has_pass = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Keyword) {
            mendo::doc_string_view word(MENDO_LIT("@staticmethod\ndef foo():\n    pass") + t.start, t.length);
            if (word == MENDO_LIT("def")) has_def = true;
            if (word == MENDO_LIT("pass")) has_pass = true;
        }
    }
    EXPECT_TRUE(has_def);
    EXPECT_TRUE(has_pass);
}

// JavaScript BigInt
TEST(Syntax, JsBigIntNumber)
{
    auto tokens = Tokenize(MENDO_LIT("42n"), SyntaxLanguage::JavaScript);
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
    auto tokens = Tokenize(MENDO_LIT(""), SyntaxLanguage::Cpp);
    EXPECT_TRUE(tokens.empty());
}

// 単一文字
TEST(Syntax, TokenizeSingleKeyword)
{
    auto tokens = Tokenize(MENDO_LIT("if"), SyntaxLanguage::Cpp);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Keyword);
}

// C++ テキスト末尾の行コメント（改行なし）
TEST(Syntax, CppCommentEol)
{
    auto tokens = Tokenize(MENDO_LIT("int x; // comment"), SyntaxLanguage::Cpp);
    bool has_comment = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Comment) has_comment = true;
    }
    EXPECT_TRUE(has_comment);
}

// ドットで始まる浮動小数点数
TEST(Syntax, NumberStartsWithDot)
{
    auto tokens = Tokenize(MENDO_LIT(".5f"), SyntaxLanguage::Cpp);
    bool has_number = false;
    for (const auto& t : tokens) {
        if (t.type == SyntaxTokenType::Number) has_number = true;
    }
    EXPECT_TRUE(has_number);
}

// C++ 行継続付きプリプロセッサ
TEST(Syntax, CppPreprocessorContinuation)
{
    auto tokens = Tokenize(MENDO_LIT("#define FOO \\\n    bar"), SyntaxLanguage::Cpp);
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
    EXPECT_EQ(DetectLanguage(MENDO_LIT("tsx")), SyntaxLanguage::TypeScript);
}

// 不明な拡張子の検出
TEST(Syntax, DetectLanguageRuby)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("ruby")), SyntaxLanguage::None);
}

// ============================================================
// 追加の言語拡張子
// ============================================================

TEST(Syntax, DetectLanguageCc)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("cc")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageHxx)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("hxx")), SyntaxLanguage::Cpp);
}

TEST(Syntax, DetectLanguageMermaidCaseInsensitive)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("Mermaid")), SyntaxLanguage::Mermaid);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("MERMAID")), SyntaxLanguage::Mermaid);
}

// ============================================================
// Mermaidトークン化は空を返す（キーワードテーブルなし）
// ============================================================

TEST(Syntax, MermaidLanguageReturnsEmpty)
{
    // Mermaidは現在の実装ではトークナイザーを持たない
    auto tokens = Tokenize(MENDO_LIT("graph TD; A-->B;"), SyntaxLanguage::Mermaid);
    EXPECT_TRUE(tokens.empty());
}

// ============================================================
// 数値のエッジケース
// ============================================================

TEST(Syntax, CppNumberExponent)
{
    mendo::doc_string_std code = MENDO_LIT("1.5e10");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("1.5e10"));
}

TEST(Syntax, CppNumberExponentNegative)
{
    mendo::doc_string_std code = MENDO_LIT("2.0e-3");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("2.0e-3"));
}

TEST(Syntax, CppNumberDigitSeparator)
{
    mendo::doc_string_std code = MENDO_LIT("1'000'000");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("1'000'000"));
}

TEST(Syntax, CppHexDigitSeparator)
{
    mendo::doc_string_std code = MENDO_LIT("0xFF'FF");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* num = FindToken(tokens, SyntaxTokenType::Number);
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(GetTokenText(code, *num), MENDO_LIT("0xFF'FF"));
}

// ============================================================
// C++ デリミタ付き生文字列
// ============================================================

TEST(Syntax, CppRawStringWithDelimiter)
{
    mendo::doc_string_std code = MENDO_LIT("R\"delim(hello \"world\")delim\"");
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
    mendo::doc_string_std code = MENDO_LIT("s = \"\"\"never closed");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PythonTripleQuoteWithBackslashEscape)
{
    // バックスラッシュエスケープ経路: `\"` をスキップしてから本物の `"""` で終端する。
    mendo::doc_string_std code = MENDO_LIT("\"\"\"abc\\\"\"\"def\"\"\"");
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
    mendo::doc_string_std code = MENDO_LIT("constexpr consteval constinit concept requires co_await co_return co_yield");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CppCastKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("static_cast dynamic_cast reinterpret_cast const_cast");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 4);
}

// ============================================================
// C++ STL型
// ============================================================

TEST(Syntax, CppStlTypes)
{
    mendo::doc_string_std code = MENDO_LIT("vector map optional variant span unique_ptr shared_ptr");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 7);
}

TEST(Syntax, CppWin32Types)
{
    mendo::doc_string_std code = MENDO_LIT("HRESULT BOOL DWORD HWND LRESULT");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

// ============================================================
// Python 例外型
// ============================================================

TEST(Syntax, PythonExceptionTypes)
{
    mendo::doc_string_std code = MENDO_LIT("ValueError TypeError KeyError IndexError RuntimeError");
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

// ============================================================
// JavaScript グローバル
// ============================================================

TEST(Syntax, JsGlobalTypes)
{
    mendo::doc_string_std code = MENDO_LIT("console document window JSON Math");
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, JsAsyncAwait)
{
    mendo::doc_string_std code = MENDO_LIT("async await");
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
    mendo::doc_string_std code = MENDO_LIT("RENDER\"hello\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    // RENDERは単一の識別子トークン（PlainまたはFunction）であるべき
    // "hello"は文字列トークンであるべき
    // 両者は重複してはならない
    bool found_render = false;
    bool found_string = false;
    for (const auto& t : tokens) {
        mendo::doc_string_std text = GetTokenText(code, t);
        if (text == MENDO_LIT("RENDER")) {
            found_render = true;
            // 文字列であってはならない
            EXPECT_NE(t.type, SyntaxTokenType::String);
        }
        if (text == MENDO_LIT("\"hello\"")) {
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
    mendo::doc_string_std code = MENDO_LIT("R\"(hello)\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_GT(str->length, 5u);
}

TEST(Syntax, CppRawStringAfterSpaceR)
{
    // "x R\"(test)\"" — スペースの後のRは動作すべき
    mendo::doc_string_std code = MENDO_LIT("x R\"(test)\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

// ============================================================
// バグ #22: 閉じられていないブロックコメントは最後の文字を含むべき
// ============================================================

TEST(Syntax, UnterminatedBlockCommentCoversAllText)
{
    mendo::doc_string_std code = MENDO_LIT("/* unterminated comment");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, UnterminatedBlockCommentEndsWithStar)
{
    // エッジケース: コメントが*で終わるが/がない
    mendo::doc_string_std code = MENDO_LIT("/* test *");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, TerminatedBlockCommentStillWorks)
{
    mendo::doc_string_std code = MENDO_LIT("/* ok */ x");
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("/* ok */"));
}

// ============================================================
// Go トークン化
// ============================================================

TEST(Syntax, DetectLanguageGo)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("go")), SyntaxLanguage::Go);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("golang")), SyntaxLanguage::Go);
}

TEST(Syntax, GoKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else for return func defer go");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, GoTypes)
{
    mendo::doc_string_std code = MENDO_LIT("int float64 string bool error");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, GoLineComment)
{
    mendo::doc_string_std code = MENDO_LIT("x := 1 // comment\ny := 2");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("// comment"));
}

TEST(Syntax, GoBlockComment)
{
    mendo::doc_string_std code = MENDO_LIT("/* multi\nline */");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, GoBacktickRawString)
{
    mendo::doc_string_std code = MENDO_LIT("`raw\\nstring`");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("`raw\\nstring`"));
}

TEST(Syntax, GoBacktickRawStringWithBackslash)
{
    // Goの生文字列はバックスラッシュをエスケープとして扱わないので、`c:\`は有効
    mendo::doc_string_std code = MENDO_LIT("`c:\\`");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("`c:\\`"));
}

TEST(Syntax, GoBacktickRawStringTrailingBackslash)
{
    // 生文字列末尾のバックスラッシュが閉じバッククォートをスキップしないことを確認
    mendo::doc_string_std code = MENDO_LIT("s := `path\\` + x");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("`path\\`"));
}

TEST(Syntax, GoNilTrueFalse)
{
    mendo::doc_string_std code = MENDO_LIT("nil true false iota");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, GoComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("package main\n\nimport \"fmt\"\n\nfunc main() {\n    // Hello\n    fmt.Println(\"Hello\")\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // package, import, func
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // Hello
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 2);     // "fmt", "Hello"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 1);   // main
}

TEST(Syntax, TokensCoverEntireTextGo)
{
    mendo::doc_string_std code = MENDO_LIT("func hello(name string) error {\n    return nil\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Rust トークン化
// ============================================================

TEST(Syntax, DetectLanguageRust)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("rust")), SyntaxLanguage::Rust);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("rs")), SyntaxLanguage::Rust);
}

TEST(Syntax, RustKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("fn let mut if else match return");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, RustTypes)
{
    mendo::doc_string_std code = MENDO_LIT("i32 u64 f64 bool String Vec Option Result");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 8);
}

TEST(Syntax, RustLineComment)
{
    mendo::doc_string_std code = MENDO_LIT("let x = 1; // comment");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, RustBlockComment)
{
    mendo::doc_string_std code = MENDO_LIT("/* block\ncomment */");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, RustStringDouble)
{
    mendo::doc_string_std code = MENDO_LIT("let s = \"hello\";");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("\"hello\""));
}

TEST(Syntax, RustSingleQuoteNotString)
{
    // Rustではシングルクォートはライフタイム('a)と文字リテラル('x')に使用される。
    // ライフタイムの問題を避けるためシングルクォート文字列はスキップする。
    mendo::doc_string_std code = MENDO_LIT("fn foo<'a>(x: &'a str) {}");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    // 'aは行の残りを飲み込む文字列トークンを生成してはならない
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 1); // fn
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 1);    // str
}

TEST(Syntax, RustSomeNoneOkErr)
{
    mendo::doc_string_std code = MENDO_LIT("Some None Ok Err");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 4);
}

TEST(Syntax, RustAsyncAwait)
{
    mendo::doc_string_std code = MENDO_LIT("async await");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 2);
}

TEST(Syntax, RustComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("use std::io;\n\nfn main() -> Result<(), Box<dyn std::error::Error>> {\n    let x: i32 = 42;\n    // comment\n    println!(\"Hello {}\", x);\n    Ok(())\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // use, fn, let
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);       // Result, i32
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);    // // comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);     // "Hello {}"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 1);     // 42
}

TEST(Syntax, TokensCoverEntireTextRust)
{
    mendo::doc_string_std code = MENDO_LIT("struct Point { x: f64, y: f64 }");
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// TypeScript トークン化
// ============================================================

TEST(Syntax, DetectLanguageTypeScript)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("typescript")), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("ts")), SyntaxLanguage::TypeScript);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("tsx")), SyntaxLanguage::TypeScript);
}

TEST(Syntax, TsKeywordsInclJsKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else while for return const let var function");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, TsSpecificKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("interface type enum namespace declare abstract readonly");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 7);
}

TEST(Syntax, TsSpecificTypes)
{
    // voidはキーワード（JSから継承）なので、型には含まれない
    mendo::doc_string_std code = MENDO_LIT("any unknown never number string boolean");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, TsUtilityTypes)
{
    mendo::doc_string_std code = MENDO_LIT("Record Partial Required Readonly Pick Omit");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 6);
}

TEST(Syntax, TsTemplateLiteral)
{
    mendo::doc_string_std code = MENDO_LIT("`hello ${name}`");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, TsComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("interface User {\n  name: string;\n  age: number;\n}\n\nconst greet = (user: User): string => {\n  return `Hello, ${user.name}`;\n};");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // interface, const, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 3);       // string, number, string
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);     // template literal
}

TEST(Syntax, TokensCoverEntireTextTs)
{
    mendo::doc_string_std code = MENDO_LIT("type Props = { value: number; onChange: (v: number) => void; };");
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Bash トークン化
// ============================================================

TEST(Syntax, DetectLanguageBash)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("bash")), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("sh")), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("zsh")), SyntaxLanguage::Bash);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("shell")), SyntaxLanguage::Bash);
}

TEST(Syntax, BashKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if then else elif fi for while do done");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 9);
}

TEST(Syntax, BashBuiltins)
{
    mendo::doc_string_std code = MENDO_LIT("echo printf read cd pwd");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, BashHashComment)
{
    mendo::doc_string_std code = MENDO_LIT("x=1  # comment\ny=2");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("# comment"));
}

TEST(Syntax, BashString)
{
    mendo::doc_string_std code = MENDO_LIT("echo \"hello world\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(GetTokenText(code, *str), MENDO_LIT("\"hello world\""));
}

TEST(Syntax, BashBacktick)
{
    mendo::doc_string_std code = MENDO_LIT("result=`ls -la`");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, BashComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("#!/bin/bash\n# Script\nfor f in *.txt; do\n    echo \"$f\"\ndone");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // for, in, do, done
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, TokensCoverEntireTextBash)
{
    mendo::doc_string_std code = MENDO_LIT("if [ -f \"$1\" ]; then\n    echo \"exists\"\nfi");
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// PowerShell トークン化
// ============================================================

TEST(Syntax, DetectLanguagePowerShell)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("powershell")), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("pwsh")), SyntaxLanguage::PowerShell);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("ps1")), SyntaxLanguage::PowerShell);
}

TEST(Syntax, PwshKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else foreach while function return");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 6);
}

TEST(Syntax, PwshKeywordsCaseInsensitive)
{
    mendo::doc_string_std code = MENDO_LIT("If Else ForEach WHILE Function RETURN");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 6);
}

TEST(Syntax, PwshTypes)
{
    mendo::doc_string_std code = MENDO_LIT("int string bool array hashtable");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, PwshHashComment)
{
    mendo::doc_string_std code = MENDO_LIT("$x = 1  # comment\n$y = 2");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, PwshAngleBlockComment)
{
    mendo::doc_string_std code = MENDO_LIT("<# block\ncomment #>");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
}

TEST(Syntax, PwshAngleBlockCommentUnterminated)
{
    mendo::doc_string_std code = MENDO_LIT("<# never closed");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, SyntaxTokenType::Comment);
    EXPECT_EQ(tokens[0].length, static_cast<uint32_t>(code.size()));
}

TEST(Syntax, PwshString)
{
    mendo::doc_string_std code = MENDO_LIT("\"hello world\"");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, PwshComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("<# Script #>\nfunction Get-Item {\n    param([string]$Path)\n    # Do work\n    return $Path\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 2);   // <# #> and # comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2);   // function, param, return
}

TEST(Syntax, TokensCoverEntireTextPwsh)
{
    mendo::doc_string_std code = MENDO_LIT("if ($x -eq 1) { Write-Host \"hello\" }");
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// Cmd トークン化
// ============================================================

TEST(Syntax, DetectLanguageCmd)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("cmd")), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("bat")), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("batch")), SyntaxLanguage::Cmd);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("dosbatch")), SyntaxLanguage::Cmd);
}

TEST(Syntax, CmdKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("if else for do goto call set echo");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CmdKeywordsCaseInsensitive)
{
    mendo::doc_string_std code = MENDO_LIT("IF ELSE FOR DO GOTO CALL SET ECHO");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 8);
}

TEST(Syntax, CmdRemComment)
{
    mendo::doc_string_std code = MENDO_LIT("REM this is a comment\nset x=1");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT("REM this is a comment"));
}

TEST(Syntax, CmdRemCommentCaseInsensitive)
{
    mendo::doc_string_std code = MENDO_LIT("rem comment here");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
}

TEST(Syntax, CmdRemNotAtLineStart)
{
    // 行の途中のREMはコメントではなくキーワードであるべき
    mendo::doc_string_std code = MENDO_LIT("echo REM");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    // "echo"はキーワード、" "はプレーン、"REM"はコメントであってはならない
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 0);
}

TEST(Syntax, CmdDoubleColonComment)
{
    mendo::doc_string_std code = MENDO_LIT(":: this is a comment\nset x=1");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* comment = FindToken(tokens, SyntaxTokenType::Comment);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(GetTokenText(code, *comment), MENDO_LIT(":: this is a comment"));
}

TEST(Syntax, CmdDoubleColonNotAtLineStart)
{
    // 行頭でない::はコメントとして扱われないべき
    mendo::doc_string_std code = MENDO_LIT("x::y");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 0);
}

TEST(Syntax, CmdString)
{
    mendo::doc_string_std code = MENDO_LIT("echo \"hello world\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    auto* str = FindToken(tokens, SyntaxTokenType::String);
    ASSERT_NE(str, nullptr);
}

TEST(Syntax, CmdTypes)
{
    mendo::doc_string_std code = MENDO_LIT("dir copy move del mkdir");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Type), 5);
}

TEST(Syntax, CmdComplexCode)
{
    mendo::doc_string_std code = MENDO_LIT("@echo off\nREM Build script\nfor %%f in (*.cpp) do (\n    echo Building %%f\n)\npause");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);   // REM
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);   // echo, for, do, echo, pause
}

TEST(Syntax, TokensCoverEntireTextCmd)
{
    mendo::doc_string_std code = MENDO_LIT("if exist \"file.txt\" (\n    del \"file.txt\"\n)");
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// JSON / JSONC
// ============================================================

TEST(Syntax, DetectLanguageJson)
{
    EXPECT_EQ(DetectLanguage(MENDO_LIT("json")), SyntaxLanguage::Json);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("jsonc")), SyntaxLanguage::Json);
    EXPECT_EQ(DetectLanguage(MENDO_LIT("json5")), SyntaxLanguage::Json);
}

TEST(Syntax, JsonLiteralsAsKeywords)
{
    mendo::doc_string_std code = MENDO_LIT("[true, false, null]");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 3);
}

TEST(Syntax, JsonString)
{
    mendo::doc_string_std code = MENDO_LIT("\"hello world\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, JsonStringWithEscapes)
{
    mendo::doc_string_std code = MENDO_LIT("\"line1\\nline2\\t\\\"quoted\\\"\"");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, JsonSingleQuoteIsNotString)
{
    // JSON は二重引用符のみ。シングルクォートは文字列として扱わない（プレーン）。
    mendo::doc_string_std code = MENDO_LIT("'not a string'");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 0);
}

TEST(Syntax, JsonNumbers)
{
    // 負号は分離されるが、数値部はトークン化される。
    mendo::doc_string_std code = MENDO_LIT("[0, 1, -2, 3.14, 1e10, 1.5e-3]");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 6);
}

TEST(Syntax, JsonObject)
{
    mendo::doc_string_std code = MENDO_LIT("{\"name\": \"alice\", \"age\": 30, \"active\": true}");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 4);
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Number), 1);
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 1);
}

TEST(Syntax, JsonNestedStructure)
{
    mendo::doc_string_std code = MENDO_LIT("{\"items\": [{\"id\": 1}, {\"id\": 2}], \"count\": 2}");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 3);
}

TEST(Syntax, JsoncLineComment)
{
    // JSONC: // 形式のコメントを許容。
    mendo::doc_string_std code = MENDO_LIT("{\n  // comment\n  \"key\": 1\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 1);
}

TEST(Syntax, JsoncBlockComment)
{
    // JSONC: /* */ 形式のコメントを許容。
    mendo::doc_string_std code = MENDO_LIT("{\n  /* block\n     comment */\n  \"key\": 1\n}");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Comment), 1);
}

TEST(Syntax, TokensCoverEntireTextJson)
{
    mendo::doc_string_std code = MENDO_LIT("{\"a\": [1, 2, null], \"b\": {\"c\": false}}");
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
}

// ============================================================
// パーサー統合: 新言語
// ============================================================

TEST(Syntax, ParserExtractsLanguageGo)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```go\nfunc main() {}\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Go);
}

TEST(Syntax, ParserExtractsLanguageTs)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```typescript\nconst x: number = 1;\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::TypeScript);
}

TEST(Syntax, ParserExtractsLanguageBash)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```bash\necho hello\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Bash);
}

TEST(Syntax, ParserExtractsLanguagePwsh)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```powershell\nWrite-Host hello\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::PowerShell);
}

TEST(Syntax, ParserExtractsLanguageCmd)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```cmd\necho hello\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cmd);
}

TEST(Syntax, ParserExtractsLanguageJson)
{
    auto nodes = ParseMarkdown(MENDO_LIT("```json\n{\"k\": 1}\n```")).nodes;
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Json);
}
