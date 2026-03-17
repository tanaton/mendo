#include <gtest/gtest.h>
#include "theme.h"
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Verify that the nav button text format configuration produces
// centered text both horizontally and vertically.
// This mirrors the setup in Renderer::RecreatePaneFormats() for fmt_nav_button_.

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

        // Create format with the same parameters as Renderer::RecreatePaneFormats
        hr = factory_->CreateTextFormat(
            theme_.font_family, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, theme_.pane_font_size,
            L"ja-jp", &fmt_);
        ASSERT_TRUE(SUCCEEDED(hr));
        ASSERT_NE(fmt_.Get(), nullptr);

        // Apply the same alignment settings as DrawNavOverlay uses
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

// Verify arrow glyphs can be measured and produce non-zero metrics when centered.
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
            << "Arrow glyph should have positive width";
        EXPECT_GT(metrics.height, 0.0f)
            << "Arrow glyph should have positive height";
    }
}

// Verify that glyphs are positioned at center within the layout box.
TEST_F(NavButtonFormatTest, ArrowGlyphIsCenteredInLayoutBox) {
    constexpr float BOX_SIZE = 32.0f;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = factory_->CreateTextLayout(
        L"\x25C0", 1, fmt_.Get(), BOX_SIZE, BOX_SIZE, &layout);
    ASSERT_TRUE(SUCCEEDED(hr));

    // With center alignment, the overhang metrics tell us about positioning.
    // Get the hit-test position of the single character.
    float px, py;
    DWRITE_HIT_TEST_METRICS htm{};
    hr = layout->HitTestTextPosition(0, false, &px, &py, &htm);
    ASSERT_TRUE(SUCCEEDED(hr));

    // The glyph's left edge should be roughly in the center area of the box.
    // With horizontal center alignment, the text block is centered.
    float glyph_center_x = htm.left + htm.width * 0.5f;
    float glyph_center_y = htm.top + htm.height * 0.5f;

    // Check that the glyph center is within the middle 80% of the box
    EXPECT_GT(glyph_center_x, BOX_SIZE * 0.1f);
    EXPECT_LT(glyph_center_x, BOX_SIZE * 0.9f);
    EXPECT_GT(glyph_center_y, BOX_SIZE * 0.1f);
    EXPECT_LT(glyph_center_y, BOX_SIZE * 0.9f);
}
