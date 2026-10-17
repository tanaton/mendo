#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

// 描画系で使う固定色ブラシの ID。ExecuteDrawCommands は brushes[BrushId::*] を
// O(1) で配列ルックアップする。
enum class BrushId : uint8_t {
    Text,
    Heading,
    CodeBg,
    CodeText,
    Link,
    Hr,
    BlockquoteBar,
    BlockquoteText,
    Selection,
    TableStripe,
    SyntaxKeyword,
    SyntaxType,
    SyntaxString,
    SyntaxNumber,
    SyntaxComment,
    SyntaxPreprocessor,
    SyntaxFunction,
    SyntaxInserted,
    SyntaxDeleted,
    AlertNote,
    AlertTip,
    AlertImportant,
    AlertWarning,
    AlertCaution,
    AlertNoteBg,
    AlertTipBg,
    AlertImportantBg,
    AlertWarningBg,
    AlertCautionBg,
    TitleBarBg,
    TitleBarText,
    TitleBarButtonHover,
    TitleBarButtonActive,
    TitleBarCloseRed,
    TitleBarCloseWhite,
    Splitter,
    PaneBg,
    PaneItemHover,
    PaneItemActive,
    ScrollbarThumb,
    OverlayWhite,
    OverlayBlack,
    OverlayGestureBg,
    GestureTrail,
    SearchBarBg,
    SearchBarBorder,
    SearchInputBg,
    SearchInputText,
    SearchHighlight,
    SearchHighlightCurrent,
    SearchNoMatchBg,
    Count,
    // ブラシ無し (構文トークンの Plain 等、既定ブラシで描く箇所の添字合わせ)。
    None = 0xFF,
};

// Alert 種別 (AlertNote..AlertCaution) の数。document_types.h の ALERT_TYPE_COUNT と
// 一致する必要があり、その検証は両方を見られる command_generator.cpp で static_assert する
// (brush_id.h は core ヘッダへ依存させないため、ここでは ALERT_TYPE_COUNT を参照しない)。
inline constexpr size_t ALERT_BRUSH_COUNT = 5;

// Alert の文字色・背景色は AlertColorIndex の順に連番で並べてある。
static_assert(std::to_underlying(BrushId::AlertCaution) - std::to_underlying(BrushId::AlertNote) + 1 == ALERT_BRUSH_COUNT);
static_assert(std::to_underlying(BrushId::AlertCautionBg) - std::to_underlying(BrushId::AlertNoteBg) + 1 == ALERT_BRUSH_COUNT);

// Alert 種別インデックス (AlertColorIndex の 0..ALERT_BRUSH_COUNT-1) → 対応する固定ブラシ ID。
// idx は呼び出し側で範囲内を保証すること。
constexpr BrushId AlertBrushIdFromIndex(size_t idx) noexcept
{
    return static_cast<BrushId>(std::to_underlying(BrushId::AlertNote) + idx);
}

constexpr BrushId AlertBgBrushIdFromIndex(size_t idx) noexcept
{
    return static_cast<BrushId>(std::to_underlying(BrushId::AlertNoteBg) + idx);
}

// 白黒のオーバーレイ (ナビ・コピー等のボタン) に使うブラシ。不透明度は描画側で掛ける。
constexpr BrushId OverlayBrushId(bool is_dark) noexcept
{
    return is_dark ? BrushId::OverlayWhite : BrushId::OverlayBlack;
}
