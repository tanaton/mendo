#pragma once
#include "diagram_raster.h"
#include "example_files.h"
#include "pele_engine.h"
#include <algorithm>
#include <filesystem>
#include <format>
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

// 止めなければ描き終えるまで何秒もかかる図。描いている途中の取り消しを試す。段の間を全部つなぐ (辺は pele の上限 5000 の内)。
// 一列の鎖は字数の上限 (5 万字) いっぱいでもすぐ描き終え、取り消しが少し遅れると間に合わない。
inline std::string SlowFlowchart()
{
    constexpr int kPerRow = 20;
    constexpr int kRows = 12;
    const auto row = [](int r) {
        std::string nodes = std::format("N{}_0", r);
        for (int i = 1; i < kPerRow; ++i) {
            nodes += std::format(" & N{}_{}", r, i);
        }
        return nodes;
    };
    std::string code = "flowchart TD\n";
    for (int r = 1; r < kRows; ++r) {
        code += std::format("  {} --> {}\n", row(r - 1), row(r));
    }
    return code;
}
