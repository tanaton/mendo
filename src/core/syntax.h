#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

enum class SyntaxLanguage : uint8_t {
    None,
    Cpp,
    Python,
    JavaScript,
    Mermaid,
    Go,
    Rust,
    TypeScript,
    Bash,
    PowerShell,
    Cmd,
    Json,
    LatexMath,
    Java,
    CSharp,
    Kotlin,
    Swift,
    Php,
    Ruby,
    Sql,
    Yaml,
    Toml,
    Ini,
    Html,
    Xml,
    Css,
    Scss,
    Diff,
    Dockerfile,
    Makefile,
    Zig,
    Haskell,
    Clojure,
    EmacsLisp,
    CommonLisp,
    Scheme, // Racket を含む
    Lua,
    Vim,
    Vim9 // コメントが # の Vim9 script
};

constexpr bool IsDiagramLanguage(SyntaxLanguage lang) noexcept
{
    return lang == SyntaxLanguage::Mermaid;
}

enum class SyntaxTokenType : uint8_t {
    Plain,
    Keyword,
    Type,
    String,
    Number,
    Comment,
    Preprocessor,
    Function,
    Inserted, // diff の追加行
    Deleted   // diff の削除行
};

struct SyntaxToken {
    uint32_t start = 0;
    uint32_t length = 0;
    SyntaxTokenType type = SyntaxTokenType::Plain;

    constexpr bool operator==(const SyntaxToken&) const noexcept = default;
};

SyntaxLanguage DetectLanguage(std::string_view info_string) noexcept;

std::vector<SyntaxToken> Tokenize(std::string_view text, SyntaxLanguage language);

// Tokenize と同じトークンを tokens の後ろに足す。位置は text の先頭から数える。
void AppendTokens(std::string_view text, SyntaxLanguage language, std::vector<SyntaxToken>& tokens);

// Tokenize の結果のうち、boundaries (昇順の位置) のどれかをまたぐトークンと text の末尾に届くトークンを返す。
// LexerConfig で解析する言語はそれだけを作るので Tokenize より速い。そのほかの言語は Tokenize の結果をそのまま返す。
std::vector<SyntaxToken> TokensCrossing(std::string_view text, SyntaxLanguage language, std::span<const size_t> boundaries);
