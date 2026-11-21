#include <gtest/gtest.h>
#include "diagram_raster.h"
#include "document.h"
#include "fake_zoom_window.h"
#include "mermaid.h"
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
    uint64_t RequestHash(float max_width = kMaxWidth) const
    {
        return mermaid_util::DiagramRequestHash(mermaid_util::DiagramCodeHash(node()), max_width, false);
    }
    uint64_t WidthFreeHash() const
    {
        return mermaid_util::DiagramWidthFreeHash(mermaid_util::DiagramCodeHash(node()), false);
    }
};

constexpr std::string_view kGamma = "flowchart TD\n  M[Gamma] --> N[Delta]\n";
constexpr std::string_view kWide = "flowchart LR\n  A[aaaaaaaaaaaaaaaa] --> B[bbbbbbbbbbbbbbbb] --> C[cccccccccccccccc] --> D[dddddddddddddddd]\n";

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

        viewer_ = {};
        renderer_.Init(window_.Get(), d2d_.Get(), wic_.Get(), SharedDWriteFactory(), scheduler_, NewEngine,
                       {
                           .on_update = [this] { OnUpdate(); },
                           .wake = wakes_.Fn(),
                           .set_trim_timer = [](UINT) {},
                           .kill_trim_timer = [] {},
                       });
        renderer_.SetRenderTarget(rt_.Get());
        // 拡大表示の図は ZoomViewer が本文と同じ結果から受け取る。画像は読まないので、wake は呼ばれない。
        zoom_.Init(window_.Get(), SharedDWriteFactory(), wic_.Get(), scheduler_, OpenFakeZoomWindow<viewer_>,
                   {
                       .on_update = [this] { OnUpdate(); },
                       .wake = [] {},
                       .on_failed = [this](std::wstring_view) { ++viewer_failed_; },
                   });
        zoom_.SetRenderTarget(rt_.Get(), nullptr, nullptr);
    }

    // 通知のたびに、拡大表示の図を今の本文の幅とテーマで頼む (ResourceManager::RequestVisibleDiagrams と同じ)。
    void OnUpdate()
    {
        ++updates_;
        if (request_viewer_) {
            zoom_.RequestDiagram(renderer_, viewer_width_, viewer_.dark);
        }
    }

    void OpenViewer(std::string_view code, int node = 0, float max_width = kMaxWidth)
    {
        viewer_width_ = max_width;
        zoom_.OpenDiagram(code, node, /*dark_mode=*/false);
    }

    bool AwaitViewerDrawings(size_t count)
    {
        return Await([&] { return viewer_.drawings.size() >= count; });
    }

    void TearDown() override
    {
        zoom_.Shutdown();
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

    // bytes を key の直列化した形として置く (表示していない項目)。
    void StoreBytesAs(uint64_t key, std::string bytes, float width, float height)
    {
        renderer_.ResultCache().Put(key, { .result = { .width = width, .height = height }, .bytes = std::make_shared<const std::string>(std::move(bytes)) });
    }

    // 別の図の描画命令を、key の直列化した形として置く。キャッシュから読んだ図はその描画命令で出るので、描き直したのと見分けられる。
    void StoreDrawingAs(const DiagramEntry& drawn, uint64_t key)
    {
        StoreBytesAs(key, mermaid_util::SerializeDrawing(*drawn.drawing), drawn.width, drawn.height);
    }

    // key の項目。無ければ null。
    const DiagramCache::Item* CachedItem(uint64_t key)
    {
        return renderer_.ResultCache().Peek(key);
    }

    // 画面外へ出た図の絵と描画命令を外し、覚えている結果からも外させる (ResourceManager の EvictOffscreenResources と、
    // 続く要求の回の終わりと同じ)。
    void Release(DiagramEntry& entry)
    {
        entry.ReleaseImage();
        renderer_.ReleaseIdleDrawings();
    }

    // 描画命令の最初のラベルの数式。無ければ null。
    static std::shared_ptr<const MathDrawing> FirstLabelMath(const mermaid_util::DiagramDrawing& drawing)
    {
        for (const auto& layer : drawing.layers) {
            for (const auto& math : layer.maths) {
                return math.drawing;
            }
        }
        return nullptr;
    }

    static std::string SvgOf(const DiagramEntry& entry)
    {
        return DrawingToSvg(*entry.drawing);
    }

    static inline PeleTestAssets assets_;
    static inline ZoomWindowLog viewer_;
    int viewer_failed_ = 0;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1RenderTarget> rt_;
    TaskScheduler scheduler_{ 2 };
    MessageOnlyWindow window_;
    WakeCounter wakes_;
    MermaidRenderer renderer_;
    ZoomViewer zoom_;
    int updates_ = 0;
    // 偽なら通知を受けても拡大表示の図を頼まない (別の文書を読み込み中の ResourceManager)。
    bool request_viewer_ = true;
    float viewer_width_ = kMaxWidth;
};

// 描き終えるまでは要求し直しても描き直さない。描いた図はキャッシュから答え、幅の上限より狭いので幅を混ぜないキーで置く。
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
    const auto* item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_NE(item->bytes, nullptr) << "直列化した形を置く";
    EXPECT_EQ(item->result.drawing, entry.drawing) << "表示している間は解いた形も持つ";
    // 描く前に高さを決めるのに、描いた図の寸法を引ける。
    const auto size = renderer_.KnownSize(d.node(), kMaxWidth, /*dark_mode=*/false);
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(size->width, entry.width);
    EXPECT_EQ(size->height, entry.height);
}

// 画面外へ出て絵と描画命令を捨てた図は、覚えている結果からも解いた形を外し、戻ったら直列化した形から読み直す (描き直さない)。
// 直列化した形を別の図のものに置き換えておき、読み直したことを見分ける。
TEST_F(MermaidRendererTest, ReleasedDrawingIsReadBackFromSerializedForm)
{
    const DiagramDoc alpha(kAlpha);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(alpha, entry));
    const float width = entry.width;
    const float height = entry.height;

    Release(entry);
    EXPECT_FALSE(entry.HasImage());
    EXPECT_EQ(entry.drawing, nullptr);
    EXPECT_TRUE(entry.NeedsRender());
    EXPECT_EQ(entry.width, width) << "寸法は残す";
    EXPECT_EQ(entry.height, height);
    const auto* item = CachedItem(alpha.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->result.drawing, nullptr) << "覚えている結果からも解いた形を外す";
    EXPECT_EQ(item->result.commands, nullptr);
    EXPECT_NE(item->bytes, nullptr) << "直列化した形は残す";

    const DiagramDoc beta(kBeta);
    DiagramEntry other;
    ASSERT_TRUE(RenderUntilSettled(beta, other));
    StoreDrawingAs(other, alpha.WidthFreeHash());
    EXPECT_FALSE(Request(alpha, entry)) << "UI では解かない";
    ASSERT_TRUE(RenderUntilSettled(alpha, entry));
    EXPECT_TRUE(SvgOf(entry).contains("Beta")) << "直列化した形から読む";
}

// テーマを切り替えて置き換わった結果は、どの図も持たなくなるので、覚えている結果からも解いた形とコマンドリストを外す。
// 直列化した形は残し、テーマを戻したら描き直さずに読み直す。
TEST_F(MermaidRendererTest, ReplacedResultReleasesItsDrawing)
{
    const DiagramDoc d(kAlpha);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    const std::string light_svg = SvgOf(entry);

    entry.MarkStale();
    ASSERT_TRUE(Await([&] { return renderer_.RequestRender(d.node(), entry, kMaxWidth, /*dark_mode=*/true); }));
    EXPECT_NE(SvgOf(entry), light_svg);
    renderer_.ReleaseIdleDrawings();
    const auto* light = CachedItem(d.WidthFreeHash());
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->result.drawing, nullptr) << "明るいテーマの解いた形は外す";
    EXPECT_NE(light->bytes, nullptr);
    EXPECT_EQ(renderer_.ResultCache().CommandLists(), 1u);

    entry.MarkStale();
    EXPECT_FALSE(Request(d, entry)) << "UI では解かない";
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    EXPECT_EQ(SvgOf(entry), light_svg);
    renderer_.ReleaseIdleDrawings();
    EXPECT_EQ(renderer_.ResultCache().CommandLists(), 1u) << "暗いテーマの結果も外す";
}

// 同じコードの図が 2 つあれば、片方が画面外へ出てももう片方が持つ解いた形は外さず、戻った図はそれをそのまま使う。
// どちらも持たなくなったら (文書を切り替えた) 外し、直列化した形は残して、また開いたら読み直す。
TEST_F(MermaidRendererTest, DrawingIsKeptWhileAnyDiagramHoldsIt)
{
    const DiagramDoc d(kAlpha);
    DiagramEntry first;
    ASSERT_TRUE(RenderUntilSettled(d, first));
    DiagramEntry second;
    ASSERT_TRUE(Request(d, second));

    Release(first);
    const auto* item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->result.drawing, second.drawing) << "もう片方が表示している";
    EXPECT_TRUE(Request(d, first)) << "解き直さない";
    EXPECT_EQ(first.drawing, second.drawing);

    const std::string svg = SvgOf(first);
    first = {};
    second = {};
    renderer_.ReleaseIdleDrawings();
    EXPECT_EQ(renderer_.ResultCache().CommandLists(), 0u);
    item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->result.drawing, nullptr);
    EXPECT_NE(item->bytes, nullptr);
    DiagramEntry reopened;
    EXPECT_FALSE(Request(d, reopened));
    ASSERT_TRUE(RenderUntilSettled(d, reopened));
    EXPECT_EQ(SvgOf(reopened), svg);
}

// 拡大表示が持つ間は、本文の図が持たなくても解いた形を外さない。閉じたら外す。
TEST_F(MermaidRendererTest, ViewerHoldsDrawingUntilClosed)
{
    const DiagramDoc d(kAlpha);
    OpenViewer(d.node().GetText());
    ASSERT_TRUE(AwaitViewerDrawings(1));
    renderer_.ReleaseIdleDrawings();
    const auto* item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_NE(item->result.drawing, nullptr);

    zoom_.Close();
    viewer_.drawings.clear();
    renderer_.ReleaseIdleDrawings();
    item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->result.drawing, nullptr);
}

// ラベルの数式の描画命令も覚え、手放した図を読み直すときは KaTeX で組み直さずに同じ描画命令を差し込む。
TEST_F(MermaidRendererTest, ReloadedDiagramReusesLabelMaths)
{
    const DiagramDoc d("flowchart LR\n  A[\"$$x^2$$\"] --> B\n");
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    const auto math = FirstLabelMath(*entry.drawing);
    ASSERT_NE(math, nullptr);

    Release(entry);
    ASSERT_EQ(CachedItem(d.WidthFreeHash())->result.drawing, nullptr) << "直列化した形から読み直す";
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    EXPECT_EQ(FirstLabelMath(*entry.drawing), math);
}

// 届いた結果をどの図も受け取らないまま要求の回が終わったら (届く前に画面外へ出た図)、解いた形を持ち続けない。
// 結果を受け取るだけでは手放さない (通知の外で頼み直す呼び出し側もある)。
TEST_F(MermaidRendererTest, UnclaimedResultReleasesItsDrawing)
{
    const DiagramDoc d(kAlpha);
    DiagramEntry entry;
    EXPECT_FALSE(Request(d, entry));
    ASSERT_TRUE(Await([&] { return updates_ > 0; }));
    renderer_.ProcessJobResults(kNoDeadline);
    const auto* item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_NE(item->result.drawing, nullptr) << "要求の回が終わるまでは受け取れる";

    renderer_.ReleaseIdleDrawings();
    item = CachedItem(d.WidthFreeHash());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->result.drawing, nullptr);
    EXPECT_NE(item->bytes, nullptr);
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    EXPECT_TRUE(SvgOf(entry).contains("Alpha"));
}

// 幅の上限より広い図は、別の幅で描き直した描画命令が同じなら、どの幅でも使える結果として幅なしのキーに置く。
// 3 つ目の幅では描き直さずにキャッシュから出す。
TEST_F(MermaidRendererTest, WideDiagramWithSameDrawingAtTwoWidthsIsReusedAtAnyWidth)
{
    const DiagramDoc d(kWide);
    DiagramEntry first;
    ASSERT_TRUE(RenderUntilSettled(d, first, 200.0f));
    ASSERT_GT(first.width, 200.0f) << "幅の上限より広い図";
    EXPECT_FALSE(first.any_width) << "1 つの幅で描いただけでは分からない";

    DiagramEntry second;
    ASSERT_TRUE(RenderUntilSettled(d, second, 300.0f));
    EXPECT_TRUE(second.any_width);

    DiagramEntry third;
    EXPECT_TRUE(Request(d, third, 420.0f)) << "描き直さない";
    EXPECT_TRUE(third.any_width);
    EXPECT_EQ(third.drawing, second.drawing);
    EXPECT_EQ(CachedItem(d.RequestHash(200.0f)), nullptr) << "前の幅の結果は幅なしの結果と同じなので捨てる";
}

// 前の幅の描画命令はハッシュで覚えるので、前の幅の結果がキャッシュから捨てられていても比べられる。
TEST_F(MermaidRendererTest, WideDiagramIsComparedAfterPreviousWidthIsEvicted)
{
    const DiagramDoc d(kWide);
    DiagramEntry first;
    ASSERT_TRUE(RenderUntilSettled(d, first, 200.0f));
    renderer_.ResultCache().Erase(d.RequestHash(200.0f));

    DiagramEntry second;
    ASSERT_TRUE(RenderUntilSettled(d, second, 300.0f));
    EXPECT_TRUE(second.any_width);
}

// 同じ幅で描き直した (キャッシュから捨てられた) だけでは、幅で出力が変わらないとは分からない。
TEST_F(MermaidRendererTest, WideDiagramRedrawnAtSameWidthIsNotAnyWidth)
{
    const DiagramDoc d(kWide);
    DiagramEntry first;
    ASSERT_TRUE(RenderUntilSettled(d, first, 200.0f));
    renderer_.ResultCache().Erase(d.RequestHash(200.0f));

    DiagramEntry second;
    EXPECT_FALSE(Request(d, second, 200.0f)) << "描き直す";
    ASSERT_TRUE(RenderUntilSettled(d, second, 200.0f));
    EXPECT_FALSE(second.any_width);
    EXPECT_NE(CachedItem(d.RequestHash(200.0f)), nullptr);
}

// 描いた図のエラーは描き直しても同じなので確定させ、エラーとして覚える。
TEST_F(MermaidRendererTest, SettlesRenderError)
{
    const DiagramDoc d(kBroken);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(d, entry));
    EXPECT_FALSE(entry.HasImage());
    EXPECT_EQ(entry.error.kind, DiagramError::Kind::Render);
    EXPECT_FALSE(entry.error.message.empty());
    const auto* item = CachedItem(d.RequestHash());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->result.error.kind, DiagramError::Kind::Render);
    EXPECT_EQ(item->bytes, nullptr);
}

// キャッシュの直列化した形に当たれば描かずに読む。
TEST_F(MermaidRendererTest, LoadsFromSerializedForm)
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

// キャッシュの描画命令を解けなければ、その項目を捨てて描き直す。描いた結果が別のキー (幅なし) に置かれても、解けない項目は
// 残らない。次は解ける描画命令が当たる。
TEST_F(MermaidRendererTest, RerendersWhenCachedDrawingIsBroken)
{
    const DiagramDoc beta(kBeta);
    const uint64_t key = beta.RequestHash();
    StoreBytesAs(key, "not a drawing", 10.0f, 10.0f);

    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(beta, entry));
    EXPECT_TRUE(entry.HasImage());
    EXPECT_TRUE(SvgOf(entry).contains("Beta"));
    EXPECT_EQ(CachedItem(key), nullptr);
    const auto hit = renderer_.ResultCache().Find(beta.WidthFreeHash(), key, kMaxWidth);
    ASSERT_NE(hit.item, nullptr);
    EXPECT_TRUE(mermaid_util::ParseDrawing(*hit.item->bytes).has_value());
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

// 見えている図から順に頼み直す回 (BeginRequestPass) では、待ちの表示用の要求をその回に頼まれた順へ並べ直す。新しい要求も
// その回の分の後ろに入り、前の回に頼んだだけの図はその後ろで待つ。数式の要求は図より先に描く。
TEST_F(MermaidRendererTest, WaitingRequestsFollowRequestPassAndMathGoesFirst)
{
    const DiagramDoc alpha(kAlpha);
    const DiagramDoc beta(kBeta);
    const DiagramDoc gamma(kGamma);
    const DiagramDoc wide(kWide);
    const DiagramDoc broken(kBroken);
    DiagramEntry ea;
    DiagramEntry eb;
    DiagramEntry eg;
    DiagramEntry ew;
    DiagramEntry ex;
    WorkerGate gate(scheduler_, 2);
    ASSERT_TRUE(gate.ready());
    // alpha は同時に描く枠 (1 つ) を使い、残りが待つ。
    EXPECT_FALSE(Request(alpha, ea));
    EXPECT_FALSE(Request(beta, eb));
    EXPECT_FALSE(Request(gamma, eg));
    EXPECT_FALSE(Request(wide, ew));
    ASSERT_EQ(renderer_.WaitingRequests(), (std::vector{ beta.RequestHash(), gamma.RequestHash(), wide.RequestHash() }));

    renderer_.BeginRequestPass();
    EXPECT_FALSE(Request(wide, ew));
    EXPECT_FALSE(Request(gamma, eg));
    EXPECT_FALSE(Request(broken, ex));
    EXPECT_EQ(renderer_.WaitingRequests(), (std::vector{ wide.RequestHash(), gamma.RequestHash(), broken.RequestHash(), beta.RequestHash() }));

    renderer_.RequestMathBatch({ { .key = 1, .tex = "x", .font_size = 16.0f } }, [](InlineMathBatchResult) {});
    EXPECT_EQ(renderer_.WaitingRequests(),
              (std::vector<uint64_t>{ 0, wide.RequestHash(), gamma.RequestHash(), broken.RequestHash(), beta.RequestHash() }));
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
    ASSERT_TRUE(PollUntil([&] { return wakes_.Take() > 0; }, std::chrono::seconds(30)));
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

// 開くと窓を出し、描けたら図を渡す。図の目印とコードを覚える。
TEST_F(MermaidRendererTest, ViewerShowsDrawingWhenRendered)
{
    OpenViewer(kAlpha, 3);
    EXPECT_EQ(viewer_.opened, 1);
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_EQ(zoom_.DiagramNode(), 3);
    EXPECT_EQ(zoom_.DiagramCode(), kAlpha);
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
    EXPECT_EQ(zoom_.DiagramNode(), 2);
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
    zoom_.UpdateDiagramCode(kBeta);
    ASSERT_TRUE(AwaitViewerDrawings(2));
    EXPECT_TRUE(DrawingToSvg(*viewer_.drawings.back()).contains("Beta"));
    EXPECT_EQ(viewer_.loading, 0);

    zoom_.UpdateDiagramCode(kBroken);
    // 同じ壊れた図の本文の結果が確定するころには、先に頼んだ拡大表示の分も済んでいる。
    const DiagramDoc broken(kBroken);
    DiagramEntry entry;
    ASSERT_TRUE(RenderUntilSettled(broken, entry));
    EXPECT_FALSE(entry.error.empty());
    EXPECT_TRUE(viewer_.alive);
    EXPECT_EQ(viewer_.drawings.size(), 2u);
    EXPECT_EQ(viewer_failed_, 0);
    EXPECT_EQ(zoom_.DiagramCode(), kBroken);
}

// 開いた図が描けなければ、窓を閉じて知らせる。
TEST_F(MermaidRendererTest, ViewerFailsWhenFirstDrawingCannotRender)
{
    OpenViewer(kBroken, 4);
    ASSERT_TRUE(Await([&] { return viewer_failed_ == 1; }));
    EXPECT_FALSE(viewer_.alive);
    EXPECT_TRUE(viewer_.drawings.empty());
    EXPECT_EQ(zoom_.DiagramCode(), std::nullopt);
    EXPECT_EQ(zoom_.DiagramNode(), -1);
}

// 窓を開けなければ知らせ、図も描かせない。
TEST_F(MermaidRendererTest, ViewerFailsWhenWindowCannotOpen)
{
    viewer_.fail_open = true;
    OpenViewer(kAlpha);
    EXPECT_EQ(viewer_failed_, 1);
    EXPECT_EQ(zoom_.DiagramCode(), std::nullopt);
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
    zoom_.SetDarkMode(true);
    EXPECT_TRUE(viewer_.dark);
    ASSERT_TRUE(AwaitViewerDrawings(2));
    EXPECT_EQ(viewer_.drawings[1]->background, kDiagramDarkBackground);
    EXPECT_EQ(viewer_.loading, 0);
}

// 拡大表示の図は RequestDiagram で頼まれるまで描かない。開く・テーマ・コードの追従・待ちを捨てる・ほかの図の結果、
// のどれからも自分では頼まない (別の文書を読み込み中に、旧文書の図を描き始めない)。
TEST_F(MermaidRendererTest, ViewerDrawingIsRequestedOnlyThroughRequestDiagram)
{
    request_viewer_ = false;
    OpenViewer(kAlpha);
    zoom_.SetDarkMode(false);
    zoom_.UpdateDiagramCode(kBeta);
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
    zoom_.Close();
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
        zoom_.Close();
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
    ASSERT_TRUE(PollUntil([&] { return wakes_.Take() > 0; }, std::chrono::seconds(30)));
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

using zoom_viewer::FitZoom;
using zoom_viewer::NextZoom;

// 全体表示は余白を除いた窓に収め、等倍より大きくしない。潰した窓では最小の倍率で止める。
TEST(ZoomViewerZoom, FitsInsidePaddingWithoutEnlarging)
{
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 800.0f, 200.0f, 100.0f), 1.0f);
    EXPECT_FLOAT_EQ(FitZoom(432.0f, 1000.0f, 800.0f, 100.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(1000.0f, 232.0f, 100.0f, 400.0f), 0.5f);
    EXPECT_FLOAT_EQ(FitZoom(10.0f, 10.0f, 1000.0f, 1000.0f), 0.05f);
}

// ボタンの止まる位置は表の値と全体表示の倍率。全体表示のすぐ隣の値は飛ばす。
TEST(ZoomViewerZoom, StepsThroughStopsIncludingFit)
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

// ===== DiagramCache =====

// 直列化した形だけの項目 (表示していない図)。中身は読まないので何でもよい。
DiagramCache::Item Serialized(size_t size, float width = 100.0f, bool any_width = false)
{
    DiagramCache::Item item;
    item.result.width = width;
    item.result.height = 50.0f;
    item.result.any_width = any_width;
    item.bytes = std::make_shared<const std::string>(size, 'x');
    return item;
}

// 表示している図の項目 (解いた形も持つ)。
DiagramCache::Item Displayed(size_t size, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing)
{
    auto item = Serialized(size);
    item.result.drawing = std::move(drawing);
    return item;
}

DiagramCache::Item Error()
{
    return { .result = { .error = { DiagramError::Kind::Render, L"syntax error" } } };
}

bool Has(const DiagramCache& cache, uint64_t key)
{
    return cache.Peek(key) != nullptr;
}

// 大きさ 1000 の項目が 2 つ入り、3 つ目で溢れる上限 (項目の器の分も数える)。
constexpr size_t kTwoItems = 3000;

// 別のキーは足し、同じキーは置き換える。
TEST(DiagramCache, PutAddsAndReplaces)
{
    DiagramCache cache;
    cache.Put(1, Serialized(1000));
    const size_t one = cache.TotalBytes();
    EXPECT_GT(one, 1000u) << "項目の器も数える";
    cache.Put(2, Serialized(2000));
    EXPECT_EQ(cache.EntryCount(), 2u);
    EXPECT_EQ(cache.TotalBytes(), one * 2 + 1000);

    cache.Put(1, Serialized(512, 300.0f));
    EXPECT_EQ(cache.EntryCount(), 2u);
    EXPECT_EQ(cache.TotalBytes(), one * 2 + 512);
    const auto hit = cache.Find(1, 1, 0.0f);
    ASSERT_NE(hit.item, nullptr);
    EXPECT_EQ(hit.key, 1u);
    EXPECT_EQ(hit.item->result.width, 300.0f);
    EXPECT_EQ(hit.item->bytes->size(), 512u);
}

// 上限を超えたら、引いた (Find した) ものを残して最も古いものから捨てる。
TEST(DiagramCache, EvictsLeastRecentlyUsedOverLimit)
{
    DiagramCache cache(kTwoItems);
    cache.Put(1, Serialized(1000));
    cache.Put(2, Serialized(1000));
    ASSERT_NE(cache.Find(1, 1, 0.0f).item, nullptr);

    cache.Put(3, Serialized(1000));
    cache.ReleaseIdleDrawings();
    EXPECT_TRUE(Has(cache, 1));
    EXPECT_FALSE(Has(cache, 2));
    EXPECT_TRUE(Has(cache, 3));
}

// 大きな図を手放して大きく溢れたら、上限に収まるまで古い順にまとめて捨てる。
TEST(DiagramCache, EvictsManyInLeastRecentlyUsedOrder)
{
    const size_t one = DiagramCache::ITEM_BYTES + 100;
    DiagramCache cache(one * 5);
    for (uint64_t key = 1; key <= 10; ++key) {
        cache.Put(key, Serialized(100));
    }
    ASSERT_NE(cache.Find(3, 3, 0.0f).item, nullptr);

    cache.ReleaseIdleDrawings();
    EXPECT_EQ(cache.EntryCount(), 5u);
    for (const uint64_t key : { 3, 7, 8, 9, 10 }) {
        EXPECT_TRUE(Has(cache, key)) << key;
    }
}

// 置くものが無い (上限より大きくて直列化した形を持てず、表示もしていない) 結果は、同じキーの項目を消さない。
TEST(DiagramCache, PutWithNothingToStoreKeepsExisting)
{
    DiagramCache cache(kTwoItems);
    cache.Put(1, Serialized(1000));
    cache.Put(1, Serialized(kTwoItems));
    cache.Put(1, {});
    const auto* item = cache.Peek(1);
    ASSERT_NE(item, nullptr);
    ASSERT_NE(item->bytes, nullptr);
    EXPECT_EQ(item->bytes->size(), 1000u);
}

// 高さを決めるために見る (Peek) だけなら、捨てる順を変えない。
TEST(DiagramCache, PeekKeepsOrder)
{
    DiagramCache cache(kTwoItems);
    cache.Put(1, Serialized(1000));
    cache.Put(2, Serialized(1000));
    EXPECT_TRUE(Has(cache, 1));

    cache.Put(3, Serialized(1000));
    cache.ReleaseIdleDrawings();
    EXPECT_FALSE(Has(cache, 1));
    EXPECT_TRUE(Has(cache, 2));
}

// 表示している (ほかに持ち主のいる) 図の項目は、古くても捨てない。持ち主がいなくなったら解いた形を外し、その時を使った時
// として捨てる順に入る。
TEST(DiagramCache, KeepsDisplayedItemsAndOrdersReleasedAsNewest)
{
    auto drawing = std::make_shared<const mermaid_util::DiagramDrawing>();
    DiagramCache cache(kTwoItems);
    cache.Put(1, Displayed(1000, drawing));
    cache.Put(2, Serialized(1000));
    cache.Put(3, Serialized(1000));
    cache.ReleaseIdleDrawings();
    EXPECT_TRUE(Has(cache, 1));
    EXPECT_FALSE(Has(cache, 2));

    drawing.reset();
    cache.ReleaseIdleDrawings();
    EXPECT_EQ(cache.Peek(1)->result.drawing, nullptr);
    cache.Put(4, Serialized(1000));
    cache.ReleaseIdleDrawings();
    EXPECT_TRUE(Has(cache, 1)) << "手放したばかりの図は残す";
    EXPECT_FALSE(Has(cache, 3));
}

// 表示している図だけで上限を超えるなら、超えたままにする (表示中の図は捨てない)。
TEST(DiagramCache, DisplayedItemsMayExceedLimit)
{
    std::vector<std::shared_ptr<const mermaid_util::DiagramDrawing>> shown;
    DiagramCache cache(kTwoItems);
    for (uint64_t key = 1; key <= 3; ++key) {
        cache.Put(key, Displayed(1000, shown.emplace_back(std::make_shared<const mermaid_util::DiagramDrawing>())));
    }
    cache.ReleaseIdleDrawings();
    EXPECT_EQ(cache.EntryCount(), 3u);
    EXPECT_GT(cache.TotalBytes(), kTwoItems);
}

// 上限より大きい図は直列化した形を持たず、表示している間だけ置く。手放したら項目ごと捨てる。
TEST(DiagramCache, OversizedDrawingIsKeptOnlyWhileDisplayed)
{
    auto drawing = std::make_shared<const mermaid_util::DiagramDrawing>();
    DiagramCache cache(kTwoItems);
    cache.Put(1, Serialized(1000));
    cache.Put(2, Displayed(kTwoItems, drawing));
    cache.ReleaseIdleDrawings();
    const auto* item = cache.Peek(2);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->bytes, nullptr);
    EXPECT_TRUE(Has(cache, 1)) << "ほかを捨てない";

    drawing.reset();
    cache.ReleaseIdleDrawings();
    EXPECT_FALSE(Has(cache, 2));
    EXPECT_TRUE(Has(cache, 1));
}

// 引いたバイト列は、worker が解く前に捨てられても残る。
TEST(DiagramCache, FoundBytesOutliveEviction)
{
    DiagramCache cache(kTwoItems);
    cache.Put(1, Serialized(1000));
    const auto bytes = cache.Find(1, 1, 0.0f).item->bytes;
    cache.Put(2, Serialized(1000));
    cache.Put(3, Serialized(1000));
    cache.ReleaseIdleDrawings();
    EXPECT_FALSE(Has(cache, 1));
    EXPECT_EQ(*bytes, std::string(1000, 'x'));
}

// エラーもほかの項目と同じく上限に数え、古いものから捨てる。
TEST(DiagramCache, ErrorsCountTowardLimit)
{
    DiagramCache cache(kTwoItems);
    cache.Put(1, Error());
    EXPECT_GT(cache.TotalBytes(), 0u);
    cache.Put(2, Serialized(1200));
    cache.Put(3, Serialized(1200));
    cache.ReleaseIdleDrawings();
    EXPECT_FALSE(Has(cache, 1));
    EXPECT_TRUE(Has(cache, 2));
    EXPECT_TRUE(Has(cache, 3));
}

// 置くときは上限を超えても捨てない (置いたエラーを図が受け取る前に捨てない)。次の ReleaseIdleDrawings で古い順に捨て、
// 上限に収まったら止める。
TEST(DiagramCache, PutDoesNotEvict)
{
    DiagramCache cache(kTwoItems);
    cache.Put(1, Serialized(1000));
    cache.Put(2, Serialized(1000));
    cache.Put(3, Error());
    cache.Put(4, Serialized(1000));
    EXPECT_EQ(cache.EntryCount(), 4u);
    EXPECT_GT(cache.TotalBytes(), kTwoItems);

    cache.ReleaseIdleDrawings();
    EXPECT_FALSE(Has(cache, 1));
    EXPECT_TRUE(Has(cache, 2)) << "上限に収まったら止める";
    EXPECT_TRUE(Has(cache, 3));
    EXPECT_TRUE(Has(cache, 4));
    EXPECT_LE(cache.TotalBytes(), kTwoItems);
}

// 持ち主のいない解いた形だけを外す。直列化した形を持たない項目は捨てる。
TEST(DiagramCache, ReleaseIdleDrawingsKeepsSerializedForm)
{
    const auto held = std::make_shared<const mermaid_util::DiagramDrawing>();
    DiagramCache cache;
    cache.Put(1, Displayed(1000, std::make_shared<const mermaid_util::DiagramDrawing>()));
    cache.Put(2, Displayed(DiagramCache::MAX_BYTES, std::make_shared<const mermaid_util::DiagramDrawing>()));
    cache.Put(3, Displayed(1000, held));
    cache.ReleaseIdleDrawings();
    ASSERT_TRUE(Has(cache, 1));
    EXPECT_EQ(cache.Peek(1)->result.drawing, nullptr);
    EXPECT_NE(cache.Peek(1)->bytes, nullptr);
    EXPECT_FALSE(Has(cache, 2));
    ASSERT_TRUE(Has(cache, 3));
    EXPECT_EQ(cache.Peek(3)->result.drawing, held);
}

// デバイスが変わったら、旧デバイスで確定したかもしれないエラーを捨てる。解いた形は残し、記録し直させる。
TEST(DiagramCache, ResetDeviceDropsErrorsAndKeepsDrawings)
{
    const auto drawing = std::make_shared<const mermaid_util::DiagramDrawing>();
    DiagramCache cache;
    cache.Put(1, Error());
    cache.Put(2, Displayed(1000, drawing));
    cache.ResetDevice();
    EXPECT_FALSE(Has(cache, 1));
    ASSERT_TRUE(Has(cache, 2));
    EXPECT_EQ(cache.Peek(2)->result.drawing, drawing);
}

// 幅なしの結果は max_width より狭いときだけ使い、使えなければ幅つきを引く。
TEST(DiagramCache, FindPrefersWidthFreeOnlyWhenNarrower)
{
    constexpr uint64_t kFree = 10;
    constexpr uint64_t kSized = 20;
    DiagramCache cache;
    cache.Put(kFree, Serialized(64, 300.0f));

    auto hit = cache.Find(kFree, kSized, 600.0f);
    ASSERT_NE(hit.item, nullptr);
    EXPECT_EQ(hit.key, kFree);

    // 幅なしの図が上限いっぱいなら使わない。幅つきが無ければ外れ。
    EXPECT_EQ(cache.Find(kFree, kSized, 300.0f).item, nullptr);

    cache.Put(kSized, Serialized(64, 280.0f));
    hit = cache.Find(kFree, kSized, 300.0f);
    ASSERT_NE(hit.item, nullptr);
    EXPECT_EQ(hit.key, kSized);
    EXPECT_EQ(hit.item->result.width, 280.0f);
}

// どの幅でも使える印のある図は、幅の上限より広くても幅なしのキーで当たる。印の無い広い図は幅つきのキーでしか引けない。
TEST(DiagramCache, AnyWidthItemHitsAtEveryWidth)
{
    DiagramCache cache;
    cache.Put(100, Serialized(64, 640.0f, /*any_width=*/true));
    cache.Put(200, Serialized(64, 640.0f));
    const auto hit = cache.Find(100, 1, 300.0f);
    ASSERT_NE(hit.item, nullptr);
    EXPECT_EQ(hit.key, 100u);
    EXPECT_EQ(cache.Find(200, 2, 300.0f).item, nullptr);
}

} // namespace
