#pragma once
#include "diagram_raster.h"
#include "example_files.h"
#include "pele_engine.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>

// ビルドで作った pele と KaTeX のバイトコードと、KaTeX の書体 (テストとベンチの exe は埋め込みリソースを持たない)。
// PeleEngine はバイト列を写さずに参照するので、作ったエンジンより長く生かす。
struct PeleTestAssets {
    std::string pele;
    std::string katex;
    // kKatexFontFiles の順。
    std::vector<std::string> fonts;

    static PeleTestAssets Load()
    {
        const std::filesystem::path bytecode(MENDO_JS_BYTECODE_DIR);
        const auto font_dir = std::filesystem::path(MENDO_THIRD_PARTY_DIR) / "katex" / "fonts";
        PeleTestAssets assets{ .pele = ReadFileBytes(bytecode / "pele.qbc"), .katex = ReadFileBytes(bytecode / "katex.qbc") };
        for (const auto name : kKatexFontFiles) {
            assets.fonts.push_back(ReadFileBytes(font_dir / (std::wstring{ name } + L".woff2")));
        }
        return assets;
    }
    // どれも読めた。
    bool ok() const noexcept
    {
        return !pele.empty() && !katex.empty() && std::ranges::none_of(fonts, [](const std::string& f) { return f.empty(); });
    }
    std::unique_ptr<PeleEngine> NewEngine(IDWriteFactory* dw) const
    {
        auto spans = fonts | std::views::transform([](const std::string& f) { return std::as_bytes(std::span{ f }); }) | std::ranges::to<std::vector>();
        return std::make_unique<PeleEngine>(std::as_bytes(std::span{ pele }), std::as_bytes(std::span{ katex }), std::move(spans), dw, L"ja-jp");
    }
};
