#pragma once
#include "mermaid_util.h"
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
    // 描けたときの pele の SVG のハッシュ。同じ図を別の幅で描いて同じなら、幅で出力が変わらない図と分かる。
    uint64_t svg_hash = 0;
};

// pele と KaTeX を QuickJS-ng で動かす。図は pele の SVG を DrawingFromPeleSvg で、数式は KaTeX の HTML を LayoutKatexHtml で
// 描画命令にする。どのメソッドもどのスレッドからでも同時に呼べ、同時に動く数だけランタイムを作って使い回す。
// 各ランタイムは使うときにバイトコードを読む。
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
    // ラベルの $$...$$ は KaTeX で描く。pele の出力 (SVG) はテーマに依らないが覚えない。テーマを切り替えるたびに JS で
    // 描き直すが、切り替えのためだけにプロセスの間ずっと最大 4MB を持つより手放すほうをユーザーが選んだ。
    // stale があれば、JS を動かしている間に *generation が value から変わったら止めて Cancelled で返す (幅・テーマ・位置が
    // 変わって古くなった表示用の描画に、ワーカーを使い続けない)。
    struct StaleWhen {
        const std::atomic<uint32_t>* generation = nullptr;
        uint32_t value = 0;
    };
    PeleOutcome Render(std::string_view code, bool dark, float max_width, StaleWhen stale = {});

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
    // 使っていないランタイムを keep 個まで捨てる。残すのは読んだモジュールの多いもので、捨てたものは次にバイトコードから
    // 作り直す。捨てる処理は全オブジェクトをたどって重いので、
    // scheduler があればその worker で行う。
    void Trim(TaskScheduler* scheduler = nullptr, size_t keep = 0);
    // 使っていないランタイムの数 (テスト用)。
    size_t IdleRuntimes();

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
    std::expected<PeleSvg, PeleOutcome> RenderSvg(PeleContext& context, std::string_view code, float max_width, StaleWhen stale);
    // 覚えていなければ nullopt。KaTeX が描けなかった TeX は null を覚えている。
    std::optional<std::shared_ptr<const std::string>> FindHtml(uint64_t key);
    void StoreHtml(uint64_t key, std::shared_ptr<const std::string> html);

    // KaTeX の HTML を覚える上限。溢れると全部捨てるのでズームに効かなくなる。2MB では根号や行列の多い式が約 840 個で
    // 溢れるので、メモリは増えるが 4MB (同じ式で約 1700 個) をユーザーが選んだ。
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
    // TeX のハッシュ → KaTeX の HTML。KaTeX に渡すのは TeX だけで、文字の大きさは配置にしか使わないので、ズームや
    // 見出しと本文の同じ式では JS を走らせずに配置だけをやり直す (根号や行列の多い 707 式で 650 → 32ms。BM_MathRender)。
    std::mutex html_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<const std::string>> htmls_;
    size_t html_bytes_ = 0;
};
