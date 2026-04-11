#include "renderer.h"

void Renderer::DrawTitleBar(const TitleBarRenderState& tb)
{
    if (tb.height <= 0.0f) {
        return;
    }

    // タイトルバー背景（完全不透明でガラス効果を隠す）
    const D2D1_RECT_F bg_rect = D2D1::RectF(0.0f, 0.0f, tb.window_width, tb.height);
    rt()->FillRectangle(bg_rect, Brush(BrushId::TitleBarBg));

    const float text_alpha = tb.window_active ? 1.0f : 0.5f;

    // アイコンボタン描画ヘルパー
    auto drawButton = [&](const D2D1_RECT_F& rect, const wchar_t* icon, bool show_bg, BrushId bg_id, BrushId text_id, float alpha) {
        if (show_bg) {
            rt()->FillRectangle(rect, Brush(bg_id));
        }
        if (fmt_.titlebar_icon) {
            auto* brush = Brush(text_id);
            if (brush) {
                brush->SetOpacity(alpha);
                rt()->DrawText(icon, 1, fmt_.titlebar_icon.Get(), rect, brush);
                brush->SetOpacity(1.0f);
            }
        }
    };

    // ファイルを開くボタン
    drawButton(
        tb.open_file_btn_rect,
        L"\uE838",
        tb.open_file_btn_hovered,
        BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    // ヘルプボタン
    drawButton(
        tb.help_btn_rect,
        L"\uE897",
        tb.help_btn_hovered,
        BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    // ダークモード切替ボタン（ダーク時: 太陽アイコン、ライト時: 月アイコン）
    drawButton(
        tb.theme_btn_rect,
        tb.is_dark_mode ? L"\uE706" : L"\uE708",
        tb.theme_btn_hovered,
        BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    // 検索ボタン（active > hover の優先度）
    drawButton(
        tb.search_btn_rect,
        L"\uE721",
        tb.search_active || tb.search_btn_hovered,
        tb.search_active ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    // ペイン切替ボタン（active > hover の優先度）
    drawButton(
        tb.file_btn_rect,
        L"\uE8B7",
        tb.file_pane_visible || tb.file_btn_hovered,
        tb.file_pane_visible ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    drawButton(
        tb.toc_btn_rect,
        L"\uE8FD",
        tb.toc_pane_visible || tb.toc_btn_hovered,
        tb.toc_pane_visible ? BrushId::TitleBarButtonActive : BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    // キャプションボタン
    drawButton(
        tb.minimize_btn_rect,
        L"\uE921",
        tb.minimize_btn_hovered,
        BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );

    const wchar_t max_icon[]{ tb.is_maximized ? L'\uE923' : L'\uE922', L'\0' };
    drawButton(
        tb.maximize_btn_rect,
        max_icon,
        tb.maximize_btn_hovered,
        BrushId::TitleBarButtonHover,
        BrushId::TitleBarText,
        text_alpha
    );
    // 閉じるボタン（ホバー時は赤背景＋白アイコン）
    if (tb.close_btn_hovered) {
        drawButton(
            tb.close_btn_rect, L"\uE8BB",
            true,
            BrushId::TitleBarCloseRed,
            BrushId::TitleBarCloseWhite,
            1.0f
        );
    }
    else {
        drawButton(
            tb.close_btn_rect,
            L"\uE8BB",
            false,
            BrushId::TitleBarButtonHover,
            BrushId::TitleBarText,
            text_alpha
        );
    }

    // アプリアイコン
    if (app_icon_bitmap_) {
        const float icon_alpha = tb.window_active ? 1.0f : 0.5f;
        rt()->DrawBitmap(app_icon_bitmap_.Get(), tb.icon_rect, icon_alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // タイトルテキスト
    if (fmt_.titlebar_text && !tb.title_text.empty()) {
        auto* brush = Brush(BrushId::TitleBarText);
        if (brush) {
            brush->SetOpacity(text_alpha);
            rt()->DrawText(
                tb.title_text.data(),
                static_cast<UINT32>(tb.title_text.size()),
                fmt_.titlebar_text.Get(),
                tb.title_text_rect,
                brush
            );
            brush->SetOpacity(1.0f);
        }
    }
}
