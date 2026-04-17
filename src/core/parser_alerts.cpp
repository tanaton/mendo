// GitHub Alerts の検出・変換処理。BlockQuote ノードからマーカー [!TYPE] を検出し、
// ラベル・アイコンの挿入と同一 blockquote グループへの alert_type 伝播を行う。
#include "parser.h"
#include <algorithm>
#include <string>
#include <string_view>

const wchar_t* GetAlertLabel(AlertType type) noexcept
{
    switch (type) {
    case AlertType::Note:      return L"Note";
    case AlertType::Tip:       return L"Tip";
    case AlertType::Important: return L"Important";
    case AlertType::Warning:   return L"Warning";
    case AlertType::Caution:   return L"Caution";
    default:                   return L"";
    }
}

const wchar_t* GetAlertIcon(AlertType type) noexcept
{
    switch (type) {
    case AlertType::Note:      return L"\u2139";         // ℹ Information Source
    case AlertType::Tip:       return L"\xD83D\xDCA1";   // 💡 Light Bulb (surrogate pair)
    case AlertType::Important: return L"\u2757";         // ❗ Heavy Exclamation Mark
    case AlertType::Warning:   return L"\u26A0";         // ⚠ Warning Sign
    case AlertType::Caution:   return L"\u26D4";         // ⛔ No Entry
    default:                   return L" ";
    }
}

namespace {

// 大文字小文字を無視して string_view を比較する（ASCII範囲のみ）
bool AsciiCaseEqual(std::string_view a, std::string_view b) noexcept
{
    constexpr auto to_upper = [](char c) noexcept -> char {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    };
    return std::ranges::equal(a, b, {}, to_upper, to_upper);
}

// テキスト先頭から [!TYPE] パターンを検出し、AlertTypeを返す（UTF-8版）。
// marker_end には ']' の次の位置（スペース/改行をスキップ済み）を設定する。
// マーカーは全てASCIIなので、バイトオフセット＝ワイド文字オフセット。
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

    AlertType type = AlertType::None;
    if (AsciiCaseEqual(type_str, "NOTE")) {
        type = AlertType::Note;
    }
    else if (AsciiCaseEqual(type_str, "TIP")) {
        type = AlertType::Tip;
    }
    else if (AsciiCaseEqual(type_str, "IMPORTANT")) {
        type = AlertType::Important;
    }
    else if (AsciiCaseEqual(type_str, "WARNING")) {
        type = AlertType::Warning;
    }
    else if (AsciiCaseEqual(type_str, "CAUTION")) {
        type = AlertType::Caution;
    }

    if (type == AlertType::None) {
        return AlertType::None;
    }

    marker_end = close + 1;
    // マーカー直後のスペースまたは改行を1つスキップ
    if (marker_end < text.size() && (text[marker_end] == ' ' || text[marker_end] == '\n')) {
        marker_end++;
    }
    return type;
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, AlertType type, size_t marker_end)
{
    const wchar_t* const label = GetAlertLabel(type);
    const size_t label_len = std::wcslen(label);
    const wchar_t* const icon = GetAlertIcon(type);
    const size_t icon_len = std::wcslen(icon);

    const auto& current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    // 新しいテキストを構築: "[icon] Label" (+ "\n \n" + 残りテキスト)
    const size_t icon_prefix_len = icon_len + 1; // アイコン文字列 + スペース
    const size_t full_label_len = icon_prefix_len + label_len;
    std::pmr::wstring new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon, icon_len);
    new_text += L' ';
    new_text.append(label, label_len);

    size_t new_content_start = full_label_len;
    if (has_content) {
        new_text += L'\n';
        new_content_start = full_label_len + 1;
        new_text.append(current_text.c_str() + marker_end, current_text.size() - marker_end);
    }

    // TextRun の調整
    const int delta = static_cast<int>(new_content_start) - static_cast<int>(marker_end);

    std::pmr::vector<TextRun> new_runs;
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

    node.SetText(std::move(new_text));
    node.runs = std::move(new_runs);
    node.alert_type = type;
    node.alert_label_length = static_cast<uint32_t>(full_label_len);
}

} // namespace

void DetectAlerts(std::pmr::vector<Node>& nodes)
{
    const auto node_count = nodes.size();
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i].type != NodeType::BlockQuote) {
            continue;
        }
        size_t marker_end = 0;
        const AlertType type = DetectAlertMarker(nodes[i].text_utf8, marker_end);
        if (type == AlertType::None) {
            continue;
        }
        const int group = nodes[i].blockquote_group;
        TransformAlertNode(nodes[i], type, marker_end);

        // 同一 blockquote_group の後続ノードにも同じ alert_type を伝播
        // ノード種別に依存せず、グループIDで判定する（リスト等も含む）
        size_t j = i + 1;
        for (; j < node_count; j++) {
            if (nodes[j].blockquote_group != group) {
                break;
            }
            nodes[j].alert_type = type;
        }
        i = j - 1; // 伝播済みノードをスキップ
    }
}
