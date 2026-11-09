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
#include "wic_util.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
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
        return mermaid_util::NodeDiagramHash(node(), kMaxWidth, false);
    }
    uint64_t WidthFreeHash() const
    {
        return mermaid_util::DiagramWidthFreeHash(mermaid_util::DiagramCodeHash(node()), false);
    }
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
        renderer_.Init(window_.Get(), d2d_.Get(), wic_.Get(), SharedDWriteFactory(), file_cache_, scheduler_, NewEngine, [this] { ++updates_; }, [] {});
        renderer_.SetRenderTarget(rt_.Get());
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

    bool Request(const DiagramDoc& d, DiagramEntry& entry)
    {
        return renderer_.RequestRender(d.node(), entry, kMaxWidth, /*dark_mode=*/false);
    }

    // 結果が出るまで要求し直す (結果を待つ図を on_update で要求し直す ResourceManager と同じ)。
    bool RenderUntilSettled(const DiagramDoc& d, DiagramEntry& entry)
    {
        return Await([&] { return Request(d, entry); });
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
// SVG の要求は残し、取り消した図が使っていた同時に描く数の枠も返す。
TEST_F(MermaidRendererTest, DropQueuedCancelsDisplayRequestsAndKeepsSvg)
{
    const DiagramDoc alpha(kAlpha);
    const DiagramDoc beta(kBeta);
    DiagramEntry ea;
    DiagramEntry eb;
    std::optional<std::string> svg;
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        EXPECT_FALSE(Request(alpha, ea));
        EXPECT_FALSE(Request(beta, eb));
        renderer_.RequestSvg(kBeta, kMaxWidth, false, [&](std::string_view s) { svg.emplace(s); });
        renderer_.DropQueued();
    }
    ASSERT_TRUE(Await([&] { return svg.has_value(); }));
    EXPECT_TRUE(svg->contains("Beta"));
    EXPECT_GT(updates_, 0);
    EXPECT_FALSE(ea.HasImage());

    ASSERT_TRUE(RenderUntilSettled(alpha, ea));
    ASSERT_TRUE(RenderUntilSettled(beta, eb));
    EXPECT_TRUE(ea.HasImage());
    EXPECT_TRUE(eb.HasImage());
}

// クリップボードの SVG は pele で描く。描けなければ空で知らせる。
TEST_F(MermaidRendererTest, RendersSvgAndReportsFailureAsEmpty)
{
    std::optional<std::string> ok;
    std::optional<std::string> broken;
    renderer_.RequestSvg(kAlpha, kMaxWidth, false, [&](std::string_view s) { ok.emplace(s); });
    renderer_.RequestSvg(kBroken, kMaxWidth, false, [&](std::string_view s) { broken.emplace(s); });
    ASSERT_TRUE(Await([&] { return ok && broken; }));
    EXPECT_TRUE(ok->contains("Alpha"));
    EXPECT_TRUE(broken->empty());
}

// 本文で幅に縛られずに描けた図は、その描画命令から SVG を作る (pele で描き直さない)。
TEST_F(MermaidRendererTest, SvgUsesWidthFreeDrawingOfBody)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry drawn;
    ASSERT_TRUE(RenderUntilSettled(alpha, drawn));
    const DiagramDoc beta(kBeta);
    StoreDrawingAs(drawn, beta.WidthFreeHash());
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));

    std::optional<std::string> svg;
    renderer_.RequestSvg(beta.node().GetText(), kMaxWidth, false, [&](std::string_view s) { svg.emplace(s); });
    ASSERT_TRUE(Await([&] { return svg.has_value(); }));
    EXPECT_TRUE(svg->contains("Alpha"));
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
    std::optional<std::string> first;
    std::optional<std::string> second;
    bool math_called = false;
    {
        WorkerGate gate(scheduler_, 2);
        ASSERT_TRUE(gate.ready());
        renderer_.RequestSvg(kAlpha, kMaxWidth, false, [&](std::string_view s) { first.emplace(s); });
        renderer_.RequestMathBatch({ { .key = 1, .tex = "x", .font_size = 16.0f } }, [&](InlineMathBatchResult) { math_called = true; });
        renderer_.DropQueuedMath();
        renderer_.RequestSvg(kBeta, kMaxWidth, false, [&](std::string_view s) { second.emplace(s); });
    }
    // 数式が残っていれば、2 つ目の SVG より先に描かれる。
    ASSERT_TRUE(Await([&] { return first && second; }));
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

// 終了は、待っている要求も描いている要求も、コールバックを呼ばずに捨てる。
TEST_F(MermaidRendererTest, ShutdownDropsPendingCallbacks)
{
    int called = 0;
    renderer_.RequestSvg(kAlpha, kMaxWidth, false, [&](std::string_view) { ++called; });
    renderer_.RequestSvg(kBeta, kMaxWidth, false, [&](std::string_view) { ++called; });
    renderer_.RequestMathBatch({ { .key = 1, .tex = "x", .font_size = 16.0f } }, [&](InlineMathBatchResult) { ++called; });
    renderer_.Shutdown();
    renderer_.ProcessJobResults(kNoDeadline);
    EXPECT_EQ(called, 0);
}

} // namespace
