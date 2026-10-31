#pragma once
#include <unknwn.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

// 描画系で使う固定色ブラシの ID。色は Theme::colors に同じ添字で持ち、ExecuteDrawCommands は
// brushes[BrushId::*] を O(1) で配列ルックアップする。
enum class BrushId : uint8_t {
    // 描画面を Clear する色。
    Background,
    Text,
    Heading,
    CodeBg,
    InlineCodeBg,
    CodeText,
    Link,
    Hr,
    BlockquoteBar,
    BlockquoteText,
    Selection,
    TableStripe,
    // SyntaxTokenType の Keyword 以降と同じ順 (SyntaxBrushId)。
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
    Splitter,
    PaneBg,
    PaneItemHover,
    PaneItemActive,
    ScrollbarThumb,
    OverlayWhite,
    // 白黒のオーバーレイ (ナビ・コピー等のボタン)。ダークは白、ライトは黒。不透明度は描画側で掛ける。
    Overlay,
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

// Alert の文字色・背景色は AlertColorIndex の順に連番で並べてある。種別の数 (ALERT_TYPE_COUNT) との一致は、
// core ヘッダへ依存させないため両方を見られる command_generator.cpp で確かめる。
static_assert(std::to_underlying(BrushId::AlertCaution) - std::to_underlying(BrushId::AlertNote) ==
              std::to_underlying(BrushId::AlertCautionBg) - std::to_underlying(BrushId::AlertNoteBg));

inline constexpr size_t BRUSH_COUNT = std::to_underlying(BrushId::Count);

// 連番で並べた ID の並び (first から offset 番目)。範囲内は呼び出し側で保証すること。
constexpr BrushId BrushIdAt(BrushId first, size_t offset) noexcept
{
    return static_cast<BrushId>(std::to_underlying(first) + offset);
}

// Alert 種別インデックス (AlertColorIndex) → 対応する固定ブラシ ID。
constexpr BrushId AlertBrushIdFromIndex(size_t idx) noexcept
{
    return BrushIdAt(BrushId::AlertNote, idx);
}

constexpr BrushId AlertBgBrushIdFromIndex(size_t idx) noexcept
{
    return BrushIdAt(BrushId::AlertNoteBg, idx);
}

// 参照カウントを持たない COM 実装の基底。寿命は実装側が管理する。
template <class Iface>
struct UncountedCom : Iface {
    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        return 1;
    }
};

// IDWriteTextLayout::SetDrawingEffect に渡す描画色の印。ブラシと違ってデバイスに依存しないので、
// 計測ワーカーがテキストレイアウトを作るときに付けられ、デバイスを作り直しても付け直さずに済む。
// 描画時に VisibleRunRenderer が BrushId の固定ブラシへ置き換える。静的な実体なので参照カウントは持たない。
class BrushEffect final : public UncountedCom<IUnknown> {
public:
    static BrushEffect* For(BrushId id) noexcept
    {
        static auto effects = []() static {
            std::array<BrushEffect, BRUSH_COUNT> a{};
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

private:
    BrushId id_ = BrushId::None;
};
