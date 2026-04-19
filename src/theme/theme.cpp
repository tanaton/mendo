#include "theme.h"
#include "theme_palette.h"

static D2D1_COLOR_F Color(uint32_t rgb, float a = 1.0f) noexcept
{
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.0f,
        ((rgb >> 8) & 0xFF) / 255.0f,
        (rgb & 0xFF) / 255.0f,
        a
    );
}

float Theme::GetHeadingSize(int level) const noexcept
{
    if (level < 1 || level > 6) {
        return font_size_body;
    }
    return font_size_h[level - 1];
}

ThemeConstants Theme::ToReducerConstants() const noexcept
{
    return ThemeConstants{
        .pane_item_height = pane_item_height,
        .pane_header_height = pane_header_height,
        .splitter_width = splitter_width,
        .margin_left = margin_left,
        .margin_right = margin_right,
        .heading_spacing_above = heading_spacing_above,
        .zoom = zoom,
        .is_dark = IsDark(),
    };
}

void Theme::ApplyZoom(float new_zoom) noexcept
{
    if (new_zoom <= 0.0f || zoom <= 0.0f) {
        return;
    }
    // 前のズームを元に戻してから新しいズームを適用する
    const float ratio = new_zoom / zoom;
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
    list_item_spacing *= ratio;
    heading_spacing_above *= ratio;
    heading_spacing_below *= ratio;
    heading_spacing_below_h1h2 *= ratio;
    code_block_spacing_above *= ratio;
    code_block_padding *= ratio;
    indent_width *= ratio;
    blockquote_bar_width *= ratio;
    list_bullet_offset *= ratio;
    hr_thickness *= ratio;
    h2_underline_thickness *= ratio;

    // ペインサイズ
    pane_item_height *= ratio;
    pane_header_height *= ratio;
    splitter_width *= ratio;
    pane_font_size *= ratio;
}

// ライトテーマとダークテーマで共有するレイアウト定数
static void ApplyCommonLayout(Theme& t)
{
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
    t.margin_top = 40.0f;
    t.paragraph_spacing = 12.0f;
    t.list_item_spacing = 6.0f;
    t.heading_spacing_above = 12.0f;
    t.heading_spacing_below = 10.0f;
    t.heading_spacing_below_h1h2 = 16.0f;
    t.code_block_spacing_above = 8.0f;
    t.code_block_padding = 12.0f;
    t.indent_width = 24.0f;
    t.blockquote_bar_width = 4.0f;
    t.list_bullet_offset = 20.0f;
    t.hr_thickness = 1.5f;
    t.h2_underline_thickness = 1.0f;

    t.pane_item_height = 28.0f;
    t.pane_header_height = 32.0f;
    t.splitter_width = 4.0f;
    t.pane_font_size = 13.0f;
}

Theme GetLightTheme()
{
    Theme t{};

    t.bg_color = Color(0xFFFFFF);
    t.text_color = Color(0x24292e);
    t.heading_color = Color(0x1a1a1a);
    t.code_bg_color = Color(theme_palette::kLight.code_bg);
    t.code_text_color = Color(theme_palette::kLight.code_text);
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
    t.syntax_keyword = Color(theme_palette::kLight.syntax_keyword);
    t.syntax_type = Color(theme_palette::kLight.syntax_type);
    t.syntax_string = Color(theme_palette::kLight.syntax_string);
    t.syntax_number = Color(theme_palette::kLight.syntax_number);
    t.syntax_comment = Color(theme_palette::kLight.syntax_comment);
    t.syntax_preprocessor = Color(theme_palette::kLight.syntax_preprocessor);
    t.syntax_function = Color(theme_palette::kLight.syntax_function);

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

    // 検索
    t.search_bar_bg_color = Color(0xf0f0f0);
    t.search_bar_border_color = Color(0xcccccc);
    t.search_input_bg_color = Color(0xffffff);
    t.search_input_text_color = Color(0x1a1a1a);
    t.search_highlight_color = D2D1::ColorF(1.0f, 0.92f, 0.0f, 0.4f);
    t.search_highlight_current_color = D2D1::ColorF(1.0f, 0.55f, 0.0f, 0.6f);
    t.search_no_match_bg_color = D2D1::ColorF(1.0f, 0.8f, 0.8f, 1.0f);

    return t;
}

Theme GetDarkTheme()
{
    Theme t{};

    t.bg_color = Color(0x1e1e1e);
    t.text_color = Color(0xd4d4d4);
    t.heading_color = Color(0xe0e0e0);
    t.code_bg_color = Color(theme_palette::kDark.code_bg);
    t.code_text_color = Color(theme_palette::kDark.code_text);
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
    t.syntax_keyword = Color(theme_palette::kDark.syntax_keyword);
    t.syntax_type = Color(theme_palette::kDark.syntax_type);
    t.syntax_string = Color(theme_palette::kDark.syntax_string);
    t.syntax_number = Color(theme_palette::kDark.syntax_number);
    t.syntax_comment = Color(theme_palette::kDark.syntax_comment);
    t.syntax_preprocessor = Color(theme_palette::kDark.syntax_preprocessor);
    t.syntax_function = Color(theme_palette::kDark.syntax_function);

    ApplyCommonLayout(t);

    // タイトルバー
    t.titlebar_bg_color = Color(0x272727);
    t.titlebar_text_color = Color(0xcccccc);
    t.titlebar_button_hover_color = Color(0x383838);
    t.titlebar_button_active_color = Color(0x444444);

    // ペインレイアウト
    t.pane_bg_color = Color(0x252526);
    t.splitter_color = Color(0x3c3c3c);
    t.pane_item_hover_color = Color(0x2a2d2e);
    t.pane_item_active_color = Color(0x094771);

    // 検索
    t.search_bar_bg_color = Color(0x2d2d2d);
    t.search_bar_border_color = Color(0x444444);
    t.search_input_bg_color = Color(0x3c3c3c);
    t.search_input_text_color = Color(0xd4d4d4);
    t.search_highlight_color = D2D1::ColorF(0.8f, 0.7f, 0.0f, 0.35f);
    t.search_highlight_current_color = D2D1::ColorF(1.0f, 0.5f, 0.0f, 0.55f);
    t.search_no_match_bg_color = D2D1::ColorF(0.5f, 0.15f, 0.15f, 1.0f);

    return t;
}
