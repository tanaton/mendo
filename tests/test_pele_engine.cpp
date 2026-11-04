#include <gtest/gtest.h>
#include "example_files.h"
#include "mermaid_util.h"
#include "pele_engine.h"
#include "string_convert.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace {

using Status = PeleOutcome::Status;

// pele のコンパイル (約 60ms) を 1 回で済ませるため、テストの間で 1 つのエンジンを使う。
class PeleEngineTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        source_ = ReadFileBytes(std::filesystem::path(MENDO_THIRD_PARTY_DIR) / "pele" / "pele.mjs");
        ASSERT_FALSE(source_.empty());
        ASSERT_HRESULT_SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.GetAddressOf())));
        engine_ = std::make_unique<PeleEngine>(Source(), dw_.Get());
    }
    static void TearDownTestSuite()
    {
        engine_.reset();
        dw_.Reset();
    }
    static std::span<const std::byte> Source()
    {
        return std::as_bytes(std::span{ source_ });
    }

    static inline std::string source_;
    static inline Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    static inline std::unique_ptr<PeleEngine> engine_;
};

} // namespace

// WebView2 を使わずに、図のコードから描画命令を作る。
TEST_F(PeleEngineTest, RendersDiagramToDrawing)
{
    const auto out = engine_->Render("flowchart LR\n  A[開始] --> B[終了]", false, 800.0f, L"ja-jp");
    ASSERT_EQ(out.status, Status::Drawn) << string_convert::WideToUtf8(out.error);
    EXPECT_GT(out.drawing.size.width, 0.0f);
    EXPECT_GT(out.drawing.size.height, 0.0f);
    std::wstring all;
    for (const auto& layer : out.drawing.layers) {
        for (const auto& t : layer.texts) {
            all += t.text;
        }
    }
    EXPECT_NE(all.find(L"開始"), std::wstring::npos);
    EXPECT_NE(all.find(L"終了"), std::wstring::npos);
}

// コードの誤りは pele の文言で返す。
TEST_F(PeleEngineTest, ReturnsPeleErrorForInvalidCode)
{
    const auto out = engine_->Render("flowchart LR\n  A[ --> ", false, 800.0f, L"ja-jp");
    EXPECT_EQ(out.status, Status::Error);
    EXPECT_FALSE(out.error.empty());
}

// ラベルの数式は KaTeX が要るので、ブラウザに任せる。
TEST_F(PeleEngineTest, LeavesMathLabelsToBrowser)
{
    EXPECT_FALSE(PeleEngine::CanRender("flowchart LR\n  A[\"$$x^2$$\"]"));
    EXPECT_TRUE(PeleEngine::CanRender("flowchart LR\n  A[\"$x$\"]"));
}

// 同時に呼ぶとランタイムを増やし、どちらも描ける。捨てた後もバイトコードから作り直して描ける。
TEST_F(PeleEngineTest, RendersConcurrentlyAndAfterTrim)
{
    PeleOutcome results[2];
    std::thread other([&] { results[0] = engine_->Render("pie\n  \"a\" : 1\n  \"b\" : 2", true, 400.0f, L"zh-cn"); });
    results[1] = engine_->Render("sequenceDiagram\n  A->>B: hi", false, 800.0f, L"ja-jp");
    other.join();
    EXPECT_EQ(results[0].status, Status::Drawn);
    EXPECT_EQ(results[1].status, Status::Drawn);
    engine_->Trim();
    EXPECT_EQ(engine_->Render("flowchart LR\n  C --> D", false, 800.0f, L"ja-jp").status, Status::Drawn);
}

TEST_F(PeleEngineTest, CancelStopsRendering)
{
    PeleEngine engine(Source(), dw_.Get());
    engine.Cancel();
    EXPECT_EQ(engine.Render("flowchart LR\n  A --> B", false, 800.0f, L"ja-jp").status, Status::Cancelled);
}

// C++ で作った描画命令はディスクキャッシュに書き、ParseDrawing で同じものに読み戻せる。
TEST_F(PeleEngineTest, SerializedDrawingParsesBack)
{
    auto out = engine_->Render("flowchart TD\n  A[\"x<sub>2</sub> & <b>y</b> 🦆\"] --> B((円))", true, 800.0f, L"ja-jp");
    ASSERT_EQ(out.status, Status::Drawn);
    out.drawing.size.dpr = 1.5f;
    const auto back = mermaid_util::ParseDrawing(string_convert::Utf8ToWide(mermaid_util::SerializeDrawing(out.drawing)));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, out.drawing);
}
