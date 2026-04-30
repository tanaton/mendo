#pragma once
// DirectWrite を使うテスト用の共通フィクスチャ。
// MockTextMeasurer 経路では到達できない `text_layout != nullptr` の経路
// （ハイライト矩形、本文 hit test 等）を踏むテストで使用する。
//
// test_helpers.h に DWrite を含めると DWrite 不要のテストにも include コストが
// 載るため、本ヘッダは独立させて使うテストだけが include する設計とする。
#include "test_helpers.h"
#include "dwrite_measurer.h"
#include "layout.h"
#include "layout_cache.h"
#include "parser.h"
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
        ASSERT_TRUE(measurer_.Init(theme_));
        ASSERT_TRUE(engine_.Init(&measurer_, theme_));
    }

    struct ParsedLayout {
        std::pmr::vector<Node> nodes;
        LayoutCache cache;
    };

    // パース → レイアウト計測まで一括実施。多くのテストで定型的に使う。
    ParsedLayout ParseAndLayout(std::wstring_view md, float viewport_w = 800.0f)
    {
        ParsedLayout r;
        r.nodes = ParseMarkdown(md).nodes;
        r.cache.Resize(r.nodes.size());
        engine_.ComputeLayout(r.nodes, r.cache, viewport_w);
        return r;
    }
};
