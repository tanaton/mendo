#include "parser.h"
#include "ascii_util.h"
#include <algorithm>
#include <iterator>
#include <utility>

std::string_view GetAlertLabel(AlertType type) noexcept
{
    switch (type) {
    case AlertType::None:
        return "";
    case AlertType::Note:
        return "Note";
    case AlertType::Tip:
        return "Tip";
    case AlertType::Important:
        return "Important";
    case AlertType::Warning:
        return "Warning";
    case AlertType::Caution:
        return "Caution";
    }
    std::unreachable();
}

std::string_view GetAlertIcon(AlertType type) noexcept
{
    switch (type) {
    case AlertType::None:
        return " ";
    case AlertType::Note:
        return "ℹ"; // ℹ Information Source (BMP)
    case AlertType::Tip:
        return "\xF0\x9F\x92\xA1"; // 💡 (U+1F4A1)
    case AlertType::Important:
        return "❗"; // ❗ Heavy Exclamation Mark
    case AlertType::Warning:
        return "⚠"; // ⚠ Warning Sign
    case AlertType::Caution:
        return "⛔"; // ⛔ No Entry
    }
    std::unreachable();
}

namespace {

struct AlertMarker {
    AlertType type = AlertType::None;
    size_t end = 0; // マーカー直後の区切り 1 文字を含む終端位置
};

// テキスト先頭の [!TYPE] パターンを検出する。
// Alert マーカーは GitHub 仕様で ASCII 固定なので大小無視 ASCII 比較でよい。
AlertMarker DetectAlertMarker(std::string_view text)
{
    if (text.size() < 3 || text[0] != '[' || text[1] != '!') {
        return {};
    }
    const auto close = text.find(']');
    if (close == std::string_view::npos || close <= 2) {
        return {};
    }

    struct AlertEntry {
        ascii_util::DocLowercaseLiteral name;
        AlertType type;
    };
    static constexpr AlertEntry kAlerts[]{
        { "note",      AlertType::Note      },
        { "tip",       AlertType::Tip       },
        { "important", AlertType::Important },
        { "warning",   AlertType::Warning   },
        { "caution",   AlertType::Caution   },
    };
    const auto type_str = text.substr(2, close - 2);
    const auto it = std::ranges::find_if(kAlerts, [type_str](const AlertEntry& e) noexcept {
        return ascii_util::iequal(type_str, e.name);
    });
    if (it == std::end(kAlerts)) {
        return {};
    }

    size_t end = close + 1;
    if (end < text.size() && (text[end] == ' ' || text[end] == '\n')) {
        end++;
    }
    return { it->type, end };
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const std::string_view label = GetAlertLabel(type);
    const std::string_view icon = GetAlertIcon(type);
    const std::string_view current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    const size_t full_label_len = icon.size() + 1 + label.size(); // "icon Label"
    std::pmr::string new_text;
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

    TextRunList new_runs;
    // アイコン絵文字は太字にしない (スペース + ラベルテキストのみ太字)。
    TextRun label_run;
    label_run.start = static_cast<uint32_t>(icon.size());
    label_run.length = static_cast<uint32_t>(full_label_len - icon.size());
    label_run.set_bold(true);
    new_runs.emplace_back(label_run);

    // マーカー部分を除外し、残りを新しい本文開始位置へ平行移動する。
    const auto marker = static_cast<uint32_t>(marker_end);
    const auto content_start = static_cast<uint32_t>(new_content_start);
    for (TextRun adjusted : node.runs) {
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
    // マーカー [!TYPE] 本体には改行が入らず、DetectAlertMarker で 1 文字だけスキップする
    // 文字が \n の場合のみ改行 1 個。marker_end 直前の 1 文字だけを見ればよい。
    const int32_t marker_newlines = (marker_end > 0 && current_text[marker_end - 1] == '\n');
    const int32_t new_line_count = node.line_count - marker_newlines + has_content;
    node.SetTextWithLineCount(std::move(new_text), new_line_count);
    node.runs = std::move(new_runs);
    node.alert_type = type;
    node.ensure_alert()->alert_label_length = static_cast<uint32_t>(full_label_len);
}

// 単一の BlockQuote ノードに対して Alert マーカーを検出・適用し、
// 同一 blockquote_group の後続ノードに alert_type を伝播する。
// 既に alert_type が設定されているノードはスキップする（伝播で当たった先頭等）。
void DetectAlertAt(std::pmr::vector<Node>& nodes, size_t i)
{
    const auto node_count = nodes.size();
    if (i >= node_count) {
        return;
    }
    auto& node = nodes[i];
    if (node.type != NodeType::BlockQuote || node.alert_type != AlertType::None) {
        return;
    }
    // GitHub 仕様: Alert は最外側 blockquote (quote_depth==1) でのみ認識する。
    // ネスト内 (`> > [!NOTE]`) は通常の引用として扱う。
    if (node.quote_depth != 1) {
        return;
    }
    const auto [type, marker_end] = DetectAlertMarker(node.GetText());
    if (type == AlertType::None) {
        return;
    }
    const int group = node.blockquote_group;
    TransformAlertNode(node, type, marker_end);

    // 同一 blockquote_group の後続ノードにも同じ alert_type を伝播。
    // ノード種別に依存せず、グループIDで判定する（リスト等も含む）。
    for (size_t j = i + 1; j < node_count; j++) {
        if (nodes[j].blockquote_group != group) {
            break;
        }
        nodes[j].alert_type = type;
    }
}

} // namespace

void DetectAlerts(std::pmr::vector<Node>& nodes, std::span<const size_t> blockquote_indices)
{
    for (const size_t i : blockquote_indices) {
        DetectAlertAt(nodes, i);
    }
}
