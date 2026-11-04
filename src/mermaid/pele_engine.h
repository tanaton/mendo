#pragma once
#include "mermaid_util.h"
#include "transposition_cache.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class KatexAssets;
struct IDWriteFactory;
struct MathDrawing;
struct PeleContext;
struct PeleSvg;

struct PeleOutcome {
    enum class Status : uint8_t {
        Drawn,
        // コードの誤りなら error に pele の文言。エンジンを用意できなかったときは error が空で、図のせいにしない。
        Error,
        Unsupported, // 描画命令にできない要素 (<foreignObject> など) がある
        Cancelled,
    };
    Status status = Status::Error;
    mermaid_util::DiagramDrawing drawing;
    std::wstring error;
};

// pele と KaTeX を QuickJS-ng で動かす。図は pele の SVG を DrawingFromPeleSvg で、数式は KaTeX の HTML を LayoutKatexHtml で
// 描画命令にする。
// どのメソッドもどのスレッドからでも同時に呼べ、同時に動く数だけランタイム (pele で約 2.9MB、KaTeX で約 2.6MB) を作って使い回す。
// 各ランタイムは使うときにバイトコードを読む (約 5ms と約 3ms)。
class PeleEngine {
public:
    // pele と katex は pele.mjs と katex.min.js をビルドの時にコンパイルしたバイトコード (js_compile)、katex_fonts は KaTeX の
    // 書体の woff2 (kKatexFontFiles の順)。どれもこのオブジェクトより長く生かす (埋め込みリソース)。
    // locale は本文のロケールで、日本語などのフォールバック先を決める。プロセスの間は変わらない (i18n::TextLocale)。
    PeleEngine(std::span<const std::byte> pele, std::span<const std::byte> katex, std::vector<std::span<const std::byte>> katex_fonts,
               IDWriteFactory* dw, std::wstring locale);
    ~PeleEngine();
    PeleEngine(const PeleEngine&) = delete;
    PeleEngine& operator=(const PeleEngine&) = delete;

    // fit は本文と同じく max_width まで縮める。拡大表示とクリップボードは縮めずに図の大きさのまま描く。
    // ラベルの $$...$$ は KaTeX で描く。
    // pele の出力 (SVG とラベルの数式) はテーマにも fit にも依らないので覚えておき、テーマの切り替えや縮めた図の拡大表示では
    // JS を走らせずに解き直すだけにする (重い図は 1 回 0.8 秒ほど、解くのは平均 0.2ms)。
    PeleOutcome Render(std::string_view code, bool dark, float max_width, bool fit = true);

    struct MathSource {
        std::string_view tex;
        float font_size = 0.0f; // 数式を置く場所の文字の大きさ (CSS px)
    };
    // インライン数式をまとめて描く。同じ順で返し、KaTeX が描けない数式 (TeX の誤り) と取り消した後の数式は null。
    std::vector<std::shared_ptr<const MathDrawing>> RenderMaths(std::span<const MathSource> maths);
    // ディスクキャッシュから読んだ図の描画命令には TeX だけがあるので、ラベルの数式を組み直す。組めない数式は null のまま。
    // 組み直す数式があって、ランタイムを使ったら true。
    bool ResolveMaths(mermaid_util::DiagramDrawing& drawing);
    // KaTeX の HTML を配置する。JS を動かさないので、pele の配置の途中 (math オプション) からも呼べる。
    std::shared_ptr<const MathDrawing> LayoutHtml(std::string_view html, float font_size);

    // 描画中のものを止め、以後の Render と RenderMaths を Cancelled・null で返す。終了時に、描き終わりを待つ前に呼ぶ。
    void Cancel() noexcept
    {
        cancel_.store(true, std::memory_order_relaxed);
    }
    // 使っていないランタイムを捨てる。次はバイトコードから作り直す。
    void Trim();

private:
    bool Cancelled() const noexcept
    {
        return cancel_.load(std::memory_order_relaxed);
    }
    // context で KaTeX を動かし、数式 1 つを配置する。KaTeX は JS のスタックを深く使うので、pele の配置の途中では呼ばない。
    std::shared_ptr<const MathDrawing> LayoutMath(PeleContext& context, std::string_view tex, float font_size);
    std::unique_ptr<PeleContext> Acquire();
    void Release(std::unique_ptr<PeleContext> context);
    bool EnsureRuntime(PeleContext& context);
    bool EnsurePele(PeleContext& context);
    bool EnsureKatex(PeleContext& context);
    KatexAssets* Assets();
    // pele で SVG を描く。描けなければ理由を返す。
    std::expected<std::shared_ptr<const PeleSvg>, PeleOutcome> RenderSvg(PeleContext& context, std::string_view code, float max_width);
    std::shared_ptr<const PeleSvg> FindSvg(uint64_t key);
    void StoreSvg(uint64_t key, std::shared_ptr<const PeleSvg> svg);

    // 図の数は文書の図の数ほどで足りる。大きさは pele の SVG の合計で抑える (図 1 個は数 KB〜数十 KB)。
    static constexpr size_t kMaxSvgEntries = 64;
    static constexpr size_t kMaxSvgBytes = 4u * 1024 * 1024;

    std::span<const std::byte> pele_bytecode_;
    std::span<const std::byte> katex_bytecode_;
    std::vector<std::span<const std::byte>> katex_fonts_;
    IDWriteFactory* dw_;
    const std::wstring locale_;
    std::atomic<bool> cancel_{ false };
    std::once_flag assets_created_;
    std::unique_ptr<KatexAssets> assets_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<PeleContext>> idle_;
    // (コード, 幅の上限) → pele の出力。
    std::mutex svg_mutex_;
    TranspositionCache<uint64_t, std::shared_ptr<const PeleSvg>, kMaxSvgEntries> svgs_;
};
