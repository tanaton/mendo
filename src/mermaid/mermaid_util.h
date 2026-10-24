#pragma once
#include <string>
#include <string_view>
#include <cstdint>

struct Node;

namespace mermaid_util {

// style でノードの枠と背景を非表示にし、数式のみが見えるようにする。
std::pmr::wstring BuildLatexFlowchartCode(std::wstring_view latex);

// 言語種別ソルトで同テキストの Mermaid / LatexMath がキー衝突しない。
uint64_t NodeDiagramHash(const Node& node, float max_width, bool dark_mode) noexcept;
// NodeDiagramHash のうちノードのテキストと言語だけで決まる部分。幅とテーマは DiagramRequestHash で混ぜる。
// テキストのハッシュは図の大きさに比例するので、覚えておけば要求し直すたびに読み直さずに済む。
uint64_t DiagramCodeHash(const Node& node) noexcept;
uint64_t DiagramRequestHash(uint64_t code_hash, float max_width, bool dark_mode) noexcept;
// mermaid は既定で useMaxWidth なので、幅の上限より狭く描けた図はどの幅でも同じ結果になる。
// その結果は幅を混ぜないこのキーでも引けるようにし、幅が変わっても描き直さない。
uint64_t DiagramWidthFreeHash(uint64_t code_hash, bool dark_mode) noexcept;
constexpr bool IsWidthFree(float css_width, float max_width) noexcept
{
    return css_width + 1.0f < max_width;
}

std::pmr::wstring JsEscape(std::wstring_view input);

int QuantizeWidth(float max_width) noexcept;
// 有限でない値 (nan / inf) は 0。
float ParseJsonNumber(std::wstring_view json, std::wstring_view key) noexcept;
bool ParseJsonTrueFlag(std::wstring_view json, std::wstring_view key) noexcept;

// JSON 文字列値を取り出し、エスケープ (\" \\ \/ \n \r \t \b \f \uXXXX) を復元する。
// キーが無い・値が文字列でない・閉じ引用符が無い場合は空を返す。
std::pmr::wstring ParseJsonString(std::wstring_view json, std::wstring_view key);

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
std::pmr::wstring SanitizeErrorMessage(std::wstring_view msg, size_t max_len);

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
    RenderResult,  // "render-result:<id>:<payload>"
    CaptureReady,  // "capture-ready:<id>"
    SvgResult,     // "svg-result:<id>:<payload>"
    RenderError,   // "render-error:<id>:<message>"
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

// FindResourceW は "#123" を数値 ID と読むので、# で始まる名前で引かせない。
constexpr bool IsModulePath(std::wstring_view path) noexcept
{
    return path.ends_with(L".mjs") && !path.starts_with(L'#');
}

} // namespace mermaid_util
