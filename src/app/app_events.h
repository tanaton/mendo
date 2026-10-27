#pragma once
#include "app_constants.h"
#include "document_utils.h"
#include "pane_layout.h"
#include "ui_types.h"
#include "titlebar.h"
#include "tooltip.h"
#include "ui_constants.h"
#include <array>
#include <variant>
#include <string>
#include <cstdint>

struct KeyDownEvent {
    int key;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
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

struct CopyClipboardAction {};
struct CopyFormattedClipboardAction {};
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
struct ToggleDarkModeAction {};
struct NavigateBackAction {};
struct NavigateForwardAction {};
struct ShowHelpAction {};
struct OpenSearchBarAction {};
struct CloseSearchBarAction {};
struct SearchNextAction {};
struct SearchPrevAction {};
// 検索の入力欄の中の全選択 (検索欄で Ctrl+A)。
struct SearchSelectAllAction {};
struct NoOpAction {};

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

    bool operator==(const HoverHighlights&) const = default;
};

// App がヒットテストで求めたホバー状態。reducer がまとめて状態へ反映する。
struct PointerHover : HoverHighlights {
    TooltipHover tooltip;
};
struct PointerHoverAction {
    PointerHover hover;
};
// ウィンドウ外へ出た。ホバー状態を全て解除しツールチップを閉じる。
struct MouseLeaveAction {};
// edge: 右端のスプリッタを掴んだペイン。
struct SplitterDragStartedAction {
    PaneTarget edge;
};
// ドラッグ中のスプリッタは ViewState::drag、ウィンドウ幅は WindowState から読む。
struct SplitterDragMovedAction {
    float dip_x;
};
// テキスト選択以外の左ドラッグの終了。テキスト選択は終点で選択を確定する TextSelectionEndedAction。
struct LeftDragEndedAction {};
struct SearchInputDragStartedAction {
    int caret_pos;
};
struct SearchInputDragMovedAction {
    int caret_pos;
};
struct MdScrollbarDragStartedAction {
    float dip_y;
};
struct MdScrollbarDragMovedAction {
    float dip_y;
};
struct PaneScrollbarDragStartedAction {
    PaneTarget pane;
    float dip_y;
};
// ドラッグ中のペインは ViewState::drag から読む。
struct PaneScrollbarDragMovedAction {
    float dip_y;
};
struct TextSelectionStartedAction {
    int node_index;
    uint32_t text_pos;
    int click_x;
    int click_y;
};
struct TextSelectionMovedAction {
    int node_index;
    uint32_t text_pos;
};
struct TextSelectionEndedAction {
    int end_node_index;
    uint32_t end_text_pos;
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
struct FilePaneRevealCurrentFileAction {};
struct TocItemClickedAction {
    int node_index;
};
struct NavigateAnchorAction {
    std::string anchor_id;
};

// 別の文書 (読み込み・ヘルプ) に差し替えた。番号に結び付いた一時状態・サイドペインのスクロール・検索を初めに戻し、
// ファイル一覧を合わせ、pending_restore があればそこへ、なければ先頭へスクロールする。
// App は推定高さを図と画像のキャッシュの実測値で補正してから送る (復元先の Y をずらさない)。
struct DocumentOpenedAction {
    bool follow_file_pane = false;
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
struct BlockHScrollDragMovedAction {
    float dip_x;
};
// D&D・ファイルペイン・ファイルダイアログから、履歴を積んでファイルを開く。
struct OpenPathAction {
    std::wstring path;
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
struct SearchSelectionAction {
    int sel_start;
    int sel_end;
};
struct ImeCompositionAction {
    std::wstring text;
};

using AppAction = std::variant<
    NoOpAction,
    KeyScrollAction,
    MouseWheelAction,
    CopyClipboardAction,
    CopyFormattedClipboardAction,
    SelectAllAction,
    SelectWordAction,
    ClearSelectionAction,
    TogglePaneAction,
    ZoomAction,
    ReloadFileAction,
    OpenFileAction,
    ToggleDarkModeAction,
    NavigateBackAction,
    NavigateForwardAction,
    ShowHelpAction,
    OpenSearchBarAction,
    CloseSearchBarAction,
    SearchNextAction,
    SearchPrevAction,
    SearchSelectAllAction,
    PointerHoverAction,
    MouseLeaveAction,
    SplitterDragStartedAction,
    SplitterDragMovedAction,
    LeftDragEndedAction,
    SearchInputDragStartedAction,
    SearchInputDragMovedAction,
    MdScrollbarDragStartedAction,
    MdScrollbarDragMovedAction,
    PaneScrollbarDragStartedAction,
    PaneScrollbarDragMovedAction,
    TextSelectionStartedAction,
    TextSelectionMovedAction,
    TextSelectionEndedAction,
    RightClickGestureStartedAction,
    RightClickGestureMovedAction,
    RightClickGestureCompletedAction,
    FilePaneDirectoryClickedAction,
    FilePaneRevealCurrentFileAction,
    TocItemClickedAction,
    NavigateAnchorAction,
    DocumentOpenedAction,
    DocumentReloadedAction,
    ReloadSettledAction,
    LoadFailedAction,
    HWheelAction,
    BlockHScrollDragStartedAction,
    BlockHScrollDragMovedAction,
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
