#include "pele_engine.h"
#include "diagram_raster.h"
#include "string_convert.h"
#include "utility.h"
#include <quickjs.h>
#include <bit>
#include <format>
#include <optional>
#include <string>
#include <utility>

using namespace std::literals;

namespace {

// QuickJS のスタックの上限。ワーカーは既定の 1MB のスタックで動く。pele は 60 段の入れ子の subgraph や
// 1000 ノードの鎖でも 512KB で足りた。残りは測定器から呼ぶ DirectWrite に残す。
constexpr size_t kJsStackBytes = 512 * 1024;
// 壊れたコードで止まらずに確保し続けても、プロセスを道連れにしない。
constexpr size_t kJsMemoryBytes = 256 * 1024 * 1024;

// 本文と同じ書体で組み、Segoe UI に無い文字はロケールで決まるフォントにする。
struct DiagramFonts {
    std::wstring_view sans;
    std::wstring_view mono;
};

constexpr DiagramFonts kJaFonts{ L"\"Segoe UI\", \"Yu Gothic UI\"", L"\"Cascadia Mono\", Consolas, \"Courier New\", \"Yu Gothic UI\"" };
constexpr DiagramFonts kZhHansFonts{ L"\"Segoe UI\", \"Microsoft YaHei UI\"",
                                     L"\"Cascadia Mono\", Consolas, \"Courier New\", \"Microsoft YaHei UI\"" };
constexpr DiagramFonts kZhHantFonts{ L"\"Segoe UI\", \"Microsoft JhengHei UI\"",
                                     L"\"Cascadia Mono\", Consolas, \"Courier New\", \"Microsoft JhengHei UI\"" };
constexpr DiagramFonts kKoFonts{ L"\"Segoe UI\", \"Malgun Gothic\"", L"\"Cascadia Mono\", Consolas, \"Courier New\", \"Malgun Gothic\"" };

const DiagramFonts& FontsFor(std::wstring_view locale) noexcept
{
    if (locale == L"zh-cn"sv) {
        return kZhHansFonts;
    }
    if (locale == L"zh-tw"sv || locale == L"zh-hk"sv) {
        return kZhHantFonts;
    }
    if (locale == L"ko-kr"sv) {
        return kKoFonts;
    }
    return kJaFonts;
}

JSValue NewString(JSContext* ctx, std::wstring_view s)
{
    return JS_NewStringUTF16(ctx, reinterpret_cast<const uint16_t*>(s.data()), s.size());
}

std::wstring ErrorText(JSContext* ctx, JSValueConst error)
{
    JSValue message = JS_GetPropertyStr(ctx, error, "message");
    const char* s = JS_ToCString(ctx, JS_IsString(message) ? message : error);
    std::wstring out = s ? string_convert::Utf8ToWide(s) : std::wstring{};
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, message);
    return out.empty() ? L"pele error"s : out;
}

} // namespace

struct PeleSvg {
    std::string svg;
    std::vector<PeleMathLabel> labels;
};

struct PeleContext {
    PeleEngine* engine = nullptr;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    // pele を読んだら (code, maxWidth, fontFamily, fontFamilyMono, withMath) → SVG
    JSValue render = JS_UNDEFINED;
    // KaTeX を読んだら (tex) → HTML
    JSValue katex = JS_UNDEFINED;
    const DiagramFonts* fonts = &kJaFonts;
    // 図を描くときに作る (数式だけのランタイムは測定器を使わない)。
    std::optional<DiagramTextMeasurer> measurer;
    // 描いている図のラベルの数式。pele の math オプションが足し、DrawingFromPeleSvg が読む。
    std::vector<PeleMathLabel> labels;

    PeleContext() = default;
    PeleContext(const PeleContext&) = delete;
    PeleContext& operator=(const PeleContext&) = delete;
    ~PeleContext()
    {
        if (ctx) {
            JS_FreeValue(ctx, render);
            JS_FreeValue(ctx, katex);
            JS_FreeContext(ctx);
        }
        if (rt) {
            JS_FreeRuntime(rt);
        }
    }
};

namespace {

// pele の measurer.width(text, size, style)。length 3 で登録するので、引数は 3 つそろって届く。
// style は Bold=1 Italic=2 Mono=4 Sub=8 Sup=16 で、下付き・上付きは canvas の測定器と同じく 1.2 で割った大きさで測る。
JSValue MeasureWidth(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
    auto* context = static_cast<PeleContext*>(JS_GetContextOpaque(ctx));
    size_t length = 0;
    const uint16_t* text = JS_ToCStringLenUTF16(ctx, &length, argv[0]);
    if (!text) {
        return JS_EXCEPTION;
    }
    double size = 0.0;
    int32_t style = 0;
    if (JS_ToFloat64(ctx, &size, argv[1]) < 0 || JS_ToInt32(ctx, &style, argv[2]) < 0) {
        JS_FreeCStringUTF16(ctx, text);
        return JS_EXCEPTION;
    }
    const DiagramFont font{
        .family = (style & 4) ? context->fonts->mono : context->fonts->sans,
        .size = static_cast<float>((style & 24) ? size / 1.2 : size),
        .weight = static_cast<uint16_t>((style & 1) ? 700 : 400),
        .italic = (style & 2) != 0,
    };
    const float width = context->measurer->Width({ reinterpret_cast<const wchar_t*>(text), length }, font);
    JS_FreeCStringUTF16(ctx, text);
    return JS_NewFloat64(ctx, width);
}

// pele の math オプションから呼ぶ layoutMath(tex, html, size)。html は先に KaTeX で作っておいたもの。
// 描けた数式は {svg, width, height} を返し、svg には番号だけを書く (DrawingFromPeleSvg が置き換える)。
// 描けなければ undefined で、pele は TeX をそのまま文字で書く。
JSValue MathLabel(JSContext* ctx, JSValueConst, int, JSValueConst* argv)
{
    auto* context = static_cast<PeleContext*>(JS_GetContextOpaque(ctx));
    size_t tex_length = 0;
    size_t html_length = 0;
    const char* tex = JS_ToCStringLen(ctx, &tex_length, argv[0]);
    const char* html = tex ? JS_ToCStringLen(ctx, &html_length, argv[1]) : nullptr;
    double size = 0.0;
    JSValue out = JS_EXCEPTION;
    if (html && JS_ToFloat64(ctx, &size, argv[2]) == 0) {
        out = JS_UNDEFINED;
        if (auto math = context->engine->LayoutHtml({ html, html_length }, static_cast<float>(size))) {
            const auto svg = std::format("<g data-mendo-math=\"{}\"/>", context->labels.size());
            out = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, out, "svg", JS_NewStringLen(ctx, svg.data(), svg.size()));
            JS_SetPropertyStr(ctx, out, "width", JS_NewFloat64(ctx, math->width));
            JS_SetPropertyStr(ctx, out, "height", JS_NewFloat64(ctx, math->height + math->depth));
            context->labels.push_back({ .tex = std::string{ tex, tex_length }, .font_size = static_cast<float>(size), .drawing = std::move(math) });
        }
    }
    JS_FreeCString(ctx, html);
    JS_FreeCString(ctx, tex);
    return out;
}

int CheckCancel(JSContext*, void* cancel)
{
    return static_cast<const std::atomic<bool>*>(cancel)->load(std::memory_order_relaxed) ? 1 : 0;
}

// 評価の結果を確かめる。モジュールの評価は Promise を返すので、ジョブを流して確定させる (pele も KaTeX も最上位に await が無い)。
bool Evaluated(PeleContext& context, JSValue evaluated)
{
    for (JSContext* job_ctx = nullptr; JS_ExecutePendingJob(context.rt, &job_ctx) > 0;) {
    }
    const bool ok = !JS_IsException(evaluated) && (!JS_IsPromise(evaluated) || JS_PromiseState(context.ctx, evaluated) == JS_PROMISE_FULFILLED);
    JS_FreeValue(context.ctx, evaluated);
    if (!ok) {
        JS_FreeValue(context.ctx, JS_GetException(context.ctx));
    }
    return ok;
}

} // namespace

PeleEngine::PeleEngine(std::span<const std::byte> pele, std::span<const std::byte> katex, std::vector<std::span<const std::byte>> katex_fonts,
                       IDWriteFactory* dw, std::wstring locale)
    : pele_bytecode_(pele), katex_bytecode_(katex), katex_fonts_(std::move(katex_fonts)), dw_(dw), locale_(std::move(locale))
{
}

PeleEngine::~PeleEngine() = default;

std::unique_ptr<PeleContext> PeleEngine::Acquire()
{
    {
        const std::lock_guard lock(mutex_);
        if (!idle_.empty()) {
            auto context = std::move(idle_.back());
            idle_.pop_back();
            // 前に別のスレッドで使ったランタイムでも、このスレッドのスタックで上限を測らせる。
            JS_UpdateStackTop(context->rt);
            return context;
        }
    }
    auto context = std::make_unique<PeleContext>();
    context->engine = this;
    context->fonts = &FontsFor(locale_);
    context->rt = JS_NewRuntime();
    context->ctx = context->rt ? JS_NewContext(context->rt) : nullptr;
    if (!context->ctx) {
        return nullptr;
    }
    JS_SetMaxStackSize(context->rt, kJsStackBytes);
    JS_SetMemoryLimit(context->rt, kJsMemoryBytes);
    JS_SetInterruptHandler(context->rt, CheckCancel, &cancel_);
    JS_SetContextOpaque(context->ctx, context.get());
    return context;
}

void PeleEngine::Release(std::unique_ptr<PeleContext> context)
{
    const std::lock_guard lock(mutex_);
    idle_.push_back(std::move(context));
}

bool PeleEngine::EnsurePele(PeleContext& context)
{
    if (JS_IsFunction(context.ctx, context.render)) {
        return true;
    }
    if (pele_bytecode_.empty()) {
        return false;
    }
    JSContext* ctx = context.ctx;
    JSValue module = JS_ReadObject(ctx, reinterpret_cast<const uint8_t*>(pele_bytecode_.data()), pele_bytecode_.size(), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(module) || JS_ResolveModule(ctx, module) < 0) {
        JS_FreeValue(ctx, module);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module));
    if (!Evaluated(context, JS_EvalFunction(ctx, module))) {
        return false;
    }
    // links・images: mermaid の securityLevel 'strict' と同じく、図からの遷移と外部の画像を許さない。
    // autoDirection: 狭いときに LR を TB に替えさせず、書いた向きのまま縮める。
    // katexHtml はラベルに $$ があるときだけ渡す。KaTeX は JS のスタックを深く使い、pele の配置の奥 (math オプション) で動かすと
    // 512KB の上限を超えるので、pele と同じ正規表現でコードから TeX を拾い、最上位で HTML にしてから配置させる。
    // pele が切り出した TeX が拾ったものと違えば (文字参照を解いたものなど)、その TeX も HTML にしてもう一度配置する。
    constexpr std::string_view kWrapper =
        "(pele, measurer, layoutMath) => (code, maxWidth, fontFamily, fontFamilyMono, katexHtml) => {"
        "  const options = { maxWidth, autoDirection: false, links: false, images: false, idPrefix: 'p', fontFamily, fontFamilyMono, measurer };"
        "  if (!katexHtml) {"
        "    return pele.render(code, options).svg;"
        "  }"
        "  const htmls = new Map();"
        "  const prepare = tex => {"
        "    if (!htmls.has(tex)) {"
        "      try { htmls.set(tex, katexHtml(tex)); } catch (e) { htmls.set(tex, null); }"
        "    }"
        "  };"
        "  for (const m of code.matchAll(/\\$\\$(.+?)\\$\\$/g)) {"
        "    prepare(m[1]);"
        "  }"
        "  const missed = new Set();"
        "  options.math = (tex, size) => {"
        "    if (!htmls.has(tex)) {"
        "      missed.add(tex);"
        "      return undefined;"
        "    }"
        "    const html = htmls.get(tex);"
        "    return html === null ? undefined : layoutMath(tex, html, size);"
        "  };"
        "  const svg = pele.render(code, options).svg;"
        "  if (missed.size === 0) {"
        "    return svg;"
        "  }"
        "  missed.forEach(prepare);"
        "  return pele.render(code, options).svg;"
        "}";
    JSValue wrapper = JS_Eval(ctx, kWrapper.data(), kWrapper.size(), "pele-wrapper.js", JS_EVAL_TYPE_GLOBAL);
    JSValue args[3] = { JS_GetModuleNamespace(ctx, def), JS_NewObject(ctx), JS_NewCFunction(ctx, MathLabel, "layoutMath", 3) };
    JS_SetPropertyStr(ctx, args[1], "width", JS_NewCFunction(ctx, MeasureWidth, "width", 3));
    context.render = JS_IsException(wrapper) ? JS_EXCEPTION : JS_Call(ctx, wrapper, JS_UNDEFINED, 3, args);
    for (JSValue& arg : args) {
        JS_FreeValue(ctx, arg);
    }
    JS_FreeValue(ctx, wrapper);
    if (!JS_IsFunction(ctx, context.render)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        context.render = JS_UNDEFINED;
        return false;
    }
    return true;
}

bool PeleEngine::EnsureKatex(PeleContext& context)
{
    if (JS_IsFunction(context.ctx, context.katex)) {
        return true;
    }
    if (katex_bytecode_.empty()) {
        return false;
    }
    // UMD の katex.min.js は、最上位の this (globalThis) の katex に入る。
    JSContext* ctx = context.ctx;
    JSValue script = JS_ReadObject(ctx, reinterpret_cast<const uint8_t*>(katex_bytecode_.data()), katex_bytecode_.size(), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(script)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    if (!Evaluated(context, JS_EvalFunction(ctx, script))) {
        return false;
    }
    // displayMode: $...$ も文中の $$...$$ も大きい形式で描く (分数が読みにくいので)。
    // trust は既定の false のまま (\href・\includegraphics・\htmlClass などを描かない)。
    constexpr std::string_view kWrapper = "tex => katex.renderToString(tex, { throwOnError: true, displayMode: true, output: 'html' })";
    context.katex = JS_Eval(ctx, kWrapper.data(), kWrapper.size(), "katex-wrapper.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsFunction(ctx, context.katex)) {
        JS_FreeValue(ctx, context.katex);
        JS_FreeValue(ctx, JS_GetException(ctx));
        context.katex = JS_UNDEFINED;
        return false;
    }
    return true;
}

KatexAssets* PeleEngine::Assets()
{
    std::call_once(assets_created_, [&] { assets_ = std::make_unique<KatexAssets>(dw_, katex_fonts_, locale_); });
    return assets_->ok() ? assets_.get() : nullptr;
}

void PeleEngine::Trim()
{
    std::vector<std::unique_ptr<PeleContext>> idle;
    {
        const std::lock_guard lock(mutex_);
        idle.swap(idle_);
    }
}

std::shared_ptr<const MathDrawing> PeleEngine::LayoutHtml(std::string_view html, float font_size)
{
    KatexAssets* assets = Assets();
    auto math = assets ? LayoutKatexHtml(html, font_size, *assets) : std::nullopt;
    return math ? std::make_shared<const MathDrawing>(std::move(*math)) : nullptr;
}

std::shared_ptr<const MathDrawing> PeleEngine::LayoutMath(PeleContext& context, std::string_view tex, float font_size)
{
    if (!EnsureKatex(context)) {
        return nullptr;
    }
    JSContext* ctx = context.ctx;
    JSValue arg = JS_NewStringLen(ctx, tex.data(), tex.size());
    JSValue html = JS_Call(ctx, context.katex, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(html)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    std::shared_ptr<const MathDrawing> out;
    size_t length = 0;
    if (const char* s = JS_ToCStringLen(ctx, &length, html)) {
        out = LayoutHtml({ s, length }, font_size);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, html);
    return out;
}

std::vector<std::shared_ptr<const MathDrawing>> PeleEngine::RenderMaths(std::span<const MathSource> maths)
{
    std::vector<std::shared_ptr<const MathDrawing>> out(maths.size());
    if (Cancelled()) {
        return out;
    }
    auto context = Acquire();
    if (!context) {
        return out;
    }
    for (size_t i = 0; i < maths.size() && !Cancelled(); ++i) {
        out[i] = LayoutMath(*context, maths[i].tex, maths[i].font_size);
    }
    Release(std::move(context));
    return out;
}

bool PeleEngine::ResolveMaths(mermaid_util::DiagramDrawing& drawing)
{
    std::vector<mermaid_util::DrawingMath*> pending;
    std::vector<MathSource> sources;
    for (auto& layer : drawing.layers) {
        for (auto& math : layer.maths) {
            if (!math.drawing) {
                pending.push_back(&math);
                sources.push_back({ math.tex, math.at.font_size });
            }
        }
    }
    if (pending.empty()) {
        return false;
    }
    auto drawings = RenderMaths(sources);
    for (size_t i = 0; i < pending.size(); ++i) {
        pending[i]->drawing = std::move(drawings[i]);
    }
    return true;
}

std::shared_ptr<const PeleSvg> PeleEngine::FindSvg(uint64_t key)
{
    const std::lock_guard lock(svg_mutex_);
    const auto* found = svgs_.Find(key);
    return found ? *found : nullptr;
}

void PeleEngine::StoreSvg(uint64_t key, std::shared_ptr<const PeleSvg> svg)
{
    const std::lock_guard lock(svg_mutex_);
    svgs_.Insert(key, std::move(svg));
    svgs_.TrimToBudget(kMaxSvgBytes, [](const std::shared_ptr<const PeleSvg>& s) static noexcept { return s->svg.size(); });
}

PeleOutcome PeleEngine::Render(std::string_view code, bool dark, float max_width, bool fit)
{
    using Status = PeleOutcome::Status;
    if (Cancelled()) {
        return { .status = Status::Cancelled };
    }
    auto context = Acquire();
    if (!context) {
        return {};
    }
    if (!context->measurer) {
        context->measurer.emplace(dw_, locale_);
    }
    // pele に渡す幅そのもので引く (要求のキーの量子化した幅では、出力が変わる幅を取り違える)。
    const uint64_t key = mermaid_util::DiagramCodeHash(code) ^ (std::bit_cast<uint32_t>(max_width) * kGoldenRatio64);
    PeleOutcome out;
    auto svg = FindSvg(key);
    if (!svg) {
        out = RenderSvg(*context, code, max_width, svg);
        if (svg) {
            StoreSvg(key, svg);
        }
    }
    if (svg) {
        auto drawing = DrawingFromPeleSvg(svg->svg, dark, fit ? max_width : 0.0f, *context->measurer, svg->labels);
        out.status = drawing ? Status::Drawn : Status::Unsupported;
        if (drawing) {
            out.drawing = std::move(*drawing);
        }
    }
    Release(std::move(context));
    return out;
}

PeleOutcome PeleEngine::RenderSvg(PeleContext& context, std::string_view code, float max_width, std::shared_ptr<const PeleSvg>& svg_out)
{
    using Status = PeleOutcome::Status;
    if (!EnsurePele(context)) {
        return {};
    }
    JSContext* ctx = context.ctx;
    JSValue args[5] = {
        JS_NewStringLen(ctx, code.data(), code.size()),
        JS_NewFloat64(ctx, max_width),
        NewString(ctx, context.fonts->sans),
        NewString(ctx, context.fonts->mono),
        code.find("$$") != std::string_view::npos && EnsureKatex(context) ? JS_DupValue(ctx, context.katex) : JS_UNDEFINED,
    };
    JSValue svg = JS_Call(ctx, context.render, JS_UNDEFINED, 5, args);
    for (JSValue& arg : args) {
        JS_FreeValue(ctx, arg);
    }
    PeleOutcome out;
    if (JS_IsException(svg)) {
        JSValue error = JS_GetException(ctx);
        if (Cancelled()) {
            out.status = Status::Cancelled;
        }
        else {
            out.status = Status::Error;
            out.error = ErrorText(ctx, error);
        }
        JS_FreeValue(ctx, error);
    }
    else {
        size_t length = 0;
        if (const char* s = JS_ToCStringLen(ctx, &length, svg)) {
            svg_out = std::make_shared<const PeleSvg>(PeleSvg{ .svg = std::string{ s, length }, .labels = std::move(context.labels) });
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, svg);
    }
    // 使い回すランタイムには、次の図のために空にして返す。
    context.labels.clear();
    return out;
}
