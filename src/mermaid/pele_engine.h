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

struct IDWriteFactory;
struct PeleContext;

struct PeleOutcome {
    enum class Status : uint8_t {
        Drawn,
        Error,        // コードの誤り。error に pele の文言
        NeedsBrowser, // 描画命令にできない要素 (<foreignObject> など) があるか、エンジンを用意できなかった
        Cancelled,
    };
    Status status = Status::NeedsBrowser;
    mermaid_util::DiagramDrawing drawing; // size.dpr は 0 のまま
    std::wstring error;
};

// pele を QuickJS-ng で動かし、SVG を DrawingFromPeleSvg で描画命令にする。WebView2 を使わない。
// Render はどのスレッドからでも同時に呼べ、同時に動く数だけランタイム (1 つ約 2.9MB) を作って使い回す。
// pele は最初に 1 回だけコンパイルし (約 60ms)、各ランタイムはそのバイトコードを読む (約 5ms)。
class PeleEngine {
public:
    // source は pele.mjs の本文で、このオブジェクトより長く生かす (埋め込みリソース)。
    PeleEngine(std::span<const std::byte> source, IDWriteFactory* dw);
    ~PeleEngine();
    PeleEngine(const PeleEngine&) = delete;
    PeleEngine& operator=(const PeleEngine&) = delete;

    // ラベルの $$...$$ は KaTeX の MathML で描くので、ブラウザに任せる (math オプションを渡していない)。
    static bool CanRender(std::string_view code) noexcept
    {
        return code.find("$$") == std::string_view::npos;
    }
    // locale は本文のロケールで、日本語などのフォールバック先を決める (mermaid.html の fonts と同じ)。
    // fit は本文と同じく max_width まで縮める。拡大表示とクリップボードは縮めずに図の大きさのまま描く。
    PeleOutcome Render(std::string_view code, bool dark, float max_width, std::wstring_view locale, bool fit = true);
    // 描画中のものを止め、以後の Render を Cancelled で返す。終了時に、描き終わりを待つ前に呼ぶ。
    void Cancel() noexcept
    {
        cancel_.store(true, std::memory_order_relaxed);
    }
    // 使っていないランタイムを捨てる。次の Render はバイトコードから作り直す。
    void Trim();

private:
    std::unique_ptr<PeleContext> Acquire();
    std::unique_ptr<PeleContext> CreateContext();
    void CompileOnce();

    std::span<const std::byte> source_;
    IDWriteFactory* dw_;
    std::atomic<bool> cancel_{ false };
    std::once_flag compiled_;
    std::vector<uint8_t> bytecode_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<PeleContext>> idle_;
};
