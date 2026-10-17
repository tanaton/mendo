// MermaidRenderer の lazy-init 状態遷移を WebView2 なしで検証する。
// mermaid.cpp 本体は WebView2 / Direct2D に依存するため mendo_core に含められないが、
// 純粋ロジック部分を mermaid_lifecycle に切り出すことでここで単体テストできる。
#include <gtest/gtest.h>
#include "mermaid_util.h"
#include "document_types.h"
#include "syntax.h"

using mermaid_lifecycle::Lifecycle;

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
