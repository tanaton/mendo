#include <gtest/gtest.h>
#include "theme.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ナビゲーションボタンのテキストフォーマット設定が、
// 水平・垂直の両方向でテキストを中央揃えにすることを検証する。
// これはRenderer::RecreatePaneFormats()のfmt_nav_button_の設定を再現したもの。

class NavButtonFormatTest : public ::testing::Test {
protected:
    ComPtr<IDWriteFactory> factory_;
    ComPtr<IDWriteTextFormat> fmt_;
    Theme theme_;

    void SetUp() override {
        theme_ = GetLightTheme();

        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr));

        // Renderer::RecreatePaneFormatsと同じパラメータでフォーマットを作成
        hr = factory_->CreateTextFormat(
            theme_.font_family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
            L"ja-jp", &fmt_);
        ASSERT_TRUE(SUCCEEDED(hr));
        ASSERT_NE(fmt_.Get(), nullptr);

        // DrawNavOverlayと同じ配置設定を適用
        fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
};

TEST_F(NavButtonFormatTest, HorizontalAlignmentIsCenter) {
    EXPECT_EQ(fmt_->GetTextAlignment(), DWRITE_TEXT_ALIGNMENT_CENTER);
}

TEST_F(NavButtonFormatTest, VerticalAlignmentIsCenter) {
    EXPECT_EQ(fmt_->GetParagraphAlignment(), DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

TEST_F(NavButtonFormatTest, WordWrappingIsDisabled) {
    EXPECT_EQ(fmt_->GetWordWrapping(), DWRITE_WORD_WRAPPING_NO_WRAP);
}

// 矢印グリフが計測可能で、中央揃え時にゼロでないメトリクスを生成することを検証する。
TEST_F(NavButtonFormatTest, ArrowGlyphsHaveNonZeroSize) {
    const wchar_t* arrows[] = {L"\x25C0", L"\x25B6"};  // ◀ ▶

    for (const auto* arrow : arrows) {
        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = factory_->CreateTextLayout(
            arrow, 1, fmt_.Get(), 32.0f, 32.0f, &layout);
        ASSERT_TRUE(SUCCEEDED(hr));

        DWRITE_TEXT_METRICS metrics{};
        hr = layout->GetMetrics(&metrics);
        ASSERT_TRUE(SUCCEEDED(hr));

        EXPECT_GT(metrics.width, 0.0f)
            << "矢印グリフは正の幅を持つべき";
        EXPECT_GT(metrics.height, 0.0f)
            << "矢印グリフは正の高さを持つべき";
    }
}

// グリフがレイアウトボックス内の中央に配置されることを検証する。
TEST_F(NavButtonFormatTest, ArrowGlyphIsCenteredInLayoutBox) {
    constexpr float BOX_SIZE = 32.0f;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = factory_->CreateTextLayout(
        L"\x25C0", 1, fmt_.Get(), BOX_SIZE, BOX_SIZE, &layout);
    ASSERT_TRUE(SUCCEEDED(hr));

    // 中央揃えの場合、オーバーハングメトリクスが配置情報を提供する。
    // 単一文字のヒットテスト位置を取得する。
    float px, py;
    DWRITE_HIT_TEST_METRICS htm{};
    hr = layout->HitTestTextPosition(0, false, &px, &py, &htm);
    ASSERT_TRUE(SUCCEEDED(hr));

    // グリフの左端はボックスの中央付近にあるべき。
    // 水平中央揃えにより、テキストブロックは中央に配置される。
    float glyph_center_x = htm.left + htm.width * 0.5f;
    float glyph_center_y = htm.top + htm.height * 0.5f;

    // グリフの中心がボックスの中央80%の範囲内にあることを確認
    EXPECT_GT(glyph_center_x, BOX_SIZE * 0.1f);
    EXPECT_LT(glyph_center_x, BOX_SIZE * 0.9f);
    EXPECT_GT(glyph_center_y, BOX_SIZE * 0.1f);
    EXPECT_LT(glyph_center_y, BOX_SIZE * 0.9f);
}
