#include <gtest/gtest.h>
#include "app_constants.h"
#include "diagram_raster.h"
#include "document.h"
#include "mermaid.h"
#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "pele_test_assets.h"
#include "task_scheduler.h"
#include "test_helpers.h"
#include "ui_constants.h"
#include "wic_util.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kMaxWidth = 800.0f;
// どちらも幅の上限より狭く描ける。ラベルで見分ける (SVG の文字)。
constexpr std::string_view kAlpha = "flowchart LR\n  P[Alpha] --> Q[Omega] --> R[Sigma]\n";
constexpr std::string_view kBeta = "flowchart TD\n  X[Beta] --> Y[Gamma]\n";
constexpr std::string_view kBroken = "flowchart LR\n  A[ --> ";

// 図を 1 つだけ持つ文書。ノードは文書が持つので、文書ごと持ち続ける。
struct DiagramDoc {
    Document doc;
    explicit DiagramDoc(std::string_view code) : doc(Document::FromMarkdown(std::format("```mermaid\n{}```\n", code), L"C:\\t.md"))
    {}
    const Node& node() const
    {
        return doc.GetNodes()[doc.GetDiagramNodeIndices().at(0)];
    }
    uint64_t RequestHash() const
    {
        return mermaid_util::DiagramRequestHash(mermaid_util::DiagramCodeHash(node()), kMaxWidth, false);
    }
    uint64_t WidthFreeHash() const
    {
        return mermaid_util::DiagramWidthFreeHash(mermaid_util::DiagramCodeHash(node()), false);
    }
};

constexpr std::string_view kGamma = "flowchart TD\n  M[Gamma] --> N[Delta]\n";
constexpr std::string_view kWide = "flowchart LR\n  A[aaaaaaaaaaaaaaaa] --> B[bbbbbbbbbbbbbbbb] --> C[cccccccccccccccc] --> D[dddddddddddddddd]\n";

// 拡大表示の窓に頼まれた操作の記録。窓が壊れた後も見られるよう、窓の外に置く。
struct ViewerLog {
    int opened = 0;
    int loading = 0;
    int front = 0;
    bool alive = false;
    bool dark = false;
    bool fail_open = false;
    std::vector<std::shared_ptr<const mermaid_util::DiagramDrawing>> drawings;
};

// 窓を作らない拡大表示 (本物は開くと前面を取る)。
class FakeViewer final : public IDiagramViewer {
public:
    FakeViewer(ViewerLog& log, bool dark) : log_(log)
    {
        ++log_.opened;
        log_.alive = true;
        log_.dark = dark;
    }
    ~FakeViewer() override
    {
        log_.alive = false;
    }
    bool IsOpen() const noexcept override
    {
        return true;
    }
    bool HasDrawing() const noexcept override
    {
        return has_drawing_;
    }
    void BringToFront() const noexcept override
    {
        ++log_.front;
    }
    void ApplyLanguage() override
    {}
    void SetDarkMode(bool dark) override
    {
        log_.dark = dark;
    }
    void ShowLoading() override
    {
        ++log_.loading;
        has_drawing_ = false;
    }
    void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing) override
    {
        has_drawing_ = true;
        log_.drawings.push_back(std::move(drawing));
    }

private:
    ViewerLog& log_;
    bool has_drawing_ = false;
};

// MermaidRenderer を、ビルドで作った pele と KaTeX のバイトコードで動かす (テストの exe は埋め込みリソースを持たない)。
// 描画先は WIC のビットマップ。bg のワーカーは 2 本で、同時に描くのは 1 つ (PumpRenders)。
class MermaidRendererTest : public ComApartmentTest {
protected:
    static void SetUpTestSuite()
    {
        ComApartmentTest::SetUpTestSuite();
        assets_ = PeleTestAssets::Load();
    }

    static std::unique_ptr<PeleEngine> NewEngine(IDWriteFactory* dw)
    {
        return assets_.NewEngine(dw);
    }

    void SetUp() override
    {
        ASSERT_TRUE(assets_.ok());
        ASSERT_HRESULT_SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()));
        wic_ = wic_util::CreateWicFactory("MermaidRendererTest");
        ASSERT_NE(wic_, nullptr);
        ComPtr<IWICBitmap> bitmap;
        ASSERT_HRESULT_SUCCEEDED(wic_->CreateBitmap(4, 4, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
        ASSERT_HRESULT_SUCCEEDED(d2d_->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &rt_));

        file_cache_.SetCacheDir(temp_.path());
        file_cache_.Init(file_scheduler_);
        viewer_ = {};
        renderer_.Init(
            window_.Get(), d2d_.Get(), wic_.Get(), SharedDWriteFactory(), file_cache_, scheduler_, NewEngine, OpenFakeViewer, [this] { OnUpdate(); },
            [this] { ++viewer_failed_; });
        renderer_.SetRenderTarget(rt_.Get());
    }

    // 通知のたびに、拡大表示の図を今の本文の幅とテーマで頼む (ResourceManager::RequestVisibleDiagrams と同じ)。
    void OnUpdate()
    {
        ++updates_;
        if (request_viewer_) {
            renderer_.RequestViewerDrawing(viewer_width_, viewer_.dark);
        }
    }

    static std::unique_ptr<IDiagramViewer> OpenFakeViewer(HWND, ID2D1Factory*, IDWriteFactory*, IWICImagingFactory*, bool dark,
                                                          std::move_only_function<void()>)
    {
        return viewer_.fail_open ? nullptr : std::make_unique<FakeViewer>(viewer_, dark);
    }

    void OpenViewer(std::string_view code, int node = 0, float max_width = kMaxWidth)
    {
        viewer_width_ = max_width;
        renderer_.OpenViewer(code, node, /*dark_mode=*/false);
    }

    // 拡大表示に渡した図が count 個になるのを待つ。
    bool AwaitViewerDrawings(size_t count)
    {
        return Await([&] { return viewer_.drawings.size() >= count; });
    }

    void TearDown() override
    {
        renderer_.Shutdown();
    }

    // 結果を受け取りながら pred が成り立つのを待つ。
    template <class Pred>
    bool Await(Pred pred)
    {
        const auto ready = [&] {
            renderer_.ProcessJobResults(kNoDeadline);
            return pred();
        };
        return PollUntil(ready, std::chrono::seconds(30));
    }

    bool Request(const DiagramDoc& d, DiagramEntry& entry, float max_width = kMaxWidth)
    {
        return renderer_.RequestRender(d.node(), entry, max_width, /*dark_mode=*/false);
    }

    // 結果が出るまで要求し直す (結果を待つ図を on_update で要求し直す ResourceManager と同じ)。
    bool RenderUntilSettled(const DiagramDoc& d, DiagramEntry& entry, float max_width = kMaxWidth)
    {
        return Await([&] { return Request(d, entry, max_width); });
    }

    // 別の図の描画命令を、key のディスクキャッシュとして置く。ディスクから読んだ図はその描画命令で出るので、描き直したのと見分けられる。
    void StoreDrawingAs(const DiagramEntry& drawn, uint64_t key)
    {
        file_cache_.StoreAsync(key, drawn.width, drawn.height, mermaid_util::SerializeDrawing(*drawn.drawing));
        ASSERT_TRUE(DrainScheduler(file_scheduler_));
    }

    static std::string SvgOf(const DiagramEntry& entry)
    {
        return DrawingToSvg(*entry.drawing);
    }

    static inline PeleTestAssets assets_;
    static inline ViewerLog viewer_;
    int viewer_failed_ = 0;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1RenderTarget> rt_;
    TaskScheduler file_scheduler_{ 1 };
    TaskScheduler scheduler_{ 2 };
    ScopedTempDir temp_;
    MermaidFileCache file_cache_;
    MessageOnlyWindow window_;
    MermaidRenderer renderer_;
    int updates_ = 0;
    // 偽なら通知を受けても拡大表示の図を頼まない (別の文書を読み込み中の ResourceManager)。
    bool request_viewer_ = true;
    float viewer_width_ = kMaxWidth;
};

// 描き終えるまでは要求し直しても描き直さない。描いた図はキャッシュから答え、幅の上限より狭いので幅を混ぜないキーでディスクに置く。
TEST_F(MermaidRendererTest, RendersDiagramThenAnswersFromCache)
{
    const DiagramDoc d(kAlpha);
    DiagramEntry entry;
    EXPECT_FALSE(Request(d, entry));
    EXPECT_FALSE(Request(d, entry));
    ASSERT_TRUE(Await([&] { return updates_ > 0; }));

    EXPECT_TRUE(Request(d, entry));
    EXPECT_TRUE(entry.HasImage());
    ASSERT_NE(entry.drawing, nullptr);
    EXPECT_TRUE(entry.error.empty());
    EXPECT_TRUE(SvgOf(entry).contains("Alpha"));
    EXPECT_TRUE(file_cache_.Find(d.WidthFreeHash(), 0, kMaxWidth, false).has_value());
}

// 描いた図のエラーは描き直しても同じなので確定させ、ディスクには置かない。
TEST_F(MermaidRendererTest, SettlesRenderError)
{
    const DiagramDoc d(kBroken);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    EXPECT_FALSE(entry.HasImage());
    EXPECT_EQ(entry.error.kind, DiagramError::Kind::Render);
    EXPECT_FALSE(entry.error.message.empty());
    EXPECT_EQ(file_cache_.EntryCount(), 0u);
}

// ディスクキャッシュに当たれば描かずに読む。
TEST_F(MermaidRendererTest, LoadsFromDiskCache)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry drawn;
    ASSERT_TRUE(RenderUntilSettled(alpha, drawn));

    const DiagramDoc beta(kBeta);
    StoreDrawingAs(drawn, beta.RequestHash());
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));
    EXPECT_TRUE(SvgOf(entry).contains("Alpha"));
    EXPECT_EQ(entry.width, drawn.width);
}

// ディスクキャッシュを解析できなければ描き直す。
TEST_F(MermaidRendererTest, RerendersWhenDiskCacheIsBroken)
{
    const DiagramDoc beta(kBeta);
    const uint64_t key = beta.RequestHash();
    file_cache_.StoreAsync(key, 10.0f, 10.0f, "not a drawing");
    ASSERT_TRUE(DrainScheduler(file_scheduler_));

    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));
    EXPECT_TRUE(entry.HasImage());
    EXPECT_TRUE(SvgOf(entry).contains("Beta"));
}

// ディスクキャッシュのファイルが無くなっていれば索引から外し、描き直す。
TEST_F(MermaidRendererTest, RerendersAndForgetsMissingDiskCache)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry drawn;
    ASSERT_TRUE(RenderUntilSettled(alpha, drawn));

    const DiagramDoc beta(kBeta);
    const uint64_t key = beta.RequestHash();
    StoreDrawingAs(drawn, key);
    ASSERT_TRUE(std::filesystem::remove(file_cache_.FilePath(key)));

    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));
    EXPECT_TRUE(SvgOf(entry).contains("Beta"));
    EXPECT_FALSE(file_cache_.Find(0, key, kMaxWidth, false).has_value());
}

// 取り消すと、投入済みの表示用の図は描かずに返り、待ちの表示用の図は捨てる。どちらも要求中の印を外して頼み直させる。
// インライン数式の要求は残し、取り消した図が使っていた同時に描く数の枠も返す。
TEST_F(MermaidRendererTest, DropQueuedCancelsDisplayRequestsAndKeepsMath)
{
    const DiagramDoc alpha(kAlpha);
    const DiagramDoc beta(kBeta);
    DiagramEntry ea;
    DiagramEntry eb;
    std::optional<InlineMathBatchResult> math;
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        EXPECT_FALSE(Request(alpha, ea));
        EXPECT_FALSE(Request(beta, eb));
        renderer_.RequestMathBatch({ { .key = 1, .tex = "x", .font_size = 16.0f } }, [&](InlineMathBatchResult r) { math = std::move(r); });
        renderer_.DropQueued();
    }
    ASSERT_TRUE(Await([&] { return math.has_value(); }));
    EXPECT_NE(math->items.at(0).drawing, nullptr);
    EXPECT_GT(updates_, 0);
    EXPECT_FALSE(ea.HasImage());

    ASSERT_TRUE(RenderUntilSettled(alpha, ea));
    ASSERT_TRUE(RenderUntilSettled(beta, eb));
    EXPECT_TRUE(ea.HasImage());
    EXPECT_TRUE(eb.HasImage());
}

// インライン数式は要求と同じ順で返し、描けない数式は描画命令も寸法も空。
TEST_F(MermaidRendererTest, RendersMathBatchInRequestOrder)
{
    std::optional<InlineMathBatchResult> result;
    renderer_.RequestMathBatch({ { .key = 7, .tex = "x^2", .font_size = 16.0f }, { .key = 9, .tex = "\\frac{", .font_size = 16.0f } },
                               [&](InlineMathBatchResult r) { result = std::move(r); });
    ASSERT_TRUE(Await([&] { return result.has_value(); }));
    ASSERT_EQ(result->items.size(), 2u);
    EXPECT_EQ(result->items[0].key, 7u);
    EXPECT_NE(result->items[0].drawing, nullptr);
    EXPECT_TRUE(result->items[0].metrics.ok());
    EXPECT_EQ(result->items[1].key, 9u);
    EXPECT_EQ(result->items[1].drawing, nullptr);
    EXPECT_FALSE(result->items[1].metrics.ok());
}

// 数式の取り消しは、待っている数式の要求だけを捨てる。
TEST_F(MermaidRendererTest, DropQueuedMathDropsOnlyWaitingMath)
{
    const DiagramDoc alpha(kAlpha);
    const DiagramDoc beta(kBeta);
    DiagramEntry ea;
    DiagramEntry eb;
    bool math_called = false;
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        EXPECT_FALSE(Request(alpha, ea));
        renderer_.RequestMathBatch({ { .key = 1, .tex = "x", .font_size = 16.0f } }, [&](InlineMathBatchResult) { math_called = true; });
        renderer_.DropQueuedMath();
        EXPECT_FALSE(Request(beta, eb));
    }
    // 数式が残っていれば、2 つ目の図より先に描かれる。
    ASSERT_TRUE(RenderUntilSettled(alpha, ea));
    ASSERT_TRUE(RenderUntilSettled(beta, eb));
    EXPECT_FALSE(math_called);
}

// 描画先が無い間は結果を受け取らない (記録できないだけの結果をエラーとして確定させない)。描画先が戻ったら受け取る。
TEST_F(MermaidRendererTest, KeepsResultsUntilRenderTargetReturns)
{
    renderer_.SetRenderTarget(nullptr);
    const DiagramDoc d(kAlpha);
    DiagramEntry entry;
    EXPECT_FALSE(Request(d, entry));
    ASSERT_TRUE(PollUntil([&] { return window_.DrainMessages(app_msg::MERMAID_JOB_DONE) > 0; }, std::chrono::seconds(30)));
    renderer_.ProcessJobResults(kNoDeadline);
    EXPECT_EQ(updates_, 0);
    EXPECT_FALSE(renderer_.HasLeftoverJobResults());

    renderer_.SetRenderTarget(rt_.Get());
    renderer_.ProcessJobResults(kNoDeadline);
    EXPECT_EQ(updates_, 1);
    EXPECT_TRUE(Request(d, entry));
    EXPECT_TRUE(entry.HasImage());
}

// 幅の上限に収まらない図は、表示の矩形を上限の幅に縮める。保存・コピーの画像は縮めた大きさ (高さは 1/64 px に切り捨てて
// から切り上げ) x DPR、クリップボードの SVG は縮めない大きさ。
TEST_F(MermaidRendererTest, WideDiagramFitsDisplayAndBitmapButNotSvg)
{
    constexpr float kNarrow = 200.0f;
    const DiagramDoc d(kWide);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(d, entry, kNarrow));
    ASSERT_TRUE(entry.CanExport());

    const std::string svg = SvgOf(entry);
    float w = 0.0f;
    float h = 0.0f;
    ASSERT_EQ(sscanf_s(svg.c_str(), "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%f\" height=\"%f\"", &w, &h), 2) << svg;
    ASSERT_GT(w, kNarrow);
    const float fit_h = mermaid_util::FittedDiagramSize(*entry.drawing, kNarrow).height;

    const D2D1_RECT_F rect = DiagramRect(entry.width, entry.height, 0.0f, kNarrow, 0.0f);
    EXPECT_NEAR(rect.right - rect.left, kNarrow, 0.001f);
    EXPECT_NEAR(rect.bottom - rect.top, fit_h, 1.0f);

    const auto bitmap = renderer_.DiagramBitmap(*entry.drawing, kNarrow);
    ASSERT_NE(bitmap, nullptr);
    UINT pw = 0;
    UINT ph = 0;
    ASSERT_HRESULT_SUCCEEDED(bitmap->GetSize(&pw, &ph));
    const float dpr = DpiScaleFrom(static_cast<float>(GetDpiForWindow(window_.Get())));
    EXPECT_EQ(pw, static_cast<UINT>(std::ceil(kNarrow * dpr)));
    EXPECT_EQ(ph, static_cast<UINT>(std::ceil(fit_h * dpr)));
}

// ---- 拡大表示 ----

// 開くと窓を出し、描けたら図を渡す。図の目印とコードを覚える。
TEST_F(MermaidRendererTest, ViewerShowsDrawingWhenRendered)
{
    OpenViewer(kAlpha, 3);
    EXPECT_EQ(viewer_.opened, 1);
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_EQ(renderer_.ViewerNode(), 3);
    EXPECT_EQ(renderer_.ViewerCode(), kAlpha);
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Alpha"));
    EXPECT_EQ(viewer_failed_, 0);
}

// 本文で描けている図は、描き直さずにその描画命令をすぐ渡す。
TEST_F(MermaidRendererTest, ViewerUsesDrawingOfBody)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(alpha, entry));
    OpenViewer(alpha.node().GetText());
    ASSERT_EQ(viewer_.drawings.size(), 1u);
    EXPECT_EQ(viewer_.drawings[0], entry.drawing);
}

// 幅の上限に収まらない図も、本文が描いた結果を使う。本文を描いた後で幅が少し変わっていても (同じ 32px の刻みの中)、
// 描き直さずに本文と同じ折り返しの図を出す。
TEST_F(MermaidRendererTest, ViewerUsesBodyDrawingOfWideDiagramInSameWidthBucket)
{
    constexpr float kNarrow = 200.0f;
    const DiagramDoc wide(kWide);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(wide, entry, kNarrow));
    ASSERT_GT(entry.width, kNarrow);
    OpenViewer(wide.node().GetText(), 0, kNarrow + 5.0f);
    ASSERT_EQ(viewer_.drawings.size(), 1u);
    EXPECT_EQ(viewer_.drawings[0], entry.drawing);
}

// 拡大表示が頼んだ図は本文と同じ要求になり、同じ図を 2 回描かない (保存のたびに本文用と拡大表示用を描いていた)。
TEST_F(MermaidRendererTest, ViewerAndBodyShareOneRender)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry entry;
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        OpenViewer(alpha.node().GetText());
        EXPECT_FALSE(Request(alpha, entry));
    }
    ASSERT_TRUE(AwaitViewerDrawings(1));
    ASSERT_TRUE(Request(alpha, entry));
    EXPECT_EQ(viewer_.drawings[0], entry.drawing);
}

// 幅の上限に収まらない図も、拡大表示には縮めない大きさで渡す。
TEST_F(MermaidRendererTest, ViewerGetsWideDiagramAtNaturalSize)
{
    constexpr float kNarrow = 200.0f;
    OpenViewer(kWide, 0, kNarrow);
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_GT(viewer_.drawings[0]->width, kNarrow);
}

// 同じ図をもう一度開くと前面に出すだけ。別の図なら読み込み中に戻して描き直す。
TEST_F(MermaidRendererTest, ViewerReopenBringsToFrontOrReplaces)
{
    OpenViewer(kAlpha, 1);
    ASSERT_TRUE(AwaitViewerDrawings(1));
    OpenViewer(kAlpha, 2);
    EXPECT_EQ(viewer_.front, 1);
    EXPECT_EQ(viewer_.loading, 0);
    EXPECT_EQ(viewer_.opened, 1);
    EXPECT_EQ(renderer_.ViewerNode(), 2);
    EXPECT_EQ(viewer_.drawings.size(), 1u);

    OpenViewer(kBeta, 5);
    EXPECT_EQ(viewer_.loading, 1);
    EXPECT_EQ(viewer_.opened, 1);
    ASSERT_TRUE(AwaitViewerDrawings(2));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Beta"));
}

// 保存で図が変わったら、読み込み中に戻さずに差し替える。描けないコードになったら今の図を残し、窓も閉じない。
TEST_F(MermaidRendererTest, ViewerFollowsCodeAndKeepsDrawingOnError)
{
    OpenViewer(kAlpha);
    ASSERT_TRUE(AwaitViewerDrawings(1));
    renderer_.UpdateViewerCode(kBeta);
    ASSERT_TRUE(AwaitViewerDrawings(2));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Beta"));
    EXPECT_EQ(viewer_.loading, 0);

    renderer_.UpdateViewerCode(kBroken);
    // 同じ壊れた図の本文の結果が確定するころには、先に頼んだ拡大表示の分も済んでいる。
    const DiagramDoc broken(kBroken);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(broken, entry));
    EXPECT_FALSE(entry.error.empty());
    EXPECT_TRUE(viewer_.alive);
    EXPECT_EQ(viewer_.drawings.size(), 2u);
    EXPECT_EQ(viewer_failed_, 0);
    EXPECT_EQ(renderer_.ViewerCode(), kBroken);
}

// 開いた図が描けなければ、窓を閉じて知らせる。
TEST_F(MermaidRendererTest, ViewerFailsWhenFirstDrawingCannotRender)
{
    OpenViewer(kBroken, 4);
    ASSERT_TRUE(Await([&] { return viewer_failed_ == 1; }));
    EXPECT_FALSE(viewer_.alive);
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_EQ(renderer_.ViewerCode(), std::nullopt);
    EXPECT_EQ(renderer_.ViewerNode(), -1);
}

// 窓を開けなければ知らせ、図も描かせない。
TEST_F(MermaidRendererTest, ViewerFailsWhenWindowCannotOpen)
{
    viewer_.fail_open = true;
    OpenViewer(kAlpha);
    EXPECT_EQ(viewer_failed_, 1);
    EXPECT_EQ(renderer_.ViewerCode(), std::nullopt);
    const DiagramDoc beta(kBeta);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_EQ(viewer_failed_, 1);
}

// テーマの切り替えは窓の配色を変え、読み込み中に戻さずに新しいテーマの図に差し替える。
TEST_F(MermaidRendererTest, ViewerFollowsTheme)
{
    OpenViewer(kAlpha);
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_EQ(viewer_.drawings[0]->background, kDiagramLightBackground);
    renderer_.SetViewerDarkMode(true);
    EXPECT_TRUE(viewer_.dark);
    ASSERT_TRUE(AwaitViewerDrawings(2));
    EXPECT_EQ(viewer_.drawings[1]->background, kDiagramDarkBackground);
    EXPECT_EQ(viewer_.loading, 0);
}

// 拡大表示の図は RequestViewerDrawing で頼まれるまで描かない。開く・テーマ・コードの追従・待ちを捨てる・ほかの図の結果、
// のどれからも自分では頼まない (別の文書を読み込み中に、旧文書の図を描き始めない)。
TEST_F(MermaidRendererTest, ViewerDrawingIsRequestedOnlyThroughRequestViewerDrawing)
{
    request_viewer_ = false;
    OpenViewer(kAlpha);
    renderer_.SetViewerDarkMode(false);
    renderer_.UpdateViewerCode(kBeta);
    renderer_.DropQueued();
    // 同時に描くのは 1 つなので、頼んでいれば gamma より先に描き終えている。
    const DiagramDoc gamma(kGamma);
    const DiagramDoc beta(kBeta);
    DiagramEntry eg;
    DiagramEntry eb;
    ASSERT_TRUE(RenderUntilSettled(gamma, eg));
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_FALSE(Request(beta, eb));

    request_viewer_ = true;
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Beta"));
}

// 描画待ちを捨てても (大きな移動・幅やテーマの変更)、戻ってきた要求の通知で拡大表示の図を頼み直す。待ちにあっても、投入済みでも。
TEST_F(MermaidRendererTest, ViewerDrawingSurvivesDropQueued)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry ea;
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        EXPECT_FALSE(Request(alpha, ea));
        OpenViewer(kBeta);
        renderer_.DropQueued();
    }
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Beta"));
    renderer_.CloseViewer();
    ASSERT_TRUE(RenderUntilSettled(alpha, ea));

    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        OpenViewer(kGamma);
        renderer_.DropQueued();
    }
    ASSERT_TRUE(AwaitViewerDrawings(2));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Gamma"));
}

// 待っている間に閉じたら、後から届いた結果で壊れた窓に触らない。開き直せば描ける。
TEST_F(MermaidRendererTest, ViewerClosedWhileWaitingIgnoresResult)
{
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        OpenViewer(kAlpha);
        renderer_.CloseViewer();
    }
    EXPECT_FALSE(viewer_.alive);
    const DiagramDoc beta(kBeta);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_EQ(viewer_failed_, 0);

    OpenViewer(kAlpha);
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Alpha"));
}

// 描画先が無い間 (デバイスの作り直し) に頼んだ拡大表示の図は、描画先が戻ってから渡す。
TEST_F(MermaidRendererTest, ViewerDrawingWaitsForRenderTarget)
{
    renderer_.SetRenderTarget(nullptr);
    OpenViewer(kAlpha);
    ASSERT_TRUE(PollUntil([&] { return window_.DrainMessages(app_msg::MERMAID_JOB_DONE) > 0; }, std::chrono::seconds(30)));
    renderer_.ProcessJobResults(kNoDeadline);
    EXPECT_TRUE(viewer_.drawings.empty());

    renderer_.SetRenderTarget(rt_.Get());
    ASSERT_TRUE(AwaitViewerDrawings(1));
    EXPECT_EQ(viewer_failed_, 0);
}

// 終了は、待っている要求も描いている要求も、コールバックを呼ばずに捨てる。
TEST_F(MermaidRendererTest, ShutdownDropsPendingCallbacks)
{
    int called = 0;
    const DiagramDoc alpha(kAlpha);
    DiagramEntry entry;
    EXPECT_FALSE(Request(alpha, entry));
    renderer_.RequestMathBatch({ { .key = 1, .tex = "x", .font_size = 16.0f } }, [&](InlineMathBatchResult) { ++called; });
    renderer_.RequestMathBatch({ { .key = 2, .tex = "y", .font_size = 16.0f } }, [&](InlineMathBatchResult) { ++called; });
    renderer_.Shutdown();
    renderer_.ProcessJobResults(kNoDeadline);
    EXPECT_EQ(called, 0);
    EXPECT_EQ(updates_, 0);
}

// ---- DiagramView の倍率 ----

using diagram_view::FitZoom;
using diagram_view::NextZoom;

// 全体表示は余白を除いた窓に収め、等倍より大きくしない。潰した窓では最小の倍率で止める。
TEST(DiagramViewZoom, FitsInsidePaddingWithoutEnlarging)
{
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 800.0f, 200.0f, 100.0f), 1.0f);
    EXPECT_FLOAT_EQ(FitZoom(432.0f, 1000.0f, 800.0f, 100.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 232.0f, 100.0f, 400.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(10.0f, 10.0f, 1000.0f, 1000.0f), 0.05f);
}

// ボタンの止まる位置は表の値と全体表示の倍率。全体表示のすぐ隣の値は飛ばす。
TEST(DiagramViewZoom, StepsThroughStopsIncludingFit)
{
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 1.0f, 1), 1.25f);
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 1.0f, -1), 0.75f);
    // 全体表示 0.7 は止まる位置になり、すぐ隣の 0.67 と 0.75 は飛ばす
    EXPECT_FLOAT_EQ(*NextZoom(0.7f, 0.7f, 1), 1.0f);
    EXPECT_FLOAT_EQ(*NextZoom(0.7f, 0.7f, -1), 0.5f);
    EXPECT_FLOAT_EQ(*NextZoom(1.0f, 0.7f, -1), 0.7f);
    EXPECT_FALSE(NextZoom(8.0f, 1.0f, 1));
    EXPECT_FALSE(NextZoom(0.05f, 0.05f, -1));
}

} // namespace
