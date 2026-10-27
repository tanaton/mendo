#pragma once
#include <cstdint>

// theme.h から分離してあるのは core 層 (selection_html.cpp) が D2D1 / Theme 構造体に
// 引きずられず、色とフォントだけを参照できるようにするため。
namespace theme_palette {

// 全テーマ共通。先頭から実在するものを選び、どれも無ければ末尾を使う。
// HTML コピーの CSS にも埋め込むので ASCII のみ。
inline constexpr const wchar_t* kMonospaceFontCandidates[]{ L"Cascadia Mono", L"Consolas", L"Courier New" };
// Segoe Fluent Icons は Windows 11 から。Windows 10 は Segoe MDL2 Assets にある同じコードポイントで代用する。
inline constexpr const wchar_t* kIconFontCandidates[]{ L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };

struct SharedColors {
    uint32_t code_bg;
    uint32_t code_text;
    // 罫線 (水平線・見出しの下線・表の枠)。
    uint32_t rule;

    // シンタックスハイライト色（ライトは VS Code Light 風、ダークは VS Code Dark+ 風）
    uint32_t syntax_keyword;
    uint32_t syntax_type;
    uint32_t syntax_string;
    uint32_t syntax_number;
    uint32_t syntax_comment;
    uint32_t syntax_preprocessor;
    uint32_t syntax_function;
    uint32_t syntax_inserted;
    uint32_t syntax_deleted;
};

inline constexpr SharedColors kLight = {
    .code_bg = 0xf6f8fa,
    .code_text = 0x24292e,
    .rule = 0xd0d0d0,
    .syntax_keyword = 0xaf00db,
    .syntax_type = 0x267f99,
    .syntax_string = 0xa31515,
    .syntax_number = 0x098658,
    .syntax_comment = 0x008000,
    .syntax_preprocessor = 0x795e26,
    .syntax_function = 0x795e26,
    .syntax_inserted = 0x22863a,
    .syntax_deleted = 0xb31d28,
};

inline constexpr SharedColors kDark = {
    .code_bg = 0x2d2d2d,
    .code_text = 0xd4d4d4,
    .rule = 0x404040,
    .syntax_keyword = 0xc586c0,
    .syntax_type = 0x4ec9b0,
    .syntax_string = 0xce9178,
    .syntax_number = 0xb5cea8,
    .syntax_comment = 0x6a9955,
    .syntax_preprocessor = 0xdcdcaa,
    .syntax_function = 0xdcdcaa,
    .syntax_inserted = 0x7ee787,
    .syntax_deleted = 0xff7b72,
};

} // namespace theme_palette
