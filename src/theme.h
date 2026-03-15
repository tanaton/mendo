#pragma once
#include <d2d1.h>
#include <cstdint>

struct Theme {
    // Colors
    D2D1_COLOR_F bg_color;
    D2D1_COLOR_F text_color;
    D2D1_COLOR_F heading_color;
    D2D1_COLOR_F code_bg_color;
    D2D1_COLOR_F code_text_color;
    D2D1_COLOR_F link_color;
    D2D1_COLOR_F hr_color;
    D2D1_COLOR_F blockquote_bar_color;
    D2D1_COLOR_F blockquote_text_color;

    // Syntax highlighting colors
    D2D1_COLOR_F syntax_keyword;
    D2D1_COLOR_F syntax_type;
    D2D1_COLOR_F syntax_string;
    D2D1_COLOR_F syntax_number;
    D2D1_COLOR_F syntax_comment;
    D2D1_COLOR_F syntax_preprocessor;
    D2D1_COLOR_F syntax_function;

    // Font
    wchar_t font_family[64];
    wchar_t monospace_font[64];

    // Font sizes (in DIP)
    float font_size_body;
    float font_size_h1;
    float font_size_h2;
    float font_size_h3;
    float font_size_h4;
    float font_size_h5;
    float font_size_h6;
    float font_size_code;

    // Margins and padding (in DIP)
    float margin_left;
    float margin_right;
    float margin_top;
    float paragraph_spacing;
    float heading_spacing_above;
    float heading_spacing_below;
    float code_block_padding;
    float indent_width;
    float blockquote_bar_width;
    float list_bullet_offset;
    float hr_thickness;

    // Pane layout
    D2D1_COLOR_F pane_bg_color;
    D2D1_COLOR_F splitter_color;
    D2D1_COLOR_F pane_item_hover_color;
    D2D1_COLOR_F pane_item_active_color;
    float pane_item_height;
    float pane_header_height;
    float splitter_width;
    float pane_font_size;

    float GetHeadingSize(int level) const;
};

Theme GetLightTheme();
Theme GetDarkTheme();
