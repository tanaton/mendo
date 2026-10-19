#pragma once
// DirectWrite を使うテスト用の共通フィクスチャ。
// MockTextMeasurer 経路では到達できない `text_layout != nullptr` の経路
// （ハイライト矩形、本文 hit test 等）を踏むテストで使用する。
//
// test_helpers.h に DWrite を含めると DWrite 不要のテストにも include コストが
// 載るため、本ヘッダは独立させて使うテストだけが include する設計とする。
#include "test_helpers.h"
#include "dwrite_measurer.h"
#include "parsed_layout.h"
#include "theme.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <iomanip>
#include <ios>

class DWriteTestBase : public ComApartmentTest {
protected:
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    DWriteTextMeasurer measurer_;
    Theme theme_;
    LayoutEngine engine_;

    void SetUp() override
    {
        const HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
        ASSERT_TRUE(SUCCEEDED(hr))
            << "DWriteCreateFactory failed: 0x" << std::hex << hr;

        theme_ = GetLightTheme();
        measurer_.SetFactory(dwrite_factory_.Get());
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
    }

    // 範囲は ComputeLayout と同じ。省略すると全ノードを計測する。
    ParsedLayout ParseAndLayout(std::string_view md, float viewport_w = 800.0f,
                                float viewport_top = -MeasureViewportRange::kInf, float viewport_bottom = MeasureViewportRange::kInf)
    {
        return sources_.ParseAndLayout(engine_, md, viewport_w, viewport_top, viewport_bottom);
    }

    // 本文フォントの標準スタイルの書式。作れなければ null。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> CreateThemeFormat(float size) const
    {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        dwrite_factory_->CreateTextFormat(
            theme_.font_family.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size,
            L"ja-jp", &fmt);
        return fmt;
    }

private:
    LayoutSourceStore sources_;
};
