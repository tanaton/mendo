#include "theme.h"

static D2D1_COLOR_F Color(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        a
    );
}

float Theme::GetHeadingSize(int level) const {
    switch (level) {
        case 1: return font_size_h1;
        case 2: return font_size_h2;
        case 3: return font_size_h3;
        case 4: return font_size_h4;
        case 5: return font_size_h5;
        case 6: return font_size_h6;
        default: return font_size_body;
    }
}

// Shared layout constants between light & dark themes
static void ApplyCommonLayout(Theme& t) {
    wcscpy_s(t.font_family,    L"Yu Gothic UI");
    wcscpy_s(t.monospace_font, L"Consolas");

    t.font_size_body = 16.0f;
    t.font_size_h1   = 32.0f;
    t.font_size_h2   = 26.0f;
    t.font_size_h3   = 22.0f;
    t.font_size_h4   = 18.0f;
    t.font_size_h5   = 16.0f;
    t.font_size_h6   = 14.0f;
    t.font_size_code = 14.0f;

    t.margin_left           = 40.0f;
    t.margin_right          = 40.0f;
    t.margin_top            = 20.0f;
    t.paragraph_spacing     = 12.0f;
    t.heading_spacing_above = 24.0f;
    t.heading_spacing_below = 8.0f;
    t.code_block_padding    = 12.0f;
    t.indent_width          = 24.0f;
    t.blockquote_bar_width  = 4.0f;
    t.list_bullet_offset    = 20.0f;
    t.hr_thickness          = 1.5f;

    t.pane_item_height      = 28.0f;
    t.pane_header_height    = 32.0f;
    t.splitter_width        = 4.0f;
    t.pane_font_size        = 13.0f;
}

Theme GetLightTheme() {
    Theme t{};

    t.bg_color              = Color(0xFFFFFF);
    t.text_color            = Color(0x24292e);
    t.heading_color         = Color(0x1a1a1a);
    t.code_bg_color         = Color(0xf6f8fa);
    t.code_text_color       = Color(0x24292e);
    t.link_color            = Color(0x0366d6);
    t.hr_color              = Color(0xd0d0d0);
    t.blockquote_bar_color  = Color(0xdfe2e5);
    t.blockquote_text_color = Color(0x6a737d);

    // Syntax highlighting
    t.syntax_keyword      = Color(0xAF00DB);  // purple
    t.syntax_type         = Color(0x267F99);  // teal
    t.syntax_string       = Color(0xA31515);  // dark red
    t.syntax_number       = Color(0x098658);  // green
    t.syntax_comment      = Color(0x008000);  // green
    t.syntax_preprocessor = Color(0x795E26);  // brown
    t.syntax_function     = Color(0x795E26);  // brown

    ApplyCommonLayout(t);

    // Pane layout
    t.pane_bg_color         = Color(0xf5f5f5);
    t.splitter_color        = Color(0xe0e0e0);
    t.pane_item_hover_color = Color(0xe8e8e8);
    t.pane_item_active_color = Color(0xd0e0f0);

    return t;
}

Theme GetDarkTheme() {
    Theme t{};

    t.bg_color              = Color(0x1e1e1e);
    t.text_color            = Color(0xd4d4d4);
    t.heading_color         = Color(0xe0e0e0);
    t.code_bg_color         = Color(0x2d2d2d);
    t.code_text_color       = Color(0xd4d4d4);
    t.link_color            = Color(0x569cd6);
    t.hr_color              = Color(0x404040);
    t.blockquote_bar_color  = Color(0x505050);
    t.blockquote_text_color = Color(0x9e9e9e);

    // Syntax highlighting (VS Code Dark+ inspired)
    t.syntax_keyword      = Color(0xC586C0);
    t.syntax_type         = Color(0x4EC9B0);
    t.syntax_string       = Color(0xCE9178);
    t.syntax_number       = Color(0xB5CEA8);
    t.syntax_comment      = Color(0x6A9955);
    t.syntax_preprocessor = Color(0xDCDCAA);
    t.syntax_function     = Color(0xDCDCAA);

    ApplyCommonLayout(t);

    // Pane layout
    t.pane_bg_color         = Color(0x252526);
    t.splitter_color        = Color(0x3c3c3c);
    t.pane_item_hover_color = Color(0x2a2d2e);
    t.pane_item_active_color = Color(0x094771);

    return t;
}
