// MermaidRenderer の lazy-init 状態遷移を WebView2 なしで検証する。
// mermaid.cpp 本体は WebView2 / Direct2D に依存するため mendo_core に含められないが、
// 純粋ロジック部分を mermaid_lifecycle に切り出すことでここで単体テストできる。
#include <gtest/gtest.h>
#include "mermaid_lifecycle.h"
#include "document_types.h"
#include "syntax.h"

using mermaid_lifecycle::Lifecycle;
using mermaid_lifecycle::ShouldTriggerInitForNode;

// ═══════════════════════════════════════════════
// 初期状態
// ═══════════════════════════════════════════════

TEST(MermaidLifecycle, DefaultConstructionNotInitializedNorReady)
{
    Lifecycle l;
    EXPECT_FALSE(l.IsInitialized());
    EXPECT_FALSE(l.IsReady());
}

// ═══════════════════════════════════════════════
// TryMarkInitialized の冪等性
// ═══════════════════════════════════════════════

TEST(MermaidLifecycle, TryMarkInitializedFirstCallReturnsTrue)
{
    Lifecycle l;
    EXPECT_TRUE(l.TryMarkInitialized());
    EXPECT_TRUE(l.IsInitialized());
}

TEST(MermaidLifecycle, TryMarkInitializedSubsequentCallsReturnFalse)
{
    Lifecycle l;
    l.TryMarkInitialized();
    EXPECT_FALSE(l.TryMarkInitialized());
    EXPECT_FALSE(l.TryMarkInitialized());
    EXPECT_TRUE(l.IsInitialized());
}

// ═══════════════════════════════════════════════
// Ready 遷移
// ═══════════════════════════════════════════════

TEST(MermaidLifecycle, MarkReadyTransitionsToReady)
{
    Lifecycle l;
    l.TryMarkInitialized();
    l.MarkReady();
    EXPECT_TRUE(l.IsReady());
    EXPECT_TRUE(l.IsInitialized());
}

TEST(MermaidLifecycle, MarkReadyIdempotent)
{
    Lifecycle l;
    l.TryMarkInitialized();
    l.MarkReady();
    l.MarkReady();
    EXPECT_TRUE(l.IsReady());
}

// ═══════════════════════════════════════════════
// Reset
// ═══════════════════════════════════════════════

TEST(MermaidLifecycle, ResetReturnsToUninitialized)
{
    Lifecycle l;
    l.TryMarkInitialized();
    l.MarkReady();
    l.Reset();
    EXPECT_FALSE(l.IsInitialized());
    EXPECT_FALSE(l.IsReady());
}

TEST(MermaidLifecycle, ResetAllowsReinitialization)
{
    Lifecycle l;
    l.TryMarkInitialized();
    l.Reset();
    EXPECT_TRUE(l.TryMarkInitialized());
}

// ═══════════════════════════════════════════════
// ShouldTriggerInitForNode
// ═══════════════════════════════════════════════

TEST(MermaidLifecycle, MermaidCodeBlockTriggersInit)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.code_language = SyntaxLanguage::Mermaid;
    EXPECT_TRUE(ShouldTriggerInitForNode(node));
}

TEST(MermaidLifecycle, LatexMathCodeBlockTriggersInit)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.code_language = SyntaxLanguage::LatexMath;
    EXPECT_TRUE(ShouldTriggerInitForNode(node));
}

TEST(MermaidLifecycle, CppCodeBlockDoesNotTriggerInit)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.code_language = SyntaxLanguage::Cpp;
    EXPECT_FALSE(ShouldTriggerInitForNode(node));
}

TEST(MermaidLifecycle, NonCodeBlockDoesNotTriggerInit)
{
    Node node;
    node.type = NodeType::Paragraph;
    // CodeBlock ではない場合、言語によらず false
    node.code_language = SyntaxLanguage::Mermaid;
    EXPECT_FALSE(ShouldTriggerInitForNode(node));
}

TEST(MermaidLifecycle, NoneLanguageCodeBlockDoesNotTriggerInit)
{
    Node node;
    node.type = NodeType::CodeBlock;
    node.code_language = SyntaxLanguage::None;
    EXPECT_FALSE(ShouldTriggerInitForNode(node));
}
