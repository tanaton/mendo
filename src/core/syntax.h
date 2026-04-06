#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <memory_resource>

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
    Cmd
};

enum class SyntaxTokenType : uint8_t {
    Plain,
    Keyword,
    Type,
    String,
    Number,
    Comment,
    Preprocessor,
    Function
};

struct SyntaxToken {
    uint32_t start = 0;
    uint32_t length = 0;
    SyntaxTokenType type = SyntaxTokenType::Plain;
};

SyntaxLanguage DetectLanguage(std::wstring_view info_string);

std::pmr::vector<SyntaxToken> Tokenize(std::wstring_view text, SyntaxLanguage language);
