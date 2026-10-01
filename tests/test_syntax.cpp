#include <gtest/gtest.h>
#include <memory_resource>
#include <string>
#include <string_view>
#include "syntax.h"
#include "parser.h"

namespace {

// 連続性の検査は start 昇順・非重複も保証する
void AssertTokensCoverText(const std::pmr::vector<SyntaxToken>& tokens, size_t text_length)
{
    if (text_length == 0) {
        EXPECT_TRUE(tokens.empty());
        return;
    }
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].start, 0u);
    for (size_t i = 1; i < tokens.size(); i++) {
        EXPECT_EQ(tokens[i].start, tokens[i - 1].start + tokens[i - 1].length)
            << "トークン " << (i - 1) << " と " << i << " の間にギャップあり";
    }
    uint32_t total = 0;
    for (const auto& t : tokens) {
        total += t.length;
    }
    EXPECT_EQ(total, static_cast<uint32_t>(text_length));
}

const SyntaxToken* FindToken(const std::pmr::vector<SyntaxToken>& tokens, SyntaxTokenType type)
{
    for (const auto& t : tokens) {
        if (t.type == type) {
            return &t;
        }
    }
    return nullptr;
}

int CountTokens(const std::pmr::vector<SyntaxToken>& tokens, SyntaxTokenType type)
{
    int count = 0;
    for (const auto& t : tokens) {
        if (t.type == type) {
            count++;
        }
    }
    return count;
}

std::string_view GetTokenText(std::string_view text, const SyntaxToken& token)
{
    return text.substr(token.start, token.length);
}

const SyntaxToken* FindTokenByText(std::string_view code, const std::pmr::vector<SyntaxToken>& tokens,
                                   std::string_view text)
{
    for (const auto& t : tokens) {
        if (GetTokenText(code, t) == text) {
            return &t;
        }
    }
    return nullptr;
}

struct LanguageCase {
    std::string_view input;
    SyntaxLanguage expected;
};

struct CodeCase {
    const char* name;
    std::string_view code;
    SyntaxLanguage lang;
};

struct TypedCase {
    const char* name;
    std::string_view code;
    SyntaxLanguage lang;
    SyntaxTokenType type;
};

struct CountCase {
    const char* name;
    std::string_view code;
    SyntaxLanguage lang;
    SyntaxTokenType type;
    int count;
};

struct TokenTextCase {
    const char* name;
    std::string_view code;
    SyntaxLanguage lang;
    SyntaxTokenType type;
    std::string_view text;
};

} // namespace

// ============================================================
// 言語判定
// ============================================================

TEST(Syntax, DetectLanguage)
{
    using enum SyntaxLanguage;
    static constexpr LanguageCase kCases[] = {
        { "cpp", Cpp }, { "c", Cpp }, { "c++", Cpp }, { "cxx", Cpp }, { "cc", Cpp },
        { "h", Cpp }, { "hpp", Cpp }, { "hxx", Cpp },
        { "python", Python }, { "py", Python },
        { "javascript", JavaScript }, { "js", JavaScript }, { "jsx", JavaScript },
        { "typescript", TypeScript }, { "ts", TypeScript }, { "tsx", TypeScript },
        { "go", Go }, { "golang", Go },
        { "rust", Rust }, { "rs", Rust },
        { "bash", Bash }, { "sh", Bash }, { "zsh", Bash }, { "shell", Bash },
        { "powershell", PowerShell }, { "pwsh", PowerShell }, { "ps1", PowerShell },
        { "cmd", Cmd }, { "bat", Cmd }, { "batch", Cmd }, { "dosbatch", Cmd },
        { "json", Json }, { "jsonc", Json }, { "json5", Json },
        { "mermaid", Mermaid },
        { "CPP", Cpp }, { "Python", Python }, { "JavaScript", JavaScript }, { "JS", JavaScript },
        { "TypeScript", TypeScript }, { "GO", Go }, { "RUST", Rust }, { "BASH", Bash },
        { "PowerShell", PowerShell }, { "CMD", Cmd }, { "JSON", Json }, { "JsonC", Json },
        { "Mermaid", Mermaid }, { "MERMAID", Mermaid },
        // md4c は言語の後に追加テキストを含む info 文字列を渡す場合がある
        { "cpp some-extra", Cpp }, { "python\ttab-separated", Python },
        { "java", None }, { "ruby", None }, { "swift", None }, { "", None },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(testing::Message() << "info=\"" << c.input << '"');
        EXPECT_EQ(DetectLanguage(c.input), c.expected);
    }
}

TEST(Syntax, ParserExtractsLanguage)
{
    using enum SyntaxLanguage;
    static constexpr LanguageCase kCases[] = {
        { "```cpp\nint x = 1;\n```", Cpp },
        { "```CPP\nint x;\n```", Cpp },
        { "```python\ndef foo(): pass\n```", Python },
        { "```js\nconst x = 1;\n```", JavaScript },
        { "```typescript\nconst x: number = 1;\n```", TypeScript },
        { "```go\nfunc main() {}\n```", Go },
        { "```rust\nfn main() {}\n```", Rust },
        { "```bash\necho hello\n```", Bash },
        { "```powershell\nWrite-Host hello\n```", PowerShell },
        { "```cmd\necho hello\n```", Cmd },
        { "```json\n{\"k\": 1}\n```", Json },
        { "```\nplain code\n```", None },
        { "```java\nclass Main {}\n```", None },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.input);
        auto nodes = ParseMarkdown(c.input).nodes;
        EXPECT_EQ(nodes.size(), 1u);
        if (nodes.empty()) {
            continue;
        }
        EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
        EXPECT_EQ(nodes[0].code_language(), c.expected);
    }
}

// ============================================================
// トークン化 (テーブル駆動)
// ============================================================

TEST(Syntax, TokenizeReturnsEmpty)
{
    using enum SyntaxLanguage;
    static constexpr CodeCase kCases[] = {
        { "EmptyTextReturnsEmpty", "", Cpp },
        { "NoneLanguageReturnsEmpty", "int main() {}", None },
        // Mermaid は現在の実装ではトークナイザーを持たない
        { "MermaidLanguageReturnsEmpty", "graph TD; A-->B;", Mermaid },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        EXPECT_TRUE(Tokenize(c.code, c.lang).empty());
    }
}

TEST(Syntax, TokensCoverEntireText)
{
    using enum SyntaxLanguage;
    static constexpr CodeCase kCases[] = {
        { "TokensCoverEntireTextCpp", "int main() { return 0; }", Cpp },
        { "TokensCoverEntireTextPython", "def hello():\n    print('world')", Python },
        { "TokensCoverEntireTextJs", "const f = () => { return 42; };", JavaScript },
        { "TokensCoverEntireTextTs", "type Props = { value: number; onChange: (v: number) => void; };", TypeScript },
        { "TokensCoverEntireTextGo", "func hello(name string) error {\n    return nil\n}", Go },
        { "TokensCoverEntireTextRust", "struct Point { x: f64, y: f64 }", Rust },
        { "TokensCoverEntireTextBash", "if [ -f \"$1\" ]; then\n    echo \"exists\"\nfi", Bash },
        { "TokensCoverEntireTextPwsh", "if ($x -eq 1) { Write-Host \"hello\" }", PowerShell },
        { "TokensCoverEntireTextCmd", "if exist \"file.txt\" (\n    del \"file.txt\"\n)", Cmd },
        { "TokensCoverEntireTextJson", "{\"a\": [1, 2, null], \"b\": {\"c\": false}}", Json },
        { "OnlyOperators", "+ - * / = == != < > <= >=", Cpp },
        // 閉じられていない文字列が無限ループを引き起こさないこと
        { "UnterminatedString", "x = \"unterminated\ny = 1", Cpp },
        { "CppRawStringTokensAreOrderedAndNonOverlapping", "R\"(one)\" + R\"delim(two)delim\" + normalCall()", Cpp },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        AssertTokensCoverText(Tokenize(c.code, c.lang), c.code.size());
    }
}

TEST(Syntax, TokenCount)
{
    using enum SyntaxLanguage;
    using enum SyntaxTokenType;
    static constexpr std::string_view kJsonObject = "{\"name\": \"alice\", \"age\": 30, \"active\": true}";
    static constexpr CountCase kCases[] = {
        { "CppTypes", "int float double bool", Cpp, Type, 4 },
        { "CppModernKeywords", "constexpr consteval constinit concept requires co_await co_return co_yield", Cpp, Keyword, 8 },
        { "CppCastKeywords", "static_cast dynamic_cast reinterpret_cast const_cast", Cpp, Keyword, 4 },
        { "CppStlTypes", "vector map optional variant span unique_ptr shared_ptr", Cpp, Type, 7 },
        { "CppWin32Types", "HRESULT BOOL DWORD HWND LRESULT", Cpp, Type, 5 },
        // コードの後の # はプリプロセッサではない
        { "CppPreprocessorNotAtLineStart", "x = a #", Cpp, Preprocessor, 0 },
        // ( が続いてもキーワードは関数ではなくキーワードのまま
        { "CppKeywordNotFunction", "if (x)", Cpp, Function, 0 },
        // 単独のドットは数値として扱わない
        { "DotNotANumber", "a.b", Cpp, Number, 0 },
        { "PythonKeywords", "if else while for return def class", Python, Keyword, 7 },
        { "PythonTypes", "int float str bool list dict", Python, Type, 6 },
        { "PythonTrueFalseNone", "x = True\ny = False\nz = None", Python, Keyword, 3 },
        { "PythonExceptionTypes", "ValueError TypeError KeyError IndexError RuntimeError", Python, Type, 5 },
        { "JsKeywords", "if else while for return const let var function", JavaScript, Keyword, 9 },
        { "JsTypes", "Array Map Set Promise", JavaScript, Type, 4 },
        { "JsTrueFalseNull", "true false null undefined", JavaScript, Type, 4 },
        { "JsGlobalTypes", "console document window JSON Math", JavaScript, Type, 5 },
        { "JsAsyncAwait", "async await", JavaScript, Keyword, 2 },
        { "TsKeywordsInclJsKeywords", "if else while for return const let var function", TypeScript, Keyword, 9 },
        { "TsSpecificKeywords", "interface type enum namespace declare abstract readonly", TypeScript, Keyword, 7 },
        // void は JS から継承したキーワードなので型には含まれない
        { "TsSpecificTypes", "any unknown never number string boolean", TypeScript, Type, 6 },
        { "TsUtilityTypes", "Record Partial Required Readonly Pick Omit", TypeScript, Type, 6 },
        { "GoKeywords", "if else for return func defer go", Go, Keyword, 7 },
        { "GoTypes", "int float64 string bool error", Go, Type, 5 },
        { "GoNilTrueFalse", "nil true false iota", Go, Type, 4 },
        { "RustKeywords", "fn let mut if else match return", Rust, Keyword, 7 },
        { "RustTypes", "i32 u64 f64 bool String Vec Option Result", Rust, Type, 8 },
        { "RustSomeNoneOkErr", "Some None Ok Err", Rust, Type, 4 },
        { "RustAsyncAwait", "async await", Rust, Keyword, 2 },
        { "BashKeywords", "if then else elif fi for while do done", Bash, Keyword, 9 },
        { "BashBuiltins", "echo printf read cd pwd", Bash, Type, 5 },
        { "PwshKeywords", "if else foreach while function return", PowerShell, Keyword, 6 },
        { "PwshKeywordsCaseInsensitive", "If Else ForEach WHILE Function RETURN", PowerShell, Keyword, 6 },
        { "PwshTypes", "int string bool array hashtable", PowerShell, Type, 5 },
        { "CmdKeywords", "if else for do goto call set echo", Cmd, Keyword, 8 },
        { "CmdKeywordsCaseInsensitive", "IF ELSE FOR DO GOTO CALL SET ECHO", Cmd, Keyword, 8 },
        { "CmdTypes", "dir copy move del mkdir", Cmd, Type, 5 },
        // 行の途中の REM や行頭でない :: はコメントではない
        { "CmdRemNotAtLineStart", "echo REM", Cmd, Comment, 0 },
        { "CmdDoubleColonNotAtLineStart", "x::y", Cmd, Comment, 0 },
        { "JsonLiteralsAsKeywords", "[true, false, null]", Json, Keyword, 3 },
        { "JsonString", "\"hello world\"", Json, String, 1 },
        { "JsonStringWithEscapes", "\"line1\\nline2\\t\\\"quoted\\\"\"", Json, String, 1 },
        // JSON は二重引用符のみ。シングルクォートは文字列として扱わない
        { "JsonSingleQuoteIsNotString", "'not a string'", Json, String, 0 },
        // 負号は分離されるが、数値部はトークン化される
        { "JsonNumbers", "[0, 1, -2, 3.14, 1e10, 1.5e-3]", Json, Number, 6 },
        { "JsonObject/String", kJsonObject, Json, String, 4 },
        { "JsonObject/Number", kJsonObject, Json, Number, 1 },
        { "JsonObject/Keyword", kJsonObject, Json, Keyword, 1 },
        // JSONC のコメントを許容する
        { "JsoncLineComment", "{\n  // comment\n  \"key\": 1\n}", Json, Comment, 1 },
        { "JsoncBlockComment", "{\n  /* block\n     comment */\n  \"key\": 1\n}", Json, Comment, 1 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensCoverText(tokens, c.code.size());
        EXPECT_EQ(CountTokens(tokens, c.type), c.count);
    }
}

TEST(Syntax, FirstTokenText)
{
    using enum SyntaxLanguage;
    using enum SyntaxTokenType;
    static constexpr TokenTextCase kCases[] = {
        { "CppSingleLineComment", "x = 1; // comment\ny = 2;", Cpp, Comment, "// comment" },
        { "TerminatedBlockCommentStillWorks", "/* ok */ x", Cpp, Comment, "/* ok */" },
        { "CppStringDouble", "x = \"hello world\"", Cpp, String, "\"hello world\"" },
        { "CppStringSingle", "c = 'x'", Cpp, String, "'x'" },
        { "CppStringEscape", "s = \"hello\\\"world\"", Cpp, String, "\"hello\\\"world\"" },
        { "CppRawStringWithDelimiter", "R\"delim(hello \"world\")delim\"", Cpp, String, "R\"delim(hello \"world\")delim\"" },
        { "CppNumberInteger", "x = 42", Cpp, Number, "42" },
        { "CppNumberHex", "x = 0xFF", Cpp, Number, "0xFF" },
        { "CppNumberFloat", "x = 3.14f", Cpp, Number, "3.14f" },
        { "CppNumberBinary", "x = 0b1010", Cpp, Number, "0b1010" },
        { "CppNumberWithSuffix", "42ULL", Cpp, Number, "42ULL" },
        { "CppNumberExponent", "1.5e10", Cpp, Number, "1.5e10" },
        { "CppNumberExponentNegative", "2.0e-3", Cpp, Number, "2.0e-3" },
        { "CppNumberDigitSeparator", "1'000'000", Cpp, Number, "1'000'000" },
        { "CppHexDigitSeparator", "0xFF'FF", Cpp, Number, "0xFF'FF" },
        { "CppFunctionCall", "foo(42)", Cpp, Function, "foo" },
        { "CppFunctionCallWithSpace", "bar (x)", Cpp, Function, "bar" },
        { "CppKeywordNotFunction", "if (x)", Cpp, Keyword, "if" },
        { "PythonComment", "x = 1  # comment\ny = 2", Python, Comment, "# comment" },
        { "PythonTripleQuoteDouble", "s = \"\"\"hello\nworld\"\"\"", Python, String, "\"\"\"hello\nworld\"\"\"" },
        { "PythonTripleQuoteSingle", "s = '''docstring'''", Python, String, "'''docstring'''" },
        // バックスラッシュエスケープ経路: `\"` をスキップしてから本物の `"""` で終端する
        { "PythonTripleQuoteWithBackslashEscape", "\"\"\"abc\\\"\"\"def\"\"\"", Python, String, "\"\"\"abc\\\"\"\"def\"\"\"" },
        { "PythonDefFunction/def", "def foo():", Python, Keyword, "def" },
        { "PythonDefFunction/foo", "def foo():", Python, Function, "foo" },
        { "JsSingleLineComment", "// comment\nx = 1", JavaScript, Comment, "// comment" },
        { "JsTemplateLiteral", "`hello ${name}`", JavaScript, String, "`hello ${name}`" },
        { "JsArrowFunction/const", "const f = () => 42", JavaScript, Keyword, "const" },
        { "JsArrowFunction/42", "const f = () => 42", JavaScript, Number, "42" },
        { "JsBigIntNumber", "42n", JavaScript, Number, "42n" },
        { "GoLineComment", "x := 1 // comment\ny := 2", Go, Comment, "// comment" },
        { "GoBacktickRawString", "`raw\\nstring`", Go, String, "`raw\\nstring`" },
        // Go の生文字列はバックスラッシュをエスケープとして扱わないので `c:\` は有効
        { "GoBacktickRawStringWithBackslash", "`c:\\`", Go, String, "`c:\\`" },
        // 生文字列末尾のバックスラッシュが閉じバッククォートをスキップしないこと
        { "GoBacktickRawStringTrailingBackslash", "s := `path\\` + x", Go, String, "`path\\`" },
        { "RustStringDouble", "let s = \"hello\";", Rust, String, "\"hello\"" },
        { "BashHashComment", "x=1  # comment\ny=2", Bash, Comment, "# comment" },
        { "BashString", "echo \"hello world\"", Bash, String, "\"hello world\"" },
        { "CmdRemComment", "REM this is a comment\nset x=1", Cmd, Comment, "REM this is a comment" },
        { "CmdDoubleColonComment", ":: this is a comment\nset x=1", Cmd, Comment, ":: this is a comment" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensCoverText(tokens, c.code.size());
        const auto* token = FindToken(tokens, c.type);
        EXPECT_NE(token, nullptr);
        if (token != nullptr) {
            EXPECT_EQ(GetTokenText(c.code, *token), c.text);
        }
    }
}

TEST(Syntax, HasToken)
{
    using enum SyntaxLanguage;
    using enum SyntaxTokenType;
    static constexpr TypedCase kCases[] = {
        { "CppRawStringAfterSpaceR", "x R\"(test)\"", Cpp, String },
        { "CppNumberOctal", "0o77", Cpp, Number },
        { "NumberStartsWithDot", ".5f", Cpp, Number },
        { "CppCommentEol", "int x; // comment", Cpp, Comment },
        { "PythonFString", "f\"hello {name}\"", Python, String },
        { "PythonUnterminatedTripleQuote", "s = \"\"\"never closed", Python, String },
        { "TsTemplateLiteral", "`hello ${name}`", TypeScript, String },
        { "RustLineComment", "let x = 1; // comment", Rust, Comment },
        { "BashBacktick", "result=`ls -la`", Bash, String },
        { "PwshHashComment", "$x = 1  # comment\n$y = 2", PowerShell, Comment },
        { "PwshString", "\"hello world\"", PowerShell, String },
        { "CmdRemCommentCaseInsensitive", "rem comment here", Cmd, Comment },
        { "CmdString", "echo \"hello world\"", Cmd, String },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensCoverText(tokens, c.code.size());
        EXPECT_NE(FindToken(tokens, c.type), nullptr);
    }
}

TEST(Syntax, WholeInputIsSingleToken)
{
    using enum SyntaxLanguage;
    using enum SyntaxTokenType;
    static constexpr TypedCase kCases[] = {
        { "TokenizeSingleKeyword", "if", Cpp, Keyword },
        { "CppMultiLineComment", "/* multi\nline\ncomment */", Cpp, Comment },
        // バグ #22: 閉じられていないブロックコメントは最後の文字まで含む
        { "UnterminatedBlockComment", "/* never closed", Cpp, Comment },
        { "UnterminatedBlockCommentCoversAllText", "/* unterminated comment", Cpp, Comment },
        { "UnterminatedBlockCommentEndsWithStar", "/* test *", Cpp, Comment },
        { "CppPreprocessorInclude", "#include <stdio.h>", Cpp, Preprocessor },
        { "CppPreprocessorDefine", "#define MAX 100", Cpp, Preprocessor },
        { "CppPreprocessorContinuation", "#define FOO \\\n    bar", Cpp, Preprocessor },
        // バグ修正: R"(...)" は R を含めて単一の String にする (Plain と String の重複禁止)
        { "CppRawStringStandaloneR", "R\"(hello)\"", Cpp, String },
        { "CppRawStringRPrefixMergedIntoSingleToken", "R\"(abc)\"", Cpp, String },
        { "JsMultiLineComment", "/* block\ncomment */", JavaScript, Comment },
        { "JsTemplateLiteralMultiLine", "`line1\nline2\nline3`", JavaScript, String },
        { "GoBlockComment", "/* multi\nline */", Go, Comment },
        { "RustBlockComment", "/* block\ncomment */", Rust, Comment },
        { "PwshAngleBlockComment", "<# block\ncomment #>", PowerShell, Comment },
        { "PwshAngleBlockCommentUnterminated", "<# never closed", PowerShell, Comment },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensCoverText(tokens, c.code.size());
        EXPECT_EQ(tokens.size(), 1u);
        if (!tokens.empty()) {
            EXPECT_EQ(tokens[0].type, c.type);
        }
    }
}

// ============================================================
// トークン化 (個別ケース)
// ============================================================

TEST(Syntax, CppKeywords)
{
    std::string code = "if else while for return";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    for (const auto& t : tokens) {
        if (t.type != SyntaxTokenType::Plain) {
            EXPECT_EQ(t.type, SyntaxTokenType::Keyword) << "offset=" << t.start;
        }
    }
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 5);
}

TEST(Syntax, OnlyWhitespace)
{
    std::string code = "   \n\t  \n  ";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());
    for (const auto& t : tokens) {
        EXPECT_EQ(t.type, SyntaxTokenType::Plain);
    }
}

// バグ #16: 生文字列 R" の検出が R で終わる識別子 (例: RENDER"hello") でトリガーされないこと
TEST(Syntax, CppRawStringNotTriggeredByIdentifierEndingR)
{
    std::string code = "RENDER\"hello\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    // RENDER は単一の識別子トークン（Plain または Function）で、"hello" と重複しない
    const auto* render = FindTokenByText(code, tokens, "RENDER");
    EXPECT_NE(render, nullptr) << "RENDERが独立したトークンとして見つかるべき";
    if (render != nullptr) {
        EXPECT_NE(render->type, SyntaxTokenType::String);
    }
    const auto* str = FindTokenByText(code, tokens, "\"hello\"");
    EXPECT_NE(str, nullptr) << "\"hello\"が文字列トークンとして見つかるべき";
    if (str != nullptr) {
        EXPECT_EQ(str->type, SyntaxTokenType::String);
    }
}

TEST(Syntax, CppRawStringNotTriggeredWhenRIsPartOfLongerIdentifier)
{
    // 識別子が "R" 単独でない ("xR") ため生文字列扱いされず、"(a)" は通常の文字列として解釈される
    std::string code = "xR\"(a)\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensCoverText(tokens, code.size());

    const auto* xr = FindTokenByText(code, tokens, "xR");
    EXPECT_NE(xr, nullptr) << "xRが独立したトークンとして見つかるべき";
    if (xr != nullptr) {
        EXPECT_NE(xr->type, SyntaxTokenType::String);
    }
    const auto* str = FindTokenByText(code, tokens, "\"(a)\"");
    EXPECT_NE(str, nullptr) << "\"(a)\"が通常の文字列トークンとして見つかるべき";
    if (str != nullptr) {
        EXPECT_EQ(str->type, SyntaxTokenType::String);
    }
}

TEST(Syntax, PythonDecorator)
{
    // "@" は特別に処理されないが、"def" と "pass" はキーワードであるべき
    std::string code = "@staticmethod\ndef foo():\n    pass";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensCoverText(tokens, code.size());
    for (const std::string_view word : { "def", "pass" }) {
        SCOPED_TRACE(word);
        const auto* token = FindTokenByText(code, tokens, word);
        EXPECT_NE(token, nullptr);
        if (token != nullptr) {
            EXPECT_EQ(token->type, SyntaxTokenType::Keyword);
        }
    }
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

TEST(Syntax, JsonNestedStructure)
{
    std::string code = "{\"items\": [{\"id\": 1}, {\"id\": 2}], \"count\": 2}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensCoverText(tokens, code.size());
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 3);
}

// ============================================================
// 複合コード
// ============================================================

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

TEST(Syntax, JsComplexCode)
{
    std::string code = "async function fetchData(url) {\n  // Fetch data\n  const resp = await fetch(url);\n  return resp.json();\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::JavaScript);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 4);  // async, function, const, await, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // // Fetch data
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2); // fetchData, fetch
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

TEST(Syntax, BashComplexCode)
{
    std::string code = "#!/bin/bash\n# Script\nfor f in *.txt; do\n    echo \"$f\"\ndone";
    auto tokens = Tokenize(code, SyntaxLanguage::Bash);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // for, in, do, done
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, PwshComplexCode)
{
    std::string code = "<# Script #>\nfunction Get-Item {\n    param([string]$Path)\n    # Do work\n    return $Path\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 2); // <# #> and # comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2); // function, param, return
}

TEST(Syntax, CmdComplexCode)
{
    std::string code = "@echo off\nREM Build script\nfor %%f in (*.cpp) do (\n    echo Building %%f\n)\npause";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensCoverText(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1); // REM
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // echo, for, do, echo, pause
}
