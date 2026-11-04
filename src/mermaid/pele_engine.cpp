#include "pele_engine.h"
#include "diagram_raster.h"
#include "string_convert.h"
#include <quickjs.h>
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

// mermaid.html の fonts() と同じ。本文と同じ書体で組み、Segoe UI に無い文字はロケールで決まるフォントにする。
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

struct PeleContext {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    // (code, maxWidth, fontFamily, fontFamilyMono) → SVG
    JSValue render = JS_UNDEFINED;
    const DiagramFonts* fonts = &kJaFonts;
    std::wstring locale;
    std::optional<DiagramTextMeasurer> measurer;

    PeleContext() = default;
    PeleContext(const PeleContext&) = delete;
    PeleContext& operator=(const PeleContext&) = delete;
    ~PeleContext()
    {
        if (ctx) {
            JS_FreeValue(ctx, render);
            JS_FreeContext(ctx);
        }
        if (rt) {
            JS_FreeRuntime(rt);
        }
    }

    void SetLocale(std::wstring_view new_locale, IDWriteFactory* dw)
    {
        if (measurer && locale == new_locale) {
            return;
        }
        locale = new_locale;
        fonts = &FontsFor(locale);
        measurer.emplace(dw, locale);
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

int CheckCancel(JSContext*, void* cancel)
{
    return static_cast<const std::atomic<bool>*>(cancel)->load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace

PeleEngine::PeleEngine(std::span<const std::byte> source, IDWriteFactory* dw) : source_(source), dw_(dw)
{
}

PeleEngine::~PeleEngine() = default;

// 使い捨てのランタイムでコンパイルする。使うランタイムでコンパイルすると、コンパイルしたモジュールが読み込んだものと
// 別に残る (約 4MB)。
void PeleEngine::CompileOnce()
{
    std::call_once(compiled_, [&] {
        PeleContext scratch;
        scratch.rt = JS_NewRuntime();
        scratch.ctx = scratch.rt ? JS_NewContext(scratch.rt) : nullptr;
        if (!scratch.ctx) {
            return;
        }
        // JS_Eval は末尾の NUL を要る。
        const std::string source(reinterpret_cast<const char*>(source_.data()), source_.size());
        JSValue module = JS_Eval(scratch.ctx, source.c_str(), source.size(), "pele.mjs", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        // 位置の情報を除くと 2.0MB → 0.95MB。pele の文言は位置を使わない。
        size_t size = 0;
        uint8_t* bytes = JS_IsException(module) ? nullptr
                                                : JS_WriteObject(scratch.ctx, &size, module,
                                                                 JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG);
        if (bytes) {
            bytecode_.assign(bytes, bytes + size);
            js_free(scratch.ctx, bytes);
        }
        JS_FreeValue(scratch.ctx, module);
        JS_FreeValue(scratch.ctx, JS_GetException(scratch.ctx));
    });
}

std::unique_ptr<PeleContext> PeleEngine::CreateContext()
{
    CompileOnce();
    if (bytecode_.empty()) {
        return nullptr;
    }
    auto context = std::make_unique<PeleContext>();
    context->rt = JS_NewRuntime();
    context->ctx = context->rt ? JS_NewContext(context->rt) : nullptr;
    if (!context->ctx) {
        return nullptr;
    }
    JS_SetMaxStackSize(context->rt, kJsStackBytes);
    JS_SetMemoryLimit(context->rt, kJsMemoryBytes);
    JS_SetInterruptHandler(context->rt, CheckCancel, &cancel_);
    JSContext* ctx = context->ctx;
    JS_SetContextOpaque(ctx, context.get());
    JSValue module = JS_ReadObject(ctx, bytecode_.data(), bytecode_.size(), JS_READ_OBJ_BYTECODE);
    if (JS_IsException(module) || JS_ResolveModule(ctx, module) < 0) {
        JS_FreeValue(ctx, module);
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module));
    JSValue evaluated = JS_EvalFunction(ctx, module);
    // pele は最上位に await が無いので、評価の Promise はジョブを流せば確定する。
    for (JSContext* job_ctx = nullptr; JS_ExecutePendingJob(context->rt, &job_ctx) > 0;) {
    }
    const bool loaded = !JS_IsException(evaluated) && (!JS_IsPromise(evaluated) || JS_PromiseState(ctx, evaluated) == JS_PROMISE_FULFILLED);
    JS_FreeValue(ctx, evaluated);
    if (!loaded) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    // links・images: mermaid の securityLevel 'strict' と同じく、図からの遷移と外部の画像を許さない。
    // autoDirection: 狭いときに LR を TB に替えさせず、書いた向きのまま縮める。mermaid.html の renderPele と同じ。
    constexpr std::string_view kWrapper =
        "(pele, measurer) => (code, maxWidth, fontFamily, fontFamilyMono) => pele.render(code, {"
        "maxWidth, autoDirection: false, links: false, images: false, idPrefix: 'p', fontFamily, fontFamilyMono, measurer}).svg";
    JSValue wrapper = JS_Eval(ctx, kWrapper.data(), kWrapper.size(), "pele-wrapper.js", JS_EVAL_TYPE_GLOBAL);
    JSValue args[2] = { JS_GetModuleNamespace(ctx, def), JS_NewObject(ctx) };
    JS_SetPropertyStr(ctx, args[1], "width", JS_NewCFunction(ctx, MeasureWidth, "width", 3));
    context->render = JS_IsException(wrapper) ? JS_EXCEPTION : JS_Call(ctx, wrapper, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, wrapper);
    if (!JS_IsFunction(ctx, context->render)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    return context;
}

std::unique_ptr<PeleContext> PeleEngine::Acquire()
{
    {
        const std::lock_guard lock(mutex_);
        if (!idle_.empty()) {
            auto context = std::move(idle_.back());
            idle_.pop_back();
            return context;
        }
    }
    return CreateContext();
}

void PeleEngine::Trim()
{
    std::vector<std::unique_ptr<PeleContext>> idle;
    {
        const std::lock_guard lock(mutex_);
        idle.swap(idle_);
    }
}

PeleOutcome PeleEngine::Render(std::string_view code, bool dark, float max_width, std::wstring_view locale)
{
    using Status = PeleOutcome::Status;
    if (cancel_.load(std::memory_order_relaxed)) {
        return { .status = Status::Cancelled };
    }
    auto context = Acquire();
    if (!context) {
        return {};
    }
    // 前に別のスレッドで使ったランタイムでも、このスレッドのスタックで上限を測らせる。
    JS_UpdateStackTop(context->rt);
    context->SetLocale(locale, dw_);
    JSContext* ctx = context->ctx;
    JSValue args[4] = {
        JS_NewStringLen(ctx, code.data(), code.size()),
        JS_NewFloat64(ctx, max_width),
        NewString(ctx, context->fonts->sans),
        NewString(ctx, context->fonts->mono),
    };
    JSValue svg = JS_Call(ctx, context->render, JS_UNDEFINED, 4, args);
    for (JSValue& arg : args) {
        JS_FreeValue(ctx, arg);
    }
    PeleOutcome out;
    if (JS_IsException(svg)) {
        JSValue error = JS_GetException(ctx);
        if (cancel_.load(std::memory_order_relaxed)) {
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
            if (auto drawing = DrawingFromPeleSvg({ s, length }, dark, max_width, *context->measurer)) {
                out.status = Status::Drawn;
                out.drawing = std::move(*drawing);
            }
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, svg);
    }
    const std::lock_guard lock(mutex_);
    idle_.push_back(std::move(context));
    return out;
}
