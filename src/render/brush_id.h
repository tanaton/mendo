#pragma once
#include <cstddef>
#include <cstdint>

// 描画系で使う固定色ブラシの ID。CommandExecutor は brushes_[BrushId::*] を
// O(1) で配列ルックアップする。動的色のみ Custom を指定し brush_pool 経由になる。
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
    AlertNote,
    AlertTip,
    AlertImportant,
    AlertWarning,
    AlertCaution,
    TitleBarBg,
    TitleBarText,
    TitleBarButtonHover,
    TitleBarButtonActive,
    TitleBarCloseRed,
    TitleBarCloseWhite,
    PaneBg,
    Splitter,
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
    // 動的色を意味するセンチネル。brush_pool 経由でブラシを解決する。
    Custom = 0xFF,
};

// Alert 種別 (AlertNote..AlertCaution) の数。document_types.h の ALERT_TYPE_COUNT と
// 一致する必要があり、その検証は両方を見られる command_generator.cpp で static_assert する
// (brush_id.h は core ヘッダへ依存させないため、ここでは ALERT_TYPE_COUNT を参照しない)。
inline constexpr size_t ALERT_BRUSH_COUNT = 5;

// Alert 種別インデックス (AlertColorIndex の 0..ALERT_BRUSH_COUNT-1) → 対応する固定ブラシ ID。
// command_generator / renderer が同じ写像を共有し、BrushId enum の並び順に依存した
// 連番算術を持たないようにする。idx は呼び出し側で範囲内を保証すること。
constexpr BrushId AlertBrushIdFromIndex(size_t idx) noexcept
{
    constexpr BrushId table[] = {
        BrushId::AlertNote,
        BrushId::AlertTip,
        BrushId::AlertImportant,
        BrushId::AlertWarning,
        BrushId::AlertCaution,
    };
    static_assert(sizeof(table) / sizeof(table[0]) == ALERT_BRUSH_COUNT);
    return table[idx];
}
