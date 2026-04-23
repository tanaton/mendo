#pragma once

struct Node;

namespace mermaid_lifecycle {

// MermaidRenderer の初期化状態遷移を WebView2 非依存で表現した純ロジック層。
// 単体テストから状態機械を検証できるようにするため、mermaid.cpp 本体から切り出している。
class Lifecycle {
public:
    constexpr bool IsInitialized() const noexcept { return initialized_; }
    constexpr bool IsReady() const noexcept { return ready_; }

    // 未初期化なら初期化済みに遷移し true。既に初期化済みなら false。
    // WebView2 環境生成の冪等トリガーとして使う。
    constexpr bool TryMarkInitialized() noexcept
    {
        if (initialized_) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    // 初回ワーカー準備完了時に呼ぶ。
    constexpr void MarkReady() noexcept { ready_ = true; }

    // Shutdown 経路などで呼ぶ。
    constexpr void Reset() noexcept
    {
        initialized_ = false;
        ready_ = false;
    }

private:
    bool initialized_ = false;
    bool ready_ = false;
};

// ノードが Mermaid 描画の初期化をトリガーすべき対象か判定する純関数。
// Node.type と code_language だけを参照し、レンダラーの状態には依存しない。
bool ShouldTriggerInitForNode(const Node& node) noexcept;

} // namespace mermaid_lifecycle
