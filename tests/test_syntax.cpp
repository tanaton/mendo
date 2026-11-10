#include <gtest/gtest.h>
#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "syntax.h"
#include "parser.h"
#include "test_helpers.h"
#include "utf8_fuzz_helpers.h"
#include "test_nodes.h"

namespace {

// 全言語を回すテストの上限。言語を足したら直す。
constexpr int kLastLanguage = static_cast<int>(SyntaxLanguage::Vim9);

// 描画と HTML 化はトークンを start 昇順・非重複として隙間を Plain で埋める。Plain 自体は出力しない。
testing::AssertionResult TokensWellFormed(const std::vector<SyntaxToken>& tokens, size_t text_length)
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

void AssertTokensWellFormed(const std::vector<SyntaxToken>& tokens, size_t text_length)
{
    EXPECT_TRUE(TokensWellFormed(tokens, text_length));
}

const SyntaxToken* FindToken(const std::vector<SyntaxToken>& tokens, SyntaxTokenType type)
{
    const auto it = std::ranges::find(tokens, type, &SyntaxToken::type);
    return it != tokens.end() ? &*it : nullptr;
}

int CountTokens(const std::vector<SyntaxToken>& tokens, SyntaxTokenType type)
{
    return static_cast<int>(std::ranges::count(tokens, type, &SyntaxToken::type));
}

std::string_view GetTokenText(std::string_view text, const SyntaxToken& token)
{
    return text.substr(token.start, token.length);
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
        { "```text\nplain\n```", None },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.input);
        auto nodes = ParseNodes(c.input);
        EXPECT_EQ(nodes.size(), 1u);
        if (nodes.empty()) {
            continue;
        }
        EXPECT_EQ(nodes[0].type, IsDiagramLanguage(c.expected) ? NodeType::Diagram : NodeType::CodeBlock);
        EXPECT_EQ(nodes[0].code_language, c.expected);
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
        // 閉じられていない文字列が行末で終わり、無限ループしないこと
        { "UnterminatedString", "x = \"unterminated\ny = 1", Cpp, String, 1 },
        { "CppRawStringsInSequence", "R\"(one)\" + R\"delim(two)delim\" + normalCall()", Cpp, String, 2 },
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
        { "JsonNestedStructure", "{\"items\": [{\"id\": 1}, {\"id\": 2}], \"count\": 2}", Json, Number, 3 },
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
        // dict は Vim9 では型名、legacy では関数の属性
        { "Vim9DictIsType", "var d: dict<string> = {}", Vim9, Type, 2 },
        { "VimDictIsKeyword", "function F() dict\nendfunction", Vim, Type, 0 },
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
        { "CppRawStringAfterSpaceR", "x R\"(test)\"", Cpp, String, "R\"(test)\"" },
        { "CppNumberOctal", "0o77", Cpp, Number, "0o77" },
        { "NumberStartsWithDot", ".5f", Cpp, Number, ".5f" },
        { "CppCommentEol", "int x; // comment", Cpp, Comment, "// comment" },
        // 識別子の末尾の R では生文字列にしない (バグ #16)。
        { "CppRawStringNotAfterIdentifierEndingR", "RENDER\"hello\"", Cpp, String, "\"hello\"" },
        { "CppRawStringNotAfterLongerIdentifier", "xR\"(a)\"", Cpp, String, "\"(a)\"" },
        { "CppRawStringWidePrefix", "p = LR\"(a\"b)\";", Cpp, String, "LR\"(a\"b)\"" },
        { "CppRawStringU8Prefix", "u8R\"x(a)x\"", Cpp, String, "u8R\"x(a)x\"" },
        { "PythonFString", "f\"hello {name}\"", Python, String, "\"hello {name}\"" },
        { "PythonUnterminatedTripleQuote", "s = \"\"\"never closed", Python, String, "\"\"\"never closed" },
        { "TsTemplateLiteral", "`hello ${name}`", TypeScript, String, "`hello ${name}`" },
        { "RustLineComment", "let x = 1; // comment", Rust, Comment, "// comment" },
        { "BashBacktick", "result=`ls -la`", Bash, String, "`ls -la`" },
        { "PwshHashComment", "$x = 1  # comment\n$y = 2", PowerShell, Comment, "# comment" },
        { "PwshString", "\"hello world\"", PowerShell, String, "\"hello world\"" },
        { "CmdRemCommentCaseInsensitive", "rem comment here", Cmd, Comment, "rem comment here" },
        { "CmdAtRemComment", "@rem build script", Cmd, Comment, "rem build script" },
        { "CmdAtDoubleColonComment", "@:: note", Cmd, Comment, ":: note" },
        { "CmdString", "echo \"hello world\"", Cmd, String, "\"hello world\"" },
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

TEST(Syntax, WholeInputIsSingleToken)
{
    using enum SyntaxLanguage;
    using enum SyntaxTokenType;
    static constexpr TypedCase kCases[] = {
        { "TokenizeSingleKeyword", "if", Cpp, Keyword },
        { "CppMultiLineComment", "/* multi\nline\ncomment */", Cpp, Comment },
        // バグ #22: 閉じられていないブロックコメントは最後の文字まで含む
        { "UnterminatedBlockComment", "/* never closed", Cpp, Comment },
        { "UnterminatedBlockCommentEndsWithStar", "/* test *", Cpp, Comment },
        { "CppPreprocessorInclude", "#include <stdio.h>", Cpp, Preprocessor },
        { "CppPreprocessorDefine", "#define MAX 100", Cpp, Preprocessor },
        { "CppPreprocessorContinuation", "#define FOO \\\n    bar", Cpp, Preprocessor },
        // バグ修正: R"(...)" は R を含めて単一の String にする (Plain と String の重複禁止)
        { "CppRawStringStandaloneR", "R\"(hello)\"", Cpp, String },
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

TEST(Syntax, PlainOnlyTextHasNoTokens)
{
    for (const std::string_view code : { "   \n\t  \n  ", "foo bar_baz + - * /", "+ - * / = == != < > <= >=" }) {
        SCOPED_TRACE(code);
        EXPECT_TRUE(Tokenize(code, SyntaxLanguage::Cpp).empty());
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
// bounds は text の ForwardDecodeBoundaries (言語ごとに作り直さない)。
testing::AssertionResult TokensAlignToCpBoundaries(std::string_view text, std::span<const uint32_t> bounds, const std::vector<SyntaxToken>& tokens)
{
    if (auto r = TokensWellFormed(tokens, text.size()); !r) {
        return r;
    }
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
    // 字句解析を持たない言語 (None など) は空を返すだけなので、全部回して新しい言語も漏らさない。
    for (const uint32_t seed : utf8_fuzz::kFuzzSeeds) {
        std::mt19937 rng{ seed };
        for (int iter = 0; iter < 300; ++iter) {
            const auto text = utf8_fuzz::RandomPiecesWithMalformed(rng, kSyntaxFuzzPieces, 1, 24);
            const auto bounds = utf8_fuzz::ForwardDecodeBoundaries(text);
            for (int l = 0; l <= kLastLanguage; ++l) {
                const auto lang = static_cast<SyntaxLanguage>(l);
                // 失敗時だけ評価されるメッセージに文脈を載せ、成功ケースで文字列を組み立てない。
                ASSERT_TRUE(TokensAlignToCpBoundaries(text, bounds, Tokenize(text, lang)))
                    << std::format("seed={} iter={} lang={} text={}", seed, iter, static_cast<int>(lang), utf8_fuzz::HexEscape(text));
            }
        }
    }
}

// ============================================================
// トークン列の完全一致 (字句状態が後続の行へ漏れないこと)
// ============================================================

namespace {

using ExpectedToken = std::pair<std::string_view, SyntaxTokenType>;

// 個数や最初の 1 個だけでは、閉じ損ねた文字列やコメントが後続を飲み込む退行を見逃すため列全体を比べる。
void ExpectTokenSequence(std::string_view code, SyntaxLanguage lang, std::initializer_list<ExpectedToken> expected)
{
    const auto tokens = Tokenize(code, lang);
    AssertTokensWellFormed(tokens, code.size());
    std::vector<ExpectedToken> actual;
    for (const auto& t : tokens) {
        actual.emplace_back(GetTokenText(code, t), t.type);
    }
    EXPECT_EQ(actual, std::vector<ExpectedToken>(expected));
}

} // namespace

TEST(SyntaxSequence, CppKeywordsOnly)
{
    using enum SyntaxTokenType;
    ExpectTokenSequence("if else while for return", SyntaxLanguage::Cpp,
                        { { "if", Keyword }, { "else", Keyword }, { "while", Keyword }, { "for", Keyword }, { "return", Keyword } });
}

// @ は特別に処理しないので、続く行のキーワードに影響しない
TEST(SyntaxSequence, PythonDecorator)
{
    using enum SyntaxTokenType;
    ExpectTokenSequence("@staticmethod\ndef foo():\n    pass", SyntaxLanguage::Python,
                        { { "staticmethod", Type }, { "def", Keyword }, { "foo", Function }, { "pass", Keyword } });
}

// Rust の ' はライフタイムにも使うので文字列にしない ('a が行の残りを飲み込まない)
TEST(SyntaxSequence, RustSingleQuoteNotString)
{
    using enum SyntaxTokenType;
    ExpectTokenSequence("fn foo<'a>(x: &'a str) {}", SyntaxLanguage::Rust, { { "fn", Keyword }, { "str", Type } });
}

// 言語ごとに、コメント・文字列・数値・語が 1 つの文書に混ざったときの列全体
TEST(SyntaxSequence, MixedCode)
{
    using enum SyntaxTokenType;
    using enum SyntaxLanguage;
    ExpectTokenSequence("#include <iostream>\n\nint main() {\n    // Hello\n    std::cout << \"Hello\" << 42;\n    return 0;\n}", Cpp,
                        { { "#include <iostream>", Preprocessor }, { "int", Type }, { "main", Function }, { "// Hello", Comment },
                          { "\"Hello\"", String }, { "42", Number }, { "return", Keyword }, { "0", Number } });
    ExpectTokenSequence("int x = 10;\nfloat y = 3.14f;\n// comment\nif (x > 0) {\n    return y;\n}", Cpp,
                        { { "int", Type }, { "10", Number }, { "float", Type }, { "3.14f", Number }, { "// comment", Comment },
                          { "if", Keyword }, { "0", Number }, { "return", Keyword } });
    ExpectTokenSequence("def greet(name: str) -> str:\n    # Greeting\n    return f\"Hello, {name}!\"\n\nprint(greet(\"World\"))", Python,
                        { { "def", Keyword }, { "greet", Function }, { "str", Type }, { "str", Type }, { "# Greeting", Comment },
                          { "return", Keyword }, { "\"Hello, {name}!\"", String }, { "print", Function }, { "greet", Function },
                          { "\"World\"", String } });
    ExpectTokenSequence("async function fetchData(url) {\n  // Fetch data\n  const resp = await fetch(url);\n  return resp.json();\n}", JavaScript,
                        { { "async", Keyword }, { "function", Keyword }, { "fetchData", Function }, { "// Fetch data", Comment },
                          { "const", Keyword }, { "await", Keyword }, { "fetch", Function }, { "return", Keyword }, { "json", Function } });
    ExpectTokenSequence("interface User {\n  name: string;\n  age: number;\n}\n\nconst greet = (user: User): string => {\n  return `Hello, ${user.name}`;\n};", TypeScript,
                        { { "interface", Keyword }, { "string", Type }, { "number", Type }, { "const", Keyword }, { "string", Type },
                          { "return", Keyword }, { "`Hello, ${user.name}`", String } });
    ExpectTokenSequence("package main\n\nimport \"fmt\"\n\nfunc main() {\n    // Hello\n    fmt.Println(\"Hello\")\n}", Go,
                        { { "package", Keyword }, { "import", Keyword }, { "\"fmt\"", String }, { "func", Keyword }, { "main", Function },
                          { "// Hello", Comment }, { "Println", Function }, { "\"Hello\"", String } });
    ExpectTokenSequence("use std::io;\n\nfn main() -> Result<(), Box<dyn std::error::Error>> {\n    let x: i32 = 42;\n    // comment\n    println!(\"Hello {}\", x);\n    Ok(())\n}", Rust,
                        { { "use", Keyword }, { "fn", Keyword }, { "main", Function }, { "Result", Type }, { "Box", Type }, { "dyn", Keyword },
                          { "let", Keyword }, { "i32", Type }, { "42", Number }, { "// comment", Comment }, { "\"Hello {}\"", String }, { "Ok", Type } });
    ExpectTokenSequence("#!/bin/bash\n# Script\nfor f in *.txt; do\n    echo \"$f\"\ndone", Bash,
                        { { "#!/bin/bash", Comment }, { "# Script", Comment }, { "for", Keyword }, { "in", Keyword }, { "do", Keyword },
                          { "echo", Type }, { "\"$f\"", String }, { "done", Keyword } });
    ExpectTokenSequence("<# Script #>\nfunction Get-Item {\n    param([string]$Path)\n    # Do work\n    return $Path\n}", PowerShell,
                        { { "<# Script #>", Comment }, { "function", Keyword }, { "param", Keyword }, { "string", Type }, { "# Do work", Comment },
                          { "return", Keyword } });
    ExpectTokenSequence("@echo off\nREM Build script\nfor %%f in (*.cpp) do (\n    echo Building %%f\n)\npause", Cmd,
                        { { "echo", Keyword }, { "off", Keyword }, { "REM Build script", Comment }, { "for", Keyword }, { "in", Keyword }, { "do", Keyword },
                          { "echo", Keyword }, { "pause", Keyword } });
}

TEST(SyntaxSequence, YamlBlockScalarEndsAtParentIndent)
{
    using enum SyntaxTokenType;
    // 指示子の後のコメント、本文中の空行を経て、親と同じ桁のキーで本文が終わる
    ExpectTokenSequence("a: | # note\n  x\n\n  y\nb: 1", SyntaxLanguage::Yaml,
                        { { "a", Type }, { "# note", Comment }, { "x", String }, { "y", String }, { "b", Type }, { "1", Number } });
}

TEST(SyntaxSequence, YamlBlockScalarInSequenceItem)
{
    using enum SyntaxTokenType;
    // "- key: |" の本文はキーの桁で終わり、"- |" は "-" の桁で終わる (GitHub Actions の steps の形)
    ExpectTokenSequence("steps:\n  - run: |\n      npm ci\n    shell: bash\n  - |\n    text\n  - 1", SyntaxLanguage::Yaml,
                        { { "steps", Type }, { "run", Type }, { "npm ci", String }, { "shell", Type }, { "text", String }, { "1", Number } });
}

TEST(SyntaxSequence, TomlLiteralStringHasNoEscape)
{
    using enum SyntaxTokenType;
    // ''' はリテラル文字列なので末尾の \ で閉じ引用符を飲み込まない
    ExpectTokenSequence("path = '''C:\\temp\\'''\nname = \"x\"", SyntaxLanguage::Toml,
                        { { "path", Type }, { R"('''C:\temp\''')", String }, { "name", Type }, { "\"x\"", String } });
    // """ は basic 文字列なので \" はエスケープのまま
    ExpectTokenSequence("a = \"\"\"x\\\"\"\"\"\nb = 1", SyntaxLanguage::Toml,
                        { { "a", Type }, { "\"\"\"x\\\"\"\"\"", String }, { "b", Type }, { "1", Number } });
}

TEST(SyntaxSequence, RawTripleQuoteHasNoEscape)
{
    using enum SyntaxTokenType;
    // Kotlin の raw string と C# 11 の raw string literal にはエスケープが無い
    ExpectTokenSequence("val p = \"\"\"C:\\\"\"\"\nval q = 1", SyntaxLanguage::Kotlin,
                        { { "val", Keyword }, { "\"\"\"C:\\\"\"\"", String }, { "val", Keyword }, { "1", Number } });
    ExpectTokenSequence("string p = \"\"\"C:\\\"\"\";\nint x = 1;", SyntaxLanguage::CSharp,
                        { { "string", Type }, { "\"\"\"C:\\\"\"\"", String }, { "int", Type }, { "1", Number } });
    // C# の raw string は開きと同じ数の " で閉じるので、4 個で囲めば本文に """ を書ける
    ExpectTokenSequence(R"(var s = """"a"""b"""";
int x = 1;)", SyntaxLanguage::CSharp,
                        { { "var", Keyword }, { R"(""""a"""b"""")", String }, { "int", Type }, { "1", Number } });
    // Java のテキストブロックは \" をエスケープとして扱う
    ExpectTokenSequence("String s = \"\"\"\na\\\"\"\"\n\"\"\";", SyntaxLanguage::Java,
                        { { "String", Type }, { "\"\"\"\na\\\"\"\"\n\"\"\"", String } });
}

// PHP の変数名はキーワードと同じ綴りでも変数 ($class・$list・$new は普通に使われる)
TEST(SyntaxSequence, PhpVariableNamedLikeKeywordIsNotKeyword)
{
    using enum SyntaxTokenType;
    ExpectTokenSequence("$class = new Foo($list, $default);", SyntaxLanguage::Php, { { "new", Keyword }, { "Foo", Function } });
}

TEST(SyntaxSequence, LispCharacterLiteralsDoNotOpenStringsOrLists)
{
    using enum SyntaxTokenType;
    // 文字リテラルの " や ( を区切りとして読むと、以降の文字列と呼び出し位置の判定がすべてずれる
    ExpectTokenSequence(R"((list #\" #\; #\( #\)) (foo "x"))", SyntaxLanguage::CommonLisp,
                        { { "list", Function }, { R"(#\")", String }, { R"(#\;)", String }, { R"(#\()", String }, { R"(#\))", String },
                          { "foo", Function }, { R"("x")", String } });
    ExpectTokenSequence(R"((insert ?" ?\" ?\( ?\;) (foo))", SyntaxLanguage::EmacsLisp,
                        { { "insert", Function }, { R"(?")", String }, { R"(?\")", String }, { R"(?\()", String }, { R"(?\;)", String },
                          { "foo", Function } });
    ExpectTokenSequence(R"((str \" \( \;) (foo))", SyntaxLanguage::Clojure,
                        { { "str", Function }, { R"(\")", String }, { R"(\()", String }, { R"(\;)", String }, { "foo", Function } });
}

TEST(SyntaxSequence, DiffHunkLineStartingWithTripleDash)
{
    using enum SyntaxTokenType;
    // ハンク内で "-- " を削除した行は "--- " で始まるがファイルヘッダではない
    ExpectTokenSequence("--- a/q.sql\n+++ b/q.sql\n@@ -1,2 +1,2 @@\n--- old\n+++ new\n SELECT 1;", SyntaxLanguage::Diff,
                        { { "--- a/q.sql", Keyword }, { "+++ b/q.sql", Keyword }, { "@@ -1,2 +1,2 @@", Type },
                          { "--- old", Deleted }, { "+++ new", Inserted } });
}

TEST(SyntaxSequence, VimHeredocNeedsMarkerRightAfterCommand)
{
    using enum SyntaxTokenType;
    // 行の途中のシフト演算子 << をヒアドキュメントと読むと、文書の残りが埋め込み言語になる
    ExpectTokenSequence("lua print(1 << 2)\nset nu", SyntaxLanguage::Vim,
                        { { "lua", Keyword }, { "print", Type }, { "1", Number }, { "2", Number }, { "set", Keyword } });
    ExpectTokenSequence("let s = \"a =<< b\"\nset nu", SyntaxLanguage::Vim,
                        { { "let", Keyword }, { "\"a =<< b\"", String }, { "set", Keyword } });
}

TEST(SyntaxSequence, VimStringAndTrailingCommentOnSameLine)
{
    using enum SyntaxTokenType;
    // 旧来の Vim script は同じ行に閉じの " があれば文字列、無ければコメント
    ExpectTokenSequence("echo \"a\\\"b\" \" c\nlet s = \"\" \" empty", SyntaxLanguage::Vim,
                        { { "echo", Keyword }, { "\"a\\\"b\"", String }, { "\" c", Comment }, { "let", Keyword }, { "\"\"", String },
                          { "\" empty", Comment } });
}

TEST(SyntaxSequence, HaskellDoubleQuoteCharLiteral)
{
    using enum SyntaxTokenType;
    ExpectTokenSequence("c = '\"' : rest\nmain = putStrLn \"ok\"", SyntaxLanguage::Haskell,
                        { { "c", Function }, { "'\"'", String }, { "main", Function }, { "\"ok\"", String } });
}

TEST(SyntaxSequence, MakefileRecipeSurvivesBlankAndCommentLines)
{
    using enum SyntaxTokenType;
    ExpectTokenSequence("t:\n\techo a\n\n# note\n\techo b\nX = 1", SyntaxLanguage::Makefile,
                        { { "t", Function }, { "echo", Type }, { "# note", Comment }, { "echo", Type }, { "X", Type } });
}

TEST(SyntaxSequence, BashHashAfterQuoteOrEscapeIsNotComment)
{
    using enum SyntaxTokenType;
    // クォートや \ の直後の # は単語の途中、タブの直後は単語の先頭
    ExpectTokenSequence("echo \"a\"#b \\#c x=1\t#tab", SyntaxLanguage::Bash,
                        { { "echo", Type }, { "\"a\"", String }, { "1", Number }, { "#tab", Comment } });
}

namespace {

// TokensCrossing が返すべきトークンに絞る。Tokenize の結果をそのまま返す言語があるので、実際の結果も通して比べる。
std::vector<SyntaxToken> KeepCrossing(std::vector<SyntaxToken> tokens, size_t text_size, std::span<const size_t> boundaries)
{
    std::erase_if(tokens, [&](const SyntaxToken& t) {
        const size_t end = size_t{ t.start } + t.length;
        const auto b = std::ranges::upper_bound(boundaries, size_t{ t.start });
        return end != text_size && (b == boundaries.end() || *b >= end);
    });
    return tokens;
}

} // namespace

// TokensCrossing は語の分類を省き、コメントや文字列が始まりうる文字まで語を飛ばす。言語ごとの規則を別に持たないことを、
// 全トークンを作った結果との突き合わせで確かめる。断片はどれかの言語でトークンを始めるか、数値や語の途中に現れる文字。
TEST(TokensCrossing, MatchesFullTokenizeOnRandomCode)
{
    constexpr std::string_view kPieces[] = {
        "\"", "'", "`", "'''", "\"\"\"", "\"\"\"\"", "/*", "*/", "//", "#", "#[", "\n", "\n", "\n", " ", " ", "\t",
        "R\"(", ")\"", "R\"x(", ")x\"", "r\"", "@\"", "$\"", "<#", "#>", "--", "--[[", "]]", "[[", "[==[", "]==]",
        "\n=begin\n", "\n=end\n", "=begin", "::", "\nrem ", "REM ", "\n@rem ", "@", "LR\"(", "\\\\", "\\", "${", "}", "$x", "$class", "@import",
        "foo", "int", "return", "class", "x_1", "日本語", "(", ")", ";", ",", "=", "-", "+", "*", "/", ".", ":", "<", ">", "[", "]",
        "123", "1'000", "0x1F", "1.5e+3", ".5", "1.", "1e", "0b1'0",
    };
    // 100 seed で足りる。語を飛ばすときに区切りを無視する・空白まで戻らない改変は、300 seed で 291 件・1179 件落ち、
    // そのうち 95 件・372 件が最初の 100 seed に入る (2026-11-06)。
    for (int l = 0; l <= kLastLanguage; ++l) {
        const auto language = static_cast<SyntaxLanguage>(l);
        for (uint32_t seed = 0; seed < 100; ++seed) {
            std::mt19937 rng(seed * 1000 + l);
            std::string text;
            const size_t pieces = rng() % 400;
            for (size_t i = 0; i < pieces; ++i) {
                text += kPieces[rng() % std::size(kPieces)];
            }
            std::vector<size_t> boundaries;
            for (size_t i = rng() % 8; i > 0 && !text.empty(); --i) {
                boundaries.push_back(1 + rng() % text.size());
            }
            std::ranges::sort(boundaries);
            const auto [first, last] = std::ranges::unique(boundaries);
            boundaries.erase(first, last);

            ASSERT_TRUE(KeepCrossing(TokensCrossing(text, language, boundaries), text.size(), boundaries) ==
                        KeepCrossing(Tokenize(text, language), text.size(), boundaries))
                << std::format("language={} seed={} text={}", l, seed, utf8_fuzz::HexEscape(text));
        }
    }
}

// 空白の無い長い行。語を飛ばす先が無く、区切りをまたぐ語の分類も要る。
// 区切りの間隔 1003 は単位の長さ (8, 9, 14) と互いに素なので、14 個目までに単位のどの位置にも区切りが当たる。30KB で 29 個ある。
TEST(TokensCrossing, MatchesFullTokenizeOnLinesWithoutBlanks)
{
    constexpr std::string_view kUnits[] = { "1,22,333,", "a.b(c,d);", "\"k\":\"v\",", "if(x){return;}" };
    constexpr SyntaxLanguage kLanguages[] = { SyntaxLanguage::Json, SyntaxLanguage::JavaScript, SyntaxLanguage::Cpp, SyntaxLanguage::Python };
    for (const SyntaxLanguage language : kLanguages) {
        for (const std::string_view unit : kUnits) {
            std::string text = RepeatTo(unit, 30000);
            std::vector<size_t> boundaries;
            for (size_t b = 1003; b < text.size(); b += 1003) {
                boundaries.push_back(b);
            }
            ASSERT_TRUE(KeepCrossing(TokensCrossing(text, language, boundaries), text.size(), boundaries) ==
                        KeepCrossing(Tokenize(text, language), text.size(), boundaries))
                << unit;
        }
    }
}

// kTokenStartStorage から漏れた文字は Tokenize でも TokensCrossing でも同じように見落とされ、突き合わせでは分からない。
// 規則ごとに、その規則だけで開始文字が入る言語で確かめる。
TEST(SyntaxSequence, TokenStartCharsReachTheirBranches)
{
    using enum SyntaxTokenType;
    using enum SyntaxLanguage;
    ExpectTokenSequence("\"a\"", Cpp, { { "\"a\"", String } });
    ExpectTokenSequence("'a'", Python, { { "'a'", String } });
    ExpectTokenSequence("'''a'''", Swift, { { "'''a'''", String } });
    ExpectTokenSequence("// a", Zig, { { "// a", Comment } });
    ExpectTokenSequence("/* a */", Sql, { { "/* a */", Comment } });
    ExpectTokenSequence("# a", Python, { { "# a", Comment } });
    ExpectTokenSequence("#define", Cpp, { { "#define", Preprocessor } });
    ExpectTokenSequence("<# a #>", PowerShell, { { "<# a #>", Comment } });
    ExpectTokenSequence("[[a]]", Lua, { { "[[a]]", String } });
    ExpectTokenSequence("--[[a]]", Lua, { { "--[[a]]", Comment } });
    ExpectTokenSequence("-- a", Sql, { { "-- a", Comment } });
    ExpectTokenSequence("::a", Cmd, { { "::a", Comment } });
    ExpectTokenSequence("@\"a\"", CSharp, { { "@\"a\"", String } });
    ExpectTokenSequence("@import", Zig, { { "@import", Function } });
    ExpectTokenSequence("\\\\a", Zig, { { "\\\\a", String } });
    ExpectTokenSequence("`a`", JavaScript, { { "`a`", String } });
    ExpectTokenSequence("=begin\na\n=end", Ruby, { { "=begin\na\n=end", Comment } });
    ExpectTokenSequence("rem a", Cmd, { { "rem a", Comment } });
    ExpectTokenSequence("REM a", Cmd, { { "REM a", Comment } });
}

namespace {

// begin から後ろで始まるトークン。begin がトークンの途中なら nullopt。
std::optional<std::vector<SyntaxToken>> TokensFrom(const std::vector<SyntaxToken>& tokens, size_t begin)
{
    const auto first = std::ranges::upper_bound(tokens, begin, {}, [](const SyntaxToken& t) { return size_t{ t.start } + t.length; });
    if (first != tokens.end() && first->start < begin) {
        return std::nullopt;
    }
    return std::vector<SyntaxToken>(first, tokens.end());
}

// begin での状態から解析を始めた結果。状態は、先頭から解析したときに残るものを使う。
std::vector<SyntaxToken> ResumeAt(std::string_view text, SyntaxLanguage language, size_t begin)
{
    SyntaxStateTrace trace;
    const size_t wanted[]{ begin };
    TokensCrossing(text, language, wanted, 0, &trace);
    std::vector<SyntaxToken> resumed;
    AppendTokens(text, language, resumed, begin, trace.For(begin).state);
    return resumed;
}

// 区切ったコードの続きのノードは、行の頭から、そこでの状態だけを頼りに解析する。トークンの外にあるどの行の頭から
// 始めても、先頭から解析したのと同じトークンになること。
void ExpectResumableAtLineStarts(SyntaxLanguage language, std::string_view text)
{
    SCOPED_TRACE(static_cast<int>(language));
    const auto full = Tokenize(text, language);
    size_t checked = 0;
    for (size_t newline = text.find('\n'); newline != std::string_view::npos; newline = text.find('\n', newline + 1)) {
        const size_t begin = newline + 1;
        const auto expected = TokensFrom(full, begin);
        if (!expected) {
            continue;
        }
        ASSERT_TRUE(ResumeAt(text, language, begin) == *expected) << "begin=" << begin << " 続き=" << text.substr(begin, 40);
        ++checked;
    }
    EXPECT_GT(checked, 3u);
}

} // namespace

TEST(SyntaxResume, Html)
{
    constexpr std::string_view kTags = "<div\n  class=\"a b\"\n  id='x1'\n  data-n=1\n  hidden>text &amp; more</div>\n"
                                       "<img src=\"x.png\"\n     alt=\"multi\nline\"/>\n<!-- a\n     comment -->\n<p>after</p>\n";
    const std::string html = std::format(
        "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<style>\n.a > b {{\n  color: red;\n  margin: 0 auto;\n}}\n@media (max-width: 600px) {{\n"
        "  .c,\n  .d {{ padding: 1px 2em; }}\n}}\n</style>\n<script\n  type=\"module\">\nfor (let i = 0; i < n; i++) {{\n  total += f(i, \"s\"); // c\n}}\n"
        "/* block\n   comment */ const x = `t\n u`;\n</script>\n<style></style>\n</head>\n<body>\n{}</body>\n</html>\n",
        kTags);
    ExpectResumableAtLineStarts(SyntaxLanguage::Html, html);
    ExpectResumableAtLineStarts(SyntaxLanguage::Xml, kTags);
    // 閉じタグの無い <script> / <style> は末尾まで中身。
    ExpectResumableAtLineStarts(SyntaxLanguage::Html, "<script>\nlet a = 1;\nif (a < 2) {\n  f();\n}\n// end\n");
    ExpectResumableAtLineStarts(SyntaxLanguage::Html, "<style>\n.a {\n  color: red;\n}\n.b {\n  top: 0;\n");
}

TEST(SyntaxResume, CssAndScss)
{
    constexpr std::string_view kCss = "@import url(foo.css);\n.a {\n  color: red;\n  margin: 0 auto;\n  .b,\n  .c {\n    width: calc(100% - 2px);\n  }\n"
                                      "  font-family: \"A\",\n    serif;\n}\n@media screen and (min-width: 10px) {\n  #d { left: -2em !important; }\n}\n"
                                      "/* comment\n   more */\n.e { color: blue; }\n";
    ExpectResumableAtLineStarts(SyntaxLanguage::Css, kCss);
    ExpectResumableAtLineStarts(SyntaxLanguage::Scss, kCss);
    ExpectResumableAtLineStarts(SyntaxLanguage::Scss, "$w: 10px;\n.a {\n  // line comment\n  $x: 1;\n  &:hover { top: $w; }\n  @include m(1px);\n}\n@mixin m($a) {\n  left: $a;\n}\n");
}

TEST(SyntaxResume, Yaml)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Yaml,
                                "name: test\nscript: |\n  echo key: value # not a comment\n  - item: 12\n\n  more\nafter: 1\nitems:\n  - text: >-\n"
                                "      folded: line \"x\"\n      next\n  - n: 2\n  - |\n   indented\nkey: [a, b]\n--- \nlast: ~\n");
}

TEST(SyntaxResume, Toml)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Toml,
                                "[server]\nhost = \"localhost\"\nlist = [\n  { name = \"a\", n = 1, on = true },\n  [1, 2.5,\n   1979-05-27],\n"
                                "  \"text = not a key\",\n]\nkey = 2\nt = { a = [1, 2], b = \"s\" }\nstr = \"\"\"\nmulti\n\"\"\"\n[[item]]\nname = \"x\"\n");
}

TEST(SyntaxResume, Lisp)
{
    constexpr std::string_view kCode = "(defun f (x &optional y)\n  \"Doc.\"\n  (cond\n    ((= x 1) (foo \"s\" 1))\n    ((bar x) nil)\n    (t (baz))))\n"
                                       "(let (\n      (v1 (compute 1))\n      (v2 'sym)\n     )\n  (message \"%d\" v1))\n(defvar\n  name\n  (list 1 2))\n"
                                       "(defun\n  g\n  (a b)\n  (h a))\n'(a\n  b)\n(next\n  1)\n";
    for (const SyntaxLanguage language : { SyntaxLanguage::EmacsLisp, SyntaxLanguage::CommonLisp, SyntaxLanguage::Scheme, SyntaxLanguage::Clojure }) {
        ExpectResumableAtLineStarts(language, kCode);
    }
}

// 入れ子の深さごとの印 (cond の節や束縛リストは呼び出しではない) は内側の 16 段だけを持ち越す。16 段より外側にある印は
// 忘れるので、そこまで抜けた後の cond の節を呼び出しとして色付けする。
TEST(SyntaxResume, LispKeepsInnermostSixteenLevels)
{
    const auto code_with_depth = [](int inner) {
        std::string code = "(cond\n";
        for (int i = 0; i < inner; ++i) {
            code += "((a\n";
        }
        code += "x\n";
        code += std::string(static_cast<size_t>(inner) * 2, ')');
        code += "\n(clause 1)\n)\n";
        return code;
    };
    // cond の段と合わせて 15 段。
    ExpectResumableAtLineStarts(SyntaxLanguage::EmacsLisp, code_with_depth(7));

    const std::string deep = code_with_depth(8);
    const size_t innermost = deep.find("x\n");
    const auto full = Tokenize(deep, SyntaxLanguage::EmacsLisp);
    const auto resumed = ResumeAt(deep, SyntaxLanguage::EmacsLisp, innermost);
    const auto is_clause = [&](const SyntaxToken& t) { return deep.substr(t.start, t.length) == "clause"; };
    EXPECT_TRUE(std::ranges::none_of(full, is_clause));
    EXPECT_TRUE(std::ranges::any_of(resumed, is_clause));
}

TEST(SyntaxResume, Diff)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Diff,
                                "diff --git a/x.sql b/x.sql\nindex 1..2\n--- a/x.sql\n+++ b/x.sql\n@@ -1,3 +1,3 @@\n--- old sql comment\n+++ new sql comment\n"
                                " context\n-removed\n+added\ndiff --git a/y b/y\n--- a/y\n+++ b/y\n@@ -1 +1 @@\n-a\n+b\n");
}

TEST(SyntaxResume, Vim)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Vim,
                                "let g:a = 1 \" comment\nlua << EOF\nlocal t = { 1, \"s\" } -- comment\n--[[ long\n     comment ]] print(t)\nEOF\n"
                                "let lines =<< trim END\n  some text \" not a comment\n  let x = 1\n  END\npython3 << EOF\nlet x = \"y\"\nEOF\n"
                                "function! F(a)\n  return a:a\nendfunction\nlua << EOF\nprint(1)\n");
    // ```vim と書いた Vim9 script。# がコメントで、閉じていない " はコメントではない。
    constexpr std::string_view kVim9 = "vim9script\nvar name = \"value\" # trailing comment\n# full line comment\ndef F(n: number): string\n  return \"x\n"
                                       "enddef\n";
    ExpectResumableAtLineStarts(SyntaxLanguage::Vim, kVim9);
    ExpectResumableAtLineStarts(SyntaxLanguage::Vim9, kVim9);
}

TEST(SyntaxResume, Makefile)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Makefile,
                                "CC = gcc\nSRCS = first.c \\\n    name.c \\\n    last.c\nall: a b\n    @echo $(CC) -o out: file=x > log.txt\n"
                                "    $(CC) a.c && \\\n        echo \"done: x\" \\\n        more\n\n    rm -f $@ # comment\nifeq ($(X),1)\nX = 1\nendif\n"
                                "clean: dep \\\n       dep2\n    rm x\n");
}

// 継続行の途中から始めると、残りは変数参照とコメントだけを色付けする。ターゲットの名前が 2 行目まで続く行では
// 名前に色が付かないが、続く行がレシピになることは持ち越す。
TEST(SyntaxResume, MakefileTargetNameOnContinuationLineIsPlain)
{
    constexpr std::string_view kCode = "first \\\n  second: dep\n    echo $(X)\n";
    const size_t line2 = kCode.find("  second");
    const auto full = Tokenize(kCode, SyntaxLanguage::Makefile);
    const auto resumed = ResumeAt(kCode, SyntaxLanguage::Makefile, line2);
    const size_t recipe = kCode.find("    echo");
    EXPECT_NE(TokensFrom(full, line2), resumed);
    EXPECT_EQ(TokensFrom(full, recipe), TokensFrom(resumed, recipe));
    EXPECT_FALSE(TokensFrom(resumed, recipe)->empty());
}

TEST(SyntaxResume, Dockerfile)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Dockerfile,
                                "FROM alpine AS build\n# comment\nRUN apk update && \\\n    add-pkg \"name\" && \\\n    run --flag from as copy\nCOPY a b\n"
                                "ENV A=1 \\\n    B=2\nRUN echo \"multi \\\n    line\" && \\\n    true\n");
}

// 状態を持たない言語は、状態 0 のままどの行の頭からでも始められる。
TEST(SyntaxResume, StatelessLanguages)
{
    ExpectResumableAtLineStarts(SyntaxLanguage::Ini, "[section]\nkey = \"value\"\n; comment\nn: 12\n[other]\nx = true\n");
    ExpectResumableAtLineStarts(SyntaxLanguage::Haskell, "module M where\n{- nested {- comment -}\n   continues -}\nmain :: IO ()\nmain = putStrLn \"x\" -- c\n#if X\nf x = 1\n");
    ExpectResumableAtLineStarts(SyntaxLanguage::Cpp, "#include <a>\nint f() { return 1; } // c\n/* block\n   comment */ #define X\nconst char* s = \"a\";\n");
}

// IsLineLocalLanguage の言語は、行の頭で区切ったコードの並びを前から解析せずに済ませる (AnnotateCodeChunkLeads)。
// トークンが改行をまたがず、どの行の頭から状態なしで始めても同じ結果になること。言語を足したら断片も見直す。
TEST(SyntaxResume, LineLocalLanguagesNeedNoState)
{
    constexpr std::string_view kPieces[] = {
        "[", "]", "=", ":", ";", "#", "\"", "'", " ", " ", "\t", "\n", "\n", "\n", "key", "1.5", "true", "\\", "値",
    };
    for (int l = 0; l <= kLastLanguage; ++l) {
        const auto language = static_cast<SyntaxLanguage>(l);
        if (!IsLineLocalLanguage(language)) {
            continue;
        }
        for (uint32_t seed = 0; seed < 200; ++seed) {
            std::mt19937 rng(seed);
            std::string text;
            for (size_t i = rng() % 200; i > 0; --i) {
                text += kPieces[rng() % std::size(kPieces)];
            }
            const auto full = Tokenize(text, language);
            for (const auto& t : full) {
                ASSERT_FALSE(GetTokenText(text, t).contains('\n')) << std::format("language={} seed={} text={}", l, seed, utf8_fuzz::HexEscape(text));
            }
            for (size_t newline = text.find('\n'); newline != std::string::npos; newline = text.find('\n', newline + 1)) {
                std::vector<SyntaxToken> resumed;
                AppendTokens(text, language, resumed, newline + 1);
                ASSERT_TRUE(resumed == TokensFrom(full, newline + 1))
                    << std::format("language={} seed={} begin={} text={}", l, seed, newline + 1, utf8_fuzz::HexEscape(text));
            }
        }
    }
}
