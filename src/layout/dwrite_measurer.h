#pragma once
#include "doc_dwrite_bridge.h"
#include "doc_text.h"
#include "text_measurer.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <memory_resource>
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
                     std::pmr::vector<SyntaxToken>* tokens_out = nullptr,
                     MeasureViewportRange viewport = {}) const override;
    void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width,
                      MeasureViewportRange viewport = {}) const override;

    // 外部のIDWriteFactoryで初期化する（Initの前に呼び出す必要がある）。
    void SetFactory(IDWriteFactory* factory) noexcept
    {
        dwrite_ = factory;
    }

private:
    // ApplyRunFormatting で適用する属性集合。Node 用は CodeBlock/Heading に応じて絞り、
    // テーブルセル用 (ForCell) は全属性 (link underline 含む) を適用する。
    struct RunFormatScope {
        bool apply_code = true;
        bool apply_code_size = true;
        bool apply_link = true;

        static constexpr RunFormatScope ForNode(NodeType t) noexcept
        {
            const bool ac = (t != NodeType::CodeBlock);
            return {
                .apply_code = ac,
                .apply_code_size = ac && (t != NodeType::Heading),
                .apply_link = false,
            };
        }
        static constexpr RunFormatScope ForCell() noexcept
        {
            return {
                .apply_code = true,
                .apply_code_size = true,
                .apply_link = true,
            };
        }
    };

    bool CreateAllFormats();
    IDWriteTextFormat* GetTextFormat(const Node& node) const noexcept;
    void ApplyRunFormatting(IDWriteTextLayout* layout, std::span<const TextRun> runs, const mendo::WideViewForDWrite& view, RunFormatScope scope) const;
    void MeasureTableCells(Node& node, NodeLayoutEntry& entry, std::pmr::vector<float>& natural_widths) const;
    void RestoreNullCellLayouts(Node& node, NodeLayoutEntry& entry, MeasureViewportRange viewport) const;
    void FinalizeTableLayout(Node& node, NodeLayoutEntry& entry, float max_width, size_t col_count, std::pmr::vector<float>& natural_widths) const;

    IDWriteFactory* dwrite_ = nullptr;
    const Theme* theme_ = nullptr;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_body_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_h_[6];
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_code_;
};
