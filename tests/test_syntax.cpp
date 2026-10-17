#include <gtest/gtest.h>
#include <algorithm>
#include <format>
#include <iterator>
#include <memory_resource>
#include <random>
#include <string>
#include <string_view>
#include "syntax.h"
#include "parser.h"
#include "utf8_fuzz_helpers.h"
#include "test_nodes.h"

namespace {

// 描画と HTML 化はトークンを start 昇順・非重複として隙間を Plain で埋める。Plain 自体は出力しない。
testing::AssertionResult TokensWellFormed(const std::pmr::vector<SyntaxToken>& tokens, size_t text_length)
{
    uint64_t prev_end = 0;
    for (size_t i = 0; i < tokens.size(); i++) {
        const auto& t = tokens[i];
        if (t.type == SyntaxTokenType::Plain) {
            return testing::AssertionFailure() << "トークン " << i << " が Plain";
        }
        if (t.length == 0) {
            return testing::AssertionFailure() << "トークン " << i << " の長さが 0";
        }
        if (t.start < prev_end) {
            return testing::AssertionFailure() << "トークン " << i << " start=" << t.start << " が直前 end=" << prev_end << " と重なっている";
        }
        prev_end = static_cast<uint64_t>(t.start) + t.length;
    }
    if (prev_end > text_length) {
        return testing::AssertionFailure() << "末尾 end=" << prev_end << " がテキスト長 " << text_length << " を超えている";
    }
    return testing::AssertionSuccess();
}

void AssertTokensWellFormed(const std::pmr::vector<SyntaxToken>& tokens, size_t text_length)
{
    EXPECT_TRUE(TokensWellFormed(tokens, text_length));
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
        { "java", Java }, { "cs", CSharp }, { "c#", CSharp }, { "csharp", CSharp },
        { "kotlin", Kotlin }, { "kt", Kotlin }, { "kts", Kotlin }, { "swift", Swift }, { "php", Php },
        { "ruby", Ruby }, { "rb", Ruby }, { "sql", Sql }, { "mysql", Sql }, { "postgresql", Sql }, { "sqlite", Sql },
        { "yaml", Yaml }, { "yml", Yaml }, { "toml", Toml }, { "ini", Ini }, { "cfg", Ini }, { "properties", Ini },
        { "html", Html }, { "htm", Html }, { "xhtml", Html }, { "xml", Xml }, { "svg", Xml }, { "xaml", Xml },
        { "css", Css }, { "scss", Scss }, { "less", Scss }, { "diff", Diff }, { "patch", Diff },
        { "dockerfile", Dockerfile }, { "docker", Dockerfile }, { "Dockerfile", Dockerfile },
        { "makefile", Makefile }, { "make", Makefile }, { "mk", Makefile }, { "Makefile", Makefile },
        { "math", LatexMath }, { "MATH", LatexMath },
        { "zig", Zig }, { "hs", Haskell }, { "haskell", Haskell },
        { "clojure", Clojure }, { "clj", Clojure }, { "cljs", Clojure }, { "edn", Clojure },
        { "elisp", EmacsLisp }, { "emacs-lisp", EmacsLisp }, { "el", EmacsLisp },
        { "lisp", CommonLisp }, { "common-lisp", CommonLisp }, { "cl", CommonLisp },
        { "scheme", Scheme }, { "scm", Scheme }, { "racket", Scheme }, { "rkt", Scheme },
        { "lua", Lua }, { "vim", Vim }, { "viml", Vim }, { "vimscript", Vim }, { "vim9", Vim9 }, { "vim9script", Vim9 },
        { "latex", None }, { "text", None }, { "plaintext", None }, { "", None },
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
        { "```java\nclass Main {}\n```", Java },
        { "```yaml\nkey: 1\n```", Yaml },
        { "```math\nx^2\n```", LatexMath },
        { "```text\nplain\n```", None },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.input);
        auto nodes = ParseNodes(c.input);
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
        { "LatexMathLanguageReturnsEmpty", "\\frac{a}{b}", LatexMath },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        EXPECT_TRUE(Tokenize(c.code, c.lang).empty());
    }
}

TEST(Syntax, TokensAreWellFormed)
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
        { "TokensCoverEntireTextJava", "public static void main(String[] args) { System.out.println(\"hi\"); }", Java },
        { "TokensCoverEntireTextCSharp", "#region R\nvar p = @\"C:\\dir\\\";\n#endregion", CSharp },
        { "TokensCoverEntireTextKotlin", "fun main() { val s = \"\"\"raw\"\"\" }", Kotlin },
        { "TokensCoverEntireTextSwift", "let x: Int = 1 // c\n#if DEBUG\n#endif", Swift },
        { "TokensCoverEntireTextPhp", "<?php\n#[Attr]\nfunction f($x) { return $x; } # c", Php },
        { "TokensCoverEntireTextRuby", "=begin\nc\n=end\ndef f(x) = x * 2 # c", Ruby },
        { "TokensCoverEntireTextSql", "SELECT id FROM t WHERE x = 'a' -- c\n/* b */", Sql },
        { "TokensCoverEntireTextYaml", "a: 1\nb: |\n  text\nc: [1, {d: e}] # c", Yaml },
        { "TokensCoverEntireTextToml", "[t]\na = \"\"\"\nx\n\"\"\"\nb = { c = 1 } # c", Toml },
        { "TokensCoverEntireTextIni", "[s]\nk = v\n; c", Ini },
        { "TokensCoverEntireTextHtml", "<p a=\"b\">&amp;<script>let x = 1;</script><style>a{b:c}</style>", Html },
        { "TokensCoverEntireTextXml", "<?xml version=\"1.0\"?><a b='c'><![CDATA[x]]></a>", Xml },
        { "TokensCoverEntireTextCss", "@media (x: 1px) { a.b:hover { c: url(d) !important; } }", Css },
        { "TokensCoverEntireTextScss", "$a: 1;\n.b { &:c { d: $a; } } // e", Scss },
        { "TokensCoverEntireTextDiff", "@@ -1 +1 @@\n-a\n+b\n c", Diff },
        { "TokensCoverEntireTextDockerfile", "FROM a AS b\nRUN c \\\n  && d \"e\"", Dockerfile },
        { "TokensCoverEntireTextMakefile", "A := 1\nall: $(A)\n\techo \"$(A)\" # c", Makefile },
        { "OnlyOperators", "+ - * / = == != < > <= >=", Cpp },
        // 閉じられていない文字列が無限ループを引き起こさないこと
        { "UnterminatedString", "x = \"unterminated\ny = 1", Cpp },
        { "CppRawStringTokensAreOrderedAndNonOverlapping", "R\"(one)\" + R\"delim(two)delim\" + normalCall()", Cpp },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        AssertTokensWellFormed(Tokenize(c.code, c.lang), c.code.size());
    }
}

TEST(Syntax, TokenCount)
{
    using enum SyntaxLanguage;
    using enum SyntaxTokenType;
    static constexpr std::string_view kJsonObject = "{\"name\": \"alice\", \"age\": 30, \"active\": true}";
    static constexpr std::string_view kYamlBlock = "run: |\n  name: x\n  echo hi\nnext: 1";
    static constexpr std::string_view kYamlFlow = "list: [1, two, {a: 3}]";
    static constexpr std::string_view kToml = "[package]\nname = \"mendo\"\nversion = \"0.1\"\n\n[[bin]]\npath = 'src/main.rs'";
    static constexpr std::string_view kIni = "; c\n# d\nkey = value\nother: 1";
    static constexpr std::string_view kIniValues = "port = 8080\ndebug = true\nname = \"x\"\npath = /usr/bin";
    static constexpr std::string_view kHtml = "<div class=\"a\" hidden>x</div>";
    static constexpr std::string_view kCss = ".btn:hover, #main > p { color: #fff; margin: 0 10px; }";
    static constexpr std::string_view kCssMedia = "@media (max-width: 600px) { a { color: red !important; } }";
    static constexpr std::string_view kScss = "$primary: #333;\n.a {\n  color: $primary; // c\n  &:hover { color: red; }\n}";
    static constexpr std::string_view kDiff =
        "diff --git a/f b/f\nindex 1..2 100644\n--- a/f\n+++ b/f\n@@ -1,2 +1,2 @@\n-old\n+new\n context\n\\ No newline at end of file";
    static constexpr std::string_view kDocker =
        "FROM node:18 AS build\n# comment\nRUN npm ci && \\\n    npm run build\nCOPY --from=build /app /app\nENV PATH=/app/bin";
    static constexpr std::string_view kMake = "CC := gcc\nCFLAGS += -O2 # opt\n\nall: main.o\n\t$(CC) $(CFLAGS) -o $@ $^\n\n.PHONY: all clean";
    static constexpr std::string_view kMakeCond = "ifeq ($(OS),Windows_NT)\nX = 1\nelse\nX = 2\nendif";
    static constexpr std::string_view kZigString = "const s =\n    \\\\hello \"x\"\n    \\\\world\n;";
    static constexpr std::string_view kHaskellImport = "module Main where\nimport qualified Data.Map as M";
    static constexpr std::string_view kCommonLisp = "(defun square (x)\n  \"Square X.\"\n  (* x x))\n(let ((a 1) (b 2))\n  (+ a b))";
    static constexpr std::string_view kLispReader = "#'car #\\Space :key &optional #xFF #|c|#";
    static constexpr std::string_view kLispCond = "(cond ((> x 0) 'pos) (t 'neg))";
    static constexpr std::string_view kElispDefun = "(defun my-cmd (arg)\n  (interactive \"p\")\n  (message \"%d\" arg))";
    static constexpr std::string_view kScheme = "#t #f #;(ignored form) ; c\n#| block |#";
    static constexpr std::string_view kRacket = "#lang racket\n(λ (x) x)";
    static constexpr std::string_view kClojure =
        "(ns app.core\n  (:require [clojure.string :as str]))\n\n(defn greet\n  \"Say hi.\"\n  [name]\n  (str \"Hello, \" name \\!))";
    static constexpr std::string_view kClojureDiscard = "#_(println \"x\") (inc 1)";
    static constexpr std::string_view kVimrc =
        "\" settings\nset number \" show numbers\nlet g:mapleader = \",\"\nif has('nvim')\n  echo \"neovim\"\nendif\n"
        "nnoremap <silent> <leader>w :w<CR>";
    static constexpr std::string_view kVimFunction = "function! s:Greet(name) abort\n  return 'hi ' . a:name\nendfunction";
    static constexpr std::string_view kVimLuaHeredoc = "lua << EOF\nlocal x = require('m')\nEOF\nset nu";
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
        // 単語の途中の # はコメントではない
        { "BashHashInsideWord", "echo ${#arr[@]} $# http://x/#frag", Bash, Comment, 0 },
        { "BashHashAfterSemicolon", "a=1;# c\n(#c2\n#c3", Bash, Comment, 3 },
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
        { "JavaKeywords", "public class Main extends Base implements Runnable", Java, Keyword, 4 },
        { "JavaTypes", "int long double boolean String List", Java, Type, 6 },
        { "CSharpKeywords", "namespace App; public record Point(int X);", CSharp, Keyword, 3 },
        { "CSharpTypes", "string int bool decimal Task", CSharp, Type, 5 },
        { "KotlinKeywords", "fun val var when data class", Kotlin, Keyword, 6 },
        { "SwiftKeywords", "func let var guard struct", Swift, Keyword, 5 },
        // Swift の ' は文字列の区切りではない
        { "SwiftSingleQuoteNotString", "let c = 'x'", Swift, String, 0 },
        { "PhpKeywordsCaseInsensitive", "FUNCTION Echo foreach", Php, Keyword, 3 },
        // PHP 8 の属性 #[...] はコメントではない
        { "PhpAttributeIsNotComment", "#[Route('/')]\nfunction a() {}", Php, Comment, 0 },
        { "PhpComments", "$x = 1; # c\n// d\n/* e */", Php, Comment, 3 },
        { "RubyKeywords", "def end class module unless", Ruby, Keyword, 5 },
        { "RubyBuiltins", "puts require attr_reader", Ruby, Type, 3 },
        // 行頭でない =begin はコメントではない
        { "RubyBeginNotAtLineStart", "x =begin", Ruby, Comment, 0 },
        { "SqlKeywordsCaseInsensitive", "SELECT name FROM users WHERE id IN (1)", Sql, Keyword, 4 },
        { "SqlTypes", "CREATE TABLE t (id INT, name VARCHAR(10))", Sql, Type, 2 },
        { "SqlComments", "SELECT 1; -- a\n/* b */", Sql, Comment, 2 },
        { "SqlMinusIsNotComment", "SELECT a - b", Sql, Comment, 0 },
        // `order` は識別子の引用なのでキーワードにしない
        { "SqlBacktickIdentifier", "SELECT `order` FROM t", Sql, Keyword, 2 },
        { "YamlKeys", "- name: a\n  run: echo\n  with:\n    key: v", Yaml, Type, 4 },
        // 値はスカラー全体で判定し、一部だけ数値に見えても色を付けない
        { "YamlPlainScalarIsNotSplit", "image: node:18-alpine\nversion: 1.2.3", Yaml, Number, 0 },
        { "YamlNumbers", "a: 3.8\nb: -1\nc: 0x1F\nd: 1e3\ne: 2024-01-01", Yaml, Number, 5 },
        { "YamlConstants", "a: true\nb: False\nc: null\nd: ~\ne: yes", Yaml, Keyword, 5 },
        { "YamlHashInsideValueIsNotComment", "url: http://x/#frag\ncolor: a#b", Yaml, Comment, 0 },
        { "YamlComments", "# top\nkey: v # trailing", Yaml, Comment, 2 },
        // ブロックスカラーの本文は "key: value" の形でもキーにしない
        { "YamlBlockScalar/String", kYamlBlock, Yaml, String, 2 },
        { "YamlBlockScalar/Type", kYamlBlock, Yaml, Type, 2 },
        { "YamlFlowCollection/Type", kYamlFlow, Yaml, Type, 2 },
        { "YamlFlowCollection/Number", kYamlFlow, Yaml, Number, 2 },
        { "YamlAnchorsAndTags", "base: &b\n  x: 1\nref: *b\ntag: !!str 1", Yaml, Preprocessor, 3 },
        { "YamlDocumentMarkers", "---\na: 1\n...", Yaml, Keyword, 2 },
        { "Toml/Keyword", kToml, Toml, Keyword, 2 },
        { "Toml/Type", kToml, Toml, Type, 3 },
        { "Toml/String", kToml, Toml, String, 3 },
        { "TomlNumbers", "a = 1_000\nb = 3.14\nc = 1979-05-27T07:32:00Z\nd = inf", Toml, Number, 4 },
        { "TomlBooleans", "a = true\nb = false", Toml, Keyword, 2 },
        // 配列の中の行頭 [ はテーブルヘッダではない
        { "TomlNestedArrayIsNotHeader", "arr = [\n  [1, 2],\n]", Toml, Keyword, 0 },
        { "TomlInlineTableKeys", "t = { a = 1, b.c = \"x\" }", Toml, Type, 3 },
        // 複数行文字列の中身の行はキーにしない
        { "TomlMultilineString", "desc = \"\"\"\nmulti = line\n\"\"\"\nnext = 1", Toml, Type, 2 },
        { "TomlComment", "a = 1 # c", Toml, Comment, 1 },
        { "IniSections", "[core]\nkey = v\n[remote \"origin\"]", Ini, Keyword, 2 },
        { "Ini/Comment", kIni, Ini, Comment, 2 },
        { "Ini/Type", kIni, Ini, Type, 2 },
        { "IniValues/Number", kIniValues, Ini, Number, 1 },
        { "IniValues/Keyword", kIniValues, Ini, Keyword, 1 },
        { "IniValues/String", kIniValues, Ini, String, 1 },
        { "Html/Keyword", kHtml, Html, Keyword, 2 },
        { "Html/Type", kHtml, Html, Type, 2 },
        { "HtmlEntities", "a &amp; b &#169; &x c", Html, Number, 2 },
        { "HtmlLessThanInText", "a < b && c", Html, Keyword, 0 },
        // <script> / <style> の中身は JS / CSS として色付けする
        { "HtmlScriptIsJs", "<script>const x = 1;</script>", Html, Keyword, 3 },
        { "HtmlStyleIsCss", "<style>a { color: red; }</style>", Html, Type, 1 },
        { "XmlScriptIsNotJs", "<script>const x</script>", Xml, Keyword, 2 },
        { "HtmlUnquotedAttributeValue", "<input type=text disabled>", Html, String, 1 },
        { "Css/Function", kCss, Css, Function, 2 },
        { "Css/Type", kCss, Css, Type, 3 },
        { "Css/Number", kCss, Css, Number, 3 },
        { "Css/Keyword", kCss, Css, Keyword, 1 },
        { "CssMedia/Keyword", kCssMedia, Css, Keyword, 3 },
        { "CssMedia/Type", kCssMedia, Css, Type, 2 },
        // url() の引数の // はコメントではない
        { "CssUrlIsNotComment", "a { background: url(http://x/y.png); }", Css, Comment, 0 },
        { "CssFunctions", "a { color: rgb(0 0 0 / 50%); width: calc(100% - 2px); }", Css, Function, 2 },
        { "CssDoubleSlashIsNotComment", "a { b: c//d; }", Css, Comment, 0 },
        { "Scss/Type", kScss, Scss, Type, 5 },
        { "Scss/Comment", kScss, Scss, Comment, 1 },
        { "Diff/Keyword", kDiff, Diff, Keyword, 4 },
        { "Diff/Type", kDiff, Diff, Type, 1 },
        { "Diff/Inserted", kDiff, Diff, Inserted, 1 },
        { "Diff/Deleted", kDiff, Diff, Deleted, 1 },
        { "Diff/Comment", kDiff, Diff, Comment, 1 },
        { "Dockerfile/Keyword", kDocker, Dockerfile, Keyword, 5 },
        { "Dockerfile/Comment", kDocker, Dockerfile, Comment, 1 },
        { "DockerfileLowercaseInstruction", "run echo hi", Dockerfile, Keyword, 1 },
        // 継続行の先頭の単語は命令ではない
        { "DockerfileContinuationIsNotInstruction", "RUN apt-get update \\\n    && apt-get install -y curl", Dockerfile, Keyword, 1 },
        { "DockerfileShellString", "RUN echo \"hi\"", Dockerfile, String, 1 },
        { "Makefile/Type", kMake, Makefile, Type, 6 },
        { "Makefile/Function", kMake, Makefile, Function, 1 },
        { "Makefile/Keyword", kMake, Makefile, Keyword, 1 },
        { "Makefile/Comment", kMake, Makefile, Comment, 1 },
        { "MakefileConditional/Keyword", kMakeCond, Makefile, Keyword, 3 },
        { "MakefileConditional/Type", kMakeCond, Makefile, Type, 3 },
        // $$ はシェル変数のエスケープで make の変数参照ではない (Type は echo のみ)
        { "MakefileDollarEscape", "t:\n\techo $$HOME", Makefile, Type, 1 },
        { "MakefileRecipeString", "t:\n\techo \"hi $(NAME)\"", Makefile, String, 2 },
        { "MakefileExport", "export PATH := /bin", Makefile, Type, 1 },
        // md4c はコードブロック行頭のタブをスペースに展開するので、ルール直後のインデント行をレシピとみなす
        { "MakefileSpaceIndentedRecipe", "all:\n    echo \"hi\"", Makefile, String, 1 },
        { "MakefileIndentedLineOutsideRule", "ifeq (a,b)\n  X = 1\nendif", Makefile, Type, 1 },
        // 代入行でルールが終わり、その後のインデント行は代入として読む (Type は echo, Y, Z)
        { "MakefileRuleEndsAtAssignment", "t:\n  echo x\nY = 2\n  Z = 3", Makefile, Type, 3 },
        { "RubyHashInRegexIsNotComment", "line[/\\A#+/].size", Ruby, Comment, 0 },
        { "ScssParentSuffix", ".a { &--primary { b: c; } }", Scss, Function, 2 },
        { "ZigKeywords", "pub fn main() !void { const x = try foo(); }", Zig, Keyword, 4 },
        { "ZigTypes", "u8 i32 usize bool comptime_int", Zig, Type, 5 },
        { "ZigBuiltins", "const std = @import(\"std\");\nconst n = @intCast(x);", Zig, Function, 2 },
        // \\ の行文字列の中の " は新しい文字列を始めない
        { "ZigMultilineString", kZigString, Zig, String, 2 },
        { "HaskellImport/Keyword", kHaskellImport, Haskell, Keyword, 5 },
        { "HaskellImport/Type", kHaskellImport, Haskell, Type, 4 },
        // ' は識別子の一部 (foldl') にもなる
        { "HaskellPrimeIdentifier", "go acc' xs = foldl' f acc' xs", Haskell, String, 0 },
        { "HaskellCharLiterals", "c = 'a'\nd = '\\n'\ne = '\\''", Haskell, String, 3 },
        { "HaskellPromotedTick", "type T = 'True", Haskell, String, 0 },
        { "HaskellOperatorIsNotComment", "x --> y\nz |-- w", Haskell, Comment, 0 },
        { "HaskellComments", "-- line\n{- block {- nested -} still -}\nx = 1", Haskell, Comment, 2 },
        // 行頭の小文字始まりの識別子は定義名 (インデントされた helper は呼び出し)
        { "HaskellDefinitions", "main :: IO ()\nmain = do\n  helper 1\nhelper x = x", Haskell, Function, 3 },
        { "CommonLisp/Keyword", kCommonLisp, CommonLisp, Keyword, 2 },
        // 定義名と呼び出し位置だけが Function。引数リスト (x) と let の束縛 (a 1) は呼び出しではない
        { "CommonLisp/Function", kCommonLisp, CommonLisp, Function, 3 },
        { "CommonLisp/Number", kCommonLisp, CommonLisp, Number, 2 },
        { "CommonLispCaseInsensitive", "(DEFUN foo () NIL)", CommonLisp, Keyword, 2 },
        { "LispReader/Function", kLispReader, CommonLisp, Function, 1 },
        { "LispReader/String", kLispReader, CommonLisp, String, 1 },
        { "LispReader/Type", kLispReader, CommonLisp, Type, 2 },
        { "LispReader/Number", kLispReader, CommonLisp, Number, 1 },
        { "LispReader/Comment", kLispReader, CommonLisp, Comment, 1 },
        // 記号を含むシンボルは 1 語 (read-line の read を別の語にしない)
        { "LispSymbolWithDashes", "(string-upcase (read-line s))", CommonLisp, Function, 2 },
        { "LispOnePlusIsFunction", "(1+ x)", CommonLisp, Function, 1 },
        // 引数リスト内のデフォルト値付き引数は呼び出しではない (Function は定義名 f と本体の g)
        { "LispParameterDefaults", "(defun f (a &key (b 3)) (g a b))", CommonLisp, Function, 2 },
        { "LispQuotedListIsNotCall", "'(a b c)", CommonLisp, Function, 0 },
        { "LispCondClauses/Function", kLispCond, CommonLisp, Function, 1 },
        { "LispCondClauses/Keyword", kLispCond, CommonLisp, Keyword, 2 },
        { "EmacsLispCharLiterals", "(insert ?a ?\\n ?\\C-x)", EmacsLisp, String, 3 },
        { "EmacsLispDefun/Function", kElispDefun, EmacsLisp, Function, 2 },
        { "EmacsLispDefun/Keyword", kElispDefun, EmacsLisp, Keyword, 2 },
        { "Scheme/Keyword", kScheme, Scheme, Keyword, 2 },
        { "Scheme/Comment", kScheme, Scheme, Comment, 3 },
        { "SchemeDefine", "(define (square x) (* x x))", Scheme, Function, 2 },
        { "Racket/Preprocessor", kRacket, Scheme, Preprocessor, 1 },
        { "Racket/Keyword", kRacket, Scheme, Keyword, 1 },
        { "RacketKeywordArgument", "(f #:size 10)", Scheme, Type, 1 },
        { "RacketBracketElse", "(cond [(zero? n) 1] [else n])", Scheme, Keyword, 2 },
        { "Clojure/Keyword", kClojure, Clojure, Keyword, 2 },
        { "Clojure/Function", kClojure, Clojure, Function, 2 },
        { "Clojure/Type", kClojure, Clojure, Type, 2 },
        { "Clojure/String", kClojure, Clojure, String, 3 },
        { "ClojureDiscard/Comment", kClojureDiscard, Clojure, Comment, 1 },
        { "ClojureDiscard/Function", kClojureDiscard, Clojure, Function, 1 },
        { "ClojureAnonFn", "#(+ % 1)", Clojure, Function, 1 },
        { "ClojureRegex", "#\"\\d+\" #{1 2}", Clojure, String, 1 },
        // Clojure の cond は節を持たないので (pos? x) は呼び出し
        { "ClojureCondIsNotClauses", "(cond (pos? x) :a :else :b)", Clojure, Function, 1 },
        { "ClojureLetVector", "(let [x 1] (f x))", Clojure, Function, 1 },
        { "LuaKeywords", "local function f(x) if x then return nil end end", Lua, Keyword, 8 },
        { "LuaBuiltins", "print(pairs(t)) require('x') vim.api", Lua, Type, 4 },
        { "LuaLongComment", "--[[ multi\nline ]] x = 1", Lua, Comment, 1 },
        // [[ でなければ添字なので長文字列ではない
        { "LuaIndexIsNotLongString", "a[b[1]] = 2", Lua, String, 0 },
        { "LuaMethodCall", "obj:method(1)", Lua, Function, 1 },
        { "Vimrc/Comment", kVimrc, Vim, Comment, 2 },
        { "Vimrc/Keyword", kVimrc, Vim, Keyword, 6 },
        { "Vimrc/Type", kVimrc, Vim, Type, 1 },
        { "Vimrc/String", kVimrc, Vim, String, 3 },
        { "Vimrc/Function", kVimrc, Vim, Function, 1 },
        { "Vimrc/Preprocessor", kVimrc, Vim, Preprocessor, 3 },
        { "VimFunction/Function", kVimFunction, Vim, Function, 1 },
        { "VimFunction/Keyword", kVimFunction, Vim, Keyword, 4 },
        { "VimFunction/Type", kVimFunction, Vim, Type, 1 },
        { "VimAbbreviatedCommands", "fu! F()\nendf\nau BufRead *.md setl spell", Vim, Keyword, 4 },
        // autocmd のイベントとパターンの後はコマンド位置
        { "VimAutocmdCommand", "autocmd FileType markdown setlocal spell", Vim, Keyword, 2 },
        { "VimBarSeparatesCommands", "if x | echo 1 | endif", Vim, Keyword, 3 },
        { "VimOrIsNotSeparator", "let x = a || b", Vim, Keyword, 1 },
        { "VimOptionEnvRegister", "let &tabstop = 4\necho $HOME @a", Vim, Type, 3 },
        { "VimUserCommand", "Plug 'tpope/vim-surround'", Vim, Function, 1 },
        { "VimAutoloadFunction", "call foo#bar#baz(1)", Vim, Function, 1 },
        // vim9script で始まるとコメントは #、" は常に文字列
        { "Vim9Comment", "vim9script\nvar x = 1 # comment\n# whole line", Vim, Comment, 2 },
        { "Vim9StringIsNotComment", "vim9script\necho \"a", Vim, Comment, 0 },
        { "Vim9Language", "def F(x: number): string\n  return 'a' # c\nenddef", Vim9, Comment, 1 },
        { "VimLuaHeredoc/Keyword", kVimLuaHeredoc, Vim, Keyword, 3 },
        { "VimLuaHeredoc/Type", kVimLuaHeredoc, Vim, Type, 1 },
        { "VimLuaInline", "lua require('x').setup()", Vim, Function, 1 },
        // let のヒアドキュメント本文は文字列で、" もコメントではない
        { "VimLetHeredoc/String", "let lines =<< trim END\n  a \" b\n  END\necho lines", Vim, String, 1 },
        { "VimLetHeredoc/Comment", "let lines =<< trim END\n  a \" b\n  END\necho lines", Vim, Comment, 0 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensWellFormed(tokens, c.code.size());
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
        { "PythonNumberUnderscore", "x = 1_000_000", Python, Number, "1_000_000" },
        { "JavaTextBlock", "String s = \"\"\"\nhello\n\"\"\";", Java, String, "\"\"\"\nhello\n\"\"\"" },
        { "JavaLongWithUnderscore", "long n = 1_000_000L;", Java, Number, "1_000_000L" },
        // 逐語的文字列では \ はエスケープではない
        { "CSharpVerbatimString", "var p = @\"C:\\dir\\\";", CSharp, String, "@\"C:\\dir\\\"" },
        { "CSharpVerbatimDoubledQuote", "s = @\"say \"\"hi\"\"\";", CSharp, String, "@\"say \"\"hi\"\"\"" },
        { "CSharpInterpolatedVerbatim", "s = @$\"{x}\\\";", CSharp, String, "@$\"{x}\\\"" },
        { "CSharpPreprocessor", "#region Foo\nint x;", CSharp, Preprocessor, "#region Foo" },
        { "CSharpDecimal", "decimal d = 1.5m;", CSharp, Number, "1.5m" },
        { "KotlinRawString", "val s = \"\"\"raw\"\"\"", Kotlin, String, "\"\"\"raw\"\"\"" },
        { "KotlinFunction", "fun main() {}", Kotlin, Function, "main" },
        { "SwiftDirective", "#if DEBUG\nprint(1)\n#endif", Swift, Preprocessor, "#if DEBUG" },
        { "PhpHashComment", "$x = 1; # note", Php, Comment, "# note" },
        { "RubyBlockComment", "=begin\nblock\n=end\nputs 1", Ruby, Comment, "=begin\nblock\n=end" },
        { "SqlLineComment", "SELECT 1 -- note\nFROM t", Sql, Comment, "-- note" },
        { "YamlKey", "name: hello world", Yaml, Type, "name" },
        { "YamlQuotedKey", "\"quoted key\": 1", Yaml, Type, "\"quoted key\"" },
        { "YamlSingleQuotedString", "s: 'it''s'", Yaml, String, "'it''s'" },
        { "YamlTrailingComment", "key: v # c", Yaml, Comment, "# c" },
        { "TomlArrayOfTablesHeader", "[[bin]]\nname = \"x\"", Toml, Keyword, "[[bin]]" },
        { "TomlDottedKey", "a.b = 1", Toml, Type, "a.b" },
        { "TomlDateTime", "t = 1979-05-27T07:32:00Z", Toml, Number, "1979-05-27T07:32:00Z" },
        { "IniSection", "[section]\nkey = value", Ini, Keyword, "[section]" },
        { "IniKey", "[s]\nkey = value", Ini, Type, "key" },
        { "HtmlTagName", "<div class=\"a\">", Html, Keyword, "div" },
        { "HtmlAttributeValue", "<div class=\"a\">", Html, String, "\"a\"" },
        { "HtmlEntity", "a &amp; b", Html, Number, "&amp;" },
        { "HtmlScriptNumber", "<script>let x = 42;</script>", Html, Number, "42" },
        { "XmlDeclaration", "<?xml version=\"1.0\"?>\n<root/>", Xml, Preprocessor, "<?xml version=\"1.0\"?>" },
        { "CssClassSelector", ".btn { color: red; }", Css, Function, ".btn" },
        { "CssPseudoElement", "a::before { content: \"x\"; }", Css, Type, "::before" },
        { "CssHexColor", "a { color: #ff0000; }", Css, Number, "#ff0000" },
        { "CssUnit", "a { width: 1.5em; }", Css, Number, "1.5em" },
        { "CssUrl", "a { background: url(http://x/y.png); }", Css, String, "http://x/y.png" },
        { "CssAtRule", "@import \"x.css\";", Css, Keyword, "@import" },
        { "ScssLineComment", ".a { b: c; } // note", Scss, Comment, "// note" },
        { "ScssVariable", "$primary: #333;", Scss, Type, "$primary" },
        { "DiffHunk", "@@ -1,2 +1,2 @@\n-a", Diff, Type, "@@ -1,2 +1,2 @@" },
        { "DiffInserted", " ctx\n+added\n-removed", Diff, Inserted, "+added" },
        { "DiffDeleted", " ctx\n+added\n-removed", Diff, Deleted, "-removed" },
        { "DockerfileParserDirective", "# syntax=docker/dockerfile:1\nFROM a", Dockerfile, Comment, "# syntax=docker/dockerfile:1" },
        { "MakefileTarget", "build: main.o\n\tcc main.o", Makefile, Function, "build" },
        { "MakefileAutomaticVariable", "t:\n\tcc -o $@", Makefile, Type, "$@" },
        { "ZigLineString", "const s =\n    \\\\hello \"x\"\n;", Zig, String, "\\\\hello \"x\"" },
        { "ZigBuiltin", "@import(\"std\")", Zig, Function, "@import" },
        { "ZigCharLiteral", "const c = 'a';", Zig, String, "'a'" },
        { "HaskellPragma", "{-# LANGUAGE GADTs #-}\nmodule M where", Haskell, Preprocessor, "{-# LANGUAGE GADTs #-}" },
        { "HaskellNestedComment", "{- a {- b -} c -} x", Haskell, Comment, "{- a {- b -} c -}" },
        { "HaskellHaddock", "-- | doc\nf = 1", Haskell, Comment, "-- | doc" },
        { "HaskellEscapedQuoteChar", "q = '\\''", Haskell, String, "'\\''" },
        { "LispDefinitionName", "(defun square (x) x)", CommonLisp, Function, "square" },
        { "LispNamedCharacter", "(list #\\Space x)", CommonLisp, String, "#\\Space" },
        { "ClojureKeyword", "{:name \"x\"}", Clojure, Type, ":name" },
        { "ClojureNamedChar", "(str \\newline)", Clojure, String, "\\newline" },
        { "LuaLongCommentText", "--[[ multi\nline ]] x = 1", Lua, Comment, "--[[ multi\nline ]]" },
        { "LuaLevelLongString", "s = [==[ a ]] b ]==]", Lua, String, "[==[ a ]] b ]==]" },
        { "LuaLineComment", "x = 1 -- c", Lua, Comment, "-- c" },
        // --[ の後に [ が続かなければ通常の行コメント
        { "LuaBracketLineComment", "--[ note\nx = 1", Lua, Comment, "--[ note" },
        { "VimLineComment", "\" comment\nset nu", Vim, Comment, "\" comment" },
        { "VimTrailingComment", "set nu \" show", Vim, Comment, "\" show" },
        { "VimKeyNotation", "nnoremap <C-]> :tag<CR>", Vim, Preprocessor, "<C-]>" },
        { "VimScopedVariable", "let s:count = 0", Vim, Type, "s:count" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensWellFormed(tokens, c.code.size());
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
        AssertTokensWellFormed(tokens, c.code.size());
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
        { "CSharpVerbatimMultiLine", "@\"line1\nline2\"", CSharp, String },
        { "RubyBlockCommentUnterminated", "=begin\nnever closed", Ruby, Comment },
        { "SqlBlockComment", "/* a\nb */", Sql, Comment },
        { "HtmlComment", "<!-- a\nb -->", Html, Comment },
        { "HtmlCommentUnterminated", "<!-- never closed", Html, Comment },
        { "HtmlDoctype", "<!DOCTYPE html>", Html, Preprocessor },
        { "XmlCData", "<![CDATA[a < b]]>", Xml, String },
        { "CssComment", "/* c */", Css, Comment },
        { "DiffInsertedLine", "+added line", Diff, Inserted },
        { "DiffNoNewlineMarker", "\\ No newline at end of file", Diff, Comment },
        { "YamlComment", "# comment", Yaml, Comment },
        { "IniComment", "; comment", Ini, Comment },
        { "DockerfileComment", "# comment", Dockerfile, Comment },
        { "MakefileComment", "# comment", Makefile, Comment },
        { "ZigLineStringOnly", "\\\\raw text", Zig, String },
        { "HaskellBlockCommentUnterminated", "{- never closed", Haskell, Comment },
        { "LispNestedBlockComment", "#| a #| b |# c |#", CommonLisp, Comment },
        { "SchemeDatumComment", "#;(a (b) \"c)\")", Scheme, Comment },
        { "ClojureNamedCharOnly", "\\newline", Clojure, String },
        { "LuaLevelLongComment", "--[==[ x ]] y ]==]", Lua, Comment },
        { "LuaLongStringUnterminated", "[[never closed", Lua, String },
        { "VimCommentOnly", "\" just a comment", Vim, Comment },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.name);
        const auto tokens = Tokenize(c.code, c.lang);
        AssertTokensWellFormed(tokens, c.code.size());
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
    AssertTokensWellFormed(tokens, code.size());
    for (const auto& t : tokens) {
        if (t.type != SyntaxTokenType::Plain) {
            EXPECT_EQ(t.type, SyntaxTokenType::Keyword) << "offset=" << t.start;
        }
    }
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::Keyword), 5);
}

TEST(Syntax, PlainOnlyTextHasNoTokens)
{
    for (const std::string_view code : { "   \n\t  \n  ", "foo bar_baz + - * /" }) {
        SCOPED_TRACE(code);
        EXPECT_TRUE(Tokenize(code, SyntaxLanguage::Cpp).empty());
    }
}

// バグ #16: 生文字列 R" の検出が R で終わる識別子 (例: RENDER"hello") でトリガーされないこと
TEST(Syntax, CppRawStringNotTriggeredByIdentifierEndingR)
{
    std::string code = "RENDER\"hello\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensWellFormed(tokens, code.size());

    // RENDER は Plain の識別子なので String に取り込まれず、"hello" だけが String になる
    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 1);
    const auto* str = FindTokenByText(code, tokens, "\"hello\"");
    EXPECT_NE(str, nullptr) << "\"hello\"が文字列トークンとして見つかるべき";
    if (str != nullptr) {
        EXPECT_EQ(str->type, SyntaxTokenType::String);
    }
}

// 区切りが規格 (16 文字以内、空白・改行なし) を満たさない R" は通常の文字列として読み、
// 後続行の '(' まで区切りとして飲み込まない。
TEST(Syntax, CppRawStringDelimiterFollowsStandardLimits)
{
    {
        const std::string_view code = "R\"x\nint y = f(1);";
        const auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
        AssertTokensWellFormed(tokens, code.size());
        const auto* str = FindToken(tokens, SyntaxTokenType::String);
        ASSERT_NE(str, nullptr);
        EXPECT_EQ(GetTokenText(code, *str), "R\"x");
        EXPECT_NE(FindToken(tokens, SyntaxTokenType::Type), nullptr);
        EXPECT_NE(FindToken(tokens, SyntaxTokenType::Function), nullptr);
    }
    {
        const std::string_view code = "R\"0123456789abcdef(a\"b)0123456789abcdef\"";
        const auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(GetTokenText(code, tokens[0]), code) << "16 文字の区切りは生文字列";
    }
    {
        const std::string_view code = "R\"0123456789abcdefg(a\"b)0123456789abcdefg\"";
        const auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
        AssertTokensWellFormed(tokens, code.size());
        const auto* str = FindToken(tokens, SyntaxTokenType::String);
        ASSERT_NE(str, nullptr);
        EXPECT_EQ(GetTokenText(code, *str), "R\"0123456789abcdefg(a\"") << "17 文字の区切りは生文字列ではない";
    }
}

TEST(Syntax, CppRawStringNotTriggeredWhenRIsPartOfLongerIdentifier)
{
    // 識別子が "R" 単独でない ("xR") ため生文字列扱いされず、"(a)" は通常の文字列として解釈される
    std::string code = "xR\"(a)\"";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_EQ(CountTokens(tokens, SyntaxTokenType::String), 1);
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
    AssertTokensWellFormed(tokens, code.size());
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
    AssertTokensWellFormed(tokens, code.size());
    // 'aは行の残りを飲み込む文字列トークンを生成してはならない
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 1); // fn
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 1);    // str
}

TEST(Syntax, JsonNestedStructure)
{
    std::string code = "{\"items\": [{\"id\": 1}, {\"id\": 2}], \"count\": 2}";
    auto tokens = Tokenize(code, SyntaxLanguage::Json);
    AssertTokensWellFormed(tokens, code.size());
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 3);
}

// ============================================================
// 複合コード
// ============================================================

TEST(Syntax, CppComplexCode)
{
    std::string code = "#include <iostream>\n\nint main() {\n    // Hello\n    std::cout << \"Hello\" << 42;\n    return 0;\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Cpp);
    AssertTokensWellFormed(tokens, code.size());

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
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 2);   // int, float
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Number), 2); // 10, 3.14f
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2); // if, return
}

TEST(Syntax, PythonComplexCode)
{
    std::string code = "def greet(name: str) -> str:\n    # Greeting\n    return f\"Hello, {name}!\"\n\nprint(greet(\"World\"))";
    auto tokens = Tokenize(code, SyntaxLanguage::Python);
    AssertTokensWellFormed(tokens, code.size());

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
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 4);  // async, function, const, await, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // // Fetch data
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 2); // fetchData, fetch
}

TEST(Syntax, TsComplexCode)
{
    std::string code = "interface User {\n  name: string;\n  age: number;\n}\n\nconst greet = (user: User): string => {\n  return `Hello, ${user.name}`;\n};";
    auto tokens = Tokenize(code, SyntaxLanguage::TypeScript);
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // interface, const, return
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Type), 3);    // string, number, string
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);  // template literal
}

TEST(Syntax, GoComplexCode)
{
    std::string code = "package main\n\nimport \"fmt\"\n\nfunc main() {\n    // Hello\n    fmt.Println(\"Hello\")\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Go);
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3);  // package, import, func
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);  // // Hello
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 2);   // "fmt", "Hello"
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Function), 1); // main
}

TEST(Syntax, RustComplexCode)
{
    std::string code = "use std::io;\n\nfn main() -> Result<(), Box<dyn std::error::Error>> {\n    let x: i32 = 42;\n    // comment\n    println!(\"Hello {}\", x);\n    Ok(())\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::Rust);
    AssertTokensWellFormed(tokens, code.size());

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
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1);
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // for, in, do, done
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::String), 1);
}

TEST(Syntax, PwshComplexCode)
{
    std::string code = "<# Script #>\nfunction Get-Item {\n    param([string]$Path)\n    # Do work\n    return $Path\n}";
    auto tokens = Tokenize(code, SyntaxLanguage::PowerShell);
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 2); // <# #> and # comment
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 2); // function, param, return
}

TEST(Syntax, CmdComplexCode)
{
    std::string code = "@echo off\nREM Build script\nfor %%f in (*.cpp) do (\n    echo Building %%f\n)\npause";
    auto tokens = Tokenize(code, SyntaxLanguage::Cmd);
    AssertTokensWellFormed(tokens, code.size());

    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Comment), 1); // REM
    EXPECT_GE(CountTokens(tokens, SyntaxTokenType::Keyword), 3); // echo, for, do, echo, pause
}

// ============================================================
// ランダム入力でのプロパティ
// ============================================================

namespace {

// 字句状態が切り替わる断片に寄せる。未終端の文字列・コメント・raw string・triple quote も生成される。
constexpr std::string_view kSyntaxFuzzPieces[] = {
    "\"", "'", "`", "/", "*", "#", "<", ">", "R", "\\", "(", ")", ":", ".",
    "\n", "\r\n", " ", "\t",
    "//", "/*", "*/", "<#", "#>", "::", "rem ", "REM", "\"\"\"", "'''", "R\"x(", ")x\"",
    "0x1F", "1.5e+3f", "42", "int", "def", "foo", "if", "あ", "漢字", "𠮷",
    "--", "-", "+", "@", "@\"", "$", "$(", "${", "&", "&amp;", "=", ";", ",", "|", "!", "%", "~",
    "[", "]", "{", "}", "#{", "---", "@@", "=begin", "\n=end", "\\\n", "<!--", "-->", "<?", "<!",
    "<script>", "</script>", "<style>", "</style>", "url(", "FROM", "RUN", "key: ", "a = ",
    "{-", "-}", "{-#", "#-}", "#|", "|#", "#;", "#_", "#\\", "#'", "#:", "#t", "#lang", "?", "?\\", "\\\\",
    "'a'", "'\\'", "λ", "(defun ", "(let ((", "(cond ", ":k", "&rest", "@import",
    "--[[", "]]", "[==[", "]==]", "<<", "=<< trim ", "EOF", "lua ", "au BufRead * ", "nnoremap ", "<CR>", "g:",
    "function! ", "vim9script\n",
};

// 境界は前方 decode の区切りで判定する。描画側 (Utf16OffsetCursor) は文字途中の境界を
// 文字先頭に丸めるため、区切り以外に境界があると色付け範囲が 1 文字ずれる。
testing::AssertionResult TokensAlignToCpBoundaries(std::string_view text, const std::pmr::vector<SyntaxToken>& tokens)
{
    if (auto r = TokensWellFormed(tokens, text.size()); !r) {
        return r;
    }
    const auto bounds = utf8_fuzz::ForwardDecodeBoundaries(text);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        const uint64_t end = static_cast<uint64_t>(t.start) + t.length;
        if (!std::ranges::binary_search(bounds, t.start) || !std::ranges::binary_search(bounds, static_cast<uint32_t>(end))) {
            return testing::AssertionFailure() << "token " << i << " [" << t.start << ", " << end << ") splits a code point";
        }
    }
    return testing::AssertionSuccess();
}

} // namespace

TEST(Syntax, FuzzTokensAlignToCodePointBoundaries)
{
    constexpr SyntaxLanguage kLanguages[] = {
        SyntaxLanguage::Cpp, SyntaxLanguage::Python, SyntaxLanguage::JavaScript, SyntaxLanguage::Go,
        SyntaxLanguage::Rust, SyntaxLanguage::TypeScript, SyntaxLanguage::Bash, SyntaxLanguage::PowerShell,
        SyntaxLanguage::Cmd, SyntaxLanguage::Json, SyntaxLanguage::Java, SyntaxLanguage::CSharp,
        SyntaxLanguage::Kotlin, SyntaxLanguage::Swift, SyntaxLanguage::Php, SyntaxLanguage::Ruby, SyntaxLanguage::Sql,
        SyntaxLanguage::Yaml, SyntaxLanguage::Toml, SyntaxLanguage::Ini, SyntaxLanguage::Html, SyntaxLanguage::Xml,
        SyntaxLanguage::Css, SyntaxLanguage::Scss, SyntaxLanguage::Diff, SyntaxLanguage::Dockerfile,
        SyntaxLanguage::Makefile, SyntaxLanguage::Zig, SyntaxLanguage::Haskell, SyntaxLanguage::Clojure,
        SyntaxLanguage::EmacsLisp, SyntaxLanguage::CommonLisp, SyntaxLanguage::Scheme, SyntaxLanguage::Lua,
        SyntaxLanguage::Vim, SyntaxLanguage::Vim9,
    };
    for (const uint32_t seed : utf8_fuzz::kFuzzSeeds) {
        std::mt19937 rng{ seed };
        for (int iter = 0; iter < 300; ++iter) {
            const auto text = utf8_fuzz::RandomPiecesWithMalformed(rng, kSyntaxFuzzPieces, 1, 24);
            for (const auto lang : kLanguages) {
                // 失敗時だけ評価されるメッセージに文脈を載せ、成功ケースで文字列を組み立てない。
                ASSERT_TRUE(TokensAlignToCpBoundaries(text, Tokenize(text, lang)))
                    << std::format("seed={} iter={} lang={} text={}", seed, iter, static_cast<int>(lang), utf8_fuzz::HexEscape(text));
            }
        }
    }
}
