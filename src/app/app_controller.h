#pragma once
// hit_test_service.h / selection_html.h を reducer など app_state.h を読む全 TU へ広げないため分ける。
#include "app_events.h"
#include "app_state.h"
#include "hit_test_service.h"
#include "selection_html.h"
#include <cstdint>
#include <optional>

// イベント→アクション変換と、ポインタの判定。App が実行する。状態は呼び出し側が持って渡す。
// ポインタの判定は、ホバーとクリックが同じ優先順位で判定するよう、AppState と確定したペイン配置だけから求める。
// px, py はクライアント座標 (物理 px)。
namespace app_controller {

// 割り当ての無いキーは nullopt。
std::optional<AppAction> HandleKeyDown(const KeyDownEvent& event);

// 検索欄にフォーカスがあるときのキー。nullopt なら入力欄 (EDIT) に処理させる。
// 検索欄は開いている間フォーカスを持ち続けるので、入力欄が編集に使うキーだけ欄に任せ、それ以外は本体と同じ割り当てにする。
std::optional<AppAction> HandleSearchEditKeyDown(const KeyDownEvent& event);

MdPaneHitContext MakeMdPaneHitContext(const AppState& state, const PaneLayout& layout, int px, int py) noexcept;
std::optional<LinkHit> LinkAtHit(const AppState& state, const hit_test::HitResult& hit);
bool IsOverMdScrollbar(const AppState& state, const PaneLayout& layout, float dip_x, float dip_y) noexcept;

// MD ペインで点が指す領域。
struct MdPaneHit {
    enum class Kind : uint8_t {
        SearchBar,
        NavButton,
        OverlayButton, // コピー / ダイアグラムコピー / 保存 / 拡大表示
        Scrollbar,
        Content,
    };
    Kind kind = Kind::Content;
    SearchBarHitZone search_bar = SearchBarHitZone::None;
    NavDirection nav = NavDirection::None;
    OverlayButtonHit overlay_button{};
};
MdPaneHit ResolveMdPaneHit(const AppState& state, const MdPaneHitContext& hit_ctx, const PaneLayout& layout, float dip_x, float dip_y) noexcept;

// サイドペインで点が指すもの。
struct SidePaneHit {
    // ヘッダーの上。ボタンの外でもクリックは消費する。
    bool in_header = false;
    PaneHeaderButton button = PaneHeaderButton::None;
    // スクロールバーの上 (ヘッダーの外だけ見る)。項目より優先する。
    bool scrollbar = false;
    // 点の高さにある項目。無ければ -1。
    int item = -1;
};
SidePaneHit ResolveSidePaneHit(const AppState& state, PaneTarget target, const PaneLayout& layout, float dip_x, float dip_y) noexcept;

// App が HCURSOR に写す。
enum class PointerCursor : uint8_t {
    Arrow,
    IBeam,
    Hand,
    SizeWE,
};
struct PointerHoverResult {
    // 各ゾーンは自分のホバーだけを書き、それ以外は既定値 (ホバー無し) のまま返して reducer に解除させる。
    PointerHover hover;
    PointerCursor cursor = PointerCursor::Arrow;
};
// maximized は最大化ボタンの文言 (元に戻す) に使う。
PointerHoverResult ResolvePointerHover(const AppState& state, const PaneLayout& layout, int px, int py, bool maximized);

} // namespace app_controller
