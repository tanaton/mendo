#pragma once
#include "mermaid_util.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
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
// pele と KaTeX はどちらも最初に 1 回だけコンパイルし (約 60ms と約 20ms)、各ランタイムは使うときにバイトコードを読む (約 5ms と約 3ms)。
class PeleEngine {
public:
    // pele は pele.mjs、katex は katex.min.js の本文、katex_fonts は KaTeX の書体の woff2 (kKatexFontFiles の順)。
    // どれもこのオブジェクトより長く生かす (埋め込みリソース)。
    PeleEngine(std::span<const std::byte> pele, std::span<const std::byte> katex, std::vector<std::span<const std::byte>> katex_fonts,
               IDWriteFactory* dw);
    ~PeleEngine();
    PeleEngine(const PeleEngine&) = delete;
    PeleEngine& operator=(const PeleEngine&) = delete;

    // locale は本文のロケールで、日本語などのフォールバック先を決める。
    // fit は本文と同じく max_width まで縮める。拡大表示とクリップボードは縮めずに図の大きさのまま描く。
    // ラベルの $$...$$ は KaTeX で描く。
    PeleOutcome Render(std::string_view code, bool dark, float max_width, std::wstring_view locale, bool fit = true);

    struct MathSource {
        std::string_view tex;
        float font_size = 0.0f; // 数式を置く場所の文字の大きさ (CSS px)
    };
    // インライン数式をまとめて描く。同じ順で返し、KaTeX が描けない数式 (TeX の誤り) と取り消した後の数式は null。
    // locale は KaTeX の書体に無い文字のフォールバック先で、最初に数式を描くときのものを使い続ける。
    std::vector<std::shared_ptr<const MathDrawing>> RenderMaths(std::span<const MathSource> maths, std::wstring_view locale);
    // ディスクキャッシュから読んだ図の描画命令には TeX だけがあるので、ラベルの数式を組み直す。組めない数式は null のまま。
    // 組み直す数式があって、ランタイムを使ったら true。
    bool ResolveMaths(mermaid_util::DiagramDrawing& drawing, std::wstring_view locale);
    // KaTeX の HTML を配置する。JS を動かさないので、pele の配置の途中 (math オプション) からも呼べる。
    std::shared_ptr<const MathDrawing> LayoutHtml(std::string_view html, float font_size, std::wstring_view locale);

    // 描画中のものを止め、以後の Render と RenderMaths を Cancelled・null で返す。終了時に、描き終わりを待つ前に呼ぶ。
    void Cancel() noexcept
    {
        cancel_.store(true, std::memory_order_relaxed);
    }
    bool Cancelled() const noexcept
    {
        return cancel_.load(std::memory_order_relaxed);
    }
    // 使っていないランタイムを捨てる。次はバイトコードから作り直す。
    void Trim();

private:
    // context で KaTeX を動かし、数式 1 つを配置する。KaTeX は JS のスタックを深く使うので、pele の配置の途中では呼ばない。
    std::shared_ptr<const MathDrawing> LayoutMath(PeleContext& context, std::string_view tex, float font_size, std::wstring_view locale);
    std::unique_ptr<PeleContext> Acquire();
    void Release(std::unique_ptr<PeleContext> context);
    bool EnsurePele(PeleContext& context);
    bool EnsureKatex(PeleContext& context);
    KatexAssets* Assets(std::wstring_view locale);

    std::span<const std::byte> pele_source_;
    std::span<const std::byte> katex_source_;
    std::vector<std::span<const std::byte>> katex_fonts_;
    IDWriteFactory* dw_;
    std::atomic<bool> cancel_{ false };
    std::once_flag pele_compiled_;
    std::vector<uint8_t> pele_bytecode_;
    std::once_flag katex_compiled_;
    std::vector<uint8_t> katex_bytecode_;
    std::once_flag assets_created_;
    std::unique_ptr<KatexAssets> assets_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<PeleContext>> idle_;
};
