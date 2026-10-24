#include "parser_alerts.h"
#include "ascii_util.h"
#include <algorithm>
#include <iterator>
#include <ranges>

namespace {

struct AlertInfo {
    ascii_util::DocLowercaseLiteral name; // [!TYPE] マーカー名 (GitHub 仕様で ASCII 固定)
    AlertType type;
    std::string_view label;
    std::string_view icon;
};

// AlertColorIndex 順。
inline constexpr AlertInfo kAlerts[]{
    { "note",      AlertType::Note,      "Note",      "ℹ"                },
    { "tip",       AlertType::Tip,       "Tip",       "\xF0\x9F\x92\xA1" }, // U+1F4A1 (BMP 外)
    { "important", AlertType::Important, "Important", "❗"               },
    { "warning",   AlertType::Warning,   "Warning",   "⚠"                },
    { "caution",   AlertType::Caution,   "Caution",   "⛔"               },
};
static_assert(std::size(kAlerts) == ALERT_TYPE_COUNT);
static_assert(std::ranges::all_of(std::views::iota(size_t{ 0 }, ALERT_TYPE_COUNT), [](size_t i) static {
    return AlertColorIndex(kAlerts[i].type) == i;
}));

// "[!" + 最長の名前 + "]"。マーカー判定はソフト改行ごとに走るので、']' を段落全体から探さない。
constexpr size_t kMaxMarkerLength = 3 + std::ranges::max(kAlerts | std::views::transform([](const AlertInfo& e) static { return e.name.value.size(); }));

} // namespace

std::string_view GetAlertLabel(AlertType type) noexcept
{
    const auto idx = AlertColorIndex(type);
    return idx < ALERT_TYPE_COUNT ? kAlerts[idx].label : "";
}

std::string_view GetAlertIcon(AlertType type) noexcept
{
    const auto idx = AlertColorIndex(type);
    return idx < ALERT_TYPE_COUNT ? kAlerts[idx].icon : " ";
}

namespace {

struct AlertMarker {
    AlertType type = AlertType::None;
    size_t end = 0; // マーカー直後の改行を含む終端位置
};

// テキスト先頭の [!TYPE] パターンを検出する。
// Alert マーカーは GitHub 仕様で ASCII 固定なので大小無視 ASCII 比較でよい。
AlertMarker DetectAlertMarker(std::string_view text)
{
    if (text.size() < 3 || text[0] != '[' || text[1] != '!') {
        return {};
    }
    const auto close = text.substr(0, kMaxMarkerLength).find(']');
    if (close == std::string_view::npos || close <= 2) {
        return {};
    }

    const auto type_str = text.substr(2, close - 2);
    const auto it = std::ranges::find_if(kAlerts, [type_str](const AlertInfo& e) noexcept {
        return ascii_util::iequal(type_str, e.name);
    });
    if (it == std::end(kAlerts)) {
        return {};
    }

    // 確定仕様: GitHub に合わせ、マーカーと本文や画像が同じ行にある "[!NOTE] 本文" は Alert にしない
    // (fe1ae552 で同じ行の画像を Alert にし、db6320d7 で戻した)。表示を GitHub と食い違わせない。
    // 行末の空白は md4c が落とすので、マーカー直後は改行か終端だけを見ればよい。
    const size_t end = close + 1;
    if (end == text.size()) {
        return { it->type, end };
    }
    if (text[end] == '\n') {
        return { it->type, end + 1 };
    }
    return {};
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, TextArena& arena, AlertType type, size_t marker_end)
{
    const std::string_view label = GetAlertLabel(type);
    const std::string_view icon = GetAlertIcon(type);
    const std::string_view current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    const size_t full_label_len = icon.size() + 1 + label.size(); // "icon Label"
    std::string new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon);
    new_text += ' ';
    new_text.append(label);

    size_t new_content_start = full_label_len;
    if (has_content) {
        new_text += '\n';
        new_content_start = full_label_len + 1;
        new_text.append(current_text.substr(marker_end));
    }

    std::pmr::vector<TextRun> new_runs;
    new_runs.reserve(node.runs().size() + 1);
    // アイコン絵文字は太字にしない (スペース + ラベルテキストのみ太字)。
    TextRun label_run;
    label_run.start = static_cast<uint32_t>(icon.size());
    label_run.length = static_cast<uint32_t>(full_label_len - icon.size());
    label_run.set_bold(true);
    new_runs.emplace_back(label_run);

    // マーカー部分を除外し、残りを新しい本文開始位置へ平行移動する。
    const auto marker = static_cast<uint32_t>(marker_end);
    const auto content_start = static_cast<uint32_t>(new_content_start);
    for (TextRun adjusted : node.runs()) {
        const uint32_t run_end = adjusted.start + adjusted.length;
        if (run_end <= marker) {
            continue;
        }
        if (adjusted.start < marker) {
            adjusted.start = marker;
            adjusted.length = run_end - marker;
        }
        adjusted.start = content_start + (adjusted.start - marker);
        new_runs.emplace_back(adjusted);
    }

    // 全文走査で改行を数え直すと O(text) かかるため、ここで差分計算する。
    // マーカー [!TYPE] 本体には改行が入らず、DetectAlertMarker が直後の改行を含めたときだけ
    // 改行 1 個。marker_end 直前の 1 文字だけを見ればよい。
    const int32_t marker_newlines = (marker_end > 0 && current_text[marker_end - 1] == '\n');
    const int32_t new_line_count = node.line_count - marker_newlines + has_content;
    node.SetText(arena.CopyText(new_text), new_line_count);
    node.SetRuns(arena.Copy(new_runs));
    node.alert_type = type;
    node.set_alert_label_length(static_cast<uint32_t>(full_label_len));
}

// 単一の BlockQuote ノードに対して Alert マーカーを検出・適用し、
// 同一 blockquote_group の後続ノードに alert_type を伝播する。
// 伝播先は先頭候補にならないので、先頭候補が既に alert_type を持つことはない。
void DetectAlertAt(std::pmr::vector<Node>& nodes, TextArena& arena, size_t i)
{
    if (!IsAlertHeadCandidate(nodes, i)) {
        return;
    }
    auto& node = nodes[i];
    const auto [type, marker_end] = DetectAlertMarker(node.GetText());
    if (type == AlertType::None) {
        return;
    }
    const int group = node.blockquote_group;
    TransformAlertNode(node, arena, type, marker_end);

    // ノード種別に依存せず、グループIDで判定する（リスト等も含む）。
    for (size_t j = i + 1; j < nodes.size(); j++) {
        if (nodes[j].blockquote_group != group) {
            break;
        }
        nodes[j].alert_type = type;
    }
}

} // namespace

// GitHub 仕様: Alert は最外側 blockquote (quote_depth==1) でのみ認識し、ネスト内 (`> > [!NOTE]`) は通常の引用。
// マーカーは blockquote の先頭でのみ有効で、"> intro" の後に続く段落の [!NOTE] は通常の引用。
bool IsAlertHeadCandidate(const std::pmr::vector<Node>& nodes, size_t i) noexcept
{
    const auto& node = nodes[i];
    return node.type == NodeType::BlockQuote && node.quote_depth == 1 &&
           (i == 0 || nodes[i - 1].blockquote_group != node.blockquote_group);
}

bool IsAlertMarkerOnly(std::string_view text)
{
    const auto [type, end] = DetectAlertMarker(text);
    return type != AlertType::None && end == text.size();
}

void DetectAlerts(std::pmr::vector<Node>& nodes, TextArena& arena, std::span<const size_t> blockquote_indices)
{
    for (const size_t i : blockquote_indices) {
        DetectAlertAt(nodes, arena, i);
    }
}
