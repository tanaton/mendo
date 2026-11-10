#pragma once
#include "document_utils.h"
#include "nav.h"
#include "chrome_layout.h"
#include "pane_layout.h"
#include "ui_types.h"
#include "titlebar.h"
#include "tooltip.h"
#include <array>
#include <memory>
#include <variant>
#include <string>
#include <cstdint>

struct KeyDownEvent {
    int key;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    // 押し続けたときの自動リピート。
    bool repeat = false;
};

enum class ScrollType : uint8_t {
    LineUp,
    LineDown,
    PageUp,
    PageDown,
    Home,
    End
};

struct KeyScrollAction {
    ScrollType type;
};

// 縦ホイール。zone はポインタの下の領域で、Ctrl ならそこをズームし、そうでなければスクロールする
// (スプリッタの上は本文)。tick でスワイプ検出の軸ロックを更新する。
struct MouseWheelAction {
    int delta; // 生の WHEEL_DELTA 単位
    bool ctrl = false;
    PaneZone zone = PaneZone::MdPane;
    uint64_t tick = 0;
};

// 選択範囲をコピーする。formatted なら HTML (書式付き) も載せる。
struct CopyAction {
    bool formatted = false;
};
struct SelectAllAction {};
// ダブルクリック位置の単語を選択する。
struct SelectWordAction {
    int node_index;
    uint32_t text_pos;
};
struct ClearSelectionAction {};

struct TogglePaneAction {
    PaneTarget target;
};

struct ZoomAction {
    ZoomDirection direction;
    // 段数。速く回したホイールのノッチをまとめて 1 回で組み直す。Reset では使わない。
    int steps = 1;
    // ズームするサイドペイン。無ければ本文。
    std::optional<PaneTarget> pane;
};

struct ReloadFileAction {};
struct OpenFileAction {};
// path が空なら表示中の文書 (ヘルプならファイル一覧のフォルダ) を新しいウィンドウで開く。
struct NewWindowAction {
    std::wstring path;
    // 新しいウィンドウで飛ぶ見出し (UTF-8)。空なら文書の先頭。
    std::string anchor;
};
struct ToggleDarkModeAction {};
// 履歴を戻る (forward が false) か進む。
struct NavigateHistoryAction {
    bool forward = false;
};
struct ShowHelpAction {};
struct OpenSearchBarAction {};
struct CloseSearchBarAction {};
// 次 (forward) か前のマッチへ移る。
struct SearchStepAction {
    bool forward = true;
};
// 検索の入力欄の中の全選択 (検索欄で Ctrl+A)。
struct SearchSelectAllAction {};
// 検索の入力欄のダブルクリック。pos (UTF-16) を含む語を選ぶ。
struct SearchSelectWordAction {
    int pos = 0;
};

// 指している本文のリンク。
struct LinkHover {
    // リンクのあるノード。-1 なら指していない。
    int node = -1;
    // node の view_link_urls() の添字。
    uint32_t index = 0;
    // ポインタが動くたびにパスを解決しないよう、別のリンクに移ったときだけ作り、同じリンクの間は前のものを共有する
    // (app_controller.cpp の HoverLink)。比べるのはポインタなので、同じリンクの上で動いても描き直さない。
    std::shared_ptr<const LinkDestination> destination;
    LinkStatusDock dock = LinkStatusDock::Left;

    bool Any() const noexcept
    {
        return node >= 0;
    }
    bool SameLink(const LinkHover& o) const noexcept
    {
        return node == o.node && index == o.index;
    }
    bool Navigable() const noexcept
    {
        return destination && destination->navigable;
    }
    std::wstring_view StatusText() const noexcept
    {
        return destination ? std::wstring_view{ destination->text } : std::wstring_view{};
    }
    bool operator==(const LinkHover&) const = default;
};

// マウス位置でのホバー強調の全体。ViewState::hover が持ち、描画はここから読む。
// 既定値はどこにもホバーしていない状態。
struct HoverHighlights {
    TitleBarHitZone titlebar = TitleBarHitZone::None;
    // Input はホバー強調が無いので None と同じに扱う。
    SearchBarHitZone search_bar = SearchBarHitZone::None;
    NavDirection nav = NavDirection::None;
    OverlayButtonHit overlay_button{};
    // マウスが図の画像領域上にあるノード。-1 なら無し。図の保存/コピーはこれがある間だけ描く。
    int diagram = -1;
    // 横スクロールバーを出すブロック。-1 なら無し。
    int h_block = -1;
    // PaneTarget 順 (File, Toc)。
    std::array<int, 2> side_index{ -1, -1 };
    std::array<PaneHeaderButton, 2> side_button{ PaneHeaderButton::None, PaneHeaderButton::None };
    LinkHover link;

    bool operator==(const HoverHighlights&) const = default;
};

// App がヒットテストで求めたホバー状態。reducer がまとめて状態へ反映する。
struct PointerHoverAction : HoverHighlights {
    TooltipHover tooltip;
};
// ウィンドウ外へ出た。ホバー状態を全て解除しツールチップを閉じる。
struct MouseLeaveAction {};
// 読み込みのスピナーを本文ペインの代わりに出す / 消す。
struct LoadingShownAction {
    bool shown = false;
};
// edge: 右端のスプリッタを掴んだペイン。
struct SplitterDragStartedAction {
    PaneTarget edge;
};
// 左ドラッグ中の移動。どの値を使うかはドラッグの種類 (ViewState::drag) で決まる。
// search_caret は検索の入力欄 (SearchInput)、node_index / text_pos はテキスト選択 (TextSelect) のときだけ App が詰める。
struct LeftDragMovedAction {
    float dip_x = 0.0f;
    float dip_y = 0.0f;
    int search_caret = 0;
    int node_index = -1;
    uint32_t text_pos = 0;
};
// 左ドラッグの終わり。テキスト選択なら node_index / text_pos の終点で選択を確定する (-1 なら選択を変えない)。
struct LeftDragEndedAction {
    int node_index = -1;
    uint32_t text_pos = 0;
};
struct SearchInputDragStartedAction {
    int caret_pos;
};
struct MdScrollbarDragStartedAction {
    float dip_y;
};
struct PaneScrollbarDragStartedAction {
    PaneTarget pane;
    float dip_y;
};
struct TextSelectionStartedAction {
    int node_index;
    uint32_t text_pos;
    int click_x;
    int click_y;
};
struct RightClickGestureStartedAction {
    float dip_x;
    float dip_y;
};
struct RightClickGestureMovedAction {
    float dip_x;
    float dip_y;
};
struct RightClickGestureCompletedAction {
    int screen_x;
    int screen_y;
};
struct FilePaneDirectoryClickedAction {
    std::wstring full_path;
};
// ドロップしたフォルダ。起動引数のフォルダと同じく、文書はそのままでファイル一覧をそのフォルダへ移す。
struct OpenDirectoryAction {
    std::wstring path;
};
struct FilePaneRevealCurrentFileAction {};
struct TocItemClickedAction {
    int node_index;
};
struct NavigateAnchorAction {
    std::string anchor_id;
};

// 別の文書 (読み込み・ヘルプ) に差し替えた。番号に結び付いた一時状態・サイドペインのスクロール・検索を初めに戻し、
// ファイル一覧を合わせ、pending_scroll があればそこへ、なければ先頭へスクロールする。
// App は推定高さを図のキャッシュの実測値で補正し、読んだことのある画像の寸法を入れてから送る (復元先の Y をずらさない)。
struct DocumentOpenedAction {
    bool follow_file_pane = false;
    // 差し替える前の文書の表示位置。確定待ちの開く・戻る/進むで、表示していた文書はこの位置で履歴に入る
    // (読み込みの間にスクロールや見出しへの移動をした分を含める)。
    NavEntry left;
};
// 同じ文書の新しい版に差し替えた (リロード)。番号に結び付いた一時状態を消し、変わらなかったノードの検索マッチは残し、
// scroll_y (変更箇所が見える位置。App が実測の高さで求める) へスクロールする。
struct DocumentReloadedAction {
    UnchangedEnds unchanged;
    float scroll_y = 0.0f;
};
// リロード後のスクロール位置と見えている範囲のレイアウトが決まった。検索中なら新しい文書で検索し直す。
struct ReloadSettledAction {};
// ファイルロードが失敗した (DocumentOpenedAction が来ない) ことの通知。
struct LoadFailedAction {};
struct HWheelAction {
    int delta; // 生の WHEEL_DELTA 単位
    uint64_t tick;
};

struct BlockHScrollDragStartedAction {
    int node_index;
    float dip_x;
};
// D&D・ファイルペイン・ファイルダイアログ・リンクから、履歴を積んでファイルを開く。
struct OpenPathAction {
    std::wstring path;
    // 開いた文書で飛ぶ見出し (UTF-8)。空なら文書の先頭。
    std::string anchor;
};

struct ResizeAction {
    uint32_t width;
    uint32_t height;
};
// suggested は WM_DPICHANGED が勧める新しいウィンドウ矩形 (スクリーン座標)。
struct DpiChangedAction {
    uint32_t dpi;
    RECT suggested;
};
struct ActivateAction {
    bool active;
};
struct EnterSizeMoveAction {};
struct ExitSizeMoveAction {};
struct CaptureChangedAction {};

// AppState を変えるタイマーだけをアクションにする。サービスを進めるだけのものは App::HandleTimer が直接処理する。
struct SwipeOverlayTimeoutAction {};

struct SearchTextChangedAction {
    std::wstring text;
};
// 時間で区切った検索を、メッセージループの空き時間に 1 回分進める。
struct SearchContinueAction {};
struct ToggleCaseSensitiveAction {};
struct ToggleHighlightAction {};
// 検索 EDIT の選択。caret は基点 (anchor) より前のこともある。EM_GETSEL は向きを失って (小さい方, 大きい方) で返すので、
// キー操作の後に読んだ選択は ordered にして、向きを reducer に推定させる。
struct SearchSelectionAction {
    int anchor;
    int caret;
    bool ordered = false;
};
struct ImeCompositionAction {
    std::wstring text;
};

using AppAction = std::variant<
    KeyScrollAction,
    MouseWheelAction,
    CopyAction,
    SelectAllAction,
    SelectWordAction,
    ClearSelectionAction,
    TogglePaneAction,
    ZoomAction,
    ReloadFileAction,
    OpenFileAction,
    NewWindowAction,
    ToggleDarkModeAction,
    NavigateHistoryAction,
    ShowHelpAction,
    OpenSearchBarAction,
    CloseSearchBarAction,
    SearchStepAction,
    SearchSelectAllAction,
    SearchSelectWordAction,
    PointerHoverAction,
    MouseLeaveAction,
    LoadingShownAction,
    SplitterDragStartedAction,
    LeftDragMovedAction,
    LeftDragEndedAction,
    SearchInputDragStartedAction,
    MdScrollbarDragStartedAction,
    PaneScrollbarDragStartedAction,
    TextSelectionStartedAction,
    RightClickGestureStartedAction,
    RightClickGestureMovedAction,
    RightClickGestureCompletedAction,
    FilePaneDirectoryClickedAction,
    OpenDirectoryAction,
    FilePaneRevealCurrentFileAction,
    TocItemClickedAction,
    NavigateAnchorAction,
    DocumentOpenedAction,
    DocumentReloadedAction,
    ReloadSettledAction,
    LoadFailedAction,
    HWheelAction,
    BlockHScrollDragStartedAction,
    OpenPathAction,
    ResizeAction,
    DpiChangedAction,
    ActivateAction,
    EnterSizeMoveAction,
    ExitSizeMoveAction,
    CaptureChangedAction,
    SwipeOverlayTimeoutAction,
    SearchTextChangedAction,
    SearchContinueAction,
    ToggleCaseSensitiveAction,
    ToggleHighlightAction,
    SearchSelectionAction,
    ImeCompositionAction>;
