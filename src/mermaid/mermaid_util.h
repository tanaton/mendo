#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

struct Node;

namespace mermaid_util {

// 言語種別ソルトで同テキストの Mermaid / LatexMath がキー衝突しない。
uint64_t NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept;
// NodeDiagramHash のうちノードのテキストと言語だけで決まる部分。幅とテーマは DiagramRequestHash で混ぜる。
// テキストのハッシュは図の大きさに比例するので、覚えておけば要求し直すたびに読み直さずに済む。
uint64_t DiagramCodeHash(const Node& node) noexcept;
// cached は DiagramEntry::code_hash。0 (未計算) のときだけテキストを読む。
uint64_t EnsureDiagramCodeHash(uint64_t& cached, const Node& node) noexcept;
uint64_t DiagramRequestHash(uint64_t code_hash, float max_width, bool dark_mode) noexcept;
// mermaid は既定で useMaxWidth なので、幅の上限より狭く描けた図はどの幅でも同じ結果になる。
// その結果は幅を混ぜないこのキーでも引けるようにし、幅が変わっても描き直さない。
uint64_t DiagramWidthFreeHash(uint64_t code_hash, bool dark_mode) noexcept;
constexpr bool IsWidthFree(float css_width, float max_width) noexcept
{
    return css_width + 1.0f < max_width;
}

std::wstring JsEscape(std::wstring_view input);

int QuantizeWidth(float max_width) noexcept;

// ページから届く値は、mermaid.js に XSS があれば任意に偽れる。描画とキャッシュに使う前に確かめる。
constexpr bool IsPlausibleDpr(float dpr) noexcept
{
    return dpr >= 0.25f && dpr <= 8.0f;
}

// キャプチャする物理ピクセルが max_side_px (ビットマップの最大辺) と 1 枚あたりの確保量の上限に収まるか。
// 収まらない図はビットマップを作れず、描き直しを繰り返すことになる。
bool FitsCaptureLimit(float css_width, float css_height, float dpr, unsigned max_side_px) noexcept;

// エラーメッセージの表示用整形: 空白・改行・制御文字の連続を空白1個に潰し、max_len で
// 切り詰める (DrawTextCmd の 255 文字上限内に収める)。切り詰め時は末尾に U+2026 を付ける。
std::wstring SanitizeErrorMessage(std::wstring_view msg, size_t max_len);

struct RequestPrefix {
    unsigned int id = 0;
    std::wstring_view payload;
    bool valid = false;
    bool has_payload = false;
};
RequestPrefix ParseRequestPrefix(std::wstring_view body) noexcept;

enum class WebMessageKind : uint8_t {
    Unknown,
    Ready,         // "mermaid-ready:<dpr>"
    RenderResult,  // "render-result:<id>:<width>:<height>:<dpr>"
    CaptureReady,  // "capture-ready:<id>"
    SvgResult,     // "svg-result:<id>:<payload>"
    RenderError,   // "render-error:<id>:<message>"
    MathResult,    // "math-result:<id>:<payload>" (ParseMathBatch)
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

ParsedWebMessage ParseWebMessage(std::wstring_view msg) noexcept;

// RenderResult の payload ("<width>:<height>:<dpr>")。欠けた値と有限でない値 (nan / inf) は 0。
struct RenderSize {
    float width = 0.0f;
    float height = 0.0f;
    float dpr = 0.0f;
};
RenderSize ParseRenderSize(std::wstring_view payload) noexcept;

// まとめて描いたインライン数式 1 つの箱 (CSS px、ページの左上が原点)。baseline は箱の上端から。幅 0 は描けなかった数式。
struct MathBox {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
};
struct MathBatchLayout {
    RenderSize size; // ページ全体
    std::vector<MathBox> boxes;
};
// MathResult の payload ("<w>:<h>:<dpr>|<x>,<y>,<w>,<h>,<baseline>;..."、描けなかった数式は "e")。
// 数が expected と違うか、ページの外を指す箱があれば nullopt。
std::optional<MathBatchLayout> ParseMathBatch(std::wstring_view payload, size_t expected);

// FindResourceW は "#123" を数値 ID と読むので、# で始まる名前で引かせない。
constexpr bool IsModulePath(std::wstring_view path) noexcept
{
    return path.ends_with(L".mjs") && !path.starts_with(L'#');
}

} // namespace mermaid_util
