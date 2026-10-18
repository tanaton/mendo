#pragma once
#include <unknwn.h>
#include <array>
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

// IDWriteTextLayout::SetDrawingEffect に渡す描画色の印。ブラシと違ってデバイスに依存しないので、
// 計測ワーカーがテキストレイアウトを作るときに付けられ、デバイスを作り直しても付け直さずに済む。
// 描画時に VisibleRunRenderer が BrushId の固定ブラシへ置き換える。静的な実体なので参照カウントは持たない。
class BrushEffect final : public IUnknown {
public:
    static BrushEffect* For(BrushId id) noexcept
    {
        static auto effects = [] {
            std::array<BrushEffect, std::to_underlying(BrushId::Count)> a{};
            for (size_t i = 0; i < a.size(); i++) {
                a[i].id_ = static_cast<BrushId>(i);
            }
            return a;
        }();
        return &effects[std::to_underlying(id)];
    }
    // 文書のテキストレイアウトに付ける描画効果はこれだけなので、QueryInterface せずに読む。
    static BrushId IdOf(IUnknown* effect) noexcept
    {
        return effect ? static_cast<BrushEffect*>(effect)->id_ : BrushId::None;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override
    {
        if (riid == __uuidof(IUnknown)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        return 1;
    }

private:
    BrushId id_ = BrushId::None;
};
