#include <benchmark/benchmark.h>
#include <windows.h>
#include <psapi.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include "block_h_scroll.h"
#include "command_generator.h"
#include "d2d_util.h"
#include "draw_cmd_test_util.h"
#include "draw_command.h"
#include "doc_dwrite_bridge.h"
#include "document.h"
#include "document_utils.h"
#include "dwrite_measurer.h"
#include "example_files.h"
#include "inline_math.h"
#include "layout.h"
#include "layout_cache.h"
#include "layout_computer.h"
#include "mock_text_measurer.h"
#include "parsed_layout.h"
#include "render_params.h"
#include "reducer_harness.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "theme.h"
#include "viewport_manager.h"

// 準備が重いベンチは Iterations を固定する。固定しないと反復回数を決める試行のたびに準備をやり直す。

namespace {

constexpr size_t kLargeBytes = 100 * 1024 * 1024;
constexpr double kMiB = 1024.0 * 1024.0;

// 準備や後始末を除いて測るベンチは UseManualTime にし、この区間を SetIterationTime に渡す。
// PauseTiming は使わない。CPU 列は準備を含み、Windows では約 15.6ms 単位と粗いので、比べるのは Time 列だけにする。
template <class F>
double SecondsOf(F&& f)
{
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// example/ 基準の相対パスで読む。読めなければベンチをスキップにして空を返す。
std::string ReadOrSkip(benchmark::State& state, std::string_view path)
{
    auto bytes = ReadExampleFileBytes(path);
    if (bytes.empty()) {
        state.SkipWithError(std::format("{} not found", path));
    }
    return bytes;
}

// 最初の表ノード。無ければベンチをスキップにして null を返す。
const Node* FirstTableOrSkip(benchmark::State& state, const Document& doc)
{
    const auto& tables = doc.GetTableNodeIndices();
    if (tables.empty()) {
        state.SkipWithError("table not parsed");
        return nullptr;
    }
    return &doc.GetNodes()[tables.front()];
}

// example/test.md を copies 回、かつ min_bytes 以上になるまでつなげる。
std::string RepeatTestMd(benchmark::State& state, size_t copies, size_t min_bytes = 0, std::string_view separator = {})
{
    const auto unit = ReadOrSkip(state, "example/test.md");
    std::string md;
    if (unit.empty()) {
        return md;
    }
    md.reserve(std::max(copies, min_bytes / unit.size() + 1) * (unit.size() + separator.size()));
    for (size_t i = 0; i < copies || md.size() < min_bytes; ++i) {
        md += unit;
        md += separator;
    }
    return md;
}

// ログのように空行のない n 行。
std::string LogLines(size_t n, std::string_view prefix = {}, std::string_view suffix = {})
{
    std::string md{ prefix };
    for (size_t i = 0; i < n; ++i) {
        std::format_to(std::back_inserter(md), "line {} of the log, value={} status=ok 日本語のテキストも少し含む\n", i, i * 7);
    }
    md += suffix;
    return md;
}

PROCESS_MEMORY_COUNTERS_EX MemoryCounters()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc;
}

// ===== パース =====

// 反復ごとにコピーしてパースする。コピーと破棄は測らない。
void RunParse(benchmark::State& state, const std::string& src)
{
    for (auto _ : state) {
        std::string copy = src;
        std::optional<Document> doc;
        state.SetIterationTime(SecondsOf([&] {
            NormalizeNewlines(copy);
            doc.emplace(Document::FromNormalizedMarkdown(std::move(copy), L"bench.md"));
        }));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(src.size()));
}

void BM_FromMarkdown(benchmark::State& state)
{
    const std::string src = RepeatTestMd(state, 1);
    if (!state.skipped()) {
        RunParse(state, src);
    }
}
BENCHMARK(BM_FromMarkdown)->UseManualTime()->Unit(benchmark::kMicrosecond);

// 時間はパースだけ。破棄の時間とパースで増えたメモリはカウンタに出す。
// メモリは最初の反復だけ測る (2 回目以降は解放済み領域の再利用で小さく出る)。
void RunLargeDocument(benchmark::State& state, const std::string& src)
{
    double destroy_s = 0.0;
    bool first = true;
    for (auto _ : state) {
        std::string copy = src;
        const auto before = MemoryCounters();
        std::optional<Document> doc;
        state.SetIterationTime(SecondsOf([&] { doc.emplace(Document::FromMarkdown(std::move(copy), L"bench.md")); }));
        if (first) {
            const auto after = MemoryCounters();
            state.counters["commit_MB"] = (static_cast<double>(after.PrivateUsage) - static_cast<double>(before.PrivateUsage)) / kMiB;
            state.counters["working_set_MB"] = (static_cast<double>(after.WorkingSetSize) - static_cast<double>(before.WorkingSetSize)) / kMiB;
            state.counters["nodes"] = static_cast<double>(doc->GetNodes().size());
            first = false;
        }
        destroy_s += SecondsOf([&] { doc.reset(); });
    }
    state.counters["destroy_ms"] = benchmark::Counter(destroy_s * 1000.0, benchmark::Counter::kAvgIterations);
    state.SetLabel(std::format("sizeof(Node)={}", sizeof(Node)));
}

void BM_LargeDocument(benchmark::State& state)
{
    const auto src = RepeatTestMd(state, 0, kLargeBytes, "\n\n");
    if (!state.skipped()) {
        RunLargeDocument(state, src);
    }
}
BENCHMARK(BM_LargeDocument)->UseManualTime()->Iterations(3)->Unit(benchmark::kMillisecond);

// リンク集のように全行がリンクを持つ文書。
void BM_LargeLinkDocument(benchmark::State& state)
{
    std::string src;
    src.reserve(kLargeBytes + 256);
    for (size_t i = 0; src.size() < kLargeBytes; ++i) {
        std::format_to(std::back_inserter(src),
                       "- [Item {0}](https://example.com/docs/page-{0}.html) と [詳細](#section-{1}) を参照\n",
                       i, i % 100);
        if (i % 20 == 19) {
            src += '\n';
        }
    }
    RunLargeDocument(state, src);
}
BENCHMARK(BM_LargeLinkDocument)->UseManualTime()->Iterations(3)->Unit(benchmark::kMillisecond);

// 80MB のリスト。毎行にリンク・太字・コード・強調・実体参照、50 行ごとに見出しと引用。
void BM_LargeInlineDocument(benchmark::State& state)
{
    constexpr size_t kBytes = 80 * 1024 * 1024;
    std::string src;
    src.reserve(kBytes + 256);
    for (size_t i = 0; src.size() < kBytes; ++i) {
        if (i % 50 == 0) {
            std::format_to(std::back_inserter(src), "## Section {0}\n\n> quoted note {0} with *emphasis*\n\n", i);
        }
        std::format_to(std::back_inserter(src), "- [link {0}](https://example.com/page/{0}) **bold {0}** `code {0}` _em_ &amp; text\n", i);
    }
    RunLargeDocument(state, src);
}
BENCHMARK(BM_LargeInlineDocument)->UseManualTime()->Iterations(3)->Unit(benchmark::kMillisecond);

// 空行のない 20 万行 (約 16MB)。長いテキストを区切ったノードの並びにする経路を通る。
void BM_ParseGiantText(benchmark::State& state)
{
    RunParse(state, LogLines(200000));
}
BENCHMARK(BM_ParseGiantText)->UseManualTime()->Unit(benchmark::kMillisecond);

// 改行も空白も無い 16MB の 1 行の段落。区切る位置を探す窓 (前後 2KB) を毎回調べ切る最悪の場合。
void BM_ParseLongLine(benchmark::State& state, std::string_view unit)
{
    std::string md = RepeatTo(unit, 16 * 1024 * 1024);
    md += '\n';
    RunParse(state, md);
}
BENCHMARK_CAPTURE(BM_ParseLongLine, ascii, std::string_view{ "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo" })->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseLongLine, cjk, std::string_view{ "あいうえおかきくけこ日本語文章" })->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseLongLine, cjk_punct, std::string_view{ "日本語の文章です、続きます。" })->UseManualTime()->Unit(benchmark::kMillisecond);

// 同じ 20 万行の 1 つのコードブロック。区切ったコードの続きのノードに、前から続くトークンを付ける字句解析も通る。
void BM_ParseGiantCode(benchmark::State& state)
{
    RunParse(state, LogLines(200000, "```cpp\n", "```\n"));
}
BENCHMARK(BM_ParseGiantCode)->UseManualTime()->Unit(benchmark::kMillisecond);

// 40KB 程度 (区切ったノード 3 つ前後) のコードブロックが 16MB 分並ぶ。
void BM_ParseManyCodeBlocks(benchmark::State& state)
{
    RunParse(state, RepeatTo(LogLines(500, "```cpp\n", "```\n\n"), 16 * 1024 * 1024));
}
BENCHMARK(BM_ParseManyCodeBlocks)->UseManualTime()->Unit(benchmark::kMillisecond);

// unit を 16MB まで繰り返した 1 つのコードブロック。
std::string RepeatedCodeBlock(std::string_view language, std::string_view unit)
{
    std::string md = RepeatTo(unit, 16 * 1024 * 1024, std::format("```{}\n", language));
    md += "\n```\n";
    return md;
}

// 実際の C++ ソース (syntax.cpp)。ログより識別子とコメントが多い。
void BM_ParseRealCode(benchmark::State& state)
{
    const auto unit = ReadOrSkip(state, "../src/core/syntax.cpp");
    if (!state.skipped()) {
        RunParse(state, RepeatedCodeBlock("cpp", unit));
    }
}
BENCHMARK(BM_ParseRealCode)->UseManualTime()->Unit(benchmark::kMillisecond);

// 語を飛ばせない言語の入力。パースと字句解析の両方のベンチで使う。
constexpr std::string_view kHtmlSnippet = "<div class=\"note\" id='a1'><p>text &amp; more <b>bold</b></p><img src=\"x.png\" alt=\"y\"/></div>\n";
constexpr std::string_view kYamlSnippet = "key: value\nlist: [a, b, {c: d}]\nitems:\n  - name: one\n    count: 12\n  - &ref {x: 1, y: \"s\"}\n";
constexpr std::string_view kElispSnippet = "(defun my-cmd (arg)\n  \"Doc.\"\n  (interactive \"p\")\n  (let ((x 'sym) (y #'fn)) (message \"%d\" (+ arg 1))))\n";

// 語を飛ばしにくいコード。空白の無い 1 行、コメントや文字列が始まりうる文字が密な場合、語を飛ばせない言語。
void BM_ParseCodeBlock(benchmark::State& state, std::string_view language, std::string_view unit)
{
    RunParse(state, RepeatedCodeBlock(language, unit));
}
BENCHMARK_CAPTURE(BM_ParseCodeBlock, json_numbers, "json", "1,22,333,")->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseCodeBlock, json_strings, "json", "\"k\":\"v\",")->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseCodeBlock, json_lines, "json", "\"k\": \"v\",\n")->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseCodeBlock, js_calls, "js", "a.b(c,d);")->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseCodeBlock, html, "html", kHtmlSnippet)->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseCodeBlock, yaml, "yaml", kYamlSnippet)->UseManualTime()->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ParseCodeBlock, elisp, "elisp", kElispSnippet)->UseManualTime()->Unit(benchmark::kMillisecond);

void BM_NormalizeNewlines(benchmark::State& state)
{
    const std::string src = RepeatTo("The quick brown fox jumps over the lazy dog 1234\r\n", kLargeBytes);
    for (auto _ : state) {
        std::string s = src;
        state.SetIterationTime(SecondsOf([&] { NormalizeNewlines(s); }));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(src.size()));
}
BENCHMARK(BM_NormalizeNewlines)->UseManualTime()->Unit(benchmark::kMillisecond);

// 1 ノードぶん (区切りの目安 16KB) を DirectWrite に渡す UTF-16 へ変換する。英文と和文混じり。
void BM_DecodeForDWrite(benchmark::State& state, std::string_view unit)
{
    const std::string src = RepeatTo(unit, 16 * 1024);
    std::wstring out;
    for (auto _ : state) {
        DecodeForDWrite(src, out);
        benchmark::DoNotOptimize(out.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(src.size()));
}
BENCHMARK_CAPTURE(BM_DecodeForDWrite, ascii, "The quick brown fox jumps over the lazy dog 1234\n")->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_DecodeForDWrite, mixed, "DirectWrite で描画する Markdown ビューア\n")->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_DecodeForDWrite, cjk, "高速で省メモリの表示。\n")->Unit(benchmark::kMicrosecond);

// ===== レイアウト (計測器のモック) =====

// MeasureNode ごとに busy spin を足し、ワーカーでの並列化の利得を見えるようにする。
class SlowMockMeasurer : public MockTextMeasurer {
public:
    static constexpr auto kSpin = std::chrono::microseconds(100);

    std::optional<float> MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width) const override
    {
        const auto deadline = std::chrono::steady_clock::now() + kSpin;
        while (std::chrono::steady_clock::now() < deadline) {
        }
        return MockTextMeasurer::MeasureNode(node, entry, max_width);
    }
};

void BM_MeasureIndicesParallel(benchmark::State& state)
{
    const auto n = static_cast<size_t>(state.range(0));
    SlowMockMeasurer mock;
    TaskScheduler ts{ 8 };
    mock.SetTheme(GetLightTheme());
    mock.SetScheduler(&ts);
    std::vector<size_t> indices(n);
    std::ranges::iota(indices, size_t{ 0 });
    for (auto _ : state) {
        StressFixture f;
        f.Build(n, /*include_code_block=*/false);
        state.SetIterationTime(SecondsOf([&] {
            layout::MeasureIndicesParallel(f.nodes, f.cache, 800.0f, mock, indices, kNoLayoutDeadline);
        }));
    }
    mock.SetScheduler(nullptr);
    ts.Shutdown();
}
// 所要時間はほぼ kSpin × ノード数で決まるので、短い計測時間で足りる。
void NodeCounts(benchmark::Benchmark* b)
{
    for (const int n : { 128, 1024, 4096, 22000 }) {
        b->Arg(n);
    }
    b->ArgName("nodes")->MinTime(0.1)->UseManualTime()->Unit(benchmark::kMicrosecond);
}
BENCHMARK(BM_MeasureIndicesParallel)->Apply(NodeCounts);

struct RecomputeFixture {
    static constexpr size_t kNodes = 22000;
    std::vector<Node> nodes;
    Theme theme = MakeLayoutTestTheme();
    LayoutCache cache;

    RecomputeFixture()
        : nodes(kNodes)
    {
        constexpr std::array kTypes{ NodeType::Heading, NodeType::Paragraph, NodeType::CodeBlock, NodeType::ListItem, NodeType::HorizontalRule };
        for (size_t i = 0; i < kNodes; i++) {
            nodes[i].type = kTypes[i % kTypes.size()];
            if (nodes[i].type == NodeType::Heading) {
                nodes[i].heading_level = 2;
            }
        }
        cache = LayoutCache(kNodes);
        layout::EstimateNodeHeights(nodes, cache, theme, 0.0f, {});
    }
};

// EnsureVisibleLayout の実際の使い方: 可視範囲 (5 ノード) の高さだけが変わり、後続 [105, N) は一定量ずらすだけで済む。
void BM_RecomputeYPositionsTailShift(benchmark::State& state)
{
    constexpr size_t kFromIndex = 100;
    constexpr size_t kSafeExitAfter = 104;
    RecomputeFixture f;
    bool grow = true;
    for (auto _ : state) {
        f.cache[kFromIndex].height += grow ? 0.5f : -0.5f;
        grow = !grow;
        layout::RecomputeYPositions(f.nodes, f.cache, f.theme, kFromIndex, kSafeExitAfter);
    }
}
BENCHMARK(BM_RecomputeYPositionsTailShift)->Unit(benchmark::kNanosecond);

// 100MB の単一コードブロック (区切ったノードの並び) の横スクロール幅。並び全体を走査する。
void BM_CodeRunHScroll(benchmark::State& state)
{
    std::string md = RepeatTo("    int value = compute(argument_one, argument_two, argument_three); // comment\n", kLargeBytes, "```cpp\n");
    md += "```\n";
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    const auto& nodes = doc.GetNodes();
    LayoutCache cache(nodes.size());
    for (size_t i = 0; i < std::min<size_t>(100, nodes.size()); ++i) {
        cache[i].set_natural_code_width(500.0f + static_cast<float>(i));
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(GetBlockHScrollGeometry(nodes, cache, nodes.size() / 2, 800.0f).block_width);
    }
    state.counters["nodes"] = static_cast<double>(nodes.size());
}
BENCHMARK(BM_CodeRunHScroll)->Iterations(10000)->Unit(benchmark::kMicrosecond);

// ===== レイアウト (DirectWrite) =====

// 実際の DirectWrite で測る計測器とエンジン。
struct DWriteLayout {
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    Theme theme = GetLightTheme();
    DWriteTextMeasurer measurer;
    LayoutEngine engine;
    TaskScheduler scheduler;
    bool ok = false;

    explicit DWriteLayout(int workers) : scheduler(workers)
    {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
            return;
        }
        measurer.SetFactory(factory.Get());
        ok = engine.Init(&measurer, theme);
        if (workers > 0) {
            measurer.SetScheduler(&scheduler);
        }
    }
    ~DWriteLayout()
    {
        measurer.SetScheduler(nullptr);
        scheduler.Shutdown();
    }

    bool SkipIfFailed(benchmark::State& state) const
    {
        if (!ok) {
            state.SkipWithError("DirectWrite init failed");
        }
        return !ok;
    }
};

// 文書と、それを組むワーカー workers 本の DWriteLayout。
struct DocLayout {
    Document doc;
    DWriteLayout l;

    DocLayout(std::string md, int workers) : doc(Document::FromMarkdown(std::move(md), L"bench.md")), l(workers)
    {}
};

// md が読めずにスキップ済みか、DirectWrite を使えなければ null。
std::unique_ptr<DocLayout> OpenDoc(benchmark::State& state, std::string md, int workers)
{
    if (state.skipped()) {
        return nullptr;
    }
    auto opened = std::make_unique<DocLayout>(std::move(md), workers);
    return opened->l.SkipIfFailed(state) ? nullptr : std::move(opened);
}

void WorkerCounts(benchmark::Benchmark* b)
{
    for (const int n : { 0, 8 }) {
        b->Arg(n);
    }
    b->ArgName("workers")->UseManualTime()->Unit(benchmark::kMillisecond);
}

// 文書の大きさのキャッシュを確保するところから全ノードを組む秒数。キャッシュの破棄は測らない。
double FullLayoutSeconds(LayoutEngine& engine, const std::vector<Node>& nodes)
{
    LayoutCache cache;
    return SecondsOf([&] {
        cache = LayoutCache(nodes.size());
        ComputeLayout(engine, nodes, cache, 1200.0f);
    });
}

// 初回はワーカーのファクトリと書式の作成を含む。
void BM_ComputeLayoutCold(benchmark::State& state)
{
    auto md = RepeatTestMd(state, 8);
    if (state.skipped()) {
        return;
    }
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    for (auto _ : state) {
        DWriteLayout l(static_cast<int>(state.range(0)));
        if (l.SkipIfFailed(state)) {
            break;
        }
        state.SetIterationTime(FullLayoutSeconds(l.engine, doc.GetNodes()));
    }
    state.counters["nodes"] = static_cast<double>(doc.GetNodes().size());
}
BENCHMARK(BM_ComputeLayoutCold)->Apply(WorkerCounts);

// 同じワーカーで 2 回目以降。
void BM_ComputeLayoutWarm(benchmark::State& state)
{
    const auto opened = OpenDoc(state, RepeatTestMd(state, 8), static_cast<int>(state.range(0)));
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    FullLayoutSeconds(l.engine, doc.GetNodes());
    for (auto _ : state) {
        state.SetIterationTime(FullLayoutSeconds(l.engine, doc.GetNodes()));
    }
}
BENCHMARK(BM_ComputeLayoutWarm)->Apply(WorkerCounts)->Iterations(3);

// 段落ごとに $...$ を 5 個と $$...$$ を 1 個。数式はどれも別の TeX。
std::string InlineMathMd(int paragraphs)
{
    std::string md;
    for (int i = 0; i < paragraphs; ++i) {
        for (int k = 0; k < 5; ++k) {
            std::format_to(std::back_inserter(md), "Some text about $\\frac{{a_{{{}}}}}{{b}}$ which relates to ", i * 5 + k);
        }
        std::format_to(std::back_inserter(md), "$$\\sum_{{k=0}}^{{{}}} k^2$$ the end.\n\n", i);
    }
    return md;
}

// 寸法の分かったインライン数式を、インラインオブジェクトに置き換えながら全ノードを組む。
void BM_InlineMathLayout(benchmark::State& state)
{
    const auto opened = OpenDoc(state, InlineMathMd(2000), static_cast<int>(state.range(0)));
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    InlineMathTable table;
    l.measurer.SetInlineMath(&table);
    FullLayoutSeconds(l.engine, doc.GetNodes());
    for (const uint64_t key : table.TakeRequests()) {
        table.Resolve(key, { 40.0f, 18.0f, 13.0f });
    }
    for (auto _ : state) {
        state.SetIterationTime(FullLayoutSeconds(l.engine, doc.GetNodes()));
    }
    l.measurer.SetInlineMath(nullptr);
}
BENCHMARK(BM_InlineMathLayout)->Arg(0)->Arg(8)->ArgName("workers")->UseManualTime()->Iterations(5)->Unit(benchmark::kMillisecond);

// 文書を開く・読み直す・ズームのたびに UI スレッドで作る、インライン数式の組。
void BM_InlineMathPlanBuild(benchmark::State& state, bool math)
{
    auto md = math ? InlineMathMd(2000) : RepeatTestMd(state, 8);
    if (state.skipped()) {
        return;
    }
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    const Theme theme = GetLightTheme();
    InlineMathPlan plan;
    for (auto _ : state) {
        plan.Build(doc.GetNodes(), theme);
        benchmark::DoNotOptimize(plan.Chunks().data());
    }
    state.counters["nodes"] = static_cast<double>(doc.GetNodes().size());
    state.counters["chunks"] = static_cast<double>(plan.Chunks().size());
}
BENCHMARK_CAPTURE(BM_InlineMathPlanBuild, test_md_x8, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_InlineMathPlanBuild, math_12000, true)->Unit(benchmark::kMicrosecond);

// 全ノードを計測したあと幅を変えたときの再計測 (直列)。反復ごとに幅を変える。
void BM_ResizeRemeasure(benchmark::State& state, const char* path)
{
    const auto opened = OpenDoc(state, ReadOrSkip(state, path), 0);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    LayoutCache cache(doc.GetNodes().size());
    ComputeLayout(l.engine, doc.GetNodes(), cache, 1200.0f);
    constexpr std::array kWidths{ 1100.0f, 1000.0f, 1300.0f };
    size_t k = 0;
    for (auto _ : state) {
        ComputeLayout(l.engine, doc.GetNodes(), cache, kWidths[k++ % kWidths.size()]);
    }
}
BENCHMARK_CAPTURE(BM_ResizeRemeasure, test_md, "example/test.md")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_ResizeRemeasure, readme, "../README.md")->Unit(benchmark::kMicrosecond);

enum class GiantBlock {
    Paragraph,
    Code,
    Table5k,
    Table50k
};

// 巨大な 1 ブロックだけの文書。段落とコードは 5 万行。
std::string GiantBlockMd(GiantBlock kind)
{
    if (kind == GiantBlock::Paragraph) {
        return LogLines(50000);
    }
    if (kind == GiantBlock::Code) {
        return LogLines(50000, "```cpp\n", "```\n");
    }
    const size_t rows = kind == GiantBlock::Table5k ? 5000 : 50000;
    std::string md = "| a | b | c | d | e | f |\n|---|---|---|---|---|---|\n";
    for (size_t i = 0; i < rows; ++i) {
        std::format_to(std::back_inserter(md), "| {} | name{} | `code` | 値{} | some longer text here | {} |\n", i, i, i * 3, i % 17);
    }
    return md;
}

enum class OpenPhase {
    FirstScreen,
    Jump,
    Rest
};

// アプリで開くときと同じ順 (推定 → 最初の画面 → 中央へのジャンプ → 残り全部) に進め、phase だけを測る。ワーカー 8 本。
void BM_GiantBlock(benchmark::State& state, GiantBlock kind, OpenPhase phase)
{
    constexpr float kWidth = 1200.0f;
    constexpr float kScreen = 900.0f;
    const auto opened = OpenDoc(state, GiantBlockMd(kind), 8);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    const auto& nodes = doc.GetNodes();
    for (auto _ : state) {
        LayoutCache cache = layout::MakeEstimatedLayoutCache(nodes, l.theme, l.theme.ContentWidth(kWidth));
        l.engine.Reestimate(nodes, cache, kWidth);
        double s = SecondsOf([&] { l.engine.EnsureVisibleLayout(nodes, cache, kWidth, 0.0f, kScreen); });
        if (phase != OpenPhase::FirstScreen) {
            const float mid = cache.Bottom(nodes.size() - 1) * 0.5f;
            s = SecondsOf([&] { l.engine.EnsureVisibleLayout(nodes, cache, kWidth, mid, mid + kScreen); });
        }
        if (phase == OpenPhase::Rest) {
            s = SecondsOf([&] {
                while (l.engine.ProcessDirtyBatch(nodes, cache, kWidth, kNoLayoutDeadline, YRange::All())) {
                }
            });
        }
        state.SetIterationTime(s);
    }
    state.counters["nodes"] = static_cast<double>(nodes.size());
}
void GiantBlockOptions(benchmark::Benchmark* b)
{
    b->UseManualTime()->Iterations(3)->Unit(benchmark::kMillisecond);
}
BENCHMARK_CAPTURE(BM_GiantBlock, paragraph_first, GiantBlock::Paragraph, OpenPhase::FirstScreen)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, paragraph_jump, GiantBlock::Paragraph, OpenPhase::Jump)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, paragraph_rest, GiantBlock::Paragraph, OpenPhase::Rest)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, code_first, GiantBlock::Code, OpenPhase::FirstScreen)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, code_jump, GiantBlock::Code, OpenPhase::Jump)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, code_rest, GiantBlock::Code, OpenPhase::Rest)->Apply(GiantBlockOptions);
// 表の残り全部は commit が約 2GB になるので測らない。
BENCHMARK_CAPTURE(BM_GiantBlock, table5k_first, GiantBlock::Table5k, OpenPhase::FirstScreen)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, table5k_jump, GiantBlock::Table5k, OpenPhase::Jump)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, table50k_first, GiantBlock::Table50k, OpenPhase::FirstScreen)->Apply(GiantBlockOptions);
BENCHMARK_CAPTURE(BM_GiantBlock, table50k_jump, GiantBlock::Table50k, OpenPhase::Jump)->Apply(GiantBlockOptions);

// 実際の C++ ソース (syntax.cpp、約 100KB) を 1 つのコードブロックにした文書を doc に作り、先頭のノードか
// 2 つ目のノード (continuation) を返す。kTextChunkTarget ごとの続きのノードに区切られる。取れなければスキップにする。
const Node* CodeChunkOrSkip(benchmark::State& state, std::optional<Document>& doc, bool continuation)
{
    const auto src = ReadOrSkip(state, "../src/core/syntax.cpp");
    if (state.skipped()) {
        return nullptr;
    }
    std::string md = "```cpp\n";
    md += src;
    md += "```\n";
    doc = Document::FromMarkdown(std::move(md), L"bench.md");
    const auto& nodes = doc->GetNodes();
    const auto it = std::ranges::find_if(nodes, [continuation](const Node& n) {
        return n.type == NodeType::CodeBlock && n.continues_prev() == continuation;
    });
    if (it == nodes.end()) {
        state.SkipWithError("code chunk not parsed");
        return nullptr;
    }
    return &*it;
}

// 区切ったコードブロックの 1 ノードのトークナイズ。計測で text_layout を作るたびに走る。
void BM_TokenizeCodeChunk(benchmark::State& state, bool continuation)
{
    std::optional<Document> doc;
    const Node* node = CodeChunkOrSkip(state, doc, continuation);
    if (!node) {
        return;
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(TokenizeCodeNode(*node));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(node->GetText().size()));
}
BENCHMARK_CAPTURE(BM_TokenizeCodeChunk, head, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_TokenizeCodeChunk, continuation, true)->Unit(benchmark::kMicrosecond);

// 文書を開いた直後 (layout を作ったのは見えている範囲だけ) の 1 回目の EvictTextLayouts。
// 30 万ノードは 100MB 程度の文書。
void BM_FirstEvictAfterOpen(benchmark::State& state)
{
    const auto n = static_cast<size_t>(state.range(0));
    for (auto _ : state) {
        LayoutCache cache(n);
        state.SetIterationTime(SecondsOf([&] { cache.EvictTextLayouts(0, 100); }));
    }
}
BENCHMARK(BM_FirstEvictAfterOpen)->Arg(10'000)->Arg(300'000)->Iterations(20)->UseManualTime()->Unit(benchmark::kMicrosecond);

// C 系以外の字句解析。区切ったノードと同じ 16KB 程度になるまで snippet を繰り返す。
void BM_TokenizeLanguage(benchmark::State& state, SyntaxLanguage language, std::string_view snippet)
{
    const std::string text = RepeatTo(snippet, kTextChunkTarget);
    for (auto _ : state) {
        benchmark::DoNotOptimize(Tokenize(text, language));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(text.size()));
}
BENCHMARK_CAPTURE(BM_TokenizeLanguage, html, SyntaxLanguage::Html, kHtmlSnippet)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_TokenizeLanguage, elisp, SyntaxLanguage::EmacsLisp, kElispSnippet)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_TokenizeLanguage, yaml, SyntaxLanguage::Yaml, kYamlSnippet)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_TokenizeLanguage, toml, SyntaxLanguage::Toml,
                  "[server.http]\nhost = \"localhost\"\nport = 8080\nratio = 1.5e3\narr = [1, 2, 3]\nwhen = 1979-05-27T07:32:00Z\n")
    ->Unit(benchmark::kMicrosecond);
// 語を大文字小文字を区別せずに表から引く言語。
BENCHMARK_CAPTURE(BM_TokenizeLanguage, sql, SyntaxLanguage::Sql,
                  "SELECT u.id, u.name, COUNT(*) AS n\nFROM users u\nJOIN orders o ON o.user_id = u.id\n"
                  "WHERE o.total > 100 AND u.active = 1\nGROUP BY u.id, u.name\nORDER BY n DESC;\n")
    ->Unit(benchmark::kMicrosecond);

// 比較用: 同じノードを text_layout から作って計測する全体 (レイアウト作成・トークナイズ・装飾)。
void BM_MeasureCodeChunk(benchmark::State& state, bool continuation)
{
    std::optional<Document> doc;
    const Node* node = CodeChunkOrSkip(state, doc, continuation);
    if (!node) {
        return;
    }
    DWriteLayout l(0);
    if (l.SkipIfFailed(state)) {
        return;
    }
    for (auto _ : state) {
        NodeLayoutEntry entry;
        benchmark::DoNotOptimize(l.measurer.MeasureNode(*node, entry, 1200.0f));
    }
}
BENCHMARK_CAPTURE(BM_MeasureCodeChunk, head, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_MeasureCodeChunk, continuation, true)->Unit(benchmark::kMicrosecond);

constexpr float kViewWidth = 1100.0f;
constexpr float kViewHeight = 950.0f;
constexpr int kViewJumps = 30;

// test.md x40 (約 3.4 万ノード) を開いたときの最初の画面と、続く前後 2 画面の先読み。ワーカー 8 本。
void BM_ViewportOpen(benchmark::State& state, bool prefetch)
{
    const auto opened = OpenDoc(state, RepeatTestMd(state, 40), 8);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    for (auto _ : state) {
        LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), l.theme);
        ViewportManager viewport;
        LayoutService service(l.engine, viewport);
        double s = SecondsOf([&] { service.ViewportLayout(doc, cache, kViewWidth, kViewHeight); });
        if (prefetch) {
            s = SecondsOf([&] { service.ProcessDirtyBatch(doc, cache, kViewWidth, kNoLayoutDeadline, kViewHeight, 2.0f); });
        }
        state.SetIterationTime(s);
    }
}
BENCHMARK_CAPTURE(BM_ViewportOpen, open, false)->UseManualTime()->Iterations(20)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_ViewportOpen, prefetch, true)->UseManualTime()->Iterations(20)->Unit(benchmark::kMicrosecond);

// 開いて先読みしたあと、文書の 1/31 〜 30/31 のノードへ順にジャンプする。
void BM_ViewportJump(benchmark::State& state)
{
    const auto opened = OpenDoc(state, RepeatTestMd(state, 40), 8);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), l.theme);
    ViewportManager viewport;
    LayoutService service(l.engine, viewport);
    service.ViewportLayout(doc, cache, kViewWidth, kViewHeight);
    service.ProcessDirtyBatch(doc, cache, kViewWidth, kNoLayoutDeadline, kViewHeight, 2.0f);
    const auto n = static_cast<int64_t>(doc.GetNodes().size());
    int64_t k = 0;
    for (auto _ : state) {
        viewport.SetScrollTarget(static_cast<int>(n * ++k / (kViewJumps + 1)), 0.0f);
        viewport.ApplyScrollTarget(cache);
        state.SetIterationTime(SecondsOf([&] { service.EnsureVisibleLayout(doc, cache, kViewWidth, kViewHeight); }));
    }
}
BENCHMARK(BM_ViewportJump)->UseManualTime()->Iterations(kViewJumps)->Unit(benchmark::kMicrosecond);

// 描画のたびに目次のアクティブ見出しを決め直すコスト (SyncActiveTocIndex の本体)。100MB の文書で、
// 位置を変えながら見出しの二分探索を繰り返す。
void BM_FindActiveTocIndex(benchmark::State& state)
{
    auto md = RepeatTestMd(state, 0, kLargeBytes);
    if (state.skipped()) {
        return;
    }
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    const Theme theme = GetLightTheme();
    const LayoutCache cache = layout::MakeEstimatedLayoutCache(doc.GetNodes(), theme, theme.ContentWidth(kViewWidth));
    const float bottom = cache.Bottom(cache.size() - 1);
    float y = 0.0f;
    for (auto _ : state) {
        y += bottom / 997.0f;
        if (y > bottom) {
            y -= bottom;
        }
        benchmark::DoNotOptimize(layout::FindActiveTocIndex(doc.GetToc(), cache, y, theme.heading_spacing_above));
    }
    state.counters["headings"] = static_cast<double>(doc.GetToc().size());
}
BENCHMARK(BM_FindActiveTocIndex)->Unit(benchmark::kNanosecond);

// 40 行の表の全セルの選択矩形 (1 フレーム分)。
void BM_TableSelectionHitTest(benchmark::State& state)
{
    std::string md = "| 名前 | 状態 | 説明 | 数量 | 単価 | 備考 |\n|---|---|---|--:|--:|:-:|\n";
    for (size_t r = 0; r < 40; ++r) {
        std::format_to(std::back_inserter(md), "| 項目{0} | 状態{0} | 説明のテキスト {0} を少し長めに書く | {0} | {0}00 | 備考{0} |\n", r);
    }
    const auto opened = OpenDoc(state, std::move(md), 0);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    const Node* table_node = FirstTableOrSkip(state, doc);
    if (!table_node) {
        return;
    }
    NodeLayoutEntry entry;
    l.measurer.MeasureNode(*table_node, entry, 1200.0f);
    const auto& tl = *entry.table_layout();
    const auto& table = *table_node->table_data();
    std::vector<DWRITE_HIT_TEST_METRICS> buffer;
    for (auto _ : state) {
        for (size_t r = 0; r < table.row_count; ++r) {
            for (size_t c = 0; c < table.col_count; ++c) {
                IDWriteTextLayout* layout = tl.GetCellLayout(r, c);
                if (!layout) {
                    continue;
                }
                const auto text = table.GetCellText(r, c);
                Utf16OffsetCursor cursor{ text };
                const auto range = cursor.WideRange(0, static_cast<uint32_t>(text.size()));
                benchmark::DoNotOptimize(FetchHitTestMetrics(layout, range.startPosition, range.length, buffer));
            }
        }
    }
    state.counters["cells"] = static_cast<double>(table.row_count * table.col_count);
}
BENCHMARK(BM_TableSelectionHitTest)->Unit(benchmark::kMicrosecond);

// md が空なら test.md を 4 回繰り返した文書を開く。
std::unique_ptr<DocLayout> OpenDocOrTestMd(benchmark::State& state, std::string md)
{
    if (md.empty()) {
        md = RepeatTestMd(state, 4);
    }
    return OpenDoc(state, std::move(md), 8);
}

// 1 画面の本文を組んでコマンドを作る一式。スクロールと組む順序はベンチごとに違うので、作るところまでを持つ。
struct MdPaneFrame {
    static constexpr PaneRect kPane{ .x = 0.0f, .y = 0.0f, .width = kViewWidth, .height = kViewHeight };

    LayoutCache cache;
    ViewportManager viewport;
    LayoutService service;
    CommandGenerator generator;
    DrawCommandList cmds;

    explicit MdPaneFrame(DocLayout& d) : cache(layout::MakeEstimatedLayoutCache(d.doc.GetNodes(), d.l.theme)), service(d.l.engine, viewport)
    {
        InitWithoutText(generator, d.l.theme);
    }
};

// 全選択のまま本文を描く 1 フレーム分のコマンド生成。step が 0 なら同じ位置、それ以外はその px ずつスクロールする。
// 比較のために選択なしも測る。
void BM_SelectionFrame(benchmark::State& state, std::string md, bool select, float step)
{
    const auto opened = OpenDocOrTestMd(state, std::move(md));
    if (!opened) {
        return;
    }
    const auto& doc = opened->doc;
    MdPaneFrame frame(*opened);
    auto& [cache, viewport, service, generator, cmds] = frame;
    const float start_y = cache.Bottom(cache.size() - 1) * 0.5f;
    viewport.SetScrollY(start_y);
    service.EnsureVisibleLayout(doc, cache, kViewWidth, kViewHeight);
    const auto last = static_cast<int>(doc.GetNodes().size()) - 1;
    const TextSelection selection = select ? TextSelection::MakeOrdered(0, 0, last, static_cast<uint32_t>(doc.GetNodes().back().GetText().size())) : TextSelection{};
    float offset = 0.0f;
    for (auto _ : state) {
        offset = offset + step > kViewHeight ? 0.0f : offset + step;
        cmds.clear();
        generator.GenerateMdPane(cmds, { .nodes = doc.GetNodes(), .cache = cache, .selection = selection, .md_pane_rect = MdPaneFrame::kPane, .scroll_y = start_y + offset }, 1.0f);
        benchmark::DoNotOptimize(cmds.data());
    }
    state.counters["cmds"] = static_cast<double>(cmds.size());
    int text_layouts = 0;
    for (size_t i = 0; i < cache.size(); ++i) {
        text_layouts += cache[i].text_layout() != nullptr;
    }
    state.counters["text_layouts"] = text_layouts;
}
// 1 段落が 15KB の unit の繰り返し。
std::string RepeatedParagraphs(std::string_view unit)
{
    const std::string paragraph = RepeatTo(unit, 15'000);
    std::string md;
    for (int i = 0; i < 40; ++i) {
        md += paragraph;
        md += "\n\n";
    }
    return md;
}
// 1 段落が 15KB の和文。区切りの上限に近い、選択の位置変換がいちばん重い本文。
std::string LongJapaneseParagraphs()
{
    return RepeatedParagraphs("吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。");
}
BENCHMARK_CAPTURE(BM_SelectionFrame, testmd_none_static, std::string{}, false, 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SelectionFrame, testmd_all_static, std::string{}, true, 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SelectionFrame, testmd_all_scroll, std::string{}, true, 3.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SelectionFrame, ja15kb_none_static, LongJapaneseParagraphs(), false, 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SelectionFrame, ja15kb_all_static, LongJapaneseParagraphs(), true, 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SelectionFrame, ja15kb_all_scroll, LongJapaneseParagraphs(), true, 3.0f)->Unit(benchmark::kMicrosecond);

constexpr PaneRect kTocPane{ .x = 0.0f, .y = 0.0f, .width = 300.0f, .height = kViewHeight };

// 2000 見出しの目次の 1 フレーム。書式が無いのでレイアウトは作らず、項目の文字列の手間だけを測る。
void BM_TocFrame(benchmark::State& state)
{
    std::string md;
    for (int i = 0; i < 2000; ++i) {
        std::format_to(std::back_inserter(md), "## 見出しの文字列をすこし長めに書く Heading {}\n\n", i);
    }
    const auto doc = Document::FromMarkdown(std::move(md), L"bench.md");
    const Theme theme = GetLightTheme();
    const SidePaneState sp = TocOnlySidePane(kTocPane, 24.0f, doc.GetToc(), doc.GetNodes(), 3);
    TextLayoutCache text_layouts;
    DrawCommandList cmds;
    for (auto _ : state) {
        cmds.clear();
        chrome::EmitSidePanes(cmds, text_layouts, {}, theme, sp);
        benchmark::DoNotOptimize(cmds.data());
    }
}
BENCHMARK(BM_TocFrame)->Unit(benchmark::kMicrosecond);

// 目次の全行が入れ替わるフレーム (文書の切り替えやつまみのドラッグ)。レイアウトの整形は描くときまで遅れるので GetMetrics で済ませる。
void BM_TocColdFrame(benchmark::State& state, bool explicit_fonts)
{
    const auto opened = OpenDoc(state, ReadOrSkip(state, "example/test.md"), 0);
    if (!opened) {
        return;
    }
    auto* const factory = opened->l.factory.Get();
    const Theme& theme = opened->l.theme;
    const auto item = CreateTextFormat(factory, { .family = theme.font_family.c_str(), .size = theme.pane_font_size, .locale = theme.text_locale, .para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, .no_wrap = true });
    const SidePaneState sp = TocOnlySidePane(kTocPane, theme.pane_item_height, opened->doc.GetToc(), opened->doc.GetNodes(), 3);
    const chrome::SidePaneFormats fmts{ .toc_item = item.Get(), .item_fonts = explicit_fonts ? &opened->l.measurer.BodyFonts() : nullptr };
    DrawCommandList cmds;
    for (auto _ : state) {
        TextLayoutCache text_layouts;
        text_layouts.SetFactory(factory);
        cmds.clear();
        chrome::EmitSidePanes(cmds, text_layouts, fmts, theme, sp);
        for (const auto& cmd : cmds) {
            if (const auto* text = std::get_if<DrawTextLayoutCmd>(&cmd)) {
                DWRITE_TEXT_METRICS metrics{};
                text->layout->GetMetrics(&metrics);
                benchmark::DoNotOptimize(metrics);
            }
        }
    }
}
BENCHMARK_CAPTURE(BM_TocColdFrame, fallback, false)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_TocColdFrame, explicit_fonts, true)->Unit(benchmark::kMicrosecond);

// 数式 10000 個の型から、画面の 200 個を引く。描画はグリフランごとに 1 回引く。
void BM_InlineMathMaskFind(benchmark::State& state)
{
    InlineMathMasks math;
    InlineMathPage page;
    std::vector<uint64_t> keys;
    for (int i = 0; i < 10000; ++i) {
        keys.push_back(InlineMathKey(std::to_string(i), 16.0f));
        math.Assign(keys.back(), { .page = &page, .src = {}, .dest = {} });
    }
    size_t at = 0;
    for (auto _ : state) {
        for (int k = 0; k < 200; ++k) {
            benchmark::DoNotOptimize(math.Find(keys[at]));
            at = (at + 3779) % keys.size();
        }
    }
}
BENCHMARK(BM_InlineMathMaskFind)->Unit(benchmark::kMicrosecond);

// 全マッチのハイライトを描く 1 フレーム分のコマンド生成。上限 (10000 件) までのマッチが見えるよう文書の先頭で測る。
// step が 0 なら同じ位置、それ以外はその px ずつスクロールする。
void BM_SearchFrame(benchmark::State& state, std::string md, std::string_view query, float step)
{
    const auto opened = OpenDocOrTestMd(state, std::move(md));
    if (!opened) {
        return;
    }
    const auto& doc = opened->doc;
    MdPaneFrame frame(*opened);
    auto& [cache, viewport, service, generator, cmds] = frame;
    service.EnsureVisibleLayout(doc, cache, kViewWidth, kViewHeight);
    viewport.SetScrollY(kViewHeight);
    service.EnsureVisibleLayout(doc, cache, kViewWidth, kViewHeight);
    SearchState search;
    RunSearchToEnd(search, doc.GetNodes(), query);
    float offset = 0.0f;
    for (auto _ : state) {
        offset = offset + step > kViewHeight ? 0.0f : offset + step;
        cmds.clear();
        generator.GenerateMdPane(cmds, { .nodes = doc.GetNodes(), .cache = cache, .selection = TextSelection{}, .md_pane_rect = MdPaneFrame::kPane, .scroll_y = offset, .search_matches = search.GetMatches(), .current_match_index = 0 }, 1.0f);
        benchmark::DoNotOptimize(cmds.data());
    }
    state.counters["matches"] = search.GetMatchCount();
    state.counters["rects"] = static_cast<double>(CountCmd<FillRectCmd>(cmds, [](const FillRectCmd& c) static { return c.brush_id == BrushId::SearchHighlight || c.brush_id == BrushId::SearchHighlightCurrent; }));
}
BENCHMARK_CAPTURE(BM_SearchFrame, testmd_e_static, std::string{}, "e", 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SearchFrame, testmd_e_scroll, std::string{}, "e", 3.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SearchFrame, ja15kb_static, LongJapaneseParagraphs(), "い", 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SearchFrame, ja15kb_scroll, LongJapaneseParagraphs(), "い", 3.0f)->Unit(benchmark::kMicrosecond);
// 英文の e は 1 画面に数百個ある。
constexpr std::string_view kEnglishUnit = "The quick brown fox jumps over the lazy dog while the green eagle sees every tree. ";
BENCHMARK_CAPTURE(BM_SearchFrame, english_e_static, RepeatedParagraphs(kEnglishUnit), "e", 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SearchFrame, english_e_scroll, RepeatedParagraphs(kEnglishUnit), "e", 3.0f)->Unit(benchmark::kMicrosecond);
// 2 文字に 1 回現れる、1 文字検索でも実際の文書には無い密度の上限。
BENCHMARK_CAPTURE(BM_SearchFrame, dense_static, RepeatedParagraphs("a "), "a", 0.0f)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_SearchFrame, dense_scroll, RepeatedParagraphs("a "), "a", 3.0f)->Unit(benchmark::kMicrosecond);

// 描画先だけのデバイス。ExecuteDrawCommands の CPU 時間を実際の D2D で測る。
struct OffscreenTarget {
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    FixedBrushArray brushes;

    bool Init(const Theme& theme, UINT width, UINT height)
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) ||
            FAILED(d3d.As(&dxgi)) || FAILED(factory->CreateDevice(dxgi.Get(), &device)) ||
            FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) {
            return false;
        }
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &props, &bitmap))) {
            return false;
        }
        context->SetTarget(bitmap.Get());
        for (size_t i = 0; i < brushes.size(); ++i) {
            CreateSolidColorBrushOrFallback(context.Get(), theme.colors[i], brushes[i]);
        }
        return true;
    }
};

// 長い行のコードブロックを描く 1 フレーム。折り返さないので、1 行のトークンのほとんどは横スクロールの clip の外にある。
void BM_CodeBlockDraw(benchmark::State& state, std::string md)
{
    const auto opened = OpenDoc(state, std::move(md), 8);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    OffscreenTarget target;
    if (!target.Init(l.theme, static_cast<UINT>(kViewWidth), static_cast<UINT>(kViewHeight))) {
        state.SkipWithError("D3D device init failed");
        return;
    }
    MdPaneFrame frame(*opened);
    auto& [cache, viewport, service, generator, cmds] = frame;
    service.EnsureVisibleLayout(doc, cache, kViewWidth, kViewHeight);
    generator.GenerateMdPane(cmds, { .nodes = doc.GetNodes(), .cache = cache, .selection = TextSelection{}, .md_pane_rect = MdPaneFrame::kPane }, 1.0f);
    InlineMathMasks math;
    for (auto _ : state) {
        target.context->BeginDraw();
        target.context->Clear(l.theme.Color(BrushId::Background));
        ExecuteDrawCommands(cmds, target.context.Get(), target.brushes, math);
        target.context->EndDraw();
    }
    state.counters["cmds"] = static_cast<double>(cmds.size());
}
// lines 行、1 行が line_bytes の JSON。lang が空ならトークンが無いコードブロックになる。
std::string JsonCodeBlock(int lines, size_t line_bytes, std::string_view lang = "json")
{
    std::string md = std::format("```{}\n", lang);
    const std::string line = RepeatTo("\"k\":\"v\",1,", line_bytes);
    for (int i = 0; i < lines; ++i) {
        md += line;
        md += '\n';
    }
    md += "```\n";
    return md;
}
BENCHMARK_CAPTURE(BM_CodeBlockDraw, json_80, JsonCodeBlock(60, 80))->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_CodeBlockDraw, json_2kb, JsonCodeBlock(60, 2 * 1024))->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_CodeBlockDraw, json_12kb, JsonCodeBlock(60, 12 * 1024))->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_CodeBlockDraw, plain_12kb, JsonCodeBlock(60, 12 * 1024, ""))->Unit(benchmark::kMicrosecond);

// 2 万行 6 列の表。日本語・右揃え・中央揃えを含み、中身はほぼ行ごとに違う。
Document MakeBenchTableDoc()
{
    std::string md = "| 名前 | 状態 | 説明 | 数量 | 単価 | 備考 |\n|---|---|---|--:|--:|:-:|\n";
    for (size_t r = 0; r < 20000; ++r) {
        const auto n = std::to_string(r);
        md += "| 項目" + n + " | 状態" + std::to_string(r % 97) + " | 説明のテキスト " + n + " を少し長めに書く | " + n + " | " +
              std::to_string(r * 7 % 1000) + " | 備考" + n + " |\n";
    }
    return Document::FromMarkdown(std::move(md), L"bench.md");
}

// 大きな表 (evict 前提) の計測。ワーカー 8 本。
class TableMeasure : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override
    {
        doc_.emplace(MakeBenchTableDoc());
        table_ = FirstTableOrSkip(state, *doc_);
        layout_.emplace(8);
        if (!table_) {
            return;
        }
        layout_->SkipIfFailed(state);
    }
    void TearDown(benchmark::State&) override
    {
        layout_.reset();
        doc_.reset();
    }

    // 1 画面 (1000 DIP) 分の行を、表を kRestores 等分した k 番目の位置で復元する。
    static constexpr int kRestores = 20;

protected:
    void MeasureInitial(NodeLayoutEntry& entry)
    {
        MeasureEntry(layout_->measurer, *table_, entry, 1200.0f, nullptr);
    }
    void RestoreScreen(NodeLayoutEntry& entry, int k)
    {
        const float top = entry.height * static_cast<float>(k) / static_cast<float>(kRestores);
        RestoreEntryTableRows(layout_->measurer, *table_, entry, { top, top + 1000.0f });
    }

    std::optional<Document> doc_;
    std::optional<DWriteLayout> layout_;
    const Node* table_ = nullptr;
};

BENCHMARK_DEFINE_F(TableMeasure, Initial)(benchmark::State& state)
{
    for (auto _ : state) {
        NodeLayoutEntry entry;
        state.SetIterationTime(SecondsOf([&] { MeasureInitial(entry); }));
    }
}
BENCHMARK_REGISTER_F(TableMeasure, Initial)->UseManualTime()->Iterations(3)->Unit(benchmark::kMillisecond);

// 反復ごとに別の位置を復元する。
BENCHMARK_DEFINE_F(TableMeasure, RestoreScreen)(benchmark::State& state)
{
    if (state.skipped()) {
        return;
    }
    NodeLayoutEntry entry;
    MeasureInitial(entry);
    int k = 0;
    for (auto _ : state) {
        state.SetIterationTime(SecondsOf([&] { RestoreScreen(entry, k++); }));
    }
}
BENCHMARK_REGISTER_F(TableMeasure, RestoreScreen)->UseManualTime()->Iterations(TableMeasure::kRestores)->Unit(benchmark::kMicrosecond);

// あちこちを復元したあとに幅を変えたときの、先頭 1 画面の計測。
BENCHMARK_DEFINE_F(TableMeasure, Resize)(benchmark::State& state)
{
    for (auto _ : state) {
        NodeLayoutEntry entry;
        MeasureInitial(entry);
        for (int k = 0; k < kRestores; ++k) {
            RestoreScreen(entry, k);
        }
        entry.layout_dirty = true;
        state.SetIterationTime(SecondsOf([&] { MeasureEntry(layout_->measurer, *table_, entry, 1000.0f, nullptr); }));
    }
}
BENCHMARK_REGISTER_F(TableMeasure, Resize)->UseManualTime()->Iterations(3)->Unit(benchmark::kMicrosecond);

// 大きな表の上下 8 画面分の行が残っている状態で幅を変えたときの、可視範囲のレイアウト。
BENCHMARK_DEFINE_F(TableMeasure, ResizeViewport)(benchmark::State& state)
{
    constexpr float kScreen = 1000.0f;
    const auto width = static_cast<float>(state.range(0));
    ViewportManager viewport;
    LayoutService service(layout_->engine, viewport);
    for (auto _ : state) {
        LayoutCache cache(doc_->GetNodes().size());
        ComputeLayout(layout_->engine, doc_->GetNodes(), cache, 1300.0f, 0.0f, kScreen);
        const float mid = cache.Bottom(cache.size() - 1) / 2.0f;
        layout_->engine.EnsureVisibleLayout(doc_->GetNodes(), cache, 1300.0f, mid - 8.0f * kScreen, mid + 9.0f * kScreen);
        viewport.SetScrollY(mid);
        state.SetIterationTime(SecondsOf([&] { service.ViewportLayout(*doc_, cache, width, kScreen); }));
    }
}
BENCHMARK_REGISTER_F(TableMeasure, ResizeViewport)
    ->ArgName("width")
    ->Arg(1100)
    ->Arg(1000)
    ->Arg(1200)
    ->Arg(900)
    ->UseManualTime()
    ->Iterations(3)
    ->Unit(benchmark::kMicrosecond);

// 列の幅が変わらない狭い表 (2 万行 5 列) で幅を変えたときの、表全体の計測。セルを組み直さない高速経路を通る。
void BM_TableResizeFastPath(benchmark::State& state)
{
    std::string md = "| A | B | C | D | E |\n|---|---|---|---|---|\n";
    for (size_t i = 0; i < 20000; ++i) {
        std::format_to(std::back_inserter(md), "| r{} | value {} | 東京都 | x | 12.5 |\n", i, i * 7);
    }
    const auto opened = OpenDoc(state, std::move(md), 0);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    const Node* table_node = FirstTableOrSkip(state, doc);
    if (!table_node) {
        return;
    }
    NodeLayoutEntry entry;
    l.measurer.MeasureNode(*table_node, entry, 760.0f);
    constexpr std::array kWidths{ 700.0f, 760.0f };
    size_t k = 0;
    for (auto _ : state) {
        state.SetIterationTime(SecondsOf([&] { l.measurer.MeasureNode(*table_node, entry, kWidths[k++ % kWidths.size()]); }));
    }
}
BENCHMARK(BM_TableResizeFastPath)->UseManualTime()->Iterations(10)->Unit(benchmark::kMillisecond);

// 列に同じ値が並ぶ大きな表の計測 (20 列)。測るセルが代表だけになるので、代表のセルの探索が時間の大半を占める。ワーカー 8 本。
void BM_TableFewValues(benchmark::State& state)
{
    constexpr int kCols = 20;
    const auto rows = static_cast<size_t>(state.range(0));
    std::string head;
    std::string rule;
    for (int c = 0; c < kCols; ++c) {
        head += "| h ";
        rule += "|---";
    }
    std::string md = head + "|\n" + rule + "|\n";
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < kCols; ++c) {
            std::format_to(std::back_inserter(md), "| 状態{} ", (r * (c + 1)) % 7);
        }
        md += "|\n";
    }
    const auto opened = OpenDoc(state, std::move(md), 8);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    const Node* table = FirstTableOrSkip(state, doc);
    if (!table) {
        return;
    }
    for (auto _ : state) {
        NodeLayoutEntry entry;
        state.SetIterationTime(SecondsOf([&] { MeasureEntry(l.measurer, *table, entry, 1200.0f, nullptr); }));
    }
}
BENCHMARK(BM_TableFewValues)->Arg(20'000)->Arg(200'000)->UseManualTime()->Iterations(5)->Unit(benchmark::kMillisecond);

// インラインコードを含む 5000 行の表。行ごとにインラインコードの背景も作られる。
Document MakeInlineCodeTableDoc()
{
    static constexpr std::array<std::string_view, 15> kWords{
        "設定",
        "値を",
        "読み込み",
        "表示する",
        "ときに",
        "使う",
        "関数で",
        "fallback",
        "cache",
        "の挙動",
        "を確認",
        "した結果",
        "render",
        "layout",
        "が変わる",
    };
    const auto words = [](size_t seed, size_t count) static noexcept {
        std::string s;
        for (size_t k = 0; k < count; ++k) {
            if (k > 0) {
                s += ' ';
            }
            s += kWords[(seed * 7 + k * 3) % kWords.size()];
        }
        return s;
    };
    std::string md = "# 大きなテーブル\n\n| ID | 呼び出し | 説明 | オプション | 数値 | 備考 |\n|---|---|---|---|---:|---|\n";
    for (size_t i = 0; i < 5000; ++i) {
        std::format_to(std::back_inserter(md), "| {0} | `get_value({0})` を呼ぶ | {1} | `--flag={0}` と `-v` | {2} | {3} |\n",
                       i, words(i, 6), i * 7 % 1000, words(i + 1, 3));
    }
    return Document::FromMarkdown(std::move(md), L"bench.md");
}

constexpr float kTableViewWidth = 1400.0f;
constexpr float kTableViewHeight = 900.0f;

// 表を開いて最初の画面を組んだキャッシュ。
LayoutCache OpenTable(const Document& doc, LayoutEngine& engine)
{
    LayoutCache cache(doc.GetNodes().size());
    engine.Reestimate(doc.GetNodes(), cache, kTableViewWidth);
    engine.EnsureVisibleLayout(doc.GetNodes(), cache, kTableViewWidth, 0.0f, kTableViewHeight);
    return cache;
}

// 1 画面ずつ下へスクロールしたときの、行の復元と evict。測るのは time_evict で選んだ方。ワーカー 8 本。
// 捨てたレイアウトの解放は呼び出し側がワーカーへ回すので測らない。
void BM_TableScroll(benchmark::State& state, bool time_evict)
{
    const auto doc = MakeInlineCodeTableDoc();
    const auto& nodes = doc.GetNodes();
    const auto& tables = doc.GetTableNodeIndices();
    DWriteLayout l(8);
    if (l.SkipIfFailed(state)) {
        return;
    }
    auto cache = OpenTable(doc, l.engine);
    float top = 0.0f;
    for (auto _ : state) {
        top += kTableViewHeight;
        const double restore = SecondsOf([&] { l.engine.EnsureVisibleLayout(nodes, cache, kTableViewWidth, top, top + kTableViewHeight); });
        const double evict = SecondsOf([&] { cache.EvictInvisibleTableRows(tables, layout::BufferedRange(top, kTableViewHeight, 2.0f)); });
        state.SetIterationTime(time_evict ? evict : restore);
        cache.TakeReleasedLayouts();
    }
}
BENCHMARK_CAPTURE(BM_TableScroll, restore, false)->UseManualTime()->Iterations(100)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_TableScroll, evict, true)->UseManualTime()->Iterations(100)->Unit(benchmark::kMicrosecond);

// 遅延レイアウトを期限で区切ったときの、全 dirty を計測し終えるまで。budget 0 は区切らない。ワーカー 8 本。
void BM_DirtyBatchDeadline(benchmark::State& state)
{
    const auto opened = OpenDoc(state, RepeatTestMd(state, 4), 8);
    if (!opened) {
        return;
    }
    auto& [doc, l] = *opened;
    const auto budget = std::chrono::milliseconds(state.range(0));
    const auto deadline = [&] {
        return budget.count() > 0 ? std::chrono::steady_clock::now() + budget : kNoLayoutDeadline;
    };
    for (auto _ : state) {
        LayoutCache cache(doc.GetNodes().size());
        l.engine.Reestimate(doc.GetNodes(), cache, 1200.0f);
        state.SetIterationTime(SecondsOf([&] {
            while (l.engine.ProcessDirtyBatch(doc.GetNodes(), cache, 1200.0f, deadline(), YRange::All())) {
            }
        }));
    }
}
BENCHMARK(BM_DirtyBatchDeadline)->ArgName("budget_ms")->Arg(4)->Arg(0)->UseManualTime()->Unit(benchmark::kMillisecond);

// ===== リロード =====

// 大きな文書の末尾近くを編集して保存したときの、リロードの UI スレッド側の処理。100MB の文書を 2 つ作る。
class Reload : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override
    {
        auto old_md = RepeatTestMd(state, 0, kLargeBytes);
        if (state.skipped()) {
            return;
        }
        auto new_md = old_md;
        new_md.insert(new_md.size() - 200, "\n\n追記した段落。\n\n");
        old_doc_.emplace(Document::FromMarkdown(std::move(old_md), L"bench.md"));
        new_doc_.emplace(Document::FromMarkdown(std::move(new_md), L"bench.md"));
        ends_ = FindUnchangedEnds(old_doc_->GetNodes(), new_doc_->GetNodes());
        ResetCaches();
        state.counters["nodes"] = static_cast<double>(new_doc_->GetNodes().size());
    }
    void TearDown(benchmark::State&) override
    {
        old_cache_ = {};
        new_cache_ = {};
        old_doc_.reset();
        new_doc_.reset();
    }

protected:
    void ResetCaches()
    {
        old_cache_ = layout::MakeEstimatedLayoutCache(old_doc_->GetNodes(), theme_, 800.0f);
        new_cache_ = layout::MakeEstimatedLayoutCache(new_doc_->GetNodes(), theme_, 800.0f);
    }

    Theme theme_ = GetLightTheme();
    std::optional<Document> old_doc_;
    std::optional<Document> new_doc_;
    LayoutCache old_cache_;
    LayoutCache new_cache_;
    UnchangedEnds ends_;
};

BENCHMARK_DEFINE_F(Reload, FindUnchangedEnds)(benchmark::State& state)
{
    for (auto _ : state) {
        benchmark::DoNotOptimize(FindUnchangedEnds(old_doc_->GetNodes(), new_doc_->GetNodes()));
    }
}
BENCHMARK_REGISTER_F(Reload, FindUnchangedEnds)->Iterations(50)->Unit(benchmark::kMillisecond);

// 旧キャッシュから項目を移すので、反復ごとにキャッシュを作り直す。
BENCHMARK_DEFINE_F(Reload, CarryOverUnchanged)(benchmark::State& state)
{
    for (auto _ : state) {
        ResetCaches();
        state.SetIterationTime(SecondsOf([&] {
            layout::CarryOverUnchanged(old_doc_->GetNodes(), old_cache_, new_doc_->GetNodes(), new_cache_, ends_);
        }));
    }
}
BENCHMARK_REGISTER_F(Reload, CarryOverUnchanged)->UseManualTime()->Iterations(3)->Unit(benchmark::kMicrosecond);

// 保存した直後の 1 回目の EvictTextLayouts。旧キャッシュで layout を持っていたのは末尾近く (編集している所) だけ。
BENCHMARK_DEFINE_F(Reload, FirstEvictAfterCarryOver)(benchmark::State& state)
{
    const size_t n = new_doc_->GetNodes().size();
    const size_t keep_begin = n - std::min<size_t>(n, 200);
    for (auto _ : state) {
        ResetCaches();
        old_cache_.EvictTextLayouts(keep_begin, old_cache_.size());
        layout::CarryOverUnchanged(old_doc_->GetNodes(), old_cache_, new_doc_->GetNodes(), new_cache_, ends_);
        state.SetIterationTime(SecondsOf([&] { new_cache_.EvictTextLayouts(keep_begin, n); }));
    }
}
BENCHMARK_REGISTER_F(Reload, FirstEvictAfterCarryOver)->UseManualTime()->Iterations(10)->Unit(benchmark::kMicrosecond);

BENCHMARK_DEFINE_F(Reload, MakeEstimatedLayoutCache)(benchmark::State& state)
{
    for (auto _ : state) {
        LayoutCache cache;
        state.SetIterationTime(SecondsOf([&] { cache = layout::MakeEstimatedLayoutCache(new_doc_->GetNodes(), theme_, 800.0f); }));
    }
}
BENCHMARK_REGISTER_F(Reload, MakeEstimatedLayoutCache)->UseManualTime()->Iterations(10)->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(Reload, RecomputeYPositions)(benchmark::State& state)
{
    if (state.skipped()) {
        return;
    }
    layout::CarryOverUnchanged(old_doc_->GetNodes(), old_cache_, new_doc_->GetNodes(), new_cache_, ends_);
    for (auto _ : state) {
        layout::RecomputeYPositions(new_doc_->GetNodes(), new_cache_, theme_);
    }
}
BENCHMARK_REGISTER_F(Reload, RecomputeYPositions)->Iterations(500)->Unit(benchmark::kMicrosecond);

// ===== reducer =====

// 既定のリソースからの確保を数える。Reduce は副作用の一覧を既定のリソースから取る。
class CountingResource final : public std::pmr::memory_resource {
public:
    explicit CountingResource(std::pmr::memory_resource* upstream) noexcept : upstream_(upstream)
    {}
    size_t allocations = 0;

private:
    void* do_allocate(size_t bytes, size_t align) override
    {
        ++allocations;
        return upstream_->allocate(bytes, align);
    }
    void do_deallocate(void* p, size_t bytes, size_t align) override
    {
        upstream_->deallocate(p, bytes, align);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }
    std::pmr::memory_resource* upstream_;
};

// マウスの移動・ホイール・ドラッグで 1 イベントごとに Reduce が受けるアクション。どれも毎回見た目が変わる (副作用を積む)。
enum class ReduceEvent : uint8_t {
    Hover,         // ホバー強調が変わる (InvalidateWindow)
    HoverSame,     // 同じ対象の上を動く (副作用なし)
    Wheel,         // 本文のスクロール (InvalidateWindow + BitmapManage)
    SelectDrag,    // テキスト選択のドラッグ (InvalidateWindow)
    ScrollbarDrag, // スクロールバーのドラッグ (InvalidateWindow + BitmapManage)
};

AppAction MakeReduceEvent(ReduceEvent kind, int i)
{
    const bool odd = (i & 1) != 0;
    switch (kind) {
    case ReduceEvent::Hover:
        return PointerHoverAction{ PointerHover{ { .h_block = odd ? 3 : -1 } } };
    case ReduceEvent::HoverSame:
        return PointerHoverAction{ PointerHover{ { .h_block = 3 } } };
    case ReduceEvent::Wheel:
        return MouseWheelAction{ .delta = odd ? WHEEL_DELTA : -WHEEL_DELTA, .tick = static_cast<uint64_t>(i) };
    case ReduceEvent::SelectDrag:
        return LeftDragMovedAction{ .node_index = odd ? 7 : 8, .text_pos = 2 };
    case ReduceEvent::ScrollbarDrag:
        return LeftDragMovedAction{ .dip_y = odd ? 100.0f : 200.0f };
    }
    std::unreachable();
}

// App::Dispatch と同じく、副作用の一覧をイベントごとのスタックの領域から取る。allocs/event は領域からあふれた確保の回数。
void BM_ReduceEvents(benchmark::State& state, ReduceEvent kind)
{
    constexpr int kEvents = 10000;
    ReducerHarness h({ .markdown = MakeHarnessMarkdown(200) });
    h.state.view.viewport.ScrollTo(5000.0f, MdMaxScroll(h.state));
    if (kind == ReduceEvent::SelectDrag) {
        h.state.view.drag = { .kind = LeftDrag::TextSelect, .select_node = 5 };
    }
    else if (kind == ReduceEvent::ScrollbarDrag) {
        h.state.view.drag = { .kind = LeftDrag::MdScrollbar };
    }
    std::vector<AppAction> events;
    events.reserve(kEvents);
    for (int i = 0; i < kEvents; ++i) {
        events.push_back(MakeReduceEvent(kind, i));
    }

    CountingResource counter(std::pmr::new_delete_resource());
    std::pmr::memory_resource* const previous = std::pmr::set_default_resource(&counter);
    size_t effects = 0;
    size_t duplicate_invalidates = 0;
    for (auto _ : state) {
        for (const auto& e : events) {
            alignas(std::max_align_t) std::array<std::byte, 1024> buffer;
            std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());
            const auto list = h.Dispatch(e, &arena);
            effects += list.size();
            const auto invalidates = CountEffect<effect::InvalidateWindow>(list);
            duplicate_invalidates += invalidates > 1 ? static_cast<size_t>(invalidates - 1) : 0;
            benchmark::DoNotOptimize(list.data());
        }
    }
    std::pmr::set_default_resource(previous);
    const auto total = static_cast<double>(state.iterations()) * kEvents;
    state.counters["allocs/event"] = static_cast<double>(counter.allocations) / total;
    state.counters["effects/event"] = static_cast<double>(effects) / total;
    state.counters["dup_invalidate/event"] = static_cast<double>(duplicate_invalidates) / total;
    state.SetItemsProcessed(state.iterations() * kEvents);
}
BENCHMARK_CAPTURE(BM_ReduceEvents, hover, ReduceEvent::Hover)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_ReduceEvents, hover_same, ReduceEvent::HoverSame)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_ReduceEvents, wheel, ReduceEvent::Wheel)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_ReduceEvents, select_drag, ReduceEvent::SelectDrag)->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_ReduceEvents, scrollbar_drag, ReduceEvent::ScrollbarDrag)->Unit(benchmark::kMicrosecond);

} // namespace
