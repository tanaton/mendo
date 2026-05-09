#pragma once
#include <cstdint>

// 描画（Direct2D）と HTML クリップボードコピーで共有する色を一元定義する。
// 0xRRGGBB 形式の uint32_t で保持し、D2D1_COLOR_F や L"#rrggbb" 文字列へ変換して参照する。
// theme.h から分離してあるのは core 層 (selection_html.cpp) が D2D1 / Theme 構造体に
// 引きずられず、色だけを参照できるようにするため。
namespace theme_palette {

struct SharedColors {
    uint32_t code_bg;
    uint32_t code_text;
    uint32_t table_border;

    // シンタックスハイライト色（ライトは VS Code Light 風、ダークは VS Code Dark+ 風）
    uint32_t syntax_keyword;
    uint32_t syntax_type;
    uint32_t syntax_string;
    uint32_t syntax_number;
    uint32_t syntax_comment;
    uint32_t syntax_preprocessor;
    uint32_t syntax_function;
};

inline constexpr SharedColors kLight = {
    .code_bg = 0xf6f8fa,
    .code_text = 0x24292e,
    .table_border = 0xd0d7de,
    .syntax_keyword = 0xaf00db,
    .syntax_type = 0x267f99,
    .syntax_string = 0xa31515,
    .syntax_number = 0x098658,
    .syntax_comment = 0x008000,
    .syntax_preprocessor = 0x795e26,
    .syntax_function = 0x795e26,
};

inline constexpr SharedColors kDark = {
    .code_bg = 0x2d2d2d,
    .code_text = 0xd4d4d4,
    .table_border = 0x3c3c3c,
    .syntax_keyword = 0xc586c0,
    .syntax_type = 0x4ec9b0,
    .syntax_string = 0xce9178,
    .syntax_number = 0xb5cea8,
    .syntax_comment = 0x6a9955,
    .syntax_preprocessor = 0xdcdcaa,
    .syntax_function = 0xdcdcaa,
};

} // namespace theme_palette
