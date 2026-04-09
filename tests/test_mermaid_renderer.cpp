#include <gtest/gtest.h>
#include "mermaid.h"
#include "layout_cache.h"

// MermaidRendererの遅延初期化テスト。
// WebView2やDirect2Dの実環境は不要な範囲で、Init後の状態遷移と
// RequestRenderによる初期化トリガーの挙動を検証する。

class MermaidRendererTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        renderer_ = std::make_unique<MermaidRenderer>();
    }

    void TearDown() override
    {
        // WIC ComPtrの解放がCoUninitializeより前になるようにする
        renderer_.reset();
        CoUninitialize();
    }

    // テスト用のMermaidノードを作成する
    static Node MakeMermaidNode(const char* code)
    {
        Node node;
        node.type = NodeType::CodeBlock;
        node.code_language = SyntaxLanguage::Mermaid;
        node.text_utf8.assign(code);
        return node;
    }

    // テスト用の非Mermaidノードを作成する
    static Node MakeNonMermaidNode()
    {
        Node node;
        node.type = NodeType::CodeBlock;
        node.code_language = SyntaxLanguage::Cpp;
        node.text_utf8.assign("int main() {}");
        return node;
    }

    std::unique_ptr<MermaidRenderer> renderer_;
};

// ═══════════════════════════════════════════════
// 初期状態
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, DefaultConstructionNotReady)
{
    EXPECT_FALSE(renderer_->IsReady());
}

TEST_F(MermaidRendererTest, DefaultConstructionNotInitialized)
{
    EXPECT_FALSE(renderer_->IsInitialized());
}

// ═══════════════════════════════════════════════
// Init後の状態（WebView2は遅延されるべき）
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, InitDoesNotStartWebView2)
{
    renderer_->Init(nullptr, nullptr, nullptr);

    EXPECT_FALSE(renderer_->IsReady());
    EXPECT_FALSE(renderer_->IsInitialized());
}

// ═══════════════════════════════════════════════
// 非MermaidノードはInitをトリガーしない
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, NonMermaidNodeDoesNotTriggerInit)
{
    renderer_->Init(nullptr, nullptr, nullptr);

    auto node = MakeNonMermaidNode();
    NodeLayoutEntry layout{};
    DiagramEntry diagram{};

    renderer_->RequestRender(node, layout, diagram, 800.0f, false, nullptr, nullptr);

    EXPECT_FALSE(renderer_->IsInitialized());
}

// ═══════════════════════════════════════════════
// MermaidノードのキャッシュミスがInitをトリガーする
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, MermaidCacheMissTriggersInit)
{
    renderer_->Init(nullptr, nullptr, nullptr);

    auto node = MakeMermaidNode("graph TD; A-->B;");
    NodeLayoutEntry layout{};
    DiagramEntry diagram{};

    renderer_->RequestRender(node, layout, diagram, 800.0f, false, nullptr, nullptr);

    // キャッシュミス → EnsureInitialized() が呼ばれる
    EXPECT_TRUE(renderer_->IsInitialized());
    // ただしWebView2の非同期初期化が完了するまではReadyにはならない
    EXPECT_FALSE(renderer_->IsReady());
}

// ═══════════════════════════════════════════════
// 複数回のRequestRenderでEnsureInitializedが冪等
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, MultipleRequestsDoNotReinitialize)
{
    renderer_->Init(nullptr, nullptr, nullptr);

    auto node1 = MakeMermaidNode("graph TD; A-->B;");
    auto node2 = MakeMermaidNode("graph LR; X-->Y;");
    NodeLayoutEntry layout{};
    DiagramEntry diagram{};

    // 2回呼んでもクラッシュしない（EnsureInitializedの冪等性）
    renderer_->RequestRender(node1, layout, diagram, 800.0f, false, nullptr, nullptr);
    renderer_->RequestRender(node2, layout, diagram, 800.0f, false, nullptr, nullptr);

    EXPECT_TRUE(renderer_->IsInitialized());
}

// ═══════════════════════════════════════════════
// Init前のRequestRenderは安全
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, RequestRenderBeforeInitIsSafe)
{
    auto node = MakeMermaidNode("graph TD; A-->B;");
    NodeLayoutEntry layout{};
    DiagramEntry diagram{};

    // Init()を呼ばずにRequestRenderしてもクラッシュしない
    renderer_->RequestRender(node, layout, diagram, 800.0f, false, nullptr, nullptr);

    EXPECT_FALSE(renderer_->IsReady());
}

// ═══════════════════════════════════════════════
// CancelPending / ClearPendingQueue / ClearCache は初期化前でも安全
// ═══════════════════════════════════════════════

TEST_F(MermaidRendererTest, CancelPendingBeforeInitIsSafe)
{
    renderer_->Init(nullptr, nullptr, nullptr);
    renderer_->CancelPending();
    renderer_->ClearPendingQueue();

    EXPECT_FALSE(renderer_->IsInitialized());
}

TEST_F(MermaidRendererTest, ClearCacheBeforeInitIsSafe)
{
    renderer_->Init(nullptr, nullptr, nullptr);
    renderer_->ClearCache();

    EXPECT_FALSE(renderer_->IsInitialized());
}
