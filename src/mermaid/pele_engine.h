#pragma once
#include "mermaid_util.h"
#include "transposition_cache.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class KatexAssets;
class TaskScheduler;
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

    // max_width は pele が折り返しと並べ方を決める幅の上限。描画命令は縮めない大きさで、上限より広いことがある。
    // ラベルの $$...$$ は KaTeX で描く。
    // pele の出力 (SVG とラベルの数式) はテーマに依らないので覚えておき、テーマの切り替えでは JS を走らせずに解き直すだけにする
    // (重い図は 1 回 0.8 秒ほど、解くのは平均 0.2ms)。
    PeleOutcome Render(std::string_view code, bool dark, float max_width);

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
    // 使っていないランタイムを捨てる。次はバイトコードから作り直す。捨てる処理は全オブジェクトをたどり、pele と KaTeX を
    // 読んだランタイム 1 個で約 3.5ms かかるので、scheduler があればその worker で行う。
    void Trim(TaskScheduler* scheduler = nullptr);

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
    // 覚えていなければ nullopt。KaTeX が描けなかった TeX は null を覚えている。
    std::optional<std::shared_ptr<const std::string>> FindHtml(uint64_t key);
    void StoreHtml(uint64_t key, std::shared_ptr<const std::string> html);

    // 図の数は文書の図の数ほどで足りる。大きさは pele の SVG の合計で抑える (図 1 個は数 KB〜数十 KB)。
    static constexpr size_t kMaxSvgEntries = 64;
    static constexpr size_t kMaxSvgBytes = 4u * 1024 * 1024;
    // KaTeX の HTML は 1 式 0.5〜2.2KB (分数や添字だけの式〜根号・行列・積分の式)。数式 978 個 (HTML 0.54MB) の文書を
    // 最後まで読むと commit +0.8MB (1.8%)・private WS +0.8MB (3.5%)、963 個 (2.16MB) で +2.6MB (5.3%)・+2.5MB (9.7%)。
    // 実際の増分は数えた量の約 1.15〜1.2 倍で、上限いっぱいだと commit 約 9%・private WS 約 17% 増える。private WS の 1 割に
    // 収まるのは 2MB だが、根号や行列の多い式は約 840 個で溢れ、溢れると全部捨てるのでズームに効かなくなる。4MB (同じ式で
    // 約 1700 個、軽い式で約 6000 個) はユーザーが選んだ。
    static constexpr size_t kMaxHtmlBytes = 4u * 1024 * 1024;
    // 表の節・文字列の本体・共有の管理ブロックの見込み。描けない TeX (HTML なし) だけでも表が膨らみ続けないように数える。
    static constexpr size_t kHtmlEntryBytes = 128;

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
    // TeX のハッシュ → KaTeX の HTML。KaTeX に渡すのは TeX だけで、文字の大きさは配置にしか使わないので、ズームや
    // 見出しと本文の同じ式では JS を走らせずに配置だけをやり直す (根号や行列の多い 707 式で 650 → 32ms。BM_MathRender)。
    std::mutex html_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<const std::string>> htmls_;
    size_t html_bytes_ = 0;
};
