#pragma once
#include "doc_dwrite_bridge.h"
#include "doc_text.h"
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <memory_resource>
#include <optional>
#include <span>


// ITextMeasurer の DirectWrite 実装。IDWriteTextFormat (重い再利用可能オブジェクト) を
// theme ごとに所有し、計測のたびに per-call で IDWriteTextLayout を生成する分業構造。
// Format の再生成は RecreateFormats 経由のみ許可し、並列計測中の同時更新を契約で禁止する。
class DWriteTextMeasurer : public ITextMeasurer {
public:
    bool Init(const Theme& theme) override;
    bool RecreateFormats() override;
    void UpdateTheme(const Theme& theme) noexcept override
    {
        theme_ = &theme;
    }

    void MeasureNode(Node& node, NodeLayoutEntry& entry, float max_width,
                     std::pmr::vector<SyntaxToken>* tokens_out = nullptr) const override;
    void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) const override;

    // 外部のIDWriteFactoryで初期化する（Initの前に呼び出す必要がある）。
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        dwrite_ = factory;
    }

private:
    bool CreateAllFormats();
    IDWriteTextFormat* GetTextFormat(const Node& node) const noexcept;
    void ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, const mendo::WideViewForDWrite& view, std::optional<NodeType> node_type) const;
    void MeasureTableCells(Node& node, NodeLayoutEntry& entry, std::pmr::vector<float>& natural_widths) const;
    void RestoreNullCellLayouts(Node& node, NodeLayoutEntry& entry) const;
    void FinalizeTableLayout(Node& node, NodeLayoutEntry& entry, float max_width, size_t col_count, std::pmr::vector<float>& natural_widths) const;

    IDWriteFactory* dwrite_ = nullptr;
    const Theme* theme_ = nullptr;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_body_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_h_[6];
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_code_;
};
