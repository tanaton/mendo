#include "parser.h"
#include "ascii_util.h"
#include "document_types.h"
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

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
        // 💡 (U+1F4A1) は BMP 外なので UTF-8 4 byte。MENDO_LIT は実体非変更なので直接バイト列で渡す。
        return "\xF0\x9F\x92\xA1";
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

// テキスト先頭から [!TYPE] パターンを検出し、AlertTypeを返す。
// Alert マーカーは GitHub 仕様で ASCII 固定なので大小無視 ASCII 比較でよい。
AlertType DetectAlertMarker(std::string_view text, size_t& marker_end)
{
    if (text.size() < 3 || text[0] != '[' || text[1] != '!') {
        return AlertType::None;
    }
    const auto close = text.find(']');
    if (close == std::string_view::npos || close <= 2) {
        return AlertType::None;
    }

    const auto type_str = text.substr(2, close - 2);

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

    AlertType type = AlertType::None;
    for (const auto& [name, t] : kAlerts) {
        if (ascii_util::iequal(type_str, name)) {
            type = t;
            break;
        }
    }
    if (type == AlertType::None) {
        return AlertType::None;
    }

    marker_end = close + 1;
    // マーカー直後のスペースまたは改行を1つスキップ
    if (marker_end < text.size() && (text[marker_end] == mendo::doc_sp || text[marker_end] == mendo::doc_lf)) {
        marker_end++;
    }
    return type;
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const std::string_view label = GetAlertLabel(type);
    const std::string_view icon = GetAlertIcon(type);
    const auto& current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    // 新しいテキストを構築: "[icon] Label" (+ "\n \n" + 残りテキスト)
    const size_t icon_prefix_len = icon.size() + 1; // アイコン文字列 + スペース
    const size_t full_label_len = icon_prefix_len + label.size();
    std::pmr::string new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon);
    new_text += mendo::doc_sp;
    new_text.append(label);

    size_t new_content_start = full_label_len;
    if (has_content) {
        new_text += mendo::doc_lf;
        new_content_start = full_label_len + 1;
        new_text.append(current_text.data() + marker_end, current_text.size() - marker_end);
    }

    // TextRun の調整
    const int delta = static_cast<int>(new_content_start) - static_cast<int>(marker_end);

    TextRunList new_runs;
    // ラベル用の太字ラン（アイコン + スペース + ラベルテキスト）
    TextRun label_run;
    label_run.start = 0;
    label_run.length = static_cast<uint32_t>(full_label_len);
    label_run.set_bold(true);
    new_runs.emplace_back(label_run);

    // 元のランを調整（マーカー部分を除外）
    for (const auto& run : node.runs) {
        const uint32_t run_end = run.start + run.length;
        if (run_end <= static_cast<uint32_t>(marker_end)) {
            continue;
        }
        TextRun adjusted = run;
        if (adjusted.start < static_cast<uint32_t>(marker_end)) {
            const uint32_t trim = static_cast<uint32_t>(marker_end) - adjusted.start;
            adjusted.start = static_cast<uint32_t>(marker_end);
            adjusted.length -= trim;
        }
        adjusted.start = static_cast<uint32_t>(static_cast<int>(adjusted.start) + delta);
        new_runs.emplace_back(adjusted);
    }

    // 全文走査で改行を数え直すと O(text) かかるため、ここで差分計算する。
    // マーカー [!TYPE] 本体には改行が入らず、DetectAlertMarker で 1 文字だけスキップする
    // 文字が \n の場合のみ改行 1 個。marker_end 直前の 1 文字だけを見ればよい。
    const int32_t marker_newlines = (marker_end > 0 && current_text[marker_end - 1] == mendo::doc_lf);
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
    size_t marker_end = 0;
    const AlertType type = DetectAlertMarker(node.GetText(), marker_end);
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
