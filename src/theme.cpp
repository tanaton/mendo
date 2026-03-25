#include "theme.h"

static D2D1_COLOR_F Color(uint32_t rgb, float a = 1.0f) noexcept {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        a
    );
}

float Theme::GetHeadingSize(int level) const noexcept {
    if (level < 1 || level > 6) {
        return font_size_body;
    }
    return font_size_h[level - 1];
}

void Theme::ApplyZoom(float new_zoom) noexcept {
    if (new_zoom <= 0.0f || zoom <= 0.0f) {
        return;
    }
    // 前のズームを元に戻してから新しいズームを適用する
    float ratio = new_zoom / zoom;
    zoom = new_zoom;

    font_size_body *= ratio;
    for (int i = 0; i < 6; ++i) {
        font_size_h[i] *= ratio;
    }
    font_size_code *= ratio;

    margin_left *= ratio;
    margin_right *= ratio;
    margin_top *= ratio;
    paragraph_spacing *= ratio;
    heading_spacing_above *= ratio;
    heading_spacing_below *= ratio;
    code_block_padding *= ratio;
    indent_width *= ratio;
    blockquote_bar_width *= ratio;
    list_bullet_offset *= ratio;
    hr_thickness *= ratio;

    // ペインサイズ
    pane_item_height *= ratio;
    pane_header_height *= ratio;
    splitter_width *= ratio;
    pane_font_size *= ratio;
}

// ライトテーマとダークテーマで共有するレイアウト定数
static void ApplyCommonLayout(Theme& t) {
    t.font_family = L"Yu Gothic UI";
    t.monospace_font = L"Consolas";

    t.font_size_body = 16.0f;
    t.font_size_h[0] = 32.0f;
    t.font_size_h[1] = 26.0f;
    t.font_size_h[2] = 22.0f;
    t.font_size_h[3] = 18.0f;
    t.font_size_h[4] = 16.0f;
    t.font_size_h[5] = 14.0f;
    t.font_size_code = 14.0f;

    t.margin_left = 40.0f;
    t.margin_right = 40.0f;
    t.margin_top = 20.0f;
    t.paragraph_spacing = 12.0f;
    t.heading_spacing_above = 24.0f;
    t.heading_spacing_below = 8.0f;
    t.code_block_padding = 12.0f;
    t.indent_width = 24.0f;
    t.blockquote_bar_width = 4.0f;
    t.list_bullet_offset = 20.0f;
    t.hr_thickness = 1.5f;

    t.pane_item_height = 28.0f;
    t.pane_header_height = 32.0f;
    t.splitter_width = 4.0f;
    t.pane_font_size = 13.0f;
}

Theme GetLightTheme() {
    Theme t{};

    t.bg_color = Color(0xFFFFFF);
    t.text_color = Color(0x24292e);
    t.heading_color = Color(0x1a1a1a);
    t.code_bg_color = Color(0xf6f8fa);
    t.code_text_color = Color(0x24292e);
    t.link_color = Color(0x0366d6);
    t.hr_color = Color(0xd0d0d0);
    t.blockquote_bar_color = Color(0xdfe2e5);
    t.blockquote_text_color = Color(0x6a737d);

    // GitHub Alerts（ライト）
    t.alert_color[0] = Color(0x0969da); // Note: 青
    t.alert_color[1] = Color(0x1a7f37); // Tip: 緑
    t.alert_color[2] = Color(0x8250df); // Important: 紫
    t.alert_color[3] = Color(0x9a6700); // Warning: 琥珀
    t.alert_color[4] = Color(0xcf222e); // Caution: 赤
    t.alert_bg_color[0] = Color(0xddf4ff, 0.4f); // Note bg
    t.alert_bg_color[1] = Color(0xdafbe1, 0.4f); // Tip bg
    t.alert_bg_color[2] = Color(0xfbefff, 0.4f); // Important bg
    t.alert_bg_color[3] = Color(0xfff8c5, 0.4f); // Warning bg
    t.alert_bg_color[4] = Color(0xffebe9, 0.4f); // Caution bg

    // シンタックスハイライト
    t.syntax_keyword = Color(0xAF00DB);  // 紫
    t.syntax_type = Color(0x267F99);  // ティール
    t.syntax_string = Color(0xA31515);  // 暗い赤
    t.syntax_number = Color(0x098658);  // 緑
    t.syntax_comment = Color(0x008000);  // 緑
    t.syntax_preprocessor = Color(0x795E26);  // 茶
    t.syntax_function = Color(0x795E26);  // 茶

    ApplyCommonLayout(t);

    // タイトルバー
    t.titlebar_bg_color = Color(0xf0f0f0);
    t.titlebar_text_color = Color(0x333333);
    t.titlebar_button_hover_color = Color(0xe0e0e0);
    t.titlebar_button_active_color = Color(0xd0d0d0);

    // ペインレイアウト
    t.pane_bg_color = Color(0xf5f5f5);
    t.splitter_color = Color(0xe0e0e0);
    t.pane_item_hover_color = Color(0xe8e8e8);
    t.pane_item_active_color = Color(0xd0e0f0);

    return t;
}

Theme GetDarkTheme() {
    Theme t{};

    t.bg_color = Color(0x1e1e1e);
    t.text_color = Color(0xd4d4d4);
    t.heading_color = Color(0xe0e0e0);
    t.code_bg_color = Color(0x2d2d2d);
    t.code_text_color = Color(0xd4d4d4);
    t.link_color = Color(0x569cd6);
    t.hr_color = Color(0x404040);
    t.blockquote_bar_color = Color(0x505050);
    t.blockquote_text_color = Color(0x9e9e9e);

    // GitHub Alerts（ダーク）
    t.alert_color[0] = Color(0x4493f8); // Note: 明るい青
    t.alert_color[1] = Color(0x3fb950); // Tip: 緑
    t.alert_color[2] = Color(0xa371f7); // Important: 紫
    t.alert_color[3] = Color(0xd29922); // Warning: 黄
    t.alert_color[4] = Color(0xf85149); // Caution: 赤
    t.alert_bg_color[0] = Color(0x0d1d31, 0.5f); // Note bg
    t.alert_bg_color[1] = Color(0x0b2212, 0.5f); // Tip bg
    t.alert_bg_color[2] = Color(0x1e0f35, 0.5f); // Important bg
    t.alert_bg_color[3] = Color(0x2a1e02, 0.5f); // Warning bg
    t.alert_bg_color[4] = Color(0x2e0b0d, 0.5f); // Caution bg

    // シンタックスハイライト（VS Code Dark+風）
    t.syntax_keyword = Color(0xC586C0);
    t.syntax_type = Color(0x4EC9B0);
    t.syntax_string = Color(0xCE9178);
    t.syntax_number = Color(0xB5CEA8);
    t.syntax_comment = Color(0x6A9955);
    t.syntax_preprocessor = Color(0xDCDCAA);
    t.syntax_function = Color(0xDCDCAA);

    ApplyCommonLayout(t);

    // タイトルバー
    t.titlebar_bg_color = Color(0x1e1e1e);
    t.titlebar_text_color = Color(0xcccccc);
    t.titlebar_button_hover_color = Color(0x383838);
    t.titlebar_button_active_color = Color(0x444444);

    // ペインレイアウト
    t.pane_bg_color = Color(0x252526);
    t.splitter_color = Color(0x3c3c3c);
    t.pane_item_hover_color = Color(0x2a2d2e);
    t.pane_item_active_color = Color(0x094771);

    return t;
}
