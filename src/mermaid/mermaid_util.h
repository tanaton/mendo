#pragma once
#include <string>
#include <string_view>
#include <cstdint>

struct Node;

namespace mermaid_util {

// LaTeX 原文を mermaid flowchart の1ノードラベルでラップする。
// style でノードの枠と背景を非表示にし、数式のみが見えるようにする。
std::pmr::wstring BuildLatexFlowchartCode(std::wstring_view latex);

// ダイアグラムノード用キャッシュキー。言語種別に応じたソルトを内部で混ぜ、
// 同じ Wide テキストの Mermaid / LatexMath がキー衝突しないようにする。
uint64_t NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept;

std::pmr::wstring JsEscape(std::wstring_view input);

std::pmr::wstring SimpleHash(std::wstring_view input);

uint64_t HashRaw(std::wstring_view input) noexcept;
uint64_t HashRaw(std::string_view input) noexcept;

uint64_t CombinedHash(std::wstring_view code, int max_width_int, bool dark_mode) noexcept;
uint64_t CombinedHash(std::string_view code, int max_width_int, bool dark_mode) noexcept;

int ComputeWorkerCount(unsigned int processor_count) noexcept;
int QuantizeWidth(float max_width) noexcept;
uint64_t HashCode(std::wstring_view code, float max_width, bool dark_mode) noexcept;
uint64_t HashCode(std::string_view code, float max_width, bool dark_mode) noexcept;
float ParseJsonNumber(std::wstring_view json, std::wstring_view key) noexcept;
bool ParseJsonTrueFlag(std::wstring_view json, std::wstring_view key) noexcept;

struct RequestPrefix {
    unsigned int id = 0;
    std::wstring_view payload;
    bool valid = false;
    bool has_payload = false;
};
RequestPrefix ParseRequestPrefix(std::wstring_view body) noexcept;

// WebMessage の種別。WebView2 から届くメッセージは prefix で識別する。
enum class WebMessageKind : uint8_t {
    Unknown,
    Ready,         // "mermaid-ready:<dpr>"
    RenderResult,  // "render-result:<id>:<payload>"
    CaptureReady,  // "capture-ready:<id>"
    SvgResult,     // "svg-result:<id>:<payload>"
    RenderError,   // "render-error:<id>"
    Failed,        // "mermaid-failed"
};

// パース済み WebMessage。kind に応じてどのフィールドが有効か変わる:
//  - Ready:       ready_dpr (devicePixelRatio)
//  - Render*/Capture/Svg: request (id, payload)
//  - Failed/Unknown: フィールドなし
struct ParsedWebMessage {
    WebMessageKind kind = WebMessageKind::Unknown;
    float ready_dpr = 0.0f;
    RequestPrefix request{};
};

// WebView2 からの string メッセージを純関数でパースする。
// switch で扱える形にすることで mermaid.cpp 側の if-else 連鎖を排除する。
ParsedWebMessage ParseWebMessage(std::wstring_view msg) noexcept;

} // namespace mermaid_util

namespace mermaid_lifecycle {

// MermaidRenderer の初期化状態遷移を WebView2 非依存で表現した純ロジック層。
// 単体テストから状態機械を検証できるようにするため、mermaid.cpp 本体から切り出している。
class Lifecycle {
public:
    constexpr bool IsInitialized() const noexcept
    {
        return initialized_;
    }
    constexpr bool IsReady() const noexcept
    {
        return ready_;
    }

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
    constexpr void MarkReady() noexcept
    {
        ready_ = true;
    }

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
