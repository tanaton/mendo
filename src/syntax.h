#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum class SyntaxLanguage : uint8_t {
    None,
    Cpp,
    Python,
    JavaScript
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

SyntaxLanguage DetectLanguage(const std::wstring& info_string);

std::vector<SyntaxToken> Tokenize(const std::wstring& text, SyntaxLanguage language);
