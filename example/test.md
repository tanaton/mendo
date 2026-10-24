# mendo ソフトウェア詳細仕様書

## 1. はじめに

### 1.1 本文書の目的

本文書は **mendo** の詳細なソフトウェア仕様を記述する。本アプリケーションは、Webブラウザを使わず **Direct2D / DirectWrite** による自前レンダリングで高速なMarkdown表示を実現するWindows向けネイティブビュアーである。

### 1.2 対象読者

- 開発者・コントリビュータ
- テスト担当者
- プロジェクト管理者

### 1.3 用語定義

| 略語 | 正式名称 | 説明 |
|:-----|:---------|:-----|
| D2D | Direct2D | Microsoftの2Dグラフィックス API |
| DWrite | DirectWrite | テキストレイアウト・描画 API |
| md4c | Markdown for C | SAX型コールバック方式のCパーサ |
| WIC | Windows Imaging Component | 画像デコード・エンコードAPI |
| DPI | Dots Per Inch | 画面解像度の単位 |
| DIP | Device Independent Pixel | 論理ピクセル単位 |
| TOC | Table of Contents | 目次 |
| SAX | Simple API for XML | イベント駆動型パーサ方式 |
| PMR | Polymorphic Memory Resource | C++17 のアロケータ抽象 |
| LRU | Least Recently Used | キャッシュエビクションポリシー |

---

## 2. システム概要

### 2.1 アーキテクチャ全体像

```mermaid
graph TB
    subgraph ユーザー入力
        A[ファイルを開く<br>D&D / Ctrl+O]
        B[スクロール / ズーム]
        C[ナビゲーション / ジェスチャ]
    end

    subgraph ウィンドウ層
        W[Win32Window<br>Win32 メッセージループ]
    end

    subgraph アプリケーションコア
        APP[App<br>サービス統括・副作用実行]
        AS[AppState<br>全状態集約]
        RD[Reducer<br>Action→Effects in-place]
        AC[app_controller<br>イベント→Action変換]
    end

    subgraph ドメインサービス
        DS[DocumentService<br>ファイル読込統括]
        FLS[FileLoadService<br>非同期ロード制御]
        LS[LayoutService<br>レイアウト計算統括]
        NH[NavHistory + HandleLinkClick<br>履歴・リンク解決]
        TS[ThemeService<br>テーマ・ズーム永続化]
        SS_SVC[SessionService<br>セッション永続化]
        RM[ResourceManager<br>画像・図 Mermaid/LaTeX 管理]
        HT[hit_test<br>当たり判定]
    end

    subgraph 描画パイプライン
        CG[CommandGenerator<br>描画コマンド生成]
        CE[ExecuteDrawCommands<br>描画コマンド実行]
        REN[Renderer<br>D2D高水準API]
        RB[D2DRenderBackend<br>D2D/DWriteファクトリ]
    end

    subgraph "外部・OS"
        MD[md4c]
        WV[WebView2]
        MJ[mermaid.js]
        D2D_OS[Direct2D / DirectWrite / WIC]
        OS[Win32 API]
    end

    A & B & C --> W
    W --> APP
    APP --> AC
    AC --> RD
    APP --> RD
    RD --> AS
    APP -->|App::Execute| OS
    APP --> REN
    REN --> CG
    CG --> CE
    REN --> RB
    CE & RB --> D2D_OS
    APP --> DS & FLS & LS & NH & TS & SS_SVC & RM & HT
    DS --> MD
    RM --> WV
    WV --> MJ
```

### 2.2 アーキテクチャ原則

mendo は **状態と振る舞いを明確に分離** する設計を採用している。

```mermaid
flowchart LR
    EVT[イベント<br>Win32 メッセージ] --> AC[app_controller]
    AC --> ACTION[AppAction<br>std::variant]
    ACTION --> RED[Reducer<br>in-place 更新]
    RED --> ST[AppState<br>更新済み]
    RED --> EFF[SideEffectList<br>std::pmr::vector]
    EFF --> EXEC[App::Execute<br>std::visit]
    EXEC --> WIN[Win32 API・サービス呼出し]
    ST --> RS[RenderParams 構築<br>App::OnPaint 内]
    RS --> RNDR[Renderer::Render]
    RNDR --> CG[CommandGenerator]
    CG --> CMDS[DrawCommandList]
    CMDS --> CE[ExecuteDrawCommands]
```

| 設計原則 | 実装場所 |
|:---------|:--------|
| **状態の単一の真実源** | `AppState`（`src/app/app_state.h`） |
| **アクション駆動** | `AppAction = std::variant<...>`（`src/app/app_events.h`） |
| **副作用の明示化** | `SideEffect`（単一variant、`src/app/side_effect.h`） |
| **副作用の実行の一元化** | `App::Execute`（`src/app/app.cpp`） |
| **DirectWrite 計測の抽象化** | `ITextMeasurer`（`src/layout/text_measurer.h`） |
| **WebView2 依存の隔離** | `IMermaidRenderer`（`src/mermaid/mermaid_renderer_interface.h`） |
| **D2D ファクトリ・デバイスの集約** | `D2DRenderBackend`（`src/render/d2d_render_backend.h`） |
| **ロジックとUI層の分離** | `mendo_core`（テスト可能）vs `mendo`（Win32/D2D 具象） |

---

## 3. コンポーネント詳細

### 3.1 Win32Window — ウィンドウ管理

#### 3.1.1 責務

`Win32Window` はWin32 APIの薄いラッパーとして、以下を担当する。

1. **ウィンドウ生成** — `WNDCLASSEXW` の登録、`CreateWindowExW` によるウィンドウ作成
2. **メッセージループ** — `MsgWaitForMultipleObjects` でメッセージ・ファイル監視イベント・フレーム待ちの waitable（`App::GetFrameWaitHandle`）を待ち、`PeekMessageW` / `TranslateMessage` / `DispatchMessageW` で配る。前フレームの表示待ちの間は `WM_PAINT` を取り出さずに入力を先に配り、waitable の成立または期限（`D2DRenderBackend::kFrameWaitTimeoutMs` = 1000ms）で `App::OnFrameReady` を呼ぶ。時間で区切った仕事（`App::HasIdleWork`）が残っていれば待たずに戻り、配るメッセージが無ければ `App::RunIdleWork` で 1 回分進める
3. **メッセージルーティング** — Win32メッセージを `App` のイベントハンドラに委譲
4. **NC領域処理** — `WM_NCCALCSIZE` / `WM_NCHITTEST` でカスタムタイトルバーを実装

#### 3.1.2 クラス構造

```cpp
class Win32Window {
public:
    explicit Win32Window(ConfigService& config);
    ~Win32Window();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    int  RunMessageLoop();

    void SetInitialDirectory(std::wstring_view dir_path);
    void StartPreloadAsync(std::pmr::wstring path);
    void RestoreScrollPosition();   // Create より前に呼ぶ

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnNcCalcSize(WPARAM wParam, LPARAM lParam);
    LRESULT OnNcHitTest(LPARAM lParam);
    LRESULT HitTestResizeFrame(POINT pt) const noexcept;
    LRESULT HitTestTitleBar(POINT pt) const noexcept;
    std::optional<LRESULT> TryHandleMouseMessage(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleAppNotification(UINT msg, WPARAM wp, LPARAM lp);
    void OnContextMenu(int screen_x, int screen_y);
    POINT KeyboardContextMenuPoint() const;
    void OnSize(WPARAM wParam, LPARAM lParam);
    void UpdateDwmFrame();
    void InitSystemMenu();
    void ResetWindowPlacement();
    void SaveWindowPlacement();
    bool RestoreWindowPlacement(int nCmdShow);
    static LRESULT CALLBACK SearchEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT OnSearchEditKeyDown(HWND edit, WPARAM wParam, LPARAM lParam);
    void OnSearchEditChanged();
    bool EnsureSearchEdit();
    void FocusSearchEdit(WPARAM mode, LPARAM selection);
    void RepositionSearchEdit();
    void SyncSearchCaretFromEdit();
    void UpdateDpiMetricsCache();

    HWND hwnd_ = nullptr;
    HWND search_edit_ = nullptr;
    bool in_sys_menu_ = false;
    bool tracking_mouse_ = false;
    bool was_minimized_ = false;
    std::pmr::wstring search_text_buf_;
    int  cached_nchit_border_ = 4;
    int  cached_nchit_frame_y_ = 8;
    int  cached_nchit_right_border_ = 8;

    ConfigService& config_;
    std::unique_ptr<App> app_;
};
```

`Win32Window` は `ConfigService` の参照を受け取り、内部で `App` を `unique_ptr` で保持する。`Create` は `App::Init` の後、ウィンドウを DWM でクロークしたまま配置の復元 → `App::LoadInitialDocument` → `App::CreateRenderDevice` と進め、デバイス作成を待つ間に届いた `PARSE_COMPLETE` があれば取り込んでから初回描画（`UpdateWindow`）し、クロークを解く。検索バーの実体は Win32 の Edit コントロール（`search_edit_`）で、サブクラス化（`SearchEditProc`）して IME・キャレット同期を行う。Edit は comctl32 を読み込ませるので、起動時には作らず、初めて検索欄にフォーカスするときに `EnsureSearchEdit` で作る。

### 3.2 App — アプリケーション統括

#### 3.2.1 責務

`App` はアプリケーション全体の統括を担う。状態は `AppState` に集約し、振る舞いはサービス群に委譲する。

1. **サービス所有** — 各種サービスの生成・接続・ライフサイクル管理
2. **イベントディスパッチ** — Win32イベントを `app_controller` 経由で `AppAction` に変換、`Reducer` に渡す
3. **副作用実行** — `Reducer` が返した `SideEffectList` を `App::Execute` が `std::visit` で Win32 API 呼び出しや App メンバ・サービスの呼び出しに振り分ける
4. **描画起点** — `OnPaint` で各レンダーステートを構築し、`RenderParams` にまとめて `Renderer::Render` に渡す

#### 3.2.2 クラス構造

```cpp
class App {
public:
    explicit App(ConfigService& config) noexcept : config_(config) {}
    bool Init(HWND hwnd);

    // 起動時に Win32Window から呼ばれる
    void SetInitialDirectory(std::wstring_view dir_path);   // Init 前
    void StartPreloadAsync(std::pmr::wstring path);         // Init 前
    void SetPendingRestoreNode(int node, int offset) noexcept;
    void LoadInitialDocument();   // preload を取り込む。無ければヘルプを開く
    bool CreateRenderDevice();    // LoadInitialDocument の後、初回描画の前

    // Win32Window から呼ばれるイベントハンドラ
    void OnPaint();
    void OnResize(UINT width, UINT height);
    void OnMinimize();
    void OnMouseWheel(int px, int py, short delta, bool ctrl = false);
    void OnMouseHWheel(short delta);
    void OnKeyDown(WPARAM key);
    void OnDropFiles(HDROP hDrop);
    void OnDpiChanged(UINT dpi, const RECT* suggested);

    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnLButtonDblClk(int px, int py);
    void OnMouseMove(int px, int py);
    void OnMouseHover(int px, int py);
    void OnMouseLeave() { Dispatch(MouseLeaveAction{}); }
    void OnContextMenu(int screen_x, int screen_y);
    bool OnRButtonDown(int px, int py);
    bool OnRButtonUp(int px, int py);
    void OnRButtonMove(int px, int py);
    void OnXButtonBack()    { Dispatch(NavigateBackAction{}); }
    void OnXButtonForward() { Dispatch(NavigateForwardAction{}); }

    HANDLE GetFileWatchEvent() const noexcept;
    void OnFileWatchEvent();
    // 前フレームの表示待ちの間だけ非 null。メッセージループはこれを待つ間 WM_PAINT を配らない
    HANDLE GetFrameWaitHandle() const noexcept;
    void OnFrameReady() noexcept;

    void HandleTimer(UINT_PTR timer_id);
    void OnAppImageLoaded();
    void OnParseComplete();
    void OnMermaidDiskLoaded();
    void OnCaptureChanged();
    void OnDestroy();
    void OnEnterSizeMove() { Dispatch(EnterSizeMoveAction{}); }
    void OnExitSizeMove()  { Dispatch(ExitSizeMoveAction{}); }
    void OnActivate(bool active) { Dispatch(ActivateAction{ active }); }

    // 検索コールバック (Win32Windowから呼ばれReducer経由で状態更新)
    void OnSearchTextChanged(const std::pmr::wstring& text);
    void OnSearchClose()        { Dispatch(CloseSearchBarAction{}); }
    void OnSearchNext()         { Dispatch(SearchNextAction{}); }
    void OnSearchPrev()         { Dispatch(SearchPrevAction{}); }
    constexpr bool IsSearchBarVisible() const noexcept;
    void SetSearchSelection(int sel_start, int sel_end);
    void SetImeComposition(std::pmr::wstring comp);
    RECT GetSearchEditRect();

    // Win32Window の NC ヒットテスト・コンテキストメニュー判定用
    constexpr float GetDpiScale() const noexcept;
    float GetTitleBarHeightDip() const noexcept;
    TitleBarHitZone TitleBarHitTest(float dip_x, float dip_y) const noexcept;
    bool IsOverMdScrollbar(float dip_x, float dip_y);

private:
    void Dispatch(const AppAction& action);   // Reduce → Execute

    // reducer が返した副作用を実行する
    void Execute(const SideEffectList& effects);
    void Execute(const SideEffect& effect);

    void LoadMarkdownFile(std::wstring_view path);

    // Win32 ハンドル
    HWND hwnd_ = nullptr;
    SystemCursors cursors_;
    Tooltip       tooltip_;   // ウィンドウ。表示対象は InteractionState::tooltip

    // コアサービス
    Renderer           renderer_;
    TaskScheduler      scheduler_;          // 汎用ワーカー（ファイル読み込み/画像/Mermaid/キャッシュ書き込み）
    TaskScheduler      layout_scheduler_;   // レイアウト計測専用ワーカー
    MermaidFileCache   file_cache_;
    // ~MermaidRenderer の CancelPending が in-flight コールバック経由で
    // clipboard_manager_ に触るため、mermaid_renderer_ より先に宣言し後に破棄させる。
    // クリップボード/画像保存 (コードコピー・PNG 保存・SVG コピー)。SVG の
    // 非同期コピー用 LRU キャッシュもこの中に閉じている。
    ClipboardManager   clipboard_manager_;
    MermaidRenderer    mermaid_renderer_;
    ImageLoader        image_loader_;
    FileWatcher        file_watcher_;
    ConfigService&     config_;
    ThemeService       theme_service_{ config_ };
    SessionService     session_{ config_ };
    FileLoadService    file_load_service_;
    PrefixShrinkRetry  prefix_shrink_retry_;   // 末尾が縮んだリロードを延期する回数の上限管理

    // 全状態集約
    AppState state_;

    // 振る舞い
    LayoutService   layout_service_{ renderer_.GetLayout(), state_.view.viewport };
    AppHost         host_{ *this };   // ResourceManager / SearchBarController からの問い合わせを App に転送
    ResourceManager resource_manager_;
};
```

#### 3.2.3 ファイル構成

`App` は責務ごとにファイル分割されている。

| ファイル | 担当 |
|:---------|:-----|
| `app.h / app.cpp` | `App` と `AppHost`（`ResourceManagerHost` / `SearchBarHost` の実装。タイマー操作・ペイン寸法・テーマ・再レイアウトの問い合わせを App に転送する）。`.cpp` はコアロジック（`Init`, `CreateRenderDevice`, `LoadInitialDocument`, `Dispatch`, `Execute`, `OnPaint`, `HandleTimer`, `OnDestroy`）、レイアウト・スクロール関連ヘルパー（`EnsureScrollTarget`, `RelayoutViewport`, `RequestDeferredLayout`, `OnDeferredLayout`, `SyncTocActiveAndAutoScroll` 等）、空き時間の仕事（`HasIdleWork` / `RunIdleWork`）、テーマ・ズーム変更の適用（`HandleApplyThemeChange`）、カスタムコンテキストメニュー処理 |
| `app_file.cpp` | `LoadMarkdownFile`, `StartPreloadAsync`, `BeginAsyncLoad`, `OnParseComplete`、文書の差し替え（`ReplaceDocument` / `ReloadDocument`）、リロード（`ReloadCurrentFile`, `FinishReload`）、`LoadHelpDocument` |
| `app_mouse.cpp` | マウス入力ハンドラ（`OnMouseMove`, `OnMouseWheel`, `OnRButton*`, ドラッグ系統括）、クリック処理（`OnLButtonDown/Up`, `HandleMdPaneClick`, `HandleLinkClick` 等）、ホバー処理（`OnMouseHover`, `HoverMdPane`, `ResolveMdPaneHit`） |
| `clipboard_manager.h / .cpp` | `ClipboardManager`（コピー/PNG 保存/SVG コピーを集約。 SVG LRU キャッシュを内包） |
| `app_controller.cpp` | `app_controller::HandleKeyDown` / `HandleMouseWheel`（宣言は `app_events.h`） |
| `reducer.h / .cpp` | `Reduce(state, action)`（in-place 更新、副作用リストを返す）と、ドメイン別の `Reduce*` 関数（スクロール・入力・ナビゲーション・検索・UI・システム）。`Reduce*` は無名名前空間に置く。左ドラッグの終了とキャプチャ解放をまとめた `EndLeftDrag` も公開する |
| `side_effect.h` | `SideEffect`（`effect::*` の variant）と `SideEffectList`。実行は `App::Execute` |
| `resource_manager.h` | 画像・図（Mermaid / LaTeX）・ビットマップ管理（ヘッダ内定義）と、App に頼むことの抽象インターフェース `ResourceManagerHost` |
| `reload_flow.h` | `PlanParseComplete`（非同期ロード完了時の分岐判定。純粋関数） |
| `startup_plan.h / .cpp` | `DecideStartupPlan`（起動引数・前回ファイル・カレントディレクトリから初期表示を決める） |
| `app_constants.h` | タイマー ID・カスタムメッセージ・閾値定数 |

#### 3.2.4 メッセージハンドリング

```mermaid
flowchart TD
    LOOP[Win32Window::RunMessageLoop<br>MsgWaitForMultipleObjects] -->|ファイル監視イベント| FW[App::OnFileWatchEvent]
    LOOP -->|フレーム waitable 成立 / 期限切れ| FR[App::OnFrameReady]
    LOOP -->|PeekMessageW / DispatchMessageW| W{Win32Window<br>WndProc}
    W -->|WM_PAINT| PAINT[App::OnPaint]
    W -->|WM_SIZE| SIZE[App::OnResize<br>最小化時は App::OnMinimize]
    W -->|WM_KEYDOWN / WM_SYSKEYDOWN Alt+←→| KEY[App::OnKeyDown]
    W -->|WM_MOUSEWHEEL| WHEEL[App::OnMouseWheel]
    W -->|WM_MOUSEHWHEEL| HWHEEL[App::OnMouseHWheel]
    W -->|WM_LBUTTONDOWN/UP/DBLCLK| L[App::OnLButton...]
    W -->|WM_MOUSEMOVE| MV[App::OnMouseMove / OnRButtonMove / OnMouseHover]
    W -->|WM_RBUTTONDOWN/UP| R[App::OnRButton...]
    W -->|WM_XBUTTONDOWN| XBTN[OnXButtonBack/Forward]
    W -->|WM_DROPFILES| DROP[App::OnDropFiles]
    W -->|WM_DPICHANGED| DPI[App::OnDpiChanged]
    W -->|WM_TIMER| TIMER[App::HandleTimer]
    W -->|WM_DESTROY| DESTROY[App::OnDestroy<br>設定保存]
    W -->|WM_ENDSESSION| ES[DestroyWindow]
    ES --> DESTROY

    KEY --> AC[app_controller::HandleKeyDown]
    AC --> ACTION[AppAction]
    ACTION --> DISP[App::Dispatch]
    DISP --> RED[Reduce]
    RED --> EFFS[SideEffectList]
    EFFS --> EXEC[App::Execute]
    EXEC --> WIN[Win32 API・App メンバ]

    PAINT --> RC[各 RenderState 構築]
    RC --> RNDR[Renderer::Render]
    RNDR --> CG[CommandGenerator]
    CG --> CE[ExecuteDrawCommands]

    W -->|WM_APP+2 IMAGE_LOADED| IMGLD[App::OnAppImageLoaded]
    W -->|WM_APP+4 SEARCH_FOCUS| SF[検索バーフォーカス]
    W -->|WM_APP+5 SEARCH_UNFOCUS| SUF[検索バーアンフォーカス]
    W -->|WM_APP+6 PARSE_COMPLETE| PC[App::OnParseComplete]
    W -->|WM_APP+7 MERMAID_DISK_LOADED| MDL[App::OnMermaidDiskLoaded]
```

`WM_MOUSEMOVE` は `MK_LBUTTON` なら `OnMouseMove`（左ドラッグ）、`MK_RBUTTON` なら `OnRButtonMove`（ジェスチャ）、どちらでもなければ `OnMouseHover` に振り分ける。`WM_MOUSEWHEEL` は Ctrl 併用ならズーム、Shift 併用なら向きを反転して `OnMouseHWheel`（横スクロール）に回す。前フレームの表示待ちの間、メッセージループは `WM_PAINT` を取り出さずに入力・ポスト・送信メッセージだけを配る（3.1.1 参照）。`WM_ENDSESSION` ではログオフ・シャットダウン時に `WM_DESTROY` が来ないままプロセスが終わるため、`DestroyWindow` で終了処理（設定保存）を通す。

#### 3.2.5 キーボードショートカット

`app_controller::HandleKeyDown()` がキー入力を `AppAction` に変換する。

| キー | アクション | 備考 |
|:-----|:-----------|:-----|
| `F1` | `ShowHelpAction` | 内蔵ヘルプ表示 |
| `F3` | `SearchNextAction` | 次の検索結果 |
| `Shift+F3` | `SearchPrevAction` | 前の検索結果 |
| `F5` | `ReloadFileAction` | 再読み込み |
| `Esc` | `ClearSelectionAction` | 選択解除（検索バー表示中は検索バーを閉じる） |
| `Ctrl+O` | `OpenFileAction` | OpenFileDialog |
| `Ctrl+A` | `SelectAllAction` | 全選択 |
| `Ctrl+C` | `CopyClipboardAction` | プレーンテキストコピー |
| `Ctrl+Shift+C` | `CopyFormattedClipboardAction` | 書式付きコピー (HTML) |
| `Ctrl+F` | `OpenSearchBarAction` | 検索バー表示 |
| `Ctrl+G` | `SearchNextAction` | 次の検索結果 |
| `Ctrl+Shift+G` | `SearchPrevAction` | 前の検索結果 |
| `Ctrl+1` | `TogglePaneAction(File)` | ファイルペイン切替 |
| `Ctrl+2` | `TogglePaneAction(Toc)` | TOCペイン切替 |
| `Ctrl++` / `Ctrl+=` | `ZoomAction(In)` | ズームイン（テンキーの `+` も可） |
| `Ctrl+-` | `ZoomAction(Out)` | ズームアウト（テンキーの `-` も可） |
| `Ctrl+0` | `ZoomAction(Reset)` | ズーム100%（テンキーの `0` も可） |
| `Ctrl+ホイール` | `ZoomAction(In/Out)` | ホイール方向で増減（端数の扱いは 3.2.7） |
| `Alt+←` | `NavigateBackAction` | 戻る（`WM_SYSKEYDOWN` で届く） |
| `Alt+→` | `NavigateForwardAction` | 進む（同上） |
| `↑` / `↓` | `KeyScrollAction(LineUp/Down)` | 1行スクロール |
| `Home` / `End` | `KeyScrollAction(Home/End)` | 先頭/末尾 |
| `PageUp` / `PageDown` | `KeyScrollAction(PageUp/Down)` | 1ページ |

#### 3.2.6 マウスジェスチャ

| 操作 | 動作 | 実装 |
|:-----|:-----|:-----|
| 右ドラッグ左 | ナビゲーション戻る | `MouseGesture`（30px以上の水平移動） |
| 右ドラッグ右 | ナビゲーション進む | 同上 |
| 右クリック（移動なし） | コンテキストメニュー | `ContextMenu` |
| Xボタン戻る/進む | ナビゲーション戻る/進む | `WM_XBUTTONDOWN` |
| ダブルクリック | 単語選択 | `OnLButtonDblClk` |
| タッチパッド水平スワイプ | ナビゲーション戻る/進む | `SwipeDetector`（`WM_MOUSEHWHEEL`） |

#### 3.2.7 app_controller — イベント→アクション変換

`app_controller` はステートレスなマッパー（namespace + 自由関数）である。宣言は `app_events.h`、実装は `app_controller.cpp` に置く。必要な状態は呼び出し側が持って渡す。Win32 / D2D 依存なし、完全にテスト可能。

```cpp
struct KeyDownEvent {
    int  key;
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
};

struct MouseWheelEvent {
    int      delta;             // 生の WHEEL_DELTA 単位
    bool     ctrl = false;
    PaneZone zone = PaneZone::MdPane;
};

namespace app_controller {
    AppAction HandleKeyDown(const KeyDownEvent& event);
    // zoom_accum は Ctrl+ホイールの delta の端数 (AppState が保持)
    AppAction HandleMouseWheel(const MouseWheelEvent& event, int& zoom_accum);
}
```

`KeyDownEvent` / `MouseWheelEvent` は `app_events.h` で定義されるプラットフォーム非依存の入力イベント。Win32 の `GetKeyState` 確認結果は呼び出し側でバンドルしてから渡す。`HandleMouseWheel` は高分解能ホイールやピンチが `WHEEL_DELTA` 未満の delta を連発しても 1 ノッチ分たまるまでズームせず、向きが反転したら端数を捨てる。

---

### 3.3 AppState — アプリケーション状態の集約

#### 3.3.1 概要

`AppState` はアプリケーションの **すべての状態** を保持する単一構造体。サブグループ別にネストされる。

```cpp
struct DocumentState {
    Document    doc;
    LayoutCache layout_cache;
};

// 進行中の左ボタンドラッグ。マウスキャプチャはこれが None でない間だけ持つ (右ボタンのジェスチャーを除く)。
struct LeftDragState {
    LeftDrag kind = LeftDrag::None;  // TextSelect / Splitter / PaneScrollbar / MdScrollbar / BlockHScroll / SearchInput
    PaneTarget pane     = PaneTarget::File;  // Splitter / PaneScrollbar: 対象のサイドペイン
    float grip_offset   = 0.0f;      // スクロールバー: つまみを掴んだ位置 (つまみ上端から)
    int   block_node    = -1;        // BlockHScroll: 対象ノードと開始時の x・横スクロール量
    float start_x       = 0.0f;
    float start_scroll  = 0.0f;
    int   search_anchor = 0;         // SearchInput: 選択の起点 (キャレット位置)
};

struct ViewState {
    ViewportManager   viewport;
    PaneController    panes;
    ScrollRestoration scroll_restore;
    NavHistory        nav_history;

    // ブロック単位の横スクロール。キーは BlockHScrollGeometry::scroll_key (ノードインデックス。
    // 区切ったコードブロックは並びの先頭)。
    // 永続化しない (ファイルロード/リロードでクリア)。スクロールバー本体は
    // 「ホバー中とドラッグ中のブロック」だけ描画するので、map のサイズは
    // ユーザー操作実績分に限定される。
    std::pmr::unordered_map<int, float> block_scroll_x;
    HoverHighlights hover;             // マウス位置でのホバー強調の全体。描画はここから読む
    int             active_toc_index = -1;
    LeftDragState   drag;

    constexpr int HDragNode() const noexcept;  // 横スクロールをドラッグ中のブロック。無ければ -1
    float GetBlockScrollX(const BlockHScrollGeometry& geom) const noexcept;  // geom.scroll_key のスクロール量
    void  ResetPerNodeTransientState();   // リロード時のクリア (選択・横スクロール・hover.h_block・active_toc_index)
    void  ResetForNewDocument();          // 別文書への切替時。サイドペインのスクロールも先頭へ戻す
};

// ViewState::hover の型 (app_events.h)。既定値はどこにもホバーしていない状態。
struct HoverHighlights {
    TitleBarHitZone  titlebar   = TitleBarHitZone::None;
    SearchBarHitZone search_bar = SearchBarHitZone::None;
    NavDirection     nav        = NavDirection::None;
    OverlayButtonHit overlay_button{};             // コピー / 保存ボタン
    int              h_block = -1;                 // 横スクロールバーを出すブロック
    std::array<int, 2>              side_index{ -1, -1 };  // PaneTarget 順 (File, Toc)
    std::array<PaneHeaderButton, 2> side_button{};
};

struct InteractionState {
    MouseGesture   gesture;
    SwipeDetector  swipe_detector;
    LastHoverPos   last_hover_pos;
    TooltipTarget  tooltip;   // 表示中か表示待ちの対象。ウィンドウ (Tooltip) は App が持つ
    ToastNotifier  toast;
    int            zoom_wheel_accum = 0;
};

struct SearchGroup {
    SearchState         search_state;
    SearchBarController search_bar_ctrl;
};

struct WindowState {
    TitleBar       titlebar;
    bool           is_sizing = false;
    // 移動だけのサイズ変更ループでは終了時のレイアウト処理 (PerformResizeEnd) を省くため。
    bool           size_changed_in_sizing = false;
    bool           window_active = true;
    float          cached_dpi_scale = 1.0f;
    // 直近の WM_SIZE のクライアント領域 (物理 px)。同じ大きさの WM_SIZE を無視するために使う。
    uint32_t       client_width  = 0;
    uint32_t       client_height = 0;
};

struct AppState {
    DocumentState    document;     // ドキュメント + LayoutCache
    ViewState        view;         // ViewportManager + PaneController + ScrollRestoration + NavHistory + ホバー + 左ドラッグ + 横スクロール
    InteractionState interaction;  // MouseGesture + SwipeDetector + TooltipTarget + ToastNotifier 等
    SearchGroup      search;       // SearchState + SearchBarController
    WindowState      window;       // TitleBar + DPI

    // Renderer 所有 Theme への非所有参照。App::Init (RestoreTheme) で設定され、
    // 以降 `renderer_.SetTheme` 経由で in-place 更新されるためポインタは安定。
    // reducer が spacing_above 等を参照するのに使う。
    const Theme*     theme = nullptr;

    FileExplorer     file_explorer;
    ContextMenu      ctx_menu;

    std::pmr::wstring cached_title_text = L"mendo";

    // ペインレイアウトのキャッシュ。 `Invalidate()` で次回再計算を強制する。
    PaneLayoutCache   pane_layout_cache;
};

// AppState 関連のフリー関数
// ViewportManager::CurrentTarget の薄いラッパ (inline)
ScrollTarget SnapshotVisibleTarget(const AppState& state) noexcept;
```

履歴の積み込み（`CurrentNavEntry` / `PushCurrentNavEntry`）は reducer だけが使うので、`reducer.cpp` の無名名前空間に置く。`PushCurrentNavEntry` は `load_path` が現在と別のファイルなら、ロード失敗（`LoadFailedAction`）で取り消す仮の履歴にする。

左ボタンのドラッグ（テキスト選択・スプリッタ・各スクロールバー・ブロック横スクロール・検索入力）は種類を問わず `ViewState::drag` 1 つで表す。ドラッグ中かどうかは `drag.kind != LeftDrag::None` だけで決まり、終了時は `EndLeftDrag`（3.4.1）が `drag` をクリアしてキャプチャを解放させる。ホバー状態も `ViewState::hover` 1 つにまとまっており、App がヒットテストで求めた `PointerHover`（`HoverHighlights` + ツールチップ対象）を `PointerHoverAction` で渡すと Reducer がまとめて反映する。

#### 3.3.2 サブグループ詳細

```mermaid
classDiagram
    class AppState {
        +DocumentState document
        +ViewState view
        +InteractionState interaction
        +SearchGroup search
        +WindowState window
        +FileExplorer file_explorer
        +ContextMenu ctx_menu
    }

    class DocumentState {
        +Document doc
        +LayoutCache layout_cache
    }

    class ViewState {
        +ViewportManager viewport
        +PaneController panes
        +ScrollRestoration scroll_restore
        +NavHistory nav_history
        +pmr::unordered_map~int,float~ block_scroll_x
        +HoverHighlights hover
        +int active_toc_index
        +LeftDragState drag
    }

    class InteractionState {
        +MouseGesture gesture
        +SwipeDetector swipe_detector
        +LastHoverPos last_hover_pos
        +TooltipTarget tooltip
        +ToastNotifier toast
        +int zoom_wheel_accum
    }

    class SearchGroup {
        +SearchState search_state
        +SearchBarController search_bar_ctrl
    }

    class WindowState {
        +TitleBar titlebar
        +bool is_sizing
        +bool size_changed_in_sizing
        +bool window_active
        +float cached_dpi_scale
        +uint32_t client_width
        +uint32_t client_height
    }

    AppState --> DocumentState
    AppState --> ViewState
    AppState --> InteractionState
    AppState --> SearchGroup
    AppState --> WindowState
```

---

### 3.4 Reducer / SideEffect — 状態更新と副作用

#### 3.4.1 Reducer

```cpp
// Reducer: UI イベント由来の状態遷移ハブ。
// 現在の状態を in-place で変更し、実行すべき副作用のリストを返す。
SideEffectList Reduce(AppState& state, const AppAction& action);

// 進行中の左ドラッグを終え、キャプチャを解放させる (右ボタンのジェスチャーが使っている間は持ち続ける)。
void EndLeftDrag(AppState& state, SideEffectList& effects);
```

`Reducer` は `AppState` を **in-place で変更** し、実行すべき副作用の `SideEffectList` を返す。テーマ定数は `AppState::theme`（Renderer 所有 `Theme` への安定ポインタ）経由で参照する。ペインレイアウトは `AppState::pane_layout_cache`（`PaneLayoutCache`）にキャッシュされ、値が古くなったら `pane_layout_cache.Invalidate()` で次回再計算を強制する。

> **Note**: 関数型「純粋関数」スタイル（新 state を返す）ではなく in-place 更新スタイルを採用するのは、`AppState` 内部の `std::pmr::vector<Node>` や `std::pmr::deque` などの大型コンテナを毎回コピーすると性能上のオーバーヘッドが大きいためである。`SideEffectList` のみが新規 vector として生成される。

#### 3.4.2 AppAction

`AppAction` は `std::variant` で 62 種類のアクションを保持する（コマンド系、マウス・ドラッグ系、システム、タイマー、検索の 5 グループ）。ファイル変更通知・画像読み込み完了・パース完了は Reducer を通さず、`App` が直接処理する。

```cpp
using AppAction = std::variant<
    // コマンド系
    NoOpAction, KeyScrollAction, DirectScrollByAction, ScrollPaneAction,
    CopyClipboardAction, CopyFormattedClipboardAction,
    SelectAllAction, SelectWordAction, ClearSelectionAction,
    TogglePaneAction, ZoomAction,
    ReloadFileAction, OpenFileAction, ToggleDarkModeAction,
    NavigateBackAction, NavigateForwardAction,
    ShowHelpAction,
    OpenSearchBarAction, CloseSearchBarAction,
    SearchNextAction, SearchPrevAction,
    // マウス・ドラッグ系
    PointerHoverAction, MouseLeaveAction,
    SplitterDragStartedAction, SplitterDragMovedAction, LeftDragEndedAction,
    SearchInputDragStartedAction, SearchInputDragMovedAction,
    MdScrollbarDragStartedAction, MdScrollbarDragMovedAction,
    PaneScrollbarDragStartedAction, PaneScrollbarDragMovedAction,
    TextSelectionStartedAction, TextSelectionMovedAction, TextSelectionEndedAction,
    RightClickGestureStartedAction, RightClickGestureMovedAction, RightClickGestureCompletedAction,
    FilePaneDirectoryClickedAction, FilePaneRevealCurrentFileAction,
    TocItemClickedAction, NavigateAnchorAction,
    RestoreScrollAfterLoadAction, LoadFailedAction, HWheelAction,
    BlockHScrollDragStartedAction, BlockHScrollDragMovedAction,
    OpenPathAction,
    InvalidateHoverAction,
    // システム
    ResizeAction, DpiChangedAction, ActivateAction,
    EnterSizeMoveAction, ExitSizeMoveAction, CaptureChangedAction,
    // タイマー
    TimerAction,
    // 検索
    SearchTextChangedAction, SearchContinueAction, ToggleCaseSensitiveAction, ToggleHighlightAction,
    SearchSelectionAction, ImeCompositionAction>;
```

`SplitterDragMovedAction` / `PaneScrollbarDragMovedAction` が運ぶのはマウスの `dip_x` / `dip_y` だけである。どのペインのスプリッタ・スクロールバーをドラッグ中かは `ViewState::drag`（`kind` と `pane`）、ウィンドウ幅は `pane_layout_cache` から Reducer が読む。ドラッグの開始は `BeginLeftDrag` が `drag` を設定して `SetCapture` を積む。

`OpenPathAction` は D&D・ファイルペインのクリック・ファイルダイアログに共通で、履歴を積んでからファイルを開く。縦ホイールは `HandleMouseWheel` が返す `DirectScrollByAction` / `ScrollPaneAction` 1 つで、スクロールと同時に `tick` で `SwipeDetector` の軸ロックを更新する（表示中のスワイプオーバーレイは消す）。`SearchContinueAction` は時間で区切った検索を、メッセージループの空き時間に 1 回分進める。

ドラッグの終了は、テキスト選択以外は種類を問わず `LeftDragEndedAction` 1 つである。Reducer が `drag.kind` を見て後処理（スプリッタと Markdown スクロールバーなら `PerformResizeEnd`、ブロック横スクロールなら再描画）を分け、`EndLeftDrag` で終える。テキスト選択の終了は、終点で選択を確定する `TextSelectionEndedAction` が担う。

#### 3.4.3 SideEffect — 単一variant

`SideEffect` は全 effect 型を 1 段の variant に束ねる。論理グループ（Ui / Window / Navigation / Layout / Timer）はコメント区切りで表現し、実行側（`App::Execute`）も同じ並びの単一 visitor で処理する。Effect 型は `namespace effect`（`side_effect.h`）に集約されている。

```cpp
using SideEffect = std::variant<
    // Ui
    effect::InvalidateWindow,
    effect::SetCapture, effect::ReleaseCapture,
    effect::ClipboardWrite, effect::ClipboardWriteHtml,
    effect::ShowTooltip, effect::ClearTooltip,
    effect::ShowContextMenu, effect::Beep,
    // Window
    effect::SearchFocus, effect::SearchUnfocus,
    effect::SetWindowPosition, effect::ApplyThemeChange,
    effect::PerformResizeEnd, effect::PerformSizingUpdate,
    effect::RendererResize, effect::RendererSetDpi,
    // Navigation
    effect::LoadFile, effect::ReloadFile, effect::OpenFileDialog,
    effect::CancelOtherFileLoad,
    // Layout
    effect::BitmapManage, effect::SyncTocActive,
    // Timer
    effect::SetTimer, effect::KillTimer>;

using SideEffectList = std::pmr::vector<SideEffect>;
```

再描画要求は `effect::InvalidateWindow`（ウィンドウ全体の `InvalidateRect(hwnd, nullptr, FALSE)`）に一本化されている。タイトルバーや Markdown ペインだけを部分無効化する effect は持たない。`OnPaint` は `rcPaint` を見ずに毎回全面を描き、`FLIP_DISCARD` のスワップチェーンでは部分描画もできないため、無効化矩形を絞っても描画量は減らないからである。

`PushEffect<T>(effects, e)` は effect を `SideEffectList` に追加するヘルパーで、最初の 1 件で 4 件分を予約する。テストで使う `HasEffect<T>(effects)` は `std::holds_alternative`、`GetEffect<T>(side_effect)` は `std::get_if` の薄いラッパである（`tests/test_helpers.h`）。

#### 3.4.4 App::Execute — 副作用の実行

`App::Dispatch` は `Reduce` が返した `SideEffectList` をそのまま `App::Execute` に渡す。`App::Execute(const SideEffect&)`（`app.cpp`）は `overloaded` の単一 `std::visit` で 1 件ずつ処理し、Win32 API もアプリ内ロジックも App のメンバから直接呼ぶ。間に抽象インターフェースは挟まない。Reducer は Win32 に依存しないので、テストは `Reduce` が返す `SideEffectList` の中身を検証する。

```cpp
void App::Dispatch(const AppAction& action)
{
    // reducer は pane_layout_cache を参照するだけなので、ここで確実に確定させておく。
    GetPaneLayout();
    auto effects = Reduce(state_, action);
    Execute(effects);   // 各要素を Execute(const SideEffect&) へ
}
```

| グループ | effect | 実行内容 |
|:--|:--|:--|
| Ui | `InvalidateWindow` / `SetCapture` / `ReleaseCapture` | `InvalidateRect(hwnd_, nullptr, FALSE)` / `::SetCapture(hwnd_)` / `::ReleaseCapture()` |
| | `ClipboardWrite` / `ClipboardWriteHtml` | `WriteClipboardText` / `WriteClipboardHtml`（`clipboard_util.h`。CF_UNICODETEXT 用の UTF-16 変換もここで行う） |
| | `ShowTooltip` / `ClearTooltip` | `tooltip_.Update` で対象が変われば `TOOLTIP` タイマーを張る / `tooltip_.Clear` でウィンドウを隠して対象を忘れ、タイマーを止める |
| | `ShowContextMenu` | `App::OnContextMenu` |
| | `Beep` | `MessageBeep(MB_OK)`（検索の折り返し） |
| Window | `SearchFocus` / `SearchUnfocus` | `app_msg::SEARCH_FOCUS` / `SEARCH_UNFOCUS` を自ウィンドウへ `PostMessageW`（`Win32Window` が検索ボックスを操作する） |
| | `SetWindowPosition` | `SetWindowPos` |
| | `ApplyThemeChange` | `App::HandleApplyThemeChange` |
| | `PerformResizeEnd` / `PerformSizingUpdate` | `App::OnResizeEnd` / `SyncMaxScroll` と再描画 |
| | `RendererResize` / `RendererSetDpi` | `Renderer::Resize(width, height, live)` / `Renderer::SetDpi` |
| Navigation | `LoadFile` / `ReloadFile` / `OpenFileDialog` / `CancelOtherFileLoad` | `App::LoadMarkdownFile` / `ReloadCurrentFile` / `OpenFileDialog` / `CancelOtherFileLoad` |
| Layout | `BitmapManage` / `SyncTocActive` | `ResourceManager::ScheduleBitmapManage` / `App::SyncTocActiveAndAutoScroll(auto_scroll)` |
| Timer | `SetTimer` / `KillTimer` | `::SetTimer` / `::KillTimer`（ID は `app_timer::Id`） |

---

### 3.5 レンダーステート構築

各レンダーステート（`SidePaneState`, `TitleBarRenderState`, `SearchBarRenderState`, `ToastRenderState`, `GestureRenderState`）は `src/render/render_params.h` に定義される値型で、`App::OnPaint`（`src/app/app.cpp`）が毎フレーム組み立て、`RenderParams` にまとめて `Renderer::Render` に渡す。`SidePaneState` / `TitleBarRenderState` は `App::BuildSidePaneState` / `App::BuildTitleBarRenderState`、`GestureRenderState` は `ResolveGestureOverlay`（`render_params.h`）、`SearchBarRenderState` は `SearchBarController::BuildRenderState()` が作り、`ToastRenderState` はアグリゲート初期化で直接作る。ホバー強調はどれも `ViewState::hover` から詰める。ロード中は `Renderer::DrawLoading` が Markdown ペインの代わりにスピナーを描く。

Markdown ペインの描画コマンドは `Renderer`（具象、`renderer.cpp`）が `CommandGenerator` を直接呼んで作り、自由関数 `ExecuteDrawCommands`（宣言は `draw_command.h`、実装は `command_executor.cpp`）で描く。タイトルバーとサイドペインのコマンドは `Renderer::PrepareChrome` が別リスト（`chrome_cmds_`）に作る。

```mermaid
sequenceDiagram
    participant APP as App::OnPaint
    participant R as Renderer
    participant CG as CommandGenerator
    participant CE as ExecuteDrawCommands
    participant RT as ID2D1DeviceContext

    APP->>APP: 各レンダーステートを構築
    APP->>R: Render(RenderParams)
    R->>R: BeginFrame: PrepareChrome (タイトルバー・サイドペイン) / AcquireFrame
    R->>RT: BeginDraw / Clear
    R->>CE: ExecuteDrawCommands(chrome_cmds_, rt, brushes)
    R->>CG: GenerateMdPane(nodes, cache, md_pane_rect, scroll_y, selection, ...)
    CG-->>R: DrawCommandList
    R->>CE: ExecuteDrawCommands(cmds, rt, brushes)
    R->>R: DrawNavOverlay / DrawGestureTrail / DrawGestureOverlay / DrawToastOverlay / DrawSearchBar / DrawMdScrollbar
    R->>RT: EndDraw
    R->>R: D2DRenderBackend::Present (スワップチェーン)
```

---

### 3.6 Parser — Markdownパーサ

#### 3.6.1 パイプライン

```mermaid
sequenceDiagram
    participant DOC as Document::FromMarkdown
    participant P as ParseMarkdown
    participant MD as md4c (UTF-8)
    participant N as Node[]

    DOC->>P: ParseMarkdown(raw_text_)
    P->>MD: md_parse(text, callbacks)
    loop SAX コールバック
        MD->>P: enter_block(type, detail)
        P->>P: ノード生成 / スタック操作
        MD->>P: enter_span(type, detail)
        P->>P: TextRun 開始記録
        MD->>P: text(type, text)
        P->>P: UTF-8 テキスト蓄積（HTML エンティティ解決）
        MD->>P: leave_span(type, detail)
        P->>P: TextRun 終了記録
        MD->>P: leave_block(type, detail)
        P->>P: ノード確定 → nodes へ push
    end
    P->>P: AppendFootnoteBackrefs (脚注定義に ↩ リンクを付与)
    P->>P: DetectAlerts (BlockQuote → Alert に変換)
    P-->>DOC: ParseResult { NodeStorage(nodes, arena, tables), toc_entries, image_indices, diagram_indices, table_indices, footnote_def_indices, footnote_refs }
    DOC->>DOC: TakeNodes でノード・arena・表を受け取る
```

```cpp
// ノードと、ノードが指すもの (テキスト・runs・リンク URL を置いた arena とテーブル) の組。
struct NodeStorage {
    std::pmr::vector<Node> nodes;
    std::unique_ptr<TextArena> arena;
    std::pmr::vector<std::unique_ptr<NodeTableData>> tables;
};

struct ParseResult {
    // ノードは arena を指すため、一時オブジェクトからノードだけを取る書き方 (ParseMarkdown(md).nodes()) は禁止。
    std::pmr::vector<Node>& nodes() & noexcept;
    void nodes() const&& = delete;
    NodeStorage TakeNodes() noexcept;            // arena ごと取り出す

    std::pmr::vector<TocEntry> toc_entries;    // 見出し (文書順)。そのまま目次になる
    std::pmr::vector<size_t> image_indices;
    std::pmr::vector<size_t> diagram_indices;
    std::pmr::vector<size_t> table_indices;
    std::pmr::vector<int32_t>     footnote_def_indices; // [id - 1] が脚注 id の定義ノード
    std::pmr::vector<FootnoteRef> footnote_refs;        // 脚注参照の位置 (文書順)
private:
    NodeStorage storage_;
};

ParseResult ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token = {});
// ノードは入力テキストを参照するため、一時文字列 (std::string / std::pmr::string の右辺値) を渡す版は delete。
```

md4c は **UTF-8 モード** (`MD_CHAR=char`) で起動するため、入力は UTF-8 byte 列。`FileLoader::LoadFile` が UTF-8 のまま読み込んで `Document::FromMarkdown(text, byte_size, path, stop_token)` に渡す経路と、埋め込みリソース（ヘルプ）から 2 引数版 `Document::FromMarkdown(utf8, path)` で直接渡す経路がある。ノードのテキストは原文と一致すれば原文（`raw_text_`）をそのまま指し、装飾や文字参照で一致しないものだけ `TextArena` にコピーする。リンクの `href`・画像の `src`・コードブロックの言語指定は md4c が属性として渡すので、`AttributeText` が文字参照（`&amp;` 等）を解いてから使う。

パース中のスクラッチ文字列・リンク URL の索引・リスト番号スタックなどはパーサ専用の arena / pool を持たず、既定リソース（`new_delete_resource`）から確保する。ノード配列などは `ReserveForInput` が入力サイズから容量を予約し、ノード数は `kInputBytesPerNode = 128`（実測 246〜283 byte/ノードの文書が予約内に収まる）で見積もる。予約を使い切ったら `GrowNodes` が読み終えた入力の割合から最終的なノード数を見積もって 1 回で広げる。パース後は、ノード配列の容量が要素数の 1.25 倍を超えて余っているときだけ `shrink_to_fit` で詰める。`TextArena` はチャンクを積み足すので予約しない。

#### 3.6.2 対応する Markdown 要素

##### ブロック要素

- **見出し** (`# H1` 〜 `###### H6`) — 6段階、アンカーID自動生成
- **段落** — 通常テキスト
- **コードブロック** — フェンス記法 (```), 言語指定対応
- **図** — ` ```mermaid ` / ` ```math ` のコードブロックと `$$...$$` だけの段落は `NodeType::Diagram` として図で表示
- **引用** (`>`) — ネスト対応、GitHub Alerts記法対応
- **水平線** (`---`, `***`, `___`)
- **順序付きリスト** (`1.`, `2.`, ...)
- **順序なしリスト** (`-`, `*`, `+`)
- **タスクリスト** (`- [ ]`, `- [x]`)
- **テーブル** — GFM拡張、列アライメント対応
- **脚注定義** (`[^label]: 本文`) — 文書末尾に参照順の番号付きリストとして表示

##### インライン要素

- **太字** (`**bold**`)
- **斜体** (`*italic*`)
- **取り消し線** (`~~strike~~`)
- **インラインコード** (`` `code` ``)
- **リンク** (`[text](url)`)
- **画像** (`![alt](path)`) — 非同期読み込み
- **太字+斜体** (`***both***`)
- **脚注参照** (`[^label]`) — `[1]` 形式で脚注定義へのリンク

#### 3.6.3 アンカーID生成（GitHub互換）

```cpp
// 1. 全角英数字 → 半角英数字
// 2. 大文字 → 小文字
// 3. スペース・アンダースコア → ハイフン
// 4. 英数字・ハイフン・CJK文字以外を除去
// 5. 連続ハイフンを単一ハイフンに圧縮
// 6. 先頭・末尾のハイフンを除去
//
// 例:
// "Hello World"     → "hello-world"
// "C++ コード例"    → "c-コード例"
// "第1章: 概要"     → "第1章-概要"
```

本体が使うのは結果を呼び出し側のバッファに書く `GenerateAnchorIdInto(text, slug)`（`core/document_utils.h`）だけ。新しい文字列を返す `GenerateAnchorId` はテストからしか使わないので、テスト用ヘルパー（`tests/document_test_helpers.h`）に置く。

#### 3.6.4 GitHub Alerts記法

引用ブロック先頭の `[!NOTE]` `[!TIP]` `[!IMPORTANT]` `[!WARNING]` `[!CAUTION]` を検出し、マーカーをアイコンとラベルに置き換え、`alert_type` を設定したうえで `set_alert_label_length` でラベル部分の長さを記録する（描画エフェクト適用範囲に使う）。同じ引用グループの後続ノードには `alert_type` だけを伝播する（ラベル長は 0）。`parser_alerts.cpp` の `DetectAlerts(nodes, arena, blockquote_indices)` が専用処理を担当し、`ParseMarkdown` の最後に呼ばれる。書き換えたテキストと runs は `TextArena` に置く。

```cpp
void DetectAlerts(std::pmr::vector<Node>& nodes, TextArena& arena, std::span<const size_t> blockquote_indices);
std::string_view GetAlertLabel(AlertType type) noexcept;
std::string_view GetAlertIcon(AlertType type) noexcept;
```

#### 3.6.5 脚注

md4c の `MD_FLAG_FOOTNOTES`（v0.6.0 で `MD_DIALECT_GITHUB` に追加）を有効にし、GitHub と同じ見た目に組み立てる[^spec-admonitions]。

| md4c のコールバック | mendo での扱い |
|:--|:--|
| `MD_SPAN_FOOTNOTE_REF` | 脚注番号を `[1]` のテキストとして補い、`#user-content-fn-1` へのリンクにする |
| `MD_BLOCK_FOOTNOTE_DEF_SECTION` | 水平線ノードを置き、続く定義をリストの深さ 1 に入れる |
| `MD_BLOCK_FOOTNOTE_DEF` | 番号付きの `ListItem` ノードにする（番号 = 脚注番号） |

- 参照ごとの番号 (`ref_id`) は md4c の値を使わず自前で数える[^spec-refcount]。
- 定義の末尾には参照へ戻る `↩` リンク（2 回目以降の参照は `↩2` …）を付ける。後の定義の中の参照は前の定義を閉じた後に届くため、`AppendFootnoteBackrefs` がパース後にまとめて付ける。
- 定義ノードは文書末尾に並ぶが原文では途中にあるので、ソース位置を記録しない。ソース位置の単調増加は `FindNodeBySourceOffset` の二分探索の前提。
- 参照されない定義は表示せず、定義の無い `[^x]` はテキストのまま残す。

#### 3.6.6 長いテキストの区切り

段落・引用・コードブロック（図を除く）のテキストは、`kTextChunkTarget`（16KB、`parser.h`）前後で区切って同じ種別のノードの並びにする。DirectWrite は 1 段落の行分割が長さに対して二乗に近く遅くなり（空行の無い 4MB のテキストで 28 秒）、1 ノードのままだと並列に計測できず、見えている間は解放もできないため。区切りはパース中に、テキストを足しながら決める（装飾の範囲が区切りをまたがず、続きのノードのソース位置も原文と正確に対応する）。

- 改行（ソフト改行・`<br>`・コードの行末）が目安の `kTextChunkWindow`（2KB）手前を過ぎたら、そこで区切る。区切るのは後に本文が続くときだけで、末尾に空のノードは作らない。
- 1 行が長ければ目安の前後 2KB にある空白の直後で、空白も無ければ目安の長さ（UTF-8 の文字の境目）で区切る。
- ソフト改行の代わりの空白は前のノードの末尾に残す。`\n` は取り除き、続きのノードに `kNewlineBeforeChunk` を立てる。
- 見出し・リスト項目・図・表は区切らない（記号や下線が増え、図は 1 つのコードから描く）。

並びは見た目もコピーも 1 つのブロックとして扱う。

| 使う側 | 扱い |
|:--|:--|
| 間隔 | `GetSpacingAbove` / `GetSpacingBelow` は継ぎ目で 0 を返す |
| コードブロックの背景 | `NodeBoxPadTop` / `NodeBoxPadBottom` は継ぎ目ではみ出さず、`GenCodeBlockBg` は継ぎ目の側の角を丸めない |
| コピーボタン | 先頭のノードにだけ置き、`ClipboardManager::CopyCodeBlock` が並び全体をコピーする |
| 横スクロール | `GetBlockHScrollGeometry` が並びの先頭を `scroll_key` として返し、幅は並びのうち計測済みの部分の最大にする |
| 構文の色分け | `TokenizeCodeNode` が前のノードからの続きとしてトークン化する（ブロックコメントの途中から始まっても色が崩れない） |
| コピー | `ExtractSelectedText` は継ぎ目に元の改行だけを戻し、`ExtractSelectedTextAsHtml` は並びを 1 つの `<p>` / `<pre>` にまとめる |

[^spec-admonitions]: 同じく追加された `MD_FLAG_ADMONITIONS` は使わない。ネストした引用でも Alert として扱うなど、`DetectAlerts`（GitHub 準拠）と挙動が違うため。
[^spec-refcount]: md4c はテーブル行の判定でもインラインを解析するため、表の中の参照が二重に数えられ、`ref_id` / `ref_count` が実際の参照数とずれる。

---

### 3.7 LayoutEngine / LayoutService — テキスト計測

#### 3.7.1 概要

`LayoutEngine` は `ITextMeasurer` インターフェース経由で各ノードを計測し、`LayoutCache` に格納する。`LayoutService` が `LayoutEngine` と `ViewportManager` を結合し、スクロール target 管理付きのレイアウト操作を提供する。

```cpp
// 計測とライフサイクルを 1 つのインターフェースにまとめる (src/layout/text_measurer.h)。
// 計測は const で、複数の worker から同じインスタンスを呼んでよい。
// lifecycle 系 (RecreateFormats / UpdateTheme) は UI スレッドからのみ呼ぶ。
class ITextMeasurer {
public:
    virtual ~ITextMeasurer() = default;

    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;

    // 並列計測では worker ごとに別ノード・別 entry を渡す。
    // ビットマップを描くノード (IsBitmapDrawNode) は渡さない。
    virtual void MeasureNode(const Node& node, NodeLayoutEntry& entry, float max_width,
                             MeasureViewportRange viewport = {}) const = 0;
    // 幅不変のまま行単位 evict されたテーブルの、viewport 内の evict 行だけセルを再生成する。
    virtual TableRestoreResult RestoreEvictedTableRows(const Node& node, NodeLayoutEntry& entry,
                                                       float max_width, MeasureViewportRange viewport) const;
};

// 計測結果を entry に確定させる。画像・図ノードは BitmapNodeHeight で高さを決める。
void MeasureEntry(const ITextMeasurer& measurer, const Node& node, NodeLayoutEntry& entry,
                  float node_width, MeasureViewportRange viewport, float entry_top,
                  const DiagramEntry* bitmap, const Theme& theme);

class LayoutEngine {
public:
    static constexpr float kWidthChangeThreshold = 2.0f;

    bool Init(ITextMeasurer* measurer, const Theme& theme);
    // nullptr なら呼び出しスレッドで直列に計測する。
    void SetLayoutScheduler(TaskScheduler* scheduler) noexcept;
    void UpdateTheme(const Theme& theme) noexcept;
    bool RecreateFormats();

    void Reestimate(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width);
    bool EnsureVisibleLayout(const std::pmr::vector<Node>& nodes, LayoutCache& cache, float viewport_width,
                             float viewport_top, float viewport_bottom);
    bool ProcessDirtyBatch(const std::pmr::vector<Node>& nodes, LayoutCache& cache,
                           float viewport_width, int batch_size,
                           layout::ViewportClip clip = {});
};

class LayoutService {
public:
    LayoutService(LayoutEngine& engine, ViewportManager& viewport) noexcept;

    struct ViewportLimit {
        float height         = 0.0f;
        float buffer_screens = 0.0f;
    };

    void ViewportLayout(const Document& doc, LayoutCache& cache, float width, float height);
    bool ProcessDirtyBatch(const Document& doc, LayoutCache& cache, float width, int batch_size,
                           ViewportLimit viewport);
    bool EnsureVisibleLayout(const Document& doc, LayoutCache& cache, float width, float height);
    // 画面外の node も計測する。
    bool EnsureNodeLayout(const Document& doc, LayoutCache& cache, float width, int node);
    void RecomputeAfterDiagram(const Document& doc, LayoutCache& cache, const Theme& theme,
                               layout::HeightChangeRange changed) noexcept;
};
```

本番実装は `DWriteTextMeasurer`（DirectWrite を使用）。書式は `SetFactory` の後に `RecreateFormats` で作る。テスト時はモックに差し替えられる。`RecomputeYPositions` / `EstimateNodeHeight` / `EstimateNodeHeights` / `MakeEstimatedLayoutCache` などのフリー関数（`layout_computer.h` の `layout` 名前空間）で Y 座標再計算と高さ推定（DirectWrite 計測前の暫定値）を提供する。リロードでは `CarryOverUnchanged` が先頭・末尾から中身の一致する範囲を数え、その計測結果を旧キャッシュから区間ごとに引き継ぐ（`LayoutCache::TakeEntries`）。グローバル名前空間への `using` エイリアスは置かず、使う側の .cpp が必要な名前だけを `using layout::X;` で取り込む。

計測用の scheduler は Mermaid 等の共有 scheduler とは別に立て、ワーカー数は `LayoutWorkerCount`（コア数 − 1 を 2〜8 本に収める。16 本は 8 本より遅い）で決める。`Renderer::SetLayoutScheduler` は設定時に `DWriteTextMeasurer::WarmUp` で本文・コード・見出し 1 つの書式の小さなレイアウトを各ワーカーで作って捨て、最初の計測がフォントの読み込みとフォールバックの解決を待たないようにする。

計測後の確定処理は `MeasureEntry` 1 か所にまとめている。実測値を `cached_width` / `cached_height` に残し、計測失敗回数 `measure_failures` を 0 に戻す。ビットマップを描くノード（画像・図、`IsBitmapDrawNode`）は `ITextMeasurer` に渡さず、`layout::BitmapNodeHeight` が `DiagramEntry` の寸法から表示高さを決める（寸法が届くまでは現在値、それも無ければプレースホルダーの高さ）。

幅が `kWidthChangeThreshold`（2 DIP）を超えて変わるか書式が作り直されると、`Reestimate` が全ノードを dirty にして推定値で Y を組み直す（DirectWrite は呼ばない）。推定に使った本文幅とズームは `LayoutCache::EstimateBasis` に残り、ワーカーが表示と同じ幅で推定したキャッシュは UI スレッドで推定し直さない（幅を知らない起動時の先読みだけ推定し直す）。`LayoutService::ViewportLayout` は `Reestimate` の後に `ViewportManager::ApplyScrollTarget` で見ていた位置を当て直してから可視範囲を計測する。ズームや幅変更で推定し直すと Y がずれるため、古い `scroll_y` のまま計測すると見えない範囲を 1 画面分ムダに計測してしまう。

ダーティノードの計測は `LayoutEngine::MeasurePass`（private）に一本化されている。`MeasurePass` は範囲内の dirty を最大 `max_nodes` 件（0 = 無制限）集めて `layout::MeasureIndicesParallel`（`layout.cpp`）で計測し、幅不変のまま行単位で evict されたテーブルは `RestoreEvictedTableRows` で戻し、高さが変わった区間だけ `RecomputeYPositions` で組み直す。`ProcessDirtyBatch` は `ViewportClip`（可視範囲 ± `buffer_screens` 画面）内の dirty を `batch_size` 件まで計測する 1 回の `MeasurePass` で、時間予算は持たない（worker 側に中断点が無いため）。`EnsureVisibleLayout` は計測で高さが変わると範囲に入るノードも変わるため、範囲内の dirty が無くなるまで `MeasurePass` を繰り返す（失敗し続けるノードに備えて最大 8 回）。`MeasureIndicesParallel` は `ParallelFor` に 1 ノード単位で配り、1 画面分（数十ノード）の dirty でも全 worker に行き渡らせる。scheduler が null なら同じ関数が呼び出しスレッドで直列に計測する。例外はノード単位で捕まえ、失敗回数が `LayoutCache::kMaxMeasureAttempts` に達したノードは dirty を下ろして諦める。CodeBlock のトークン化と、リンク色・アラートラベル色の描画効果（`BrushEffect`）・インラインコード背景の付与は worker が担当ノードの `NodeLayoutEntry` へ直接書き込み、UI スレッドでの集約は行わない（`Tokenize` 完了後に代入するので、例外時に途中のトークンが残らない）。`ViewportClip` の型は `layout.h` に置く。`ProcessDirtyBatch` の `clip` を省略すると可視範囲で絞らず全 dirty を対象にする。

計測待ちの dirty が残っているかは `LayoutCache::HasDirtyNodes` が持ち、キャッシュと一緒に動く。`Reestimate` / `EnsureVisibleLayout` / `ProcessDirtyBatch` が更新し、`EnsureVisibleLayout` は走査しない範囲外に dirty が残っていると保守的に仮定する。

遅延レイアウト（`App::OnDeferredLayout`）はメッセージループの空き時間（`App::RunIdleWork`）に、32 ノードずつ 4ms まで、入力が来たら途中でも譲って進める。`ResourceManager::EVICT_BUFFER_SCREENS`（2 画面）を `buffer_screens` に渡すため、画面外の計測は前後 2 画面までに限られる。範囲外へジャンプした場合は `EnsureVisibleLayout` が同期計測で拾う。

#### 3.7.2 レイアウト計算フロー

```mermaid
flowchart TB
    START[LayoutService 呼出し] --> MODE{モード}
    MODE -->|ViewportLayout| REEST[Reestimate<br>推定値で Y 再構築]
    MODE -->|EnsureVisibleLayout / EnsureNodeLayout| ENSURE[可視領域保証<br>dirty が無くなるまで繰り返す]
    MODE -->|ProcessDirtyBatch| BATCH[ダーティバッチ処理<br>ViewportClip 内を batch_size 件]
    MODE -->|RecomputeAfterDiagram| YPOS

    REEST --> TARGET[ApplyScrollTarget<br>見ていた位置を当て直す]
    TARGET --> ENSURE
    ENSURE --> PASS[MeasurePass]
    BATCH --> PASS
    PASS --> MEASURE[MeasureIndicesParallel<br>MeasureEntry / ITextMeasurer::MeasureNode]

    MEASURE --> CACHE[LayoutCache 格納<br>height, text_layout]
    CACHE --> YPOS[RecomputeYPositions]
    YPOS --> APPLY[ViewportManager::ApplyScrollTarget]
    APPLY --> DONE[完了]
```

#### 3.7.3 テキストフォーマット

| 用途 | フォントファミリ | 既定サイズ (DIP) |
|:-----|:----------------|:-----------------|
| H1 | Yu Gothic UI | 32 |
| H2 | Yu Gothic UI | 26 |
| H3 | Yu Gothic UI | 22 |
| H4 | Yu Gothic UI | 18 |
| H5 | Yu Gothic UI | 16 |
| H6 | Yu Gothic UI | 14 |
| 本文 | Yu Gothic UI | 16 |
| コード | Consolas | 14 |

> **Note**: すべてのフォントサイズはズーム倍率 (`Theme::zoom`) で乗算される。

---

### 3.8 Renderer — Direct2D 描画

#### 3.8.1 Command パターン

```mermaid
sequenceDiagram
    participant APP as App::OnPaint
    participant R as Renderer
    participant CG as CommandGenerator
    participant CE as ExecuteDrawCommands
    participant BE as D2DRenderBackend
    participant RT as ID2D1DeviceContext

    APP->>R: Render(params)
    R->>R: PrepareChrome (タイトルバー・サイドペイン)
    R->>BE: AcquireFrame (フレーム待ち)
    R->>RT: BeginDraw / Clear
    R->>CE: ExecuteDrawCommands(chrome_cmds_, rt, &brushes_)
    R->>CG: GenerateMdPane(nodes, cache, selection, ...)
    CG-->>R: DrawCommandList
    R->>CE: ExecuteDrawCommands(cmds, rt, &brushes_)
    CE->>RT: 各描画コマンド実行
    R->>RT: オーバーレイ等を直接描画
    R->>RT: EndDraw
    R->>BE: Present (スワップチェーン)
```

#### 3.8.2 DrawCommand 型

```cpp
using DrawCommand = std::variant<
    FillRectCmd,
    FillRoundedRectCmd,
    DrawLineCmd,
    DrawTextLayoutCmd,
    DrawBitmapCmd,
    FillEllipseCmd,
    DrawEllipseCmd,
    PushClipCmd,
    PopClipCmd,
    SetTransformCmd
>;

using DrawCommandList = std::pmr::vector<DrawCommand>;

using FixedBrushArray = std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, std::to_underlying(BrushId::Count)>;

// brushes が null、または該当ブラシが未作成のコマンドは描かない (実装は command_executor.cpp)。
void ExecuteDrawCommands(const DrawCommandList& cmds, ID2D1RenderTarget* rt, const FixedBrushArray* brushes);
```

`DrawCommandList` は `DrawCommand` variant の単一 `std::pmr::vector` である。毎フレーム構築して 1 回読むだけのリストなので、型別ベクタや発行順テーブルは持たない。`CommandGenerator` は毎フレーム `cmds_` を clear して容量を使い回し、自由関数 `ExecuteDrawCommands` は先頭から `std::visit` で順に実行する。

各コマンドは値型で、`IDWriteTextLayout*` や `ID2D1Bitmap*` などのポインタは **非所有**（ライフタイムは `LayoutCache` / `TextLayoutCache` / `Renderer` が管理）。色はすべて `BrushId`（`brush_id.h`）で固定ブラシを指し、`Renderer` が保持する `FixedBrushArray` を `ExecuteDrawCommands` が配列ルックアップで引く。半透明は `opacity` で掛ける。アラートの背景も固定ブラシ（`AlertNoteBg` 〜 `AlertCautionBg`）、オーバーレイボタンは白黒の固定ブラシ（`OverlayBrushId`）に不透明度を掛けて描くので、コマンドが任意の色を持つことはない。ブラシが未作成のコマンドは描かない。

`DrawTextLayoutCmd` はすべて `ID2D1RenderTarget::DrawTextLayout` ではなく、自前の `IDWriteTextRenderer`（`VisibleRunRenderer`）を渡した `IDWriteTextLayout::Draw` で描く。テキストレイアウトに付く描画効果は計測ワーカーが `SetDrawingEffect` で付けた `BrushEffect`（リンク色・アラートラベル色）で、デバイスに依存しない印なので、デバイスを作り直しても付け直さずに済む。描画時に `BrushEffect::IdOf` で固定ブラシへ置き換える。

レイアウトが可視範囲より高い場合（巨大なコードブロックなど）、`CommandGenerator` がコマンドに可視の縦範囲（`cull_top` / `cull_bottom`、クリップ範囲と同じ）を載せ、`VisibleRunRenderer` は範囲外のグリフラン・下線・取り消し線を飛ばす（D2D の `DrawTextLayout` は画面外も含め全ランを処理し、1 万行のコードブロックで 1 フレーム約 20ms かかる）。低いレイアウトでは範囲を既定の無限のままにする。

コードブロックの構文色も描画時に付ける。`DrawTextLayoutCmd::syntax_tokens`（同じ `NodeLayoutEntry` が持つ UTF-16 位置のトークン、非所有）があれば、`VisibleRunRenderer` はグリフランを `clusterMap` でトークン境界に分け、種別ごとの固定ブラシ（`SyntaxKeyword` 〜 `SyntaxDeleted`）で描く。右から左のランも分割し、境界がクラスタ（合字等）の途中にあればそのクラスタは後ろのトークンの色になる。`SetDrawingEffect` は 1 回ごとに既存の範囲数に比例して重くなり、巨大なコードブロックで O(n²) になるため構文色には使わない。トークンは計測ワーカーが text_layout と一緒に作り、トークン間の空白だけの隙間は直前のトークンに含めて同種をまとめ、グリフランの分割を減らしておく。

#### 3.8.3 D2DRenderBackend

D3D11 デバイス + DXGI フリップモデルのスワップチェーン上の `ID2D1DeviceContext`、D2D / DirectWrite / WIC ファクトリ、DPI を保持する（`src/render/d2d_render_backend.h`）。抽象インターフェースは持たず、`Renderer` がメンバとして持つ。ブラシとテキストフォーマットは `Renderer` 側で管理する。

```cpp
class D2DRenderBackend final {
public:
    // デバイスとスワップチェーンは作らない (CreateDevice で作る)。
    bool Init(HWND hwnd);
    bool CreateDevice();
    void Resize(UINT width, UINT height, bool live = false) noexcept;
    void SetDpi(float dpi) noexcept;
    float GetDpi() const noexcept;
    D2D1_SIZE_F GetSize() const noexcept;

    void AcquireFrame() noexcept;
    static constexpr DWORD kFrameWaitTimeoutMs = 1000;
    HANDLE FrameWaitHandle() const noexcept;
    void MarkFrameAcquired() noexcept;
    void Present() noexcept;
    void Trim() noexcept;
    bool IsDeviceLost() const noexcept;

    ID2D1Factory* GetD2DFactory() const noexcept;
    ID2D1DeviceContext* GetRenderTarget() const noexcept;
    IDWriteFactory* GetDWriteFactory() const noexcept;
    IWICImagingFactory* GetWICFactory() const noexcept;
};
```

- **デバイス作成**: D3D デバイスの作成（数十 ms）は `PrefetchD3DDevice` で wWinMain の冒頭から起動処理と重ね、`Renderer::CreateDevice` → `D2DRenderBackend::CreateDevice` で受け取る。作り直しに失敗している間はロスト扱いのままにする。
- **フレーム待ち**: Frame Latency Waitable Object のシグナルで次のフレームを描いてよいかを判断する。メッセージループが `FrameWaitHandle` を待って `MarkFrameAcquired` で記録し、取れていなければ `Renderer::BeginFrame` から `AcquireFrame` が最大 `kFrameWaitTimeoutMs`（1 秒）待つ。取れたら Present するまで取れた扱いのままにし、表示されなかったフレームの後で待ち直さない。
- **リサイズ**: ライブリサイズ中（`live`）は `ResizeBuffers` をせず、モニタ全体の大きさのバッファを使い回して表示範囲（source size）だけを変える。
- **デバイスロスト**: `Present` が `DXGI_ERROR_DEVICE_REMOVED` / `RESET` / `HUNG` / `DRIVER_INTERNAL_ERROR` を返すと `IsDeviceLost` が true になり、`Renderer` が次フレーム冒頭の `HandleDeviceLost` で作り直す。
- **最小化**: `Trim` で D2D の内部キャッシュとドライバ側の一時確保を手放す（`Renderer::TrimForMinimize`）。

#### 3.8.4 Renderer ファイル構成

| ファイル | 担当 |
|:---------|:-----|
| `renderer.cpp` | フレームの開始・終了（`BeginFrame` / `CheckEndDraw`）とデバイスロスト復旧、Markdown ペイン描画統括、タイトルバーのコマンド生成、オーバーレイ（ナビボタン・ジェスチャ・トースト）、ローディング |
| `renderer_resources.cpp` | 固定ブラシ・ペイン用フォーマット管理、フォント解決、アプリアイコン |
| `renderer_pane.cpp` | ファイル/TOC ペインのコマンド生成、Markdown ペインのスクロールバー |
| `renderer_search.cpp` | 検索バー描画、検索入力欄のヒットテスト |

#### 3.8.5 描画順（`Renderer::Render`）

1. 背景クリア
2. カスタムタイトルバー・ファイルペイン / TOCペイン・スプリッタ（`PrepareChrome` が `chrome_cmds_` に積み、`ExecuteDrawCommands` で実行）
3. Markdownコンテンツ（`GenerateMdPane` → `ExecuteDrawCommands`）
   - 引用ブロックのバー・背景（`GenBlockQuoteGroupDecorations`、グループ単位でノードより先に描く）
   - 以下、ノードごとに文書順（`GenerateNode`）
   - コードブロック背景 → テキスト（構文色は描画時に付ける）→ コピーボタン
   - テーブル（行背景・グリッド線・セル）
   - 水平線 / 見出し下線 / リスト記号 / タスクチェックボックス
   - 画像・Mermaidダイアグラムビットマップ → 保存 / コピーボタン
   - テキストを持つノードはインラインコード背景 → 検索ハイライト → テキスト選択ハイライト → テキストの順
   - テーブル・コードブロックの横スクロールバー（ホバー中・ドラッグ中のブロックのみ）
4. ナビゲーションボタン（戻る/進むオーバーレイ、`DrawNavOverlay`）
5. マウスジェスチャトレイル（`DrawGestureTrail`）
6. スワイプオーバーレイ（`DrawGestureOverlay`）
7. トースト通知（`DrawToastOverlay`）
8. 検索バー（下部ドッキング、`DrawSearchBar`）
9. Markdownペインのスクロールバー（`DrawMdScrollbar`）

オーバーレイボタン（コピー / 保存）はホバーの有無にかかわらず描き、ホバー中だけ濃くする。矩形は `layout::ForEachOverlayButton` が列挙し、描画とヒットテストで同じ矩形を使う。

ローディングアニメーション（`DrawLoading`）は同じ `BeginFrame`（タイトルバー・サイドペイン）の後にスピナーと、`Render` の 6・7 と共通の `DrawTransientOverlays`（ジェスチャオーバーレイ・トースト）だけを描く。コンテキストメニューはこのシーケンスとは別経路で描画される。

タイトルバーとサイドペインはデバイスに依存しないコマンド列なので、デバイスの作成前に `PrepareChrome` を呼んで最初のフレームの文字レイアウトを先に作れる（`App::CreateRenderDevice`）。起動直後には出ないオーバーレイ（トースト・検索バー等）は即時描画のままにしている。

描画コストを抑えるためのキャッシュ:

- **サイドペインの項目テキスト**: 可視項目は数十個なのでオフスクリーンに溜めず毎フレームコマンドに積む。左寄せ NO_WRAP の項目名・見出しはレイアウト幅を固定して `TextLayoutCache` に載せ、はみ出しはクリップで切るため、スプリッタのドラッグ中もレイアウトを作り直さない。
- **短い文字列**: `DrawText` は呼び出しごとに内部でテキストレイアウトを作るため使わない。アイコン・リスト番号・プレースホルダ・タイトル・ナビボタン・ジェスチャー・トースト・検索バーのラベルや件数は、(書式, 文字列, 箱の大きさ) ごとに `TextLayoutCache`（`text_layouts_`、`CommandGenerator` と共有）に作り置き、文字列と書式が同じ間は使い回す。箱を指定しなければ中央揃え書式のレイアウトを幅/高さ 0 で作って矩形中心に置き、固定サイズの箱が必要なものは箱サイズで作って左上に置く（`TextLayoutCache::Origin`）。コマンド列に積むものは `EmitChromeText` / `EmitText`、即時描画のものは `DrawCenteredText` を使う。項目数が `kMaxEntries`（512）を超えたら `BeginFrame` で直前のフレームに使わなかったものを捨て、書式を作り直す `RecreatePaneFormats` でまとめて破棄する。
- **検索入力欄**: IME の合成中テキストを挿入した表示テキストと幅が前フレームと同じ間は `search_input_` のレイアウトを使い回す。

---

### 3.9 Theme — テーマシステム

#### 3.9.1 テーマ構造

```cpp
struct Theme {
    // カラーパレット
    D2D1_COLOR_F bg_color, text_color, heading_color;
    D2D1_COLOR_F code_bg_color, code_text_color;
    D2D1_COLOR_F link_color, hr_color;
    D2D1_COLOR_F blockquote_bar_color, blockquote_text_color;

    // GitHub Alerts (Note, Tip, Important, Warning, Caution の5種)
    D2D1_COLOR_F alert_color[ALERT_TYPE_COUNT];     // バー・ラベル色
    D2D1_COLOR_F alert_bg_color[ALERT_TYPE_COUNT];  // 背景色

    // シンタックスハイライト
    D2D1_COLOR_F syntax_keyword, syntax_type;
    D2D1_COLOR_F syntax_string, syntax_number;
    D2D1_COLOR_F syntax_comment, syntax_preprocessor;
    D2D1_COLOR_F syntax_function;
    D2D1_COLOR_F syntax_inserted, syntax_deleted;

    // タイトルバー
    D2D1_COLOR_F titlebar_bg_color, titlebar_text_color;
    D2D1_COLOR_F titlebar_button_hover_color, titlebar_button_active_color;

    // ペイン
    D2D1_COLOR_F pane_bg_color, splitter_color;
    D2D1_COLOR_F pane_item_hover_color, pane_item_active_color;

    // 検索バー
    D2D1_COLOR_F search_bar_bg_color, search_bar_border_color;
    D2D1_COLOR_F search_input_bg_color, search_input_text_color;
    D2D1_COLOR_F search_highlight_color;
    D2D1_COLOR_F search_highlight_current_color;
    D2D1_COLOR_F search_no_match_bg_color;

    // フォント
    std::wstring font_family;     // "Yu Gothic UI"
    std::wstring monospace_font;  // "Consolas"
    std::wstring icon_font;       // "Segoe Fluent Icons"

    // フォントサイズ (DIP)
    float font_size_body;
    float font_size_h[6];
    float font_size_code;

    // スペーシング
    float margin_left, margin_right, margin_top, margin_bottom;
    float paragraph_spacing, list_item_spacing;
    float heading_spacing_above;
    float heading_spacing_below;       // h3〜h6 の下マージン
    float heading_spacing_below_h1h2;  // h1/h2 の下マージン (下線+次行余白を確保)
    float code_block_spacing_above, code_block_padding;
    float indent_width, blockquote_bar_width;
    float list_bullet_offset, list_bullet_radius, hr_thickness, h2_underline_thickness;

    // ペインレイアウト
    float pane_item_height;
    float pane_header_height;
    float splitter_width;
    float pane_font_size;

    // ズーム (1.0 = 100%)
    float zoom = 1.0f;

    constexpr float ContentWidth(float viewport_width) const noexcept;
    float GetHeadingSize(int level) const noexcept;
    constexpr float GetHeadingUnderlineThickness(int level) const noexcept;
    constexpr bool IsDark() const noexcept;
    void ApplyZoom(float new_zoom) noexcept;
};
```

Reducer は `AppState::theme`（`Renderer` 所有 `Theme` への非所有ポインタ）経由で寸法を直接参照する。

#### 3.9.2 ThemeService — テーマ管理サービス

```cpp
class ThemeService {
public:
    explicit ThemeService(ConfigService& config) noexcept;
    constexpr bool IsDarkMode() const noexcept;
    const Theme& BaseTheme() const;
    Theme CreateTheme(int zoom_index) const;
    bool ToggleDarkMode() noexcept;
    void SaveDarkMode();
    void LoadDarkMode();
    void SaveZoomLevel(int zoom_index);
    int  LoadZoomIndex() const;
};
```

起動時は `App::Init` が `Renderer` より先に `LoadDarkMode` / `LoadZoomIndex` を読み、保存済みのテーマとズームで作った `Theme` を `Renderer::Init(hwnd, theme)` に渡す。書式とブラシは 1 回の生成で済み、ライトテーマで作ってから作り直す二重生成を避ける。

ライト/ダークの切替はフォントと寸法が共通なので色だけの変更として扱う。`Renderer::SetTheme` は書式やテキストレイアウトを作り直さず、固定ブラシの色を `SetColor` で差し替えるだけである（ズーム変更は `ApplyZoomFromBase` の別経路）。テキストレイアウトに付けた描画効果はブラシ ID の印（`BrushEffect`）なので付け直しも要らない。Reducer 側（`ReduceToggleDarkMode`）もテキストレイアウトと描画エフェクトを維持し、テーマ色を焼き込んだ Mermaid ビットマップだけを `LayoutCache::InvalidateDiagramBitmaps` で破棄する。

#### 3.9.3 ズームシステム

17段階の離散的なズームレベル（`theme.h::ZOOM_STEPS`）。

| インデックス | 倍率 | インデックス | 倍率 |
|:------------|:------|:------------|:------|
| 0 | 0.25x | 9 | 1.25x |
| 1 | 0.33x | 10 | 1.50x |
| 2 | 0.50x | 11 | 1.75x |
| 3 | 0.67x | 12 | 2.00x |
| 4 | 0.75x | 13 | 2.50x |
| 5 | 0.80x | 14 | 3.00x |
| 6 | 0.90x | 15 | 4.00x |
| 7 | **1.00x** (default) | 16 | 5.00x |
| 8 | 1.10x | | |

`ZOOM_DEFAULT_INDEX = 7`、`ZOOM_STEP_COUNT = 17`。

ズーム変更では `ReduceZoom` が可視先頭のノードとノード内オフセット（ズーム比でスケール）を scroll target に置いて全ノードのレイアウトを無効化し、`Renderer::ApplyZoomFromBase` が zoom=1.0 の基準テーマから作り直して倍率を掛ける（累積適用による誤差を避ける）。続く `App::RelayoutViewport(RelayoutResources::Diagrams)` の `LayoutService::ViewportLayout` が推定し直した Y で見ていた位置を当て直してから可視範囲を計測する（3.7.1）。

---

### 3.10 Syntax — シンタックスハイライト

#### 3.10.1 対応言語

```mermaid
graph LR
    subgraph 対応言語
        CPP[C / C++]
        PY[Python]
        JS[JavaScript]
        TS[TypeScript]
        GO[Go]
        RS[Rust]
        BASH[Bash]
        PS[PowerShell]
        CMD[Cmd]
        JSON[JSON / JSONC / JSON5]
        JVM[Java / Kotlin]
        CS[C#]
        SW[Swift]
        PHP[PHP]
        RB[Ruby]
        SQL[SQL]
        CONF[YAML / TOML / INI]
        WEB[HTML / XML / CSS / SCSS]
        DIFF[diff]
        BUILD[Dockerfile / Makefile]
        ZIG[Zig]
        HS[Haskell]
        LISP[Clojure / Emacs Lisp / Common Lisp / Scheme・Racket]
        LUA[Lua]
        VIM[Vim script / Vim9 script]
        MER[Mermaid]
    end

    subgraph トークンタイプ
        KW[Keyword]
        TY[Type]
        ST[String]
        NU[Number]
        CM[Comment]
        PP[Preprocessor]
        FN[Function]
        IN[Inserted]
        DE[Deleted]
    end

    CPP --> KW & TY & ST & NU & CM & PP & FN
    PY --> KW & TY & ST & NU & CM
    JS --> KW & TY & ST & NU & CM & FN
    TS --> KW & TY & ST & NU & CM & FN
    GO --> KW & TY & ST & NU & CM & FN
    RS --> KW & TY & ST & NU & CM & FN
    BASH --> KW & ST & NU & CM
    PS --> KW & TY & ST & NU & CM
    CMD --> KW & ST & CM
    JSON --> KW & ST & NU
    JVM --> KW & TY & ST & NU & CM & FN
    CS --> KW & TY & ST & NU & CM & PP & FN
    SW --> KW & TY & ST & NU & CM & PP & FN
    PHP --> KW & TY & ST & NU & CM & FN
    RB --> KW & TY & ST & NU & CM & FN
    SQL --> KW & TY & ST & NU & CM & FN
    CONF --> KW & TY & ST & NU & CM & PP
    WEB --> KW & TY & ST & NU & CM & PP & FN
    DIFF --> KW & TY & CM & IN & DE
    BUILD --> KW & TY & ST & NU & CM & FN
    ZIG --> KW & TY & ST & NU & CM & FN
    HS --> KW & TY & ST & NU & CM & PP & FN
    LISP --> KW & TY & ST & NU & CM & PP & FN
    LUA --> KW & TY & ST & NU & CM & FN
    VIM --> KW & TY & ST & NU & CM & PP & FN
```

`SyntaxTokenType` には `Plain` も定義されているが、トークナイザは `Plain` トークンを出力しない。描画も HTML 化もトークン間の隙間を既定色のテキストとして扱うため、出力しても同じ結果になるうえトークン数の大半を占めるだけだからである。トークンは計測で text_layout を作るときに一緒に作り、UTF-16 位置に直し、空白だけの隙間を直前のトークンに含めて隣り合う同種をまとめてから `NodeLayoutEntry` に置き、画面外で layout を捨てるときに一緒に捨てる。描画は `VisibleRunRenderer`（`command_executor.cpp`）がグリフランをトークン境界で分けて種別の色で描く（3.8.2）。書式付きコピーは表示の有無に関係なくその場でトークン化する。

C++ の生文字列 `R"delim(...)delim"` は、`R"` の直後から規格どおり最大 16 文字・空白/括弧/`\` を含まない区切りの範囲だけ `(` を探す。上限なしに探すと `(` の無い `R"` が続く文書で末尾までの走査を繰り返し、O(n²) になるためである。

JSON 系は ` ```json ` / ` ```jsonc ` / ` ```json5 ` の言語タグで認識される。`true` / `false` / `null` をキーワードとしてハイライトする。

Java / C# / Kotlin / Swift / PHP / Ruby / SQL は C++ 等と同じ汎用 lexer (`TokenizeRange<LexerConfig>`) に設定フラグを足して扱う。C# の逐語的文字列 `@"..."`、SQL の `--` コメント、PHP 8 の属性 `#[...]` と `$` 付きの変数名（`$class` をキーワードにしない）、Ruby の `=begin` / `=end` がそのためのフラグである。トリプルクォート文字列は `TripleQuote` で、`\` をエスケープにする形（Python / Java / Swift）、しない形（Kotlin の raw string）、開きと同じ数以上の `"` で閉じる形（C# 11 の raw string literal）を読み分ける。数値リテラルは `'` に加えて `_` も桁区切りとして受け付ける。

YAML / TOML / INI / HTML / XML / CSS / SCSS / diff / Dockerfile / Makefile は、キーと値・タグと属性・セレクタと宣言・行頭の記号といった行や宣言の構造で色が決まるため専用 lexer を持つ。設定ファイル系の値はスカラー全体で数値・定数を判定し、`1.2.3` や `node:18-alpine` の一部だけを数値として色付けしない。埋め込み言語は汎用 lexer を範囲指定で呼んで色付けする（HTML の `<script>` / `<style>` を JS / CSS、Dockerfile の命令引数と Makefile のレシピ行をシェル）。diff の追加行・削除行には専用のトークン種別 `Inserted` / `Deleted` を使う。ハンク内では `-- ` を消した行も `--- ` で始まるので、`--- ` `+++ ` `@@` の 3 行が並ぶときだけ次のファイルのヘッダとみなす。YAML のブロックスカラー（`|` / `>`）の本文は、行内で最後に現れた `-` かキーの桁より深い行とする。TOML の `'''` は `\` をエスケープにしない。

Zig は汎用 lexer に `\\` の行文字列と `@import` 等の組み込み関数のフラグを足して扱う。Haskell は専用 lexer で、`'` を識別子の一部 (`foldl'`)・文字リテラル・型レベルの昇格 (`'True`) に読み分け、記号と隣り合う `--` は演算子として扱う。大文字始まりの識別子を型・コンストラクタ、行頭の小文字始まりを定義名とする。Lisp 系 4 方言は 1 つの lexer を方言ごとのキーワード表と字句フラグ（大文字小文字の区別、`?a` / `\a` の文字リテラル、`cond` の節）で使い分ける。`-` や `?` を含むシンボルを 1 語として読み、リスト先頭の呼び出し位置を Function、特殊形式を Keyword にする。定義名（`defun foo` の `foo`）は Function とし、引数リストと `let` の束縛リストは呼び出しと見ない。

Lua は汎用 lexer に長括弧（`[[ ]]` / `[==[ ]==]` の文字列と `--[[ ]]` のコメント）のフラグを足して扱う。Vim script は専用 lexer で、行頭と `|` の後をコマンド位置として読み、コマンド名（省略形を含む）を Keyword、`g:` / `s:` / `a:` 等のスコープ付き変数と `&option` / `$ENV` / `@r` を Type、`<CR>` 等のキー表記を Preprocessor にする。`"` は行頭ならコメント、式の中では同じ行に閉じの `"` があれば文字列、無ければ行末コメントとみなす。マッピングコマンドの引数はキー列なのでキー表記だけを色付けし、`lua << EOF` の本文は Lua として、`let x =<< END` の本文は文字列として色付けする。`<<` はコマンド（`let` は代入の `=`）の直後に来たときだけ本文の始まりとみなし、行の途中のシフト演算子や文字列中の `<<` では始めない。先頭のコマンドが `vim9script` の文書（と ` ```vim9 `）は Vim9 script として `#` をコメント、`"` を文字列とする。

キーワードリストは `syntax_keywords.h` に集約され、長さでバケット分けしたソート済み配列（`BucketedTable<N>`）で持つ。判定は `word.size()` で該当バケットへ直接ジャンプし、バケット内を `std::ranges::binary_search` で二分探索する。なお `SyntaxLanguage` には図形言語として `Mermaid` に加え `LatexMath`（数式、トークン化対象外）もある。`LatexMath` になるのは ` ```math ` コードブロックと、`$$...$$` だけの段落である。どちらの言語のブロックもパーサが `NodeType::Diagram` のノードにする。

---

### 3.11 ファイルI/O — FileLoader / FileWatcher / DocumentService / FileLoadService

#### 3.11.1 ファイル読み込み

```mermaid
flowchart LR
    OPEN[CreateFileW<br>GENERIC_READ<br>FILE_SHARE_*] --> READ[ReadFile<br>pmr::string バッファへ直接読込]
    READ --> BOM[UTF-8 BOM 検出・除去]
    BOM --> LF[改行を LF に正規化]
    LF --> OUT["LoadedFileDoc { text UTF-8, byte_size }"]
```

```cpp
enum class FileLoadError : uint8_t {
    NotFound,    // ファイルが見つからない、またはアクセス拒否
    TooLarge,    // ファイルサイズが上限 (file_loader.cpp の MAX_FILE_SIZE、~2GB-1) を超過
    ReadFailed,  // 読み込み中の I/O エラー
};

struct LoadedFileDoc {
    std::pmr::string text;           // UTF-8 (BOM 除去・LF 正規化済み)
    size_t           byte_size = 0;  // BOM 込みのファイルバイト数
};

class FileLoader {
public:
    static std::expected<LoadedFileDoc, FileLoadError>
        LoadFile(const std::pmr::wstring& path);
};
```

- 共有モード: `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`
- 他プロセスが編集中でも読み取り可能
- `ReadFile` で `std::pmr::string` バッファへ直接読み込み、UTF-16 変換は行わず **UTF-8 のまま** 返す（md4c も UTF-8 モードで動くため変換不要）
- 改行は LF に正規化して返す。`Document` とリロード差分はこの出力が LF のみであることを前提にする
- 上限は ~2GB-1（md4c が `int` を取るため、`INT_MAX` 超は `TooLarge` として弾く）
- ファイル選択ダイアログは `FileDialogService`（`src/io/file_dialog_service.h`）に分離されている

#### 3.11.2 FileWatcher — ファイル変更監視

`ReadDirectoryChangesW` による非同期ディレクトリ監視。対象ファイルの親ディレクトリを監視し、ファイル名一致（大小無視。通知が 8.3 短縮名で来る場合に備えて短縮名とも照合）で変更を検出する。

```cpp
class FileWatcher {
public:
    using ChangeCallback = std::move_only_function<void()>;

    void StartWatching(const std::pmr::wstring& file_path, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();
    void ResumeWatching();
    // paused 中に溜めた通知を捨てる。読み直しを始める直前に呼ぶ。
    constexpr void DiscardPendingChange() noexcept;
    // 通知済みの変更を読まずに取りやめたときに呼び、次の ResumeWatching で通知し直させる。
    constexpr void RequeueIfPaused() noexcept;
    constexpr HANDLE GetEventHandle() const noexcept;  // read_pending 中のみ非nullptr
};
```

`MsgWaitForMultipleObjects` で `EventHandle` を監視し、変化時は `App::OnFileWatchEvent` が `FileWatcher::CheckForChanges` を直接呼ぶ（Reducer は通さない）。変更を検出したコールバックはデバウンスタイマーを張り直し、タイマー満了でリロードする。

通知を出すと `ResumeWatching` まで paused になり、その間の通知は 1 件に溜めて Resume 時に通知し直す。リロードを始める直前に `DiscardPendingChange` で溜めた通知を捨てる（これから読む内容に含まれるため）。別ファイルのロード中でリロードを見送ったときや、別ファイルを開き始めて進行中のリロードを取り消したときは `RequeueIfPaused` で積み直し、そのロードが取り消し・失敗で終わったときの `ResumeWatching` で読み直させる。

| 定数 | 値 | 用途 |
|:-----|:---|:-----|
| `FILE_RELOAD_DEBOUNCE_MS` | 200ms | ファイル変更通知のデバウンス |
| `FILE_RELOAD_RETRY_MS` | 50ms | truncate→rewrite 検出後の短縮リトライ |

ReadDirectoryChangesW の通知バッファは 64KB 確保（バッファ溢れで通知喪失を防ぐため）。

#### 3.11.3 DocumentService — 読み込みオーケストレーション

```cpp
// src/io/file_load_service.h。static ユーティリティ群。FileWatcher は保持しない（監視は FileWatcher を直接使う）。
class DocumentService {
public:
    static std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);

    static bool IsAsyncLoadCandidate(const std::pmr::wstring& path) noexcept;
    static bool ShouldShowLoadingAnimation(const std::pmr::wstring& path) noexcept;
    // size は QueryFileSize の結果。取得失敗 (nullopt) は上限超え扱い。
    static bool IsAsyncLoadCandidate(std::optional<uint64_t> size) noexcept;
    static bool ShouldShowLoadingAnimation(std::optional<uint64_t> size) noexcept;
};
```

`LoadFile` は `std::expected` で成功時 `Document`（FromMarkdown 済み）を、失敗時 `FileLoadError` を返す。`IsAsyncLoadCandidate` / `ShouldShowLoadingAnimation` はファイルサイズで判定する閾値関数。ファイル読み込み（`FileLoader`）とパース（`Document`）をつなぐ役なので、`core` ではなく `io` に置く。`core` は `io` / `layout` に依存しない。

#### 3.11.4 FileLoadService — 非同期ロード制御 + アニメーション

```cpp
// ワーカースレッドでのパース結果。cache は doc 向けに EstimateNodeHeights 済み。
struct AsyncLoadResult {
    Document    doc;
    LayoutCache cache;
    // 同一ファイルのリロードで、worker がパース前に差分判定まで済ませた結果。
    std::optional<ReloadCheck> reload;
};

// 非同期ロード 1 回分の結果。成功時は AsyncLoadResult、失敗時はその理由。
using AsyncLoadOutcome = std::expected<AsyncLoadResult, FileLoadError>;

// スピナーの状態 (loading_ / loading_angle_) と AsyncLoadCoordinator を持つ薄い facade。
// 起動時の読み込み (preload) も通知先を後から渡すだけで、同じ AsyncLoadCoordinator を通る。
class FileLoadService {
public:
    // ローディング状態
    constexpr bool  IsLoading() const noexcept;
    bool            IsAsyncLoading() const noexcept;  // スピナー非表示でも非同期パース進行中
    constexpr float GetLoadingAngle() const noexcept;
    constexpr void BeginLoadingAnimation() noexcept;   // 角度を 0 に戻して表示を始める
    constexpr void StopLoading() noexcept;
    constexpr void TickLoadingAnimation() noexcept;

    // 同期ロード
    std::expected<Document, FileLoadError> ExecuteLoad();

    // 非同期ロード
    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, Theme theme,
                        std::shared_ptr<const std::pmr::string> reload_base = nullptr);
    std::optional<AsyncLoadOutcome> TakeAsync() noexcept;
    void CancelAsyncLoad() noexcept;

    // preload (App::Init 前から走らせる経路)。通知先は AttachPreload で渡す。
    void StartPreloadAsync(TaskScheduler& scheduler, std::pmr::wstring path, Theme theme);
    using PreloadAttachResult = AsyncLoadCoordinator::AttachResult;
    PreloadAttachResult AttachPreload(HWND hwnd, UINT msg_id);

    constexpr const std::pmr::wstring& GetLoadingPath() const noexcept;
    constexpr void SetLoadingPath(std::pmr::wstring path) noexcept;
};
```

非同期ロードはワーカースレッドで Document を構築し、推定高さまで埋めた `LayoutCache` を返す。ワーカー同期は `AsyncLoadCoordinator`（`src/io/async_load_coordinator.h`）に分離されており、ワーカーは世代カウンタ（atomic な `gen_`）の確認と mutex 保護の `outcome_` への書き込みだけを行う。`Start` は世代を進めて進行中の読み込み（preload を含む）を置き換える。成功と失敗は `AsyncLoadOutcome`（`std::expected`）1 つで運ぶので、結果とエラーが両方入っている状態や、片方だけ取り残される状態は起きない。UI スレッドは `App::OnParseComplete` で `TakeAsync` を 1 回呼んで受け取る。

起動時の読み込みは、ウィンドウを作る前に `StartPreloadAsync` が通知先なし（`hwnd` = null）で `Start` する。ウィンドウ作成後の `App::LoadInitialDocument` が `AttachPreload` で通知先を渡し、結果がもう出ていれば `AppliedSync` を返してその場で `OnParseComplete` を呼び（直後の同期 `WM_PAINT` で初回フレームが空にならないように）、まだなら `AttachedAsync` で完了時に `PARSE_COMPLETE` を送らせる。読み込んでいなければ `None` でヘルプを表示する。

#### 3.11.5 対応ファイル形式

| フィルタ名 | 拡張子 |
|:----------|:-------|
| Markdown Files | `*.md`, `*.markdown`, `*.mkd`, `*.txt` |
| All Files | `*.*` |

---

### 3.12 MermaidRenderer / MermaidFileCache

#### 3.12.1 アーキテクチャ

```mermaid
sequenceDiagram
    participant APP as App
    participant RM as ResourceManager
    participant MR as MermaidRenderer
    participant FC as MermaidFileCache
    participant WV as WebView2 (画面外ポップアップ)
    participant JS as mermaid.js
    participant WIC as WIC

    APP->>RM: RequestMermaidRenders()
    RM->>MR: RequestRender(node, diagram_entry, max_width, dark_mode)
    alt メモリキャッシュヒット (ビットマップかエラー)
        MR-->>RM: 結果を DiagramEntry へ書いて true（高さは RM が寸法から決める）
    else ディスクキャッシュヒット
        MR->>FC: LookupPath(hash)（初回のみ索引を読み込む）
        FC-->>MR: PNG パス + CSS 寸法
        MR->>WIC: worker で PNG 読み込み + デコード (IWICBitmap)
        WIC-->>APP: WM_APP+7 MERMAID_DISK_LOADED
        APP->>MR: ProcessDiskLoads → ID2D1Bitmap 化してメモリキャッシュへ (UI スレッド)
        MR-->>RM: on_update → RequestMermaidRenders（要求し直してメモリキャッシュから受け取る）
    else キャッシュミス
        MR->>MR: EnsureInitialized（ワーカーが無ければ WebView2 環境を生成）
        MR->>WV: ExecuteScript(render)
        WV->>JS: mermaid.render(code)
        JS-->>WV: SVG文字列
        WV-->>MR: 寸法 (WebMessage)
        MR->>WV: CapturePreview(PNG)
        WV-->>MR: PNG ストリーム
        MR->>WIC: worker でデコード (ディスク読み込みと同じ DiskLoad 経路)
        WIC-->>APP: WM_APP+7 MERMAID_DISK_LOADED
        APP->>MR: ProcessDiskLoads → ID2D1Bitmap 化してメモリキャッシュへ
        MR->>FC: StoreAsync(hash, css_width, css_height, png)
        MR-->>RM: on_update → RequestMermaidRenders
    end
```

#### 3.12.2 IMermaidRenderer インターフェース

```cpp
class IMermaidRenderer {
public:
    // svg 非空: 成功 / 空かつ cancelled=false: レンダリング失敗 / 空かつ cancelled=true: 終了時に破棄
    using SvgCallback = std::move_only_function<void(std::pmr::wstring svg, bool cancelled)>;

    virtual ~IMermaidRenderer() = default;

    // 結果 (ビットマップかエラー) が出ていれば diagram_entry に書いて true。まだなら描画を始めて false
    // (始めていれば何もしない)。結果が出たら初期化時に渡した通知で知らせ、呼び出し側が要求し直す。
    // 要求と結果はコードのハッシュで引くので、ノードや diagram_entry を持ち続けない。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry,
                               float max_width, bool dark_mode) = 0;
    // SVG をクリップボード用に取得する。PNG レンダリングと同じワーカー枠を共有する。code は UTF-8。
    virtual void RequestSvg(std::string_view code, float max_width, bool dark_mode,
                            SvgCallback callback) = 0;
    // 描画待ちの表示用要求だけを捨てる (描画中と SVG 要求は維持)。捨てた図は再要求される。
    virtual void DropQueued() {}
};
```

`ResourceManager` は描画タイマー（`MERMAID_BATCH`）・`FlushPendingResources`・結果の通知（`on_update` から `RequestMermaidRenders`）から、再レイアウトしない `RequestVisibleDiagrams` で可視範囲 → 下 → 上の順に図を走査し、1 図ずつ `RequestDiagramRender` で要求する。その場で結果が出た図の高さは `height_changed_` に溜め、呼び出し側が再レイアウトを 1 回にまとめる（`RequestMermaidRenders` は直後に、`FlushPendingResources` は画像の適用とあわせて）。大きく移動したとき・幅やテーマが変わったとき・文書を切り替えたときは `DropQueued` で不要になった描画待ちを捨てる。

要求と結果のキーは、ノードのテキストと言語で決まる `DiagramCodeHash` に量子化幅とテーマを混ぜた `DiagramRequestHash`（`NodeDiagramHash` と同じ値で、ディスクキャッシュのキーにもなる）。mermaid は既定で `useMaxWidth` なので、幅の上限より狭く描けた図（`IsWidthFree`）はどの幅でも同じ結果になる。その結果は幅を混ぜない `DiagramWidthFreeHash` で持ち、メモリ（`FindCached`）でもディスク（`MermaidFileCache::LookupDiagram`）でも先に引いて、いまの幅の上限より狭いときだけ使う。ウィンドウやペインの幅を変えて開き直しても、描き直すのは幅に縛られる図（gantt など）とエラーだけになる。コード部分は `DiagramEntry::code_hash` に覚え、幅・テーマの変更で結果を捨てても残すので、要求し直すたびにテキストを読み直さない。結果はビットマップかエラーの `CachedResult` 1 型で、`LruCache`（`MAX_CACHE_ENTRIES` = 128 件、`MAX_CACHE_BYTES` = 64MB）1 つに持つ。同じコードは同じ結果になるので、失敗も成功と同じく覚えて描き直さない。待ち・読み込み中・描画中の要求は `inflight_`（ハッシュの集合）で重複を弾く。要求はコードを UTF-8 のまま持ち、UTF-16 への変換と LaTeX の flowchart への包みは `RenderInWorker` で描く直前に行う。

`mendo_core` には `IMermaidRenderer` 抽象のみが入り、WebView2 を呼び出す具象 `MermaidRenderer` は `mendo` 実行ファイル側に分離されている。`MermaidRenderer` は最大 2 ワーカー（`MAX_WORKERS`、独立した WebView2 インスタンス）を並行稼働させ、PNG/SVG を同時にレンダリングできる。ワーカーは最初 1 つだけ起動し、全ワーカーが描画中で待ちが溜まったときに `MaybeGrowWorkers` で増やす。

PNG のデコードは UI スレッドで行わない。ディスクキャッシュから読む場合も、WebView2 の `CapturePreview` で描画した直後の PNG も同じ `DiskLoad` 経路に流し、バックグラウンド `TaskScheduler` でファイル読み込み（キャプチャ時は不要）と WIC デコードを済ませた `IWICBitmap` を作る。完了は `app_msg::MERMAID_DISK_LOADED` で通知され、UI スレッドの `ProcessDiskLoads` が `CreateBitmapFromWicBitmap` で `ID2D1Bitmap` 化してメモリキャッシュに入れ、結果が出たものがあれば `on_update_` をまとめて 1 回呼ぶ。1 回の転送は `kUploadSliceBudget`（4ms）までで、残した分は `HasLeftoverDiskLoads` が真になり、メッセージループの空き時間（`App::RunIdleWork`）で続ける。キャプチャ由来の PNG はこのときディスクキャッシュへ `StoreAsync` される。ディスクから読めなかった・デコードできなかった図は WebView2 での描画に回し、キャプチャ直後の PNG がデコードできなければエラーとして確定させる。

描画要求が無いまま `IDLE_SHUTDOWN_MS`（60 秒）経つと、`app_timer::Id::MERMAID_IDLE` → `App::HandleTimer` → `OnIdleTimer` で全ワーカーと WebView2 環境（browser / renderer / GPU プロセス）をまとめて閉じ、準備完了の印 (`ready_`) も戻す。待ちキューが空になるたび（`ProcessQueue` 末尾）にタイマーを張り直し、待ちや描画中のワーカーが残っていれば閉じるのを見送る。次の描画要求で `EnsureInitialized` から作り直し、準備完了の通知（`on_update_`）は再初期化のたびに呼ばれて未描画の図を再要求させる。

#### 3.12.3 MermaidFileCache — 永続キャッシュ

```cpp
class MermaidFileCache {
public:
    struct CacheEntry {
        float css_width  = 0.0f;
        float css_height = 0.0f;
    };

    // 索引の読み込みは初回利用まで遅らせる (ディスクに触れない)。
    void Init(float current_dpr, TaskScheduler& scheduler);
    // 以降の参照・保存のキーに混ぜる DPR を差し替える。別の DPR のエントリは LRU で淘汰させる。
    void SetDpr(float dpr) noexcept;
    // PNG のパスと寸法を返し LRU を更新する。読み込みは呼び出し側 (worker) が行う。
    bool LookupPath(uint64_t key, CacheEntry& entry, std::filesystem::path& png_path);
    // ファイルが確実に存在しない場合のみ索引から除く。
    void OnReadFailed(uint64_t key, DWORD read_error);
    bool LookupDimensions(uint64_t key, CacheEntry& entry);
    // png_data は表示側 (メモリキャッシュ / DiagramEntry) と共有し、書き込み用にコピーしない。
    // PngBytes は utility.h の std::shared_ptr<const std::pmr::vector<uint8_t>>。
    void StoreAsync(uint64_t key, float css_width, float css_height, PngBytes png_data);
    void SaveIndex();   // 索引に変更が無ければ書かない
    void SetCacheDir(const std::filesystem::path& dir);
    void SetLimits(size_t max_entries, uint64_t max_total_size);
    void Shutdown();

    // 索引が未読み込みならここで読み込むため非 const。
    size_t   EntryCount();
    uint64_t TotalSize();

private:
    static constexpr uint32_t MAGIC   = 0x4D454D43u;  // "MEMC"
    static constexpr uint32_t VERSION = 1;
    static constexpr size_t   DEFAULT_MAX_ENTRIES    = 4096;
    static constexpr uint64_t DEFAULT_MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB
};
```

外部キーは内部で DPR 量子化値（1/100 単位、典型値 100/125/150/175/200）に定数 `0x9E3779B97F4A7C15` を乗じた値と XOR して内部キーに変換し、DPR ごとに独立したエントリとして保存する。これにより DPI 変更/モニタ切替時に全消去を発生させない。DPR は `Init` 時には画面外のワーカー位置のモニタから推定し（`MermaidRenderer::EstimateWorkerDpr`）、ワーカーの準備完了で実際に描く DPR が届いたら `SetDpr` で差し替える。書き込みは `TaskScheduler` のワーカースレッドで非同期実行される。`pending_writes_`（書き込み中のキー → 書き込み中に追い出されたか）で同じキーの 2 本目の書き込みを出さず、書き込み中に追い出されたエントリの PNG は着地後にワーカーが消すので、索引に無いファイルは残らない。書き込み中のキーは読み込みに失敗しても `OnReadFailed` で索引から除かない。

索引（`index.bin`）は `Init` では読まず、`LookupPath` / `LookupDimensions` / `StoreAsync` などの初回利用時に、唯一の入口 `Index()` で読み込む。図を含まない文書の起動ではディスクに触れない。索引・合計サイズ・LRU シーケンス・変更フラグは `IndexState` にまとめ、`std::optional<IndexState>`（未読み込みは `nullopt`）で持つ。変更系の操作（`Add` / `Remove` / `Touch`）は `IndexState` 自身が `dirty` を立てる。索引本体は `std::pmr::unordered_map<uint64_t, IndexEntry>` 1 本で、各エントリが `last_used`（単調増加シーケンス）を持つ。追い出しは満杯時にしか起きないため、LRU 順の補助構造は持たず `last_used` の線形走査で最古を探す。終了時の `SaveIndex` は未読み込み（`nullopt`）か変更が無ければ書き戻さないので、空の索引でディスクを上書きすることは構造上起きない。

#### 3.12.4 初期化

WebView2 は起動時には作らず、最初の描画要求（`RequestRender` / `RequestSvg`）で `EnsureInitialized` が立ち上げる。

1. 画面外に置いたポップアップウィンドウを作成（`CapturePreview` は可視状態を要求するため非表示にはしない）
2. WebView2 環境とコントローラを非同期に生成。環境の生成に失敗したら `MERMAID_INIT_RETRY` タイマーで `ENV_RETRY_INTERVAL_MS`（500ms）おきに `MAX_ENV_RETRIES`（3 回）まで再試行し、上限に達したら待ちの要求を失敗で完了させて未初期化に戻す
3. `https://app.local/index.html` へ `Navigate` し、`WebResourceRequested` で HTML テンプレート（`IDR_MERMAID_HTML`）と `mermaid.min.js`（`IDR_MERMAID_JS`）を RCDATA から無圧縮のまま配信する
4. 最初のワーカーの準備完了で `ready_` を立て `on_update_` を呼ぶ。準備前の描画要求は捨てているので、これで図の描画を要求し直させる

---

### 3.13 NavHistory / リンク解決 — ナビゲーション

#### 3.13.1 NavHistory — 履歴スタック

ブラウザスタイルの戻る/進む履歴。純粋ロジック・Win32 依存なし。

```cpp
struct NavEntry {
    std::pmr::wstring file_path;
    int   node   = -1;     // ノードインデックス（位置の安定基準）
    float offset = 0.0f;   // ノード先頭からのピクセルオフセット

    NavEntry() = default;
    NavEntry(std::wstring_view fp, int n = -1, float off = 0.0f);
};

class NavHistory {
public:
    static constexpr size_t MAX_HISTORY = 1024;

    NavHistory() = default;
    explicit NavHistory(size_t max_history);   // テストで上限を差し替える

    void Push(NavEntry current);
    // 別ファイルを開く前の仮の Push。ロード結果で確定 (Commit) か取り消し (Rollback) をする。
    void PushPending(NavEntry current);
    void CommitPending() noexcept;
    void RollbackPending();
    bool GoBack(NavEntry current, NavEntry& out);     // 仮の操作として積む
    bool GoForward(NavEntry current, NavEntry& out);  // 同上

    bool   CanGoBack() const noexcept;
    bool   CanGoForward() const noexcept;
    size_t BackSize() const noexcept;
    size_t ForwardSize() const noexcept;
    void   Clear() noexcept;

private:
    std::pmr::deque<NavEntry> back_stack_;
    std::pmr::deque<NavEntry> forward_stack_;
    std::pmr::vector<PendingOp> pending_;   // 確定待ちの操作 (古い順)
    size_t max_history_ = MAX_HISTORY;
};
```

戻る/進むスタックは `NavEntry` をそのまま `std::pmr::deque` に積み、各エントリがファイルパスを持つ（履歴は最大 1024 件程度なので、パスのインターン化は行わない）。容量を超えたら最古のエントリを捨てる。位置はノード単位で表現するため、ファイルが編集されて絶対 y 座標が変わっても同一ノードに戻れる。

別ファイルへの移動（`PushPending` と、別ファイルへの `GoBack` / `GoForward`）は確定待ちの操作として `pending_` に残し、ロードが終わったら `RestoreScrollAfterLoadAction` で `CommitPending`、取り消し・失敗（`LoadFailedAction` や次のロードの開始）なら `RollbackPending` で元に戻す。開けなかったファイルの分だけ履歴が重複したり、戻る/進むの位置がずれたりしないためである。ロード中に次の操作が来たら、ロード中の文書はまだ表示されていないので、`PushPending` は前の仮の操作を取り消してから積み直し、戻る/進むの連続はロード中の移動先を起点に続ける（続けた操作は `RollbackPending` でまとめて取り消す）。表示中のファイル内への戻る/進むはロードを伴わないので、その場で `CommitPending` する。`Push` や `Clear` は確定待ちを捨てる。同じファイルの開き直しはリロードとして終わり確定が来ないので、`PushPending` ではなく `Push` で積む。

#### 3.13.2 リンク解決

リンククリックは `nav.h` のフリー関数 `HandleLinkClick(url)` で解析し、結果に応じて `App::HandleLinkClick` がアンカーへのスクロール（`NavigateAnchorAction`）と外部 URL を `ShellExecuteW` で開く処理を分岐する。`#` で始まる URL がアンカー、`http://` / `https://` / `mailto:` で始まる URL（`IsSafeUrlScheme`）が外部 URL で、それ以外（相対パスや `file://` を含む）は何もしない。外部 URL は関連付けのコマンドに差し込まれるので、`"` と空白・制御文字を `%XX` にしてから渡す。リンクから別ファイルへは遷移しない。別ファイルへの遷移（履歴の戻る/進む、ファイルペイン・ダイアログ・D&D で開く操作）は Markdown ファイルパスを `LoadFile` 副作用として発行する。

```cpp
struct LinkClickResult {
    enum class Type : uint8_t { None, Anchor, ExternalUrl };
    Type             type = Type::None;
    std::pmr::string target;   // Anchor: アンカー名 (UTF-8、先頭 '#' を除きパーセントデコード済み)
                               // ExternalUrl: URL (UTF-8)。ShellExecuteW に渡す直前で wstring 化する。
};

LinkClickResult  HandleLinkClick(std::string_view url);
bool             IsSafeUrlScheme(std::string_view url) noexcept;
// md4c は URL をエンコードされたまま渡すので、ファイルパスやアンカーとして引く前に通す。
std::pmr::string PercentDecode(std::string_view s);
```

リンク切れのアンカーでは履歴を積まない（進む履歴が消えて、戻っても同じ位置になるため）。

脚注のリンク (`#user-content-fn-N` / `#user-content-fnref-N[-M]`) も Anchor として扱う。`Document::FindAnchorIndex` が見出しの索引より先に `FindFootnoteAnchor` で照合する[^spec-anchor-format]。

[^spec-anchor-format]: アンカーは GitHub が出力する HTML の id と同じ形式。N は脚注番号、M は同じ脚注への何回目の参照か（1 回目は省略）。

#### 3.13.3 ナビゲーションフロー

```mermaid
flowchart TD
    LINK[リンククリック] --> APP[App::HandleLinkClick]
    APP --> KIND{解決結果}
    KIND -->|Anchor| ANCHOR[NavigateAnchorAction<br>Document::FindAnchorIndex<br>脚注 → 見出しの順に引く]
    KIND -->|ExternalUrl| SHELL[ShellExecuteW]
    KIND -->|None| NOP[何もしない]
    ANCHOR -->|見つかった| PUSH[NavHistory::Push → ScrollTarget 設定]
    HIST[ジェスチャ / スワイプ / Alt+矢印 / Xボタン / メニュー] --> NAV[NavigateBack/ForwardAction<br>NavHistory::GoBack / GoForward]
    NAV --> SAME{移動先は表示中のファイル?}
    SAME -->|はい| COMMIT[CommitPending → ScrollTarget 設定]
    SAME -->|いいえ| LOAD[復元位置を記録 → effect::LoadFile]
    LOAD --> RESULT{ロード結果}
    RESULT -->|成功| RESTORE[RestoreScrollAfterLoadAction<br>CommitPending → 復元位置へスクロール]
    RESULT -->|取り消し・失敗| ROLLBACK[RollbackPending]
```

---

### 3.14 MouseGesture — マウスジェスチャ

#### 3.14.1 状態遷移

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Pressed : 右ボタン押下
    Pressed --> Tracking : 30px以上移動
    Pressed --> Idle : 右ボタン解放<br>(コンテキストメニュー表示)
    Tracking --> Idle : 右ボタン解放<br>(方向確定ならナビゲーション、<br>未確定ならコンテキストメニュー)
```

#### 3.14.2 パラメータ

| パラメータ | 値 | 説明 |
|:----------|:---|:-----|
| `GESTURE_THRESHOLD` | 30.0f (DIP) | ジェスチャ開始の移動距離閾値 |
| `MIN_POINT_DISTANCE` | 2.0f | トレイル軌跡のサンプリング最小距離 |
| `TRAIL_MAX_POINTS` | 512 | トレイル軌跡の最大ポイント数 |

左へのドラッグは `NavDirection::Back`、右へのドラッグは `NavDirection::Forward` になる。ジェスチャ・スワイプ・オーバーレイ描画は向きを同じ `NavDirection` で受け渡し、符号付き整数や別の列挙との変換は挟まない。

---

### 3.15 SwipeDetector — タッチパッドスワイプ検出

`WM_MOUSEHWHEEL` の累積デルタに基づくスワイプ検出。

```cpp
// util/ui_types.h。MouseGesture::GetDirection・GestureRenderState::direction・ナビボタンのホバーも同じ型を使う。
enum class NavDirection : uint8_t { None, Back, Forward };

class SwipeDetector {
public:
    constexpr void OnHWheel(int delta, uint64_t now_ms) noexcept;
    constexpr NavDirection Commit() noexcept;
    constexpr void NotifyVScroll(uint64_t now_ms) noexcept;
    constexpr void Reset() noexcept;
    constexpr bool IsOverlayVisible() const noexcept;
    // 右スワイプで戻る、左スワイプで進む。
    constexpr NavDirection GetOverlayDirection() const noexcept;

    static constexpr int      TRIGGER_THRESHOLD = 400;
    static constexpr uint64_t AXIS_LOCK_MS      = 200;
    static constexpr uint64_t RESET_TIMEOUT_MS  = 500;
    static constexpr uint64_t COMMIT_TIMEOUT_MS = 150;
};
```

| パラメータ | 値 | 説明 |
|:----------|:---|:-----|
| `TRIGGER_THRESHOLD` | 400 | 発火の累積デルタ閾値（WHEEL_DELTA単位） |
| `AXIS_LOCK_MS` | 200ms | 縦スクロール後の軸ロック期間 |
| `RESET_TIMEOUT_MS` | 500ms | 入力なしで蓄積をリセットするタイムアウト |
| `COMMIT_TIMEOUT_MS` | 150ms | 指を離してから発火までの待機 |

---

### 3.16 ViewportManager — ビューポート管理

スクロール・選択・ズームの純粋な状態管理。Win32依存なし。

```cpp
struct ScrollTarget {
    int   node   = -1;       // -1 = 無効
    float offset = 0.0f;     // ノード先頭からのピクセル距離
    constexpr bool IsValid() const noexcept;
};

// 見出しノードを md ペイン上端の heading_spacing_above 分だけ下に配置する ScrollTarget。
constexpr ScrollTarget MakeHeadingTopTarget(int node, float heading_spacing_above) noexcept;

class ViewportManager {
public:
    // スクロール
    constexpr float GetScrollY() const noexcept;
    constexpr float GetMaxScroll() const noexcept;
    constexpr void  ScrollTo(float position) noexcept;       // scroll_target を無効化
    constexpr void  DirectScrollBy(float delta) noexcept;    // 同上
    constexpr void  SyncMaxScroll(float total_h, float viewport_h) noexcept;
    constexpr int   FindFirstVisibleNode(const LayoutCache& cache) const noexcept;
    // 可視先頭ノード基準の現在位置。ノードが無ければ node=-1, offset=scroll_y
    constexpr ScrollTarget CurrentTarget(const LayoutCache& cache) const noexcept;
    constexpr void  SetScrollY(float y) noexcept;            // クランプしない。target は無効化
    constexpr void  ClampAndDetach() noexcept;               // scroll_y をクランプし target を無効化

    // ScrollTarget — ノード相対位置によるスクロール復元
    constexpr void SetScrollTarget(int node, float offset) noexcept;
    constexpr bool HasScrollTarget() const noexcept;
    constexpr const ScrollTarget& GetScrollTarget() const noexcept;
    constexpr void ApplyScrollTarget(const LayoutCache& cache) noexcept;
    constexpr void EnsureScrollTarget(const LayoutCache& cache) noexcept;

    // 選択 (deducing this で const/非 const を1本に集約)
    constexpr auto& GetSelection(this auto& self) noexcept;
    constexpr void SetSelection(const TextSelection& sel) noexcept;
    constexpr void SelectAll(const std::pmr::vector<Node>& nodes) noexcept;
    constexpr void ClearSelection() noexcept;
    constexpr int      GetAnchorNode() const noexcept;
    constexpr uint32_t GetAnchorPos() const noexcept;
    constexpr void     SetAnchor(int node, uint32_t pos) noexcept;
    constexpr int      GetClickStartX() const noexcept;
    constexpr int      GetClickStartY() const noexcept;
    constexpr void     SetClickStart(int x, int y) noexcept;

    // ズーム — 戻り値 bool は zoom_index が変化したかどうか
    constexpr int   GetZoomIndex() const noexcept;
    constexpr void  SetZoomIndex(int idx) noexcept;          // [0, ZOOM_STEP_COUNT) に必ず clamp
    constexpr float GetCurrentZoom() const noexcept;         // ZOOM_STEPS[zoom_index]
    constexpr bool  ZoomIn() noexcept;
    constexpr bool  ZoomOut() noexcept;
    constexpr bool  ZoomReset() noexcept;
};
```

`ScrollTarget` は **「ノード i の先頭から offset ピクセル下」** という相対位置を保持する。ファイル再読み込み・ズーム・DPI 変更でレイアウトが変わっても、同じノードに戻ることで「見ている位置を保つ」を実現する。ノード数は `LayoutCache` のエントリ数で数える。文書のノードとキャッシュのエントリは常に同数で（文書を差し替える `ReplaceDocument` が assert で確かめる）、ノード数を別の引数で渡して突き合わせることはしない。`SetZoomIndex` は外部入力経路から不正値が来ても out-of-bounds にならないよう setter 側で必ず clamp する。テキスト選択のドラッグ中かどうかはここでは持たず、`ViewState::drag`（`LeftDrag::TextSelect`、3.3）で表す。

---

### 3.17 PaneController — ペイン管理

3ペイン（File / TOC / Markdown）の表示状態・幅・スクロールを管理する。Win32依存なし。File / TOC の個別メソッドは持たず、`PaneTarget` 引数で共通化する。ホバーは `ViewState::hover`（`HoverHighlights` の `side_index` / `side_button`）、スプリッタやスクロールバーのドラッグは `ViewState::drag`（`LeftDragState`）が持つ（3.3）。

```cpp
class PaneController {
public:
    // 表示切替
    constexpr bool IsSidePaneVisible(PaneTarget t) const noexcept;
    constexpr void SetSidePaneVisible(PaneTarget t, bool v) noexcept;
    constexpr void ToggleSidePane(PaneTarget t) noexcept;

    // 幅
    constexpr float GetSidePaneWidth(PaneTarget t) const noexcept;
    constexpr void  SetSidePaneWidth(PaneTarget t, float w) noexcept;  // PANE_MIN_WIDTH に clamp

    // スクロール
    constexpr ScrollState&       SidePaneScroll(PaneTarget t) noexcept;
    constexpr const ScrollState& SidePaneScroll(PaneTarget t) const noexcept;
    constexpr void ResetScrollStates() noexcept;
    bool ScrollSidePaneBy(PaneTarget t, float delta, float max_scroll) noexcept;  // 変化した場合 true

    // スプリッタ — edge のペインの右端にあるスプリッタを dip_x へ動かす
    void DragSplitterTo(PaneTarget edge, float dip_x, float total_width,
                        float splitter_w) noexcept;

    // ズーム / レイアウト
    void ApplyZoom(float ratio) noexcept;
    PaneLayout ComputeLayout(float total_w, float total_h, float splitter_w,
                             float top_offset = 0.0f) const noexcept;

    static constexpr float PANE_DEFAULT_WIDTH = 220.0f;
    static constexpr float PANE_MIN_WIDTH     = 100.0f;
};
```

スプリッタのドラッグでは、reducer が `LeftDrag::Splitter` の対象ペイン（`LeftDragState::pane`）をそのまま `DragSplitterTo` に渡す。座標からのゾーン判定はメンバではなくフリー関数 `DetectPaneZone(...)`（`src/util/pane_layout.h`、実装 `src/util/pane_layout.cpp`）が担う。Markdown ペインの最小幅 `MD_PANE_MIN_WIDTH = 200.0f` は `src/util/ui_constants.h` の inline 定数として定義される。

#### 3.17.1 ペイン構成

```mermaid
graph LR
    subgraph ウィンドウ
        FP[ファイルペイン]
        S1[スプリッタ1]
        TP[TOCペイン]
        S2[スプリッタ2]
        MP[Markdownペイン]
    end
    FP --- S1 --- TP --- S2 --- MP
```

| ペイン | 既定幅 | 最小幅 | 内容 |
|:-------|:-------|:-------|:-----|
| ファイルペイン | 220px | 100px | ディレクトリ内のファイル一覧 |
| TOCペイン | 220px | 100px | 見出しベースの目次 |
| Markdownペイン | 残り全幅 | 200px | Markdownコンテンツ |
| スプリッタ | テーマ依存（`Theme::splitter_width`） | — | ペイン間の境界線 |

ファイルペインの一覧は `FileExplorer`（`AppState::file_explorer`）が持つ。`SetDirectory` / `Refresh` は一覧を古いと印すだけで、列挙は次の `GetEntries` まで遅らせる。`GetEntries` を呼ぶのは表示中のファイルペインの描画・ホバー・クリック・スクロールだけなので、非表示の間は列挙しない（起動時やファイルを開くたびの UI スレッド I/O を省く）。列挙し直すたびに世代 (`GetGeneration`) が進み、ツールチップは項目 index と世代の組で対象を識別する。

---

### 3.18 hit_test — ヒットテスト

```cpp
struct MdPaneHitContext {
    const std::pmr::vector<Node>& nodes;
    const LayoutCache&            cache;
    const Theme&                  theme;
    float    scroll_y;
    PaneRect md_rect;
    float    dpi_scale;
    int      screen_x;
    int      screen_y;
    // ボタン位置とブロック横スクロールの clamp に使う
    float content_width = 0.0f;
    // ブロック単位の横スクロール量。null は全ノード 0 として扱う。
    const std::pmr::unordered_map<int, float>* block_scroll_x = nullptr;
};

// MD ペインのヒットテスト。状態を持たない純粋関数群 (src/input/hit_test_service.h)。
namespace hit_test {

struct HitResult {
    static constexpr uint32_t kNoChar = std::numeric_limits<uint32_t>::max();

    int      node_index = -1;
    // キャレット位置 (選択の端)。文字の右半分を指すとその文字の後ろになる。
    uint32_t text_pos   = 0;
    // 点の下にある文字の位置 (リンクの判定)。文字の上でなければ kNoChar。
    uint32_t char_pos   = kNoChar;
};

HitResult HitTest(const MdPaneHitContext& ctx) noexcept;
// HitTest と同じノードを返す。文字位置は求めない。
int HitNode(const MdPaneHitContext& ctx) noexcept;
HitResult HitTestTable(const Node& node, const NodeLayoutEntry& entry,
                       float entry_text_top, int node_index, const Theme& theme,
                       float dip_x, float dip_y, float h_scroll_x = 0.0f) noexcept;

NavDirection NavButtonHitTest(float dip_x, float dip_y, const PaneRect& md_rect) noexcept;

// 可視ノードのオーバーレイボタン (コードのコピー / 図の保存・コピー) のうちマウス位置にあるもの。
OverlayButtonHit OverlayButtonHitTest(const MdPaneHitContext& ctx) noexcept;
bool BlockHScrollbarHitTest(const MdPaneHitContext& ctx, int node_index, float visible_width) noexcept;

} // namespace hit_test

// src/util/ui_types.h — コードブロック・図の右上に重ねるボタン
enum class OverlayButton : uint8_t { None, CodeCopy, DiagramSave, DiagramCopy };

// マウス座標は 1 点なので同時に当たるのは 1 つだけ。
struct OverlayButtonHit {
    int           node = -1;
    OverlayButton kind = OverlayButton::None;
    constexpr bool Any() const noexcept;   // kind != None
    bool operator==(const OverlayButtonHit&) const = default;
};
```

オーバーレイボタンの矩形は `layout::ForEachOverlayButton`（`layout/layout_computer.h`）が `fn(rect, kind)` で列挙し、描画（`CommandGenerator::GenOverlayButtons`）と `OverlayButtonHitTest` が同じ矩形を使う。コードブロックはコピー 1 つ、ビットマップを持つ図（`NodeType::Diagram`）は保存とコピーの 2 つを並べる。

テーブルの行・列の判定（`FindTableRow` / `FindTableCol`）は `TableLayoutData` の累積配列（`row_cum_y` / `col_cum_x`）だけを二分探索する。幾何が揃っていない（未計測、または `InvalidateGeometry` の後）ときはヒットなしとして扱い、行高さを推定し直して線形に探す経路は持たない。セル本文の原点は描画と同じ `TableCellTextOrigin` で求める。

本文はペイン基準の座標（ペイン原点 `md_rect.x/y` は描画変換に乗る）で描くので、マウス座標は `ScreenToPaneDip` で一度だけ「ペイン左端基準の X とドキュメント Y」に変換し、ノードやボタン、ブロック横スクロールバーの矩形はドキュメント座標のまま比べる。可視範囲はどこでも `[scroll_y, scroll_y + md_rect.height]` で一致する。

`hit_test` は状態を持たず、ヒット結果のキャッシュもしない。呼ばれるたびに判定する（`HitTestPoint` は 1 万行のコードブロックでも数〜十数 µs で済むため）。ホバー中のナビボタンは、ジェスチャやスワイプが示す履歴移動の向きと同じ `NavDirection`（`util/ui_types.h`、`{ None, Back, Forward }`）で表す。

ホバーとクリックは同じ `App::ResolveMdPaneHit` で「点が MD ペインのどの領域を指すか」を決める。優先順位は 検索バー → ナビボタン → オーバーレイボタン（コピー / ダイアグラムコピー / 保存）→ スクロールバー → 本文 で、縮小表示でボタンがスクロールバーのヒット余白に掛かってもボタンが勝つ。結果は `App::MdPaneHit`（`kind` と、検索バーのゾーン・ナビボタン・`OverlayButtonHit`）で返す。ホバー表示（カーソルとツールチップ）とクリックの結果は常に一致する。ホバーでは判定結果を `PointerHover` に書いて `PointerHoverAction` で reducer に渡し、reducer が `ViewState::hover` に反映する。

リンクは `FindLinkHitAtPosition`（`core/selection_html.h`）で URL とその添字（`node.view_link_urls()` の位置）を一度に求める。ツールチップはこの添字で対象を識別し、URL 文字列から添字を探し直さない。

---

### 3.19 ConfigService / SessionService — 設定永続化

#### 3.19.1 保存先・形式

```
%LOCALAPPDATA%\mendo\settings.ini
```

`IniParser` で読み書きし、セクション+キーの2階層でアクセスする。書き込みは `*.tmp` → `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` の原子的差し替え。書き出しはアプリ終了時 (WM_DESTROY) に集約し、読み込み後に値が変わっていなければ書かない。ログオフ・シャットダウンでは WM_DESTROY が来ないため、WM_ENDSESSION で `DestroyWindow` を呼んで同じ終了処理を通す。

#### 3.19.2 保存項目

| セクション | キー | 型 | 既定値 | 説明 |
|:-----------|:-----|:---|:------|:-----|
| `General` | `Language` | `wstring` | （OS自動判定） | UI言語 (`ja` / `en`) |
| `View` | `DarkMode` | `bool` | `false` | ダークモード |
| `View` | `ZoomLevel` | `int` | `7` (1.00x) | ズームインデックス |
| `Window` | `X` / `Y` | `int` | `0` | 通常表示時のウィンドウ位置 |
| `Window` | `Width` / `Height` | `int` | `0`（未保存） | 通常表示時のウィンドウサイズ。未保存なら OS 既定の位置に既定サイズ（1600×900）で開く |
| `Window` | `Maximized` | `bool` | `false` | 最大化状態（最小化中に閉じたときは最小化前の状態） |
| `Pane` | `ShowFile` | `bool` | `true` | ファイルペイン表示 |
| `Pane` | `ShowToc` | `bool` | `true` | TOCペイン表示 |
| `Pane` | `FileWidth` | `int` | `220` (DIP) | ファイルペイン幅 |
| `Pane` | `TocWidth` | `int` | `220` (DIP) | TOCペイン幅 |
| `Session` | `LastFile` | `wstring` | 空文字列 | 最後に開いたファイルパス |
| `Session` | `ScrollNode` | `int` | `-1` | スクロール位置（ノードインデックス） |
| `Session` | `ScrollOffset` | `int` | `0` | スクロール位置（ノード内オフセット） |

#### 3.19.3 ConfigService — 設定永続化

設定 (settings.ini) のメモリ上マップとディスク永続化を保持する。インスタンスごとに独立した状態を持つので、テストでは新しいインスタンスを使うだけで分離できる。

```cpp
class ConfigService {
public:
    ConfigService() noexcept = default;

    // 設定ディレクトリ
    void SetConfigDirOverride(const std::filesystem::path& dir);    // テスト用
    const std::filesystem::path& GetConfigDir() const;
    // 区切り文字や ".." を含む名前は拒否する。
    std::filesystem::path GetConfigPath(std::wstring_view filename) const;

    // ディスク永続化
    void Load();
    void Flush();        // 変更があればメモリ上のデータを書き出す（WM_DESTROY 時に呼ぶ）

    // 型付きアクセサ
    void SaveBool(std::string_view section, std::string_view key, bool value);
    bool LoadBool(std::string_view section, std::string_view key, bool default_value = false) const;

    void SaveInt(std::string_view section, std::string_view key, int value);
    int  LoadInt(std::string_view section, std::string_view key, int def, int min_v, int max_v) const;

    void              SaveWString(std::string_view section, std::string_view key, std::wstring_view value);
    std::pmr::wstring LoadWString(std::string_view section, std::string_view key) const;
};
```

LocalAppData は環境変数 `LOCALAPPDATA` を優先し（`SHGetKnownFolderPath` は初回に `windows.storage.dll` のロード等で ms 単位かかるため）、取れない・絶対パスでない場合だけ `SHGetKnownFolderPath` にフォールバックする。結果は private static 関数 `DefaultConfigDir()` の関数ローカル static で一度だけ解決し、override 未設定時の問い合わせをプロセス全体で 1 回に抑える（別スレッドから `GetConfigDir` を呼んでも安全）。

`Save*` は既存値と同じなら何もせず、値が変わったときだけ `dirty_` を立てる。`Flush` は `dirty_` が立っていなければ書き込まず、書き込みに成功したら `dirty_` を下ろす。終了のたびに同期書き込み（`FlushFileBuffers` + `MOVEFILE_WRITE_THROUGH`）が走るのを、設定を変えたセッションだけに限るためである。

#### 3.19.4 SessionService

セッション状態（最後に開いたファイル、ペイン構成、スクロール位置）の永続化を担当する。`ConfigService` のセクション "Session" / "Pane" を排他的に扱う薄いラッパ。

```cpp
class SessionService {
public:
    explicit SessionService(ConfigService& config) noexcept;

    void              SaveLastFilePath(std::wstring_view path);
    std::pmr::wstring LoadLastFilePath() const;

    // PaneController との変換は呼び出し側が担う。
    struct PaneState {
        bool  show_file  = true;
        bool  show_toc   = true;
        float file_width = 220.0f;
        float toc_width  = 220.0f;
    };
    void      SavePaneState(const PaneState& state);
    PaneState LoadPaneState(float min_width, float default_width) const;

    struct ScrollPosition {
        int node   = -1;
        int offset = 0;
    };
    void           SaveScrollPosition(int node, float offset);
    ScrollPosition LoadScrollPosition() const;
};
```

`SaveScrollPosition` はノード内オフセット（`SnapshotVisibleTarget` の `offset`）を整数に丸めて永続化する（`Session.ScrollOffset` セクションキー）。

---

### 3.20 TitleBar — カスタムタイトルバー

#### 3.20.1 概要

Win32 標準タイトルバーを `WM_NCCALCSIZE` で非表示にし、Direct2D で自前描画したカスタムタイトルバーに置き換える。ファイルを開く・検索・テーマ切替・ヘルプ・ファイル/目次ペイン切替・最小化/最大化/閉じるボタンを含む。

```cpp
// OpenFile..Close はボタン。連続した値であることを TitleBarButtonIndex が前提にする。
enum class TitleBarHitZone : uint8_t {
    None, Caption, Icon,
    OpenFile, Help, ThemeToggle, Search,
    FileToggle, TocToggle,
    Minimize, Maximize, Close,
};

constexpr bool IsTitleBarButton(TitleBarHitZone zone) noexcept;   // OpenFile..Close か
inline constexpr size_t kTitleBarButtonCount = /* OpenFile..Close の個数 */;
constexpr size_t TitleBarButtonIndex(TitleBarHitZone zone) noexcept;  // zone - OpenFile

// TitleBarButtonIndex で引くボタン矩形。
using TitleBarButtons = std::array<DipRect, kTitleBarButtonCount>;
constexpr auto& TitleBarButtonRect(auto& buttons, TitleBarHitZone zone) noexcept;

class TitleBar {
public:
    static constexpr float BASE_HEIGHT       = 32.0f;
    static constexpr float BUTTON_WIDTH      = 32.0f;
    static constexpr float ICON_LEFT_MARGIN  = 8.0f;
    static constexpr float ICON_SIZE         = 24.0f;
    static constexpr float ICON_RIGHT_GAP    = 4.0f;
    // キャプションボタン（最小化/最大化/閉じる）はタイトルバーの全高を使う
    static constexpr float CAPTION_BTN_WIDTH = 46.0f;

    constexpr float GetHeight() const noexcept;
    void UpdateLayout(float window_width_dip) noexcept;
    TitleBarHitZone HitTest(float dip_x, float dip_y) const noexcept;

    constexpr const DipRect& GetButton(TitleBarHitZone zone) const noexcept;
    constexpr const TitleBarButtons& GetButtons() const noexcept;
    constexpr const DipRect& GetIconRect() const noexcept;
    constexpr const DipRect& GetTitleTextRect() const noexcept;
};
```

ボタン矩形はボタンごとのメンバではなく `TitleBarButtons` 配列 1 つで持ち、`TitleBarHitZone` から `TitleBarButtonIndex` で引く。ヒットテストと描画（`TitleBarRenderState::buttons` と `Button(zone)`）は同じ配列を使い、`WM_NCHITTEST` は `IsTitleBarButton` でボタンかどうかを判定する。狭幅でボタン同士が重なったときは、`HitTest` がキャプションボタン（閉じる・最大化・最小化）を優先する。

`TitleBar` はホバー状態を持たない。ホバー中のゾーンは `ViewState::hover.titlebar` が持ち、`TitleBarRenderState::hovered_zone` で描画に渡す。

#### 3.20.2 ボタン配置

```
┌────────────────────────────────────────────────────────────────────┐
│ [Icon] [Open] [Search] [Theme] [Help] ←タイトル→ [File] [TOC] [─][□][×]│
│  左側グループ                                        右側グループ        │
└────────────────────────────────────────────────────────────────────┘
```

- **左側**（アイコンの右から）: OpenFile → Search → ThemeToggle → Help
- **右側**（右端から）: Close ← Maximize ← Minimize ← TocToggle ← FileToggle
- **タイトルテキスト**: 左側グループの右端から FileToggle の左端まで

---

### 3.21 SearchState / SearchBarController — 検索

#### 3.21.1 SearchState（ドメインロジック）

```cpp
struct SearchMatch {
    int      node_index;
    uint32_t start;          // node.text（またはセル.text）内の UTF-8 byte オフセット
    uint32_t length;         // UTF-8 byte 長
    int      table_row = -1; // テーブルセル用（-1 なら通常ノード）
    int      table_col = -1;
    uint32_t start_w  = 0;   // UTF-16 code unit 範囲（描画ハイライト用）
    uint32_t length_w = 0;
};

class SearchState {
public:
    constexpr bool IsVisible() const noexcept;
    constexpr void Show() noexcept;
    void Hide() noexcept;       // visible_ = false + ClearMatches
    void Reset() noexcept;      // Hide + クエリも空にする

    constexpr uint32_t GetGeneration() const noexcept;  // 描画側のハイライトキャッシュ判定用

    constexpr const std::pmr::string&              GetQuery() const noexcept;  // UTF-8
    constexpr const std::pmr::vector<SearchMatch>& GetMatches() const noexcept;
    constexpr int GetCurrentMatchIndex() const noexcept;
    constexpr int GetMatchCount() const noexcept;

    constexpr bool IsCaseSensitive() const noexcept;
    constexpr void SetCaseSensitive(bool v) noexcept;
    constexpr void ToggleCaseSensitive() noexcept;
    constexpr bool IsHighlightEnabled() const noexcept;
    constexpr void ToggleHighlightEnabled() noexcept;

    void SetQuery(std::string_view query);
    void ExecuteSearch(const std::pmr::vector<Node>& nodes);
    bool NextMatch() noexcept;  // ラップ時 true
    bool PrevMatch() noexcept;  // ラップ時 true
    void SetCurrentMatchNear(float scroll_y, const LayoutCache& cache) noexcept;
};
```

一致件数の上限は private 定数 `MAX_MATCHES = 10000`。

`generation_` は `ExecuteSearch / Hide / Reset` のたびにインクリメントされる世代カウンタ。0 は未初期化センチネルとして扱い、32-bit ラップアラウンドで 0 に戻ったときは 1 へ補正する。

大文字小文字を無視する検索でも文書の小文字コピーは持たない。クエリだけを ASCII 小文字にし、`ascii_util::FindAsciiCaseInsensitive` が走査しながら文書側を畳み込む（畳み込むのは ASCII 英字だけ）。クエリに ASCII 英字が無ければ通常一致と結果が同じなので、畳み込みを省いて `ascii_util::Find` を使う。テーブルは `concat_text` を 1 回だけ走査し、セル区切りをまたぐヒットを除いてセル単位のマッチに振り分ける。UTF-16 位置（`start_w` / `length_w`）は `Utf16OffsetCursor` が最初のヒットから前方へ数えて求め、ノード全文を UTF-16 に変換しない。

#### 3.21.2 SearchBarController（UI制御）

検索バーのUI状態（キャレット、選択範囲、IME 組成文字列、入力欄の UTF-16 クエリ）を管理する。再描画・フォーカス・ビープは各ハンドラが受け取る `SideEffectList&` に `effect::InvalidateWindow` / `SearchFocus` / `SearchUnfocus` / `Beep` として積み、`App::Execute` に任せる。表示中は常に入力欄がフォーカスを持つので、`HasFocus()` は `SearchState::IsVisible()` を返す。ホバーは `ViewState::hover.search_bar`（`App` が `SearchBarRenderState::hovered` に詰める）、入力欄のドラッグ選択は `ViewState::drag`（`LeftDrag::SearchInput`、起点は `search_anchor`）が持つ（3.3）。

App への問い合わせは抽象インターフェース `SearchBarHost`（`search_bar_controller.h`）越しに行う。問い合わせるのは MD ペイン高さ・スクロール上限・マッチのノードの計測（`EnsureNodeLayout`）だけで、実装は `app.h` の `AppHost`（`ResourceManagerHost` と兼ねる）が App へ転送する。クラスは `search_bar_controller.h` にヘッダ内定義する。タイマーは持たない。キャレットは点滅させず、フォーカス中かつウィンドウがアクティブな間は常時表示する（点滅タイマーが約 530ms ごとに全画面再描画を起こしていたため廃止した）。`SearchBarRenderState::caret_visible` は `App::OnPaint` が `WindowState::window_active` から詰める。

```cpp
// 検索バーがマッチへスクロールするときに App へ問い合わせること。
class SearchBarHost {
public:
    virtual float MdPaneHeight() = 0;
    virtual float MdMaxScroll() = 0;
    // node_index のノードを計測済みにする (検索マッチの行位置を引けるようにする)。
    virtual void EnsureNodeLayout(int node_index) = 0;

protected:
    ~SearchBarHost() = default;
};

class SearchBarController {
public:
    constexpr void Init(SearchState& state, ViewportManager& viewport,
                        LayoutCache& cache, SearchBarHost& host) noexcept;

    // イベントハンドラ。再描画・フォーカス・ビープは effects に積む
    void OnOpen(SideEffectList& effects, const std::pmr::vector<Node>& nodes);
    void OnClose(SideEffectList& effects);
    void OnNext(SideEffectList& effects);   // 折り返したら effect::Beep
    void OnPrev(SideEffectList& effects);

    void OnTextChanged(SideEffectList& effects, std::wstring_view text, const std::pmr::vector<Node>& nodes);
    void OnToggleCaseSensitive(SideEffectList& effects, const std::pmr::vector<Node>& nodes);
    void OnToggleHighlight(SideEffectList& effects);
    void SetSelection(SideEffectList& effects, int sel_start, int sel_end);
    void SetImeComposition(SideEffectList& effects, std::wstring_view comp);

    // 検索を始めて 1 回分進め、終わったらスクロール位置に近いマッチを現在にする
    void RunSearchAndLocate(SideEffectList& effects, const std::pmr::vector<Node>& nodes, bool scroll_to_match = false);
    void OnContinue(SideEffectList& effects, const std::pmr::vector<Node>& nodes);   // SearchContinueAction

    void Reset();   // クエリも消す

    // レンダーステート構築（hovered は App が詰める）
    SearchBarRenderState BuildRenderState() const;

    // フォーカス・キャレット・IME 状態のアクセサ
    bool HasFocus() const noexcept;   // 表示中なら true
    int  GetCaretPos() const noexcept;
    int  GetSelectionStart() const noexcept;
    const std::pmr::wstring& GetImeComposition() const noexcept;
    const std::pmr::wstring& GetQueryWide() const noexcept;   // GetQuery() の UTF-16 版

private:
    // 1 回でこれを超える分は、メッセージループの空き時間 (OnContinue) へ回す
    static constexpr auto kSearchSliceBudget = std::chrono::milliseconds(8);
    void ScrollToCurrentMatch();
};
```

`AppState`（`mendo_core`）の `SearchGroup::search_bar_ctrl` として値で保持され、`App::Init` が `AppHost` を渡して `Init` する。インターフェース越しなので `mendo_core` は App に依存せず、テストは `SearchBarHost` を実装したテスト用の host（`tests/test_search_bar_controller.cpp` の `TestSearchBarHost`、`tests/reducer_harness.h` の `ReducerTestSearchBarHost`）を渡す。

`OnTextChanged` は入力が前回と同じなら何もしない（文書の切替で入力欄を空にしたときの `EN_CHANGE` など）。空になったらその場でマッチを消す。それ以外は入力のたびにすぐ検索を始める。検索は `kSearchSliceBudget`（8ms）ずつ区切って進み、残りは `App::HasIdleWork` が `SearchState::IsSearching` を見て、メッセージループの空き時間に `SearchContinueAction` で続けるので、大きな文書でも入力や描画を待たせない（デバウンスは持たない）。

途中のマッチは見せない。調べている間は前回の結果（件数・ハイライト。0 件なら赤背景も）を見せ続け、調べ終えたときにまとめて差し替える。初めての検索を調べ終えるまでは `SearchState::HasResult` が偽で、`SearchBarRenderState::total_matches` を -1 にして赤背景も件数も出さない。リロードでは `App::ReloadDocument` が `CarryOverUnchanged` の求めた先頭・末尾の一致範囲（`UnchangedEnds`）を `SearchState::KeepMatchesOfUnchangedNodes` に渡し、変わらなかったノードのマッチだけを新しい番号で残してから調べ直す。保存のたびに件数が 0 に戻ってハイライトが消えるちらつきを出さず、同期の再検索で UI を止めることもないためである。

別文書（ヘルプを含む）へ切り替えると、`App::ResetViewForNewDocument` が `Reset()` で検索バーを常に閉じ、クエリとマッチも空にしてから入力欄を空にする（`effect::SearchUnfocus{ clear_text = true }`）。旧文書のマッチ位置を新しいノードに使わないためである。

ホバー領域は `SearchBarHitZone`（`ui_constants.h`）で定義され、None / Up / Down / CaseSensitive / Highlight / Close / Input の 7 値。Input はホバー強調が無いので None と同じに扱う。

#### 3.21.3 検索バーUI

下部にドッキングされ、以下のコントロールを含む。

- テキスト入力（IME対応）
- 前へ / 次へボタン
- 大文字小文字区別トグル
- ハイライト切替トグル
- 閉じるボタン
- 一致件数表示（`X / Y`）

| 対象 | 色 | アルファ |
|:-----|:---|:--------|
| 現在の一致 | オレンジ系 (`search_highlight_current_color`) | 60%（ダーク 55%） |
| その他の一致 | 黄色系 (`search_highlight_color`) | 40%（ダーク 35%） |
| 一致なし入力背景 | 薄赤、ダークは暗い赤 (`search_no_match_bg_color`) | — |

---

### 3.22 ContextMenu — カスタムコンテキストメニュー

`ContextMenu` は Win32 のシステムメニューの代わりに、Direct2D で自前描画するカスタムコンテキストメニュー。実装は `context_menu.cpp`（mendo_core 入り）にまとまっている。

```cpp
struct ContextMenuParams {
    int   screen_x = 0;
    int   screen_y = 0;
    float dpi_scale = 1.0f;
    bool  can_go_back = false;
    bool  can_go_forward = false;
    bool  has_file = false;
    bool  has_selection = false;
    bool  dark_mode_checked = false;
    bool  file_pane_checked = false;
    bool  toc_pane_checked = false;
    bool  show_file_items = false;  // MdPaneの場合のみtrue
    const Theme* theme = nullptr;
};

class ContextMenu {
public:
    enum class ItemType : uint8_t { NavRow, Separator, Text };

    void Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory);
    int  Show(HWND owner, const ContextMenuParams& params);
    int  HitTest(float x, float y) const noexcept;     // 無効項目・区切り上は 0
    int  NavHitTest(float x, float y) const noexcept;
};
```

#### 3.22.1 メニュー構成

```
┌──────────────────────────────────┐
│ ← [戻る]  [進む] →               │  NavRow（横並び）
├──────────────────────────────────┤
│ エディタで開く                    │  ※ MdPaneのみ。ファイルあり時のみ有効
│ コピー                           │  ※ MdPaneのみ。選択あり時のみ有効
│ 書式付きコピー                    │  ※ MdPaneのみ。HTML形式、選択あり時のみ有効
├──────────────────────────────────┤  ※ MdPaneのみ
│ ☑ ダークモード                    │
├──────────────────────────────────┤
│ ☑ ファイルペイン                  │
│ ☑ 目次ペイン                     │
└──────────────────────────────────┘
```

---

### 3.23 ImageLoader — 非同期画像読み込み

WIC を使用した非同期画像読み込み。ワーカースレッドでデコードし、UIスレッドで `ID2D1Bitmap` に変換する。

```cpp
class ImageLoader {
public:
    using Callback = std::move_only_function<void()>;

    ~ImageLoader();

    // 失敗時 false; 以降の RequestLoadAsync はデコードせず失敗扱いになる。
    bool Init(IWICImagingFactory* wic = nullptr);
    // UI スレッドからのみ。デバイスロスト後の差し替えは CancelPending の後に呼ぶ。
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept;
    void InitAsync(HWND hwnd, UINT msg_id, TaskScheduler& scheduler);

    struct CachedImage {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        float width  = 0.0f;
        float height = 0.0f;
        // この幅 (物理 px) までは縮小によるぼけ無しで描ける。原寸でデコードしていれば上限なし。
        UINT sharp_width_px = UINT_MAX;
    };
    // ヒットしたら LRU を更新する。次の挿入・Clear まで有効。
    const CachedImage* FindCachedImage(const std::wstring& abs_path);
    // max_width_dip: 表示幅。これを量子化した物理 px まで縮小してデコードする。0 は原寸。
    void RequestLoadAsync(const std::wstring& abs_path, Callback on_complete, float max_width_dip = 0.0f);
    // 表示幅が広がり、キャッシュの解像度では足りなくなったか。
    bool NeedsSharperDecode(const CachedImage& image, float max_width_dip) const noexcept;
    // deadline を過ぎた分は残し、HasLeftoverUploads が真になる。
    void ProcessCompletedDecodes(std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kUploadSliceBudget);
    bool HasLeftoverUploads() const noexcept;   // UI スレッド専用
    // 始まっていないデコードを捨てる。始まったものは要求中のまま残し、結果はキャッシュに入れる。
    void DropQueued();
    // 始まったものも含めて結果を捨てる。
    void CancelPending();

    void ClearCache();             // failed_paths_ もクリア
    void ResetFailedPaths();
    size_t CacheSize() const noexcept;
    void Shutdown();

#ifdef MENDO_TESTING
    // bitmap を持たない寸法だけの項目を入れる。キャッシュ動作のテスト専用。
    void InsertCacheEntryForTest(const std::wstring& path, float width, float height);
#endif
};
```

LRU キャッシュは絶対パス → `CachedImage` で保持し、件数（最大 128、private 定数 `MAX_CACHE_ENTRIES`）とビットマップの合計バイト数（128MB、`MAX_CACHE_BYTES`）の両方で上限を掛ける。`RequestLoadAsync` は同じパスが要求中なら何もせず（`pending_paths_`）、`kMaxImageRetries`（3）回失敗したパスは要求しない。ワーカーは `TaskScheduler` 上でファイル（上限 256MB、`MAX_IMAGE_FILE_SIZE`）を読んで WIC デコードし、結果を `result_mutex_` 保護の `completed_` に積んでウィンドウへ `msg_id`（`app_msg::IMAGE_LOADED`）を投げる。UI スレッドの `ProcessCompletedDecodes` が `ID2D1Bitmap` に変換してキャッシュに入れ、要求中・失敗の記録を 1 回のロックでまとめて更新してから、最後の 1 件の `on_complete` だけを呼ぶ（どれも同じ再描画の合図なので）。1 回の転送は `kUploadSliceBudget`（4ms）までで、残した分は `HasLeftoverUploads` が真になり、メッセージループの空き時間（`App::RunIdleWork`）で続ける。新しい結果は投函で知らせるので、空き時間の判定でキューを覗くためにロックは取らない。画像の読み込みはこの非同期経路だけで、UI スレッドで原寸デコードする同期 API は持たない。

待ちの捨て方は 2 通りある。`DropQueued` はデコードを始めていない要求だけを捨て、始めたものは要求中のまま残して結果をキャッシュに入れる。キーがパスなので文書をまたいでも結果を使え、結果が届く前に同じパスを要求し直しても二重に読まない。文書の切替と大きなジャンプ（3.26）で使う。`CancelPending` は始めたものも含めて結果を捨て、要求中・失敗の記録も消す。デバイスロスト（`ClearCache` と組で使う）と終了時に使う。リロードではキャッシュを捨てず、`ResetFailedPaths` で失敗記録だけを消す。

ワーカーは表示幅（物理 px を 256 px 単位に切り上げたもの）と GPU ビットマップの最大辺に収まるよう縮小デコードする（`wic_util::ComputeDecodeSize` / `DecodeToWicBitmap`）。幅で縮めた画像は `sharp_width_px` にその幅を記録し、表示幅が広がると `NeedsSharperDecode` で読み直しを判定する。アルファを持たないフレーム（JPEG などの 24bpp BGR / 8bpp Gray）は PBGRA への形式変換より先に縮小し、JPEG デコーダの縮小デコード（`IWICBitmapSourceTransform`）を効かせる。アルファ付き形式は straight alpha のまま補間すると縁がにじむため、変換後に縮小する。使わないメタデータを先読みしないよう、デコーダは `WICDecodeMetadataCacheOnDemand` で作る。

形式は拡張子ではなく先頭バイトで判定し（`wic_util::SniffImageContainer`）、その形式のデコーダだけを使う。WIC の内容判定に任せると、HEIF・RAW や他社製コーデックなどインストール済みのどのデコーダもプロセス内で動かせてしまうため、ブラウザが表示する形式に限る。どれにも当たらなければ失敗扱いにする。

| 形式 | 先頭バイト |
|:-----|:------|
| PNG | `89 'PNG' 0D 0A 1A 0A` |
| JPEG | `FF D8 FF` |
| GIF | `'GIF8'` |
| BMP | `'BM'` |
| WebP | `'RIFF'` + 8 バイト目から `'WEBP'` |
| ICO | `00 00 01 00` |

---

### 3.24 ToastNotifier — トースト通知

一定時間だけ表示する通知。

```cpp
class ToastNotifier {
public:
    void Show(std::wstring_view message);
    void Hide() noexcept;
    bool IsVisible() const noexcept;     // メッセージが空でない
    std::wstring_view GetMessage() const noexcept;
};
```

`App::ShowToast` が `Show` と同時に `app_timer::Id::TOAST` を `TOAST_DURATION_MS`（1300ms）で仕掛け、タイマーで reducer が `Hide` する。フェードは毎フレームの全画面再描画を伴うのでせず、描き直すのは表示と消去の 2 回だけ。描画は `ToastRenderState` 経由で `DrawToastOverlay` が行う。

---

### 3.25 Tooltip — ツールチップ

Win32 の `TOOLTIPS_CLASS` を `TTF_TRACK` モードでラップし、マウスホバー時のツールチップを表示する。

```cpp
struct TooltipTarget {
    enum class Zone : uint8_t {
        None, TitleBarButton, SearchBarButton,
        FilePaneItem, FilePaneButton,
        TocPaneItem,  TocPaneButton,
        MdLink, MdImage,
        OverlayButton,   // key は OverlayButton
        NavButton,
    };

    Zone              zone = Zone::None;
    uint64_t          key  = 0;   // zone 内の対象識別子 (項目 index・ボタン種別など)
    std::pmr::wstring text;       // 対象が変わった時だけ設定すればよい

    constexpr TooltipTarget() = default;
    TooltipTarget(Zone z, uint64_t k) noexcept;
    constexpr bool SameTarget(const TooltipTarget& o) const noexcept;  // zone と key で比較 (text は見ない)
    constexpr bool IsEmpty() const noexcept;     // zone == Zone::None
};

// current と同じ対象なら fill / text を使わず、表示文字列を作らない
TooltipTarget MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, Fill&& fill);
TooltipTarget MakeTooltip(const TooltipTarget& current, TooltipTarget::Zone zone, uint64_t key, std::wstring_view text);

class Tooltip {
public:
    Tooltip() = default;
    ~Tooltip();
    Tooltip(const Tooltip&) = delete;
    Tooltip& operator=(const Tooltip&) = delete;

    void Init(HWND parent_hwnd);
    bool Update(const TooltipTarget& target);   // 対象が変わり、空でなければ true (表示中なら隠す)
    void Show();                                // 表示時点のカーソル位置に出す
    void Hide();
    void ApplyDarkMode(bool dark);
    // 隠して対象も忘れる。次のホバーで同じ対象に戻っても表示し直す。
    void Clear();

private:
    bool EnsureWindow();   // 最初の表示までウィンドウを作らない

    HWND          hwnd_   = nullptr;
    HWND          parent_ = nullptr;
    TooltipTarget current_;
    bool          visible_ = false;
    bool          dark_    = false;
};
```

`Tooltip` のウィンドウは `App::tooltip_` が持ち、`AppState` は表示中か表示待ちの対象（`InteractionState::tooltip`、`TooltipTarget`）だけを持つ。Win32 の `TOOLTIPS_CLASS` を `TTF_TRACK` モードでラップし、最大幅 600px、カーソルから 20px 下にオフセット、DPI スケーリング対応。`HWND` はヘッダで `<windows.h>` を巻き込まないよう前方宣言（`struct HWND__; using HWND = HWND__*;`）で扱う。

`TooltipTarget` の同一判定は `zone` と `key` だけで行う。`key` は表示文字列の内容ではなく対象の番号で、リンクは「ノード番号とリンクの添字」、画像はノード番号、ファイルペインの項目は「`FileExplorer` の世代と項目 index」、目次の項目は index、各ボタンはボタン種別から作る。ホバー処理（`App::BuildMdContentTooltip` / `BuildSidePaneTooltip` など）は現在の対象と異なるときだけ `text` を組み立てる（確保と UTF-8→UTF-16 変換をマウス移動のたびに行わない）。番号は文書が変わると別の対象を指すので、文書を差し替えると `InvalidateHitPositions` で現在の対象を捨てる。

ホバーの結果が別の対象なら reducer が `InteractionState::tooltip` を書き換えて `effect::ShowTooltip` を出し、`Update` が true を返したら `app_timer::Id::TOOLTIP`（`TOOLTIP_DELAY_MS` = 500ms）を仕掛け、`App::HandleTimer` がタイマーを止めて `tooltip_.Show()` する（reducer を通さない）。`Show` は保存した座標ではなく表示時点の `GetCursorPos` を使うので、待つ間に同じ対象の中でカーソルが動いても最新の位置に出る。

---

### 3.26 ResourceManager — 画像・Mermaidリソース管理

App から画像読み込み、Mermaid の描画要求、ビットマップ解放の責務を分離する。

App への依頼は `SearchBarController` と同様に抽象インターフェース `ResourceManagerHost`（`resource_manager.h`）越しに行い、実装は `app.h` の `AppHost` が App へ転送する。クラスは `resource_manager.h` にヘッダ内定義する。

```cpp
// ResourceManager が App に頼むこと。
class ResourceManagerHost {
public:
    virtual void SetTimer(app_timer::Id id, UINT ms) = 0;
    virtual void KillTimer(app_timer::Id id) = 0;
    virtual float MdContentWidth() = 0;
    virtual float MdPaneHeight() = 0;
    virtual const Theme& GetTheme() = 0;
    // 画像・図の高さが changed の範囲で変わった。見ている位置を保って Y を組み直し、再描画させる。
    virtual void RecomputeLayout(layout::HeightChangeRange changed) = 0;

protected:
    ~ResourceManagerHost() = default;
};

class ResourceManager {
public:
    // 遅延レイアウトの計測範囲も兼ねる。
    static constexpr float EVICT_BUFFER_SCREENS    = 2.0f;
    // evict 範囲より広く先読みすると、evict 直後の flush で範囲外を毎回読み直す。
    static constexpr float PREFETCH_BUFFER_SCREENS = 2.0f;
    static_assert(PREFETCH_BUFFER_SCREENS <= EVICT_BUFFER_SCREENS);

    // 依存は ResourceManagerDeps 構造体 (doc / cache / viewport / image_loader /
    // mermaid / mermaid_file_cache の 6 ポインタ) に集約して受け取る。
    constexpr void Init(const ResourceManagerDeps& deps, ResourceManagerHost& host) noexcept;

    // 画像
    int  ApplyCachedImages(bool respect_viewport = true);
    int  ApplyCachedImagesForReload();   // respect_viewport=false で全画像走査
    // リロード直後のスクロール位置計算用に、ディスクキャッシュにある図の寸法を推定高さへ上書きする。
    bool ApplyMermaidCacheHeightsForReload();
    void LoadImages();

    // Mermaid
    // 可視分を要求し、高さが変わったら再レイアウトを 1 回だけ行う。
    // 図の結果が出た通知 (MermaidRenderer の初期化時に渡すラムダ) からも呼び、待っていた図を要求し直す。
    int  RequestMermaidRenders();
    void CancelMermaidBatch();
    void ScheduleMermaidBatch();    // MERMAID_BATCH タイマー (FRAME_INTERVAL_MS) を仕掛ける
    void ProcessMermaidBatch();     // タイマーを止めて RequestMermaidRenders

    // ビットマップ管理
    void EvictOffscreenBitmaps();
    void FlushPendingResources();
    void ScheduleBitmapManage();
    void OnBitmapManageTimer();

private:
    // 再レイアウトはしない。高さが変わったノードは height_changed_ に溜まる。
    int  RequestVisibleDiagrams();
    // indices の先読み範囲を、可視部分 → 下側 → 上側の 3 区間に分ける。
    std::array<std::span<const size_t>, 3> PrioritizedSlices(std::span<const size_t> indices, float screens);
    // 前回の要求位置から EVICT_BUFFER_SCREENS 画面を越えて移動していたら、図と画像の待ちを捨てる。
    void DropQueuesIfJumped();
    // 解決結果は src 単位で覚える。開かない src も空文字列で覚える。
    const std::wstring* ResolveImagePath(std::string_view src, const std::pmr::wstring& doc_dir);
};
```

画像の実寸（読み込んだビットマップの幅・高さ）は `DiagramEntry` だけが持ち、文書モデルの `Node` には書き込まない。`ApplyCachedImages` はキャッシュに当たった画像の寸法を `DiagramEntry` に入れて表示高さを決め（`layout::BitmapDisplayHeight`、最大幅を超えるときだけアスペクト比を保って縮小）、幅変更やズームの再計測では `MeasureEntry` が `BitmapNodeHeight` でその寸法から高さを求め直す。画像の最大幅は `layout::NodeContentWidth(node, theme, content_width)` で、レイアウト計算と同じ字下げを使う。Mermaid の描画要求も `const Node&` と `DiagramEntry&` を渡すだけで、`Document` を書き換える経路は無い。

画像パスの解決結果は src をキーに覚え、文書フォルダが変わったときだけ捨てる（同じフォルダでのリロードでは解決し直さない）。リモート（`://` を含む）・ローカル以外・解決に失敗した src も空文字列で覚える。src はパーセントデコードしてから UTF-16 に変換し、相対パスは `\\?\` を外した文書フォルダ（`path_util::ToWin32Path`）と結合する。

画像と図は `PrioritizedSlices` で、先読み範囲（可視範囲 ± `PREFETCH_BUFFER_SCREENS` 画面）を見えている分 → 下側 → 上側の順に要求する。要求キューは FIFO なので、スクロール位置の復元やジャンプの直後に画面より上の分が先に処理されないようにするためである。前回の要求位置から `EVICT_BUFFER_SCREENS` 画面を越えて移動していたら（TOC ジャンプや Ctrl+End）、`DropQueuesIfJumped` が図の描画待ち（`IMermaidRenderer::DropQueued`）と画像のデコード待ち（`ImageLoader::DropQueued`）を捨てる。

図は `IMermaidRenderer::RequestRender` で要求する。結果（ビットマップかエラー）が出ていればその場で `DiagramEntry` に書かれ、高さを決める。まだなら描画が始まり、結果が出ると `MermaidRenderer` が初期化時に渡された通知で `RequestMermaidRenders` を呼び、待っていた図を要求し直させる。要求と結果はコードのハッシュで引くので、リロード・幅・テーマの変更でも出来上がった結果を使い回す（3.12）。`RequestVisibleDiagrams` 自体は再レイアウトせず、高さの変化を `height_changed_` に溜める。`RequestMermaidRenders` はその後に見ている位置を保つ再レイアウト（`ResourceManagerHost::RecomputeLayout`）を 1 回だけ行い、`FlushPendingResources` は画像の適用と合わせて再レイアウトを 1 回にまとめる。量子化した本文幅が変わると、`InvalidateMermaidForWidthChange` が新旧どちらの幅にも収まる図以外を作り直し対象にし、旧幅の描画待ちを捨てる。

可視範囲外のビットマップは解放する（前後 `EVICT_BUFFER_SCREENS` = 2 画面分は保持、`PREFETCH_BUFFER_SCREENS` = 2 画面分は先読み）。先読み範囲は evict 範囲を超えない（超えると evict 直後の flush で範囲外の画像/図を毎回読み直す）。`EVICT_BUFFER_SCREENS` は遅延レイアウトの計測範囲も兼ね、範囲外へのスクロールは `EnsureVisibleLayout` が同期計測で拾う。text_layout と表のセルは、文書の大きさによらず前後 `LAYOUT_EVICT_BUFFER_SCREENS`（8 画面）の外で捨てる。2 画面と比べてメモリに差が出ず、少し戻るだけで作り直しになるのを避けられる。小さい文書で全部残すと、1MB の文書を読み終えたときに物理メモリが約 4 割増えたため、全部残す扱いは持たない。`ScheduleBitmapManage` は前回の flush から 50ms 以上経っていれば、スクロールの停止を待たずに evict と flush を行い、`BITMAP_MANAGE` タイマー（150ms）を仕掛け直す。スクロール停止後の `OnBitmapManageTimer` は evict → 強制 flush を行い、flush でリソースを適用してレイアウトが変わったときだけ再描画を要求する（evict 対象は可視範囲外なので見た目は変わらない）。タイマー ID は `app_constants.h` の `app_timer::Id` enum で集約管理される。

別文書への切替では、`App::CancelPendingResources` が `CancelMermaidBatch` と図・画像の `DropQueued` を呼び、始まっていない要求だけを捨てる。リロードでは描画・デコードの待ちも結果も捨てない（図はコードのハッシュ、画像はパスがキーなので、そのまま使える）。編集で画像が置かれた可能性があるので、画像の失敗記録（`ResetFailedPaths`）だけを消す。

---

### 3.27 補助コンポーネント

#### 3.27.1 ScrollRestoration — スクロール位置復元

```cpp
struct ScrollRestoration {
    int   pending_restore_node    = -1;
    int   pending_restore_offset  = 0;

    constexpr bool HasNodeRestore() const noexcept;
    constexpr void SetNodeRestore(int node, int offset) noexcept;
    constexpr void ClearNodeRestore() noexcept;
};
```

セッション復元・ファイル切替時のスクロール復元情報を保持する（`src/util/ui_types.h`、`ViewState::scroll_restore`）。

#### 3.27.2 SystemCursors — カーソルハンドルキャッシュ

```cpp
struct SystemCursors {
    HCURSOR arrow   = nullptr;
    HCURSOR hand    = nullptr;
    HCURSOR ibeam   = nullptr;
    HCURSOR size_we = nullptr;

    void Load() noexcept;
};
```

`LoadCursorW` の呼び出しを初期化時に一度だけ行う（`src/app/app.h`、`App::cursors_`）。

#### 3.27.3 LastHoverPos — 同一座標ホバーの抑止

```cpp
struct LastHoverPos {
    static constexpr POINT kUnsetPos{ LONG_MIN, LONG_MIN };  // sentinel

    POINT pos = kUnsetPos;

    constexpr void Reset() noexcept;
    // 完全同一座標の連続ディスパッチを抑止
    constexpr bool IsRepeat(int px, int py) noexcept;
};
```

OS は同一座標の `WM_MOUSEMOVE` を繰り返し送ることがあるため、直前と完全に同じ座標のホバー処理だけを捨てる前段フィルタである。距離・時間による間引きやヒットテスト結果のキャッシュは行わない（`HitTestPoint` は 1 万行のコードブロックでも数〜十数 µs で、間引きはカーソル形状やリンク状態の更新を遅らせるだけだったため）。スクロール、リフローや文書の差し替え（`InvalidateHitPositions`）、ウィンドウ外への移動で reducer の `InvalidateHover` が `Reset()` し、同じ座標でも次のマウス移動で再評価される。`src/ui/last_hover_pos.h` に置き、`InteractionState::last_hover_pos` が持つ。

---

### 3.28 ユーティリティ

#### 3.28.1 LruCache — 汎用LRUキャッシュ

容量をコンパイル時に固定した `std::array` 上の循環バッファ実装（ヒープ確保なし）。順序は self-organizing list（transposition rule）で保ち、ヒットしたエントリを 1 つ前と入れ替えて前へ寄せる。新規 `Insert` は先頭位置を 1 つずらすだけの O(1) で、満杯なら末尾を捨てる。非スレッド安全。

```cpp
template <typename Key, typename Value, size_t MaxEntries>
class LruCache {
public:
    constexpr LruCache() = default;
    constexpr auto* Find(this auto& self, const Key& key);  // 無ければ nullptr。非 const ならヒットを前へ寄せる
    constexpr bool  Contains(const Key& key) const;
    constexpr void  Insert(const Key& key, Value value);
    // 合計コストが budget 以下になるまで末尾から捨てる (最低 1 件は残す)
    template <class CostFn>
    constexpr void  TrimToBudget(size_t budget, CostFn cost);
    constexpr void  Clear();
    constexpr size_t Size() const noexcept;
    constexpr bool   Empty() const noexcept;
    constexpr size_t MaxSize() const noexcept;
};
```

#### 3.28.2 UniqueResource — RAII ハンドルラッパー

ポリシーベースの汎用 RAII リソースラッパー。

```cpp
template<typename Traits>
class UniqueResource {
public:
    explicit UniqueResource(handle_t h) noexcept;
    explicit operator bool() const noexcept;
    handle_t get() const noexcept;
    handle_t release() noexcept;
    void reset(handle_t h = Traits::invalid()) noexcept;
};

// 定義済み (win_handle.h)
using UniqueHandle       = UniqueResource<HandleTraits>;        // CloseHandle (INVALID_HANDLE_VALUE)
using UniqueEventHandle  = UniqueResource<EventHandleTraits>;   // CloseHandle (nullptr)
using UniqueFindHandle   = UniqueResource<FindHandleTraits>;    // FindClose
using UniqueCoTaskMemStr = UniqueResource<CoTaskMemStrTraits>;  // CoTaskMemFree
using UniqueGlobalMem    = UniqueResource<GlobalMemTraits>;     // GlobalFree

// GMEM_MOVEABLE で確保し、ロック中の領域へ fill(void* dst) -> bool で書かせる。失敗したら空
template <class Fill>
UniqueGlobalMem AllocGlobalFilled(size_t size, Fill&& fill);
```

クリップボード関連のユーティリティは `src/util/clipboard_util.h` に分離されている。どの書き込みも、バッファを全部確保してからクリップボードを開く（確保に失敗しても既存の内容を壊さない）:

- `ClipboardSession`: RAII で `OpenClipboard` + `EmptyClipboard` / `CloseClipboard` を扱う
- `BuildGlobalZeroTerminated<CharT>(text)`: 末尾 NUL 付きの `UniqueGlobalMem` を作る
- `BuildGlobalWideFromUtf8(utf8)`: UTF-8 を GlobalAlloc 先へ直接 UTF-16 変換する（巨大な全選択コピーで中間 wstring を持たない）
- `BuildGlobalCfHtml(fragment_utf8)`: CF_HTML 形式のペイロードを GlobalAlloc 先に直接組み立てる
- `CommitClipboardGlobal(format, mem)`: `SetClipboardData` に成功したら所有権を手放す
- `WriteClipboardText(hwnd, text_utf8)`: `CF_UNICODETEXT` 書き込み
- `WriteClipboardHtml(hwnd, fragment_utf8, plain_text_utf8)`: CF_HTML + CF_UNICODETEXT を同時書き込み
- `WriteClipboardDiagram(hwnd, dib, svg_text)`: `image/svg+xml` + `CF_DIB` + `CF_UNICODETEXT` を同時書き込み（Office や Inkscape がベクタ画像として認識）。`DibTotalBytes` / `WriteDibHeader` で 32bpp トップダウンの DIB を組む

#### 3.28.3 TaskScheduler — ワーカースレッドプール

```cpp
// Mermaid / 画像デコード等で共有する scheduler のワーカー数。コア数の半分を 2〜4 に収める。
constexpr int SharedWorkerCount(unsigned int cores) noexcept;
// レイアウト計測用 scheduler のワーカー数。コア数 - 1 を 2〜8 に収める (コア数 0 は 2)。
constexpr int LayoutWorkerCount(unsigned int cores) noexcept;

class TaskScheduler {
public:
    // スレッド数を記録するだけで、ワーカーはまだ起動しない。二重 Init は最初の指定を維持する。
    void Init(int thread_count);
    // スレッドセーフ。キューが MAX_PENDING_TASKS (1024) を超えると破棄して false。
    // 初回の Post でワーカーを起動する。
    bool Post(std::move_only_function<void()> task);
    // 起動前でも Init で指定した数を返す (ParallelFor の分割数に使うため)。
    size_t WorkerCount() const noexcept;
    void Shutdown();
};
```

`App::Init` は `std::thread::hardware_concurrency()` を上の 2 関数に渡して、レイアウト用（`layout_scheduler_`）とバックグラウンド用（`scheduler_`）のワーカー数を決める。ワーカー数の規則は `task_scheduler.h` の 2 関数だけにあり、呼び出し側では計算しない。レイアウト用の上限 8 は、DirectWrite の計測が 4〜8 本で頭打ちになり、それ以上は共有ファクトリ内の競合で遅くなるため（24 コアで `test.md` x8 の全計測が 8 本 200ms、16 本 234ms）。バックグラウンド用の `Init` は `App::InitSharedScheduler` にまとめてあり、起動時の読み込み（`App::StartPreloadAsync`、`App::Init` より前に呼ばれる）と `App::Init` の両方から呼ぶ（2 回目は何もしない）。

ワーカーは初回 `Post` で指定数をまとめて起動する。レイアウト用は `Renderer::SetLayoutScheduler` が `DWriteTextMeasurer::WarmUp`（本文・コード・見出し 1 つの書式で小さなテキストレイアウトを作る）を `Post` するので、起動直後にワーカーとフォントが温まる。バックグラウンド用は起動時の読み込みや画像・Mermaid の要求が来るまで起動しない。各ワーカースレッドは `CoInitializeEx(COINIT_MULTITHREADED)` を自動呼び出しする。バックグラウンド用は `MermaidFileCache` の非同期書き込み、`FileLoadService` のバックグラウンドロードなどに使用される。

#### 3.28.4 IniParser — INIファイルパーサ

ヘッダオンリーの軽量パーサ。`[Section]` と `Key=Value` 形式をサポートし、`;` / `#` コメントを無視する。

```cpp
namespace ini {
    using IniData = std::map<std::string,
                             std::map<std::string, std::string, std::less<>>,
                             std::less<>>;

    IniData     Parse(std::string_view text);
    std::string Serialize(const IniData& data);
}
```

#### 3.28.5 i18n — 国際化

UI文字列のローカライズ。日本語 (`ja`) / 英語 (`en`) の2言語をサポート。

```cpp
namespace i18n {
    struct Strings { /* 文字列フィールド多数 */ };

    void Init(std::wstring_view config_lang) noexcept;
    const Strings& S() noexcept;
    std::wstring_view GetLangKey() noexcept;
}
```

言語は `Strings` 定義済みインスタンス（`kJa` / `kEn`）へのポインタで選択し、`std::atomic<const Strings*>` で保持する。`config_lang` が `"ja"` / `"en"` なら直接選択、空または未知の場合は `GetUserDefaultUILanguage()` から自動判定する。

#### 3.28.6 StringConvert — 文字列変換ユーティリティ

UTF-8 ↔ ワイド文字変換のヘッダオンリーユーティリティ。

```cpp
namespace string_convert {
    void Utf8ToWide(std::string_view utf8, std::pmr::wstring& out);
    std::pmr::wstring Utf8ToWide(std::string_view utf8);
    // std::string / std::pmr::string の両方に書ける
    template <typename Alloc>
    void WideToUtf8(std::wstring_view wide, std::basic_string<char, std::char_traits<char>, Alloc>& out);
    std::string WideToUtf8(std::wstring_view wide);
}
```

#### 3.28.7 Profiler — パフォーマンス計測

Tracy プロファイラベースの計測マクロ（`src/util/profiler.h`）。`MENDO_PROFILE(label)` は `ZoneNamedN(...)` に展開される。ほかにフレーム境界の `MENDO_FRAME_MARK()`、数値プロットの `MENDO_PLOT`、カウンタの `MENDO_COUNT_INC` / `MENDO_COUNT_SET`、Tracy 有効時だけ展開する `MENDO_IF_TRACY(...)`、ログの `MENDO_TRACE` / `MENDO_TRACEF` / `MENDO_STATF` を持つ。CMake オプション `-DMENDO_USE_TRACY=ON` で有効化され、既定 OFF では全マクロが no-op（`((void)0)` か空）に展開される。

---

### 3.29 UIConstants / DipRect

#### 3.29.1 UIConstants

`ui_constants.h` には UI 全体で共有される定数とヘルパー関数を集約する。描画とヒットテストで同じ矩形を使うものはここに式を置く。

| 定数グループ | 内容 |
|:------------|:-----|
| Spinner | ローディングスピナーの半径・ドット数・回転速度 |
| DPI | `DpiScaleFrom` / `DipToPixel` / `PixelToDip` |
| Table | セルパディング・ボーダー幅・ストライプのアルファ・行高さ係数 |
| NodeWidth | `MIN_NODE_CONTENT_WIDTH` と `ShrinkKeepingMinContent`（深いネストでも本文幅を残す） |
| NavButton | 戻る/進むボタンのサイズ・マージン・角丸 |
| CopyButton | コピー/保存ボタンのサイズ・マージン・間隔と `OverlayButtonRect`（図のボタンは右から保存・コピー） |
| Diagram | `MIN_DIAGRAM_PLACEHOLDER_HEIGHT` と `MermaidBitmapRect`（縮小 + 中央寄せ） |
| Scrollbar | 縦スクロールバー・ブロック横スクロールバーの位置・つまみ幅・ヒット矩形 |
| ScrollSnap | ピクセルスナップ関数 |
| PaneButton | ペイン閉じる/更新/現在のファイルの場所へ移動ボタンと見出しの矩形計算 |
| SearchBar | 検索バーの寸法と `ComputeSearchBarLayout` / `HitTestSearchBar`（`SearchBarHitZone`） |
| MouseScroll | `MOUSE_WHEEL_SCROLL_MULTIPLIER` / `HSCROLL_DIP_PER_NOTCH` などのホイール量 |

#### 3.29.2 DipRect

```cpp
struct DipRect {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

// 点包含判定はフリー関数 (任意の Rect 型に使えるテンプレート)。D2D に合わせ右辺・下辺は含まない
template <class Rect>
constexpr bool PointInRect(float x, float y, const Rect& r) noexcept;
// 右辺・下辺を含む版
template <class Rect>
constexpr bool PointInRectInclusive(float x, float y, const Rect& r) noexcept;
```

タイトルバーボタンやコンテキストメニューなど、`<d2d1.h>` を取り込まずに DIP 単位の矩形を扱う箇所で使用する（`src/util/ui_types.h`）。

---

## 4. データ構造

### 4.1 Node

ドメイン中核となるパース出力。レイアウト情報は `LayoutCache` に分離されている。md4c が UTF-8 モードで動作するため、テキストは UTF-8（`GetText()` は `std::string_view`）で保持される。

```mermaid
classDiagram
    class Node {
        -const char* text_ (UTF-8)
        -const TextRun* runs_
        -const string_view* link_urls_
        -Payload payload_
        -uint32_t text_size_
        -uint32_t run_count_
        -uint32_t source_offset_
        +int32_t blockquote_group
        +int32_t line_count
        -uint16_t link_url_count_
        +NodeType type
        +AlertType alert_type
        +int8_t quote_depth
        +int8_t quote_outer_indent
        +int8_t indent_level
        -int8_t heading_level_
        -SyntaxLanguage code_language_
        -uint8_t flags_
    }

    class Payload {
        <<union>>
        +NodeTableData* table
        +const NodeImageData* image
        +int32_t list_number
        +uint32_t alert_label_length
    }

    class NodeTableData {
        +pmr::string concat_text (SoA)
        +pmr::vector~TextRun~ all_runs
        +pmr::vector~uint32_t~ cell_text_starts / cell_run_starts
        +pmr::vector~TableAlign~ aligns
        +pmr::vector~bool~ is_header_row
        +uint32_t row_count
        +uint16_t col_count
    }

    class NodeImageData {
        +string_view src (UTF-8)
    }

    class TextRun {
        +uint32_t start
        +uint32_t length
        +int16_t link_url_index
        -uint8_t flags (kBold 等の packbit)
    }

    Node "1" --> "*" TextRun
    Node "1" *-- "1" Payload
    Payload --> "0..1" NodeTableData : Table
    Payload --> "0..1" NodeImageData : Image
```

`NodeImageData` が持つのは Markdown に書かれた `src` だけである。読み込んだ画像の幅・高さはパース結果ではないので `Node` には置かず、`LayoutCache` の `DiagramEntry` が持つ（[4.2](#42-layoutcache)）。

`Node` は自分では何も所有しない trivially copyable なビューで、`static_assert(sizeof(Node) == 64)` と `static_assert(std::is_trivially_copyable_v<Node>)` で退行を検出する。ノード種別ごとのデータは 8B の union `payload_` に 1 つだけ置き、どれを読むかは `type` で決まる（`list_number` と `alert_label_length` は `flags_` の `kListOrdered` / `kHasAlertLabel` で判定する）。Table は `Document` が `unique_ptr` で所有する `NodeTableData` を、Image は `TextArena` に置いた `NodeImageData` を指す。`heading_level_` / `code_language_` / `flags_` は末尾の 1 バイト領域に置く。`heading_level()` は Heading 以外では 0 を返し、`code_language()` は設定されない限り既定値の `SyntaxLanguage::None` を返す（設定するのは CodeBlock と Diagram だけ）。`flags_` は `kListOrdered`・`kTaskChecked`・`kHasChildBlocks`（loose list の LI が中身を子ブロックに持つ）・`kHasAlertLabel` と、長いテキストを区切ったノードの並びを表す `kContinuesPrev`・`kContinuesNext`・`kNewlineBeforeChunk`（3.6.6）を持つ。

> **Why**: ノード配列の伸長やパース後の詰め直しを memcpy で済ませるため、ノードには所有するメンバを置かない。`TextArena` のチャンク化と合わせて、100MB の文書でパース後のコミットが 162MB → 82MB に減った。

テキスト・`runs`・リンク URL はノード自身では持たず、「先頭ポインタ + 長さ / 件数」で外部の領域を指す（長さと件数は 4B / 2B 領域にまとめて 64B に収める）。原文をそのまま表示できるノードは `Document` の `raw_text_` を、加工が要るノード（SOFTBR の空白化、HTML entity、Alert など）のテキストと全ノードの `runs`・URL 配列は `Document` が持つ `TextArena` を指す。Table ノードのテキストは `NodeTableData::concat_text` 全体を指すので、表の本文も `GetText()` で取れる。`TextArena` はチャンクを積み足すだけのバンプアロケータで、個別には解放しない。チャンクは 4KB から倍々に広げて 1MB で頭打ちにし、256KB を超える要求は専用に確保する（小さな文書で大きく確保せず、使い残しを末尾のチャンク 1 個分程度に抑えるため）。URL 配列の要素と画像の `src` は `string_view` で、md4c の href / src が原文を指していればそのまま、参照定義の展開や脚注 URL のように原文の外にあるものだけ arena へコピーする。パース中はテキスト・`runs`・URL を `ParseContext` のスクラッチに貯め、ノード確定時に arena へ 1 回だけコピーするので、ノードごとの確保・解放がほぼ無く、旧文書の破棄も arena の塊を返すだけで済む。ソース位置は `uint32_t source_offset_` に明示的に持ち、`SourceOffset()` で取り出す。`TextRun` の装飾は個別 bool ではなく `uint8_t flags` に packbit され、`bold()` / `italic()` / `code()` / `strikethrough()` アクセサで判定する。

アクセサは `payload_` と `flags_` への薄いラッパーとして提供される:

```cpp
NodeTableData* table_data() noexcept;            // Table 以外・未設定は nullptr (const 版は const を返す)
const NodeImageData* image_data() const noexcept;
bool has_table() const noexcept;
bool has_image() const noexcept;
int8_t heading_level() const noexcept;           // Heading 以外は 0
SyntaxLanguage code_language() const noexcept;   // 未設定は None
int32_t list_number() const noexcept;            // 順序なしリストは 0
bool list_ordered() const noexcept;
bool task_checked() const noexcept;
bool has_child_blocks() const noexcept;
uint32_t alert_label_length() const noexcept;    // ラベルを持たない (Alert の伝播先) ノードは 0
bool continues_prev() const noexcept;            // 区切ったテキストの続き (前のノードと同じブロック)
bool continues_next() const noexcept;            // 次のノードが続き
bool newline_before_chunk() const noexcept;      // 前のノードとの間の改行を、区切るときに取り除いた
void set_list_number(int32_t number) noexcept;   // payload に書いて kListOrdered を立てる
void set_alert_label_length(uint32_t length) noexcept;
```

リロードで中身が同じノードの計測結果を引き継ぐときの照合（`FindUnchangedEnds`、`document_utils.h`。比較本体は `document_utils.cpp` 内部の `NodeContentEquals`）は、ノードが指す先の内容（テキスト・`runs`・URL・画像の `src`・表）と `flags_` を比べる。表は `NodeTableData` の既定の `operator==` で比べる。

### 4.2 LayoutCache

レイアウト情報を Node から分離して管理する。

```mermaid
classDiagram
    class LayoutCache {
        +Resize(node_count)
        +operator[](i) NodeLayoutEntry
        +Top(i) float
        +ShiftTops(from, delta)
        +FindDiagram(i) DiagramEntry*
        +EnsureDiagram(i) DiagramEntry&
        +InvalidateAllLayouts(nodes)
        +InvalidateAllDiagramBitmaps()
        +EvictTextLayouts(first_keep, last_keep)
        +EvictInvisibleTableRows(table_indices, ...)
        +TakeEntries(src, from, to, count)
        +TakeReleasedLayouts() ReleasedLayouts
        +GetEstimateBasis() EstimateBasis
        +HasDirtyNodes() bool
        -pmr::vector~float~ tops_
    }

    class NodeLayoutEntry {
        +float height
        +float cached_width / cached_height
        +bool layout_dirty
        +uint8_t measure_failures
        -unique_ptr~Aux~ aux_ (mutable)
        +text_layout() IDWriteTextLayout*
        +syntax_tokens() span~const SyntaxToken~
        +table_layout() TableLayoutData*
    }

    class Aux {
        +ComPtr~IDWriteTextLayout~ text_layout
        +float first_line_height / natural_code_width
        +pmr::vector~InlineCodeBg~ inline_code_bgs
        +pmr::vector~SyntaxToken~ syntax_tokens
        +unique_ptr~TableLayoutData~ table_layout
        +unique_ptr~SearchHlCache~ search_hl_cache
        +unique_ptr~SelectionHlCache~ selection_hl_cache
        +unique_ptr~LineWindow~ line_window
    }

    class TableLayoutData {
        +pmr::vector~ComPtr~IDWriteTextLayout~~ cell_layouts (flat)
        +pmr::vector~pmr::vector~CellInlineCodeBg~~ row_code_bgs
        +size_t col_count
        +pmr::vector~float~ col_widths / row_heights
        +pmr::vector~float~ row_cum_y / col_cum_x
        +pmr::vector~float~ natural_col_widths / cell_heights / cell_applied_widths / cell_natural_widths / cell_nowrap_heights
        +pmr::vector~uint8_t~ row_evicted
        +float last_applied_max_width
        +float natural_total_width
        +size_t live_row_begin / live_row_end
        +AllocateCells(row_count, col_count)
        +HasGeometry(row_count, col_count) bool
        +InvalidateGeometry()
        +TableWidth() float
        +ColumnStep(c) float
    }

    class DiagramEntry {
        +ComPtr~ID2D1Bitmap~ bitmap
        +float width (画像・図の実寸)
        +float height
        +PngBytes png
        +pmr::wstring error
        +uint64_t code_hash
    }

    LayoutCache "1" --> "*" NodeLayoutEntry
    LayoutCache "1" --> "*" DiagramEntry : unordered_map (遅延確保)
    NodeLayoutEntry "1" --> "0..1" Aux : unique_ptr (計測・描画時に確保)
    Aux "1" --> "0..1" TableLayoutData
```

`NodeLayoutEntry` は全ノード分並ぶので、全ノードで要る高さ・実測キャッシュ・dirty フラグだけを直に持ち (24B)、テキストレイアウトやエフェクト状態など計測・描画したノードでしか使わないものは `Aux` に置いて必要になったときに確保する。既定値を書く setter（`set_first_line_height(0.0f)` など）は `Aux` を確保しない。evict では `Aux` ごと解放し (行単位 evict するテーブルは列幅・行高さを残す)、ズーム等の全無効化ではテーブル幾何だけ残して作り直す。`IDWriteTextLayout` の解放は 1 個 2〜3µs かかり、ズームや巨大な表の evict では数千〜数十万個になるので、捨てたレイアウトは `ReleasedLayouts` に溜め、`App::RunIdleWork` が多ければワーカーへ渡して解放する。文書を差し替えたときの旧 `LayoutCache` も、D2D のビットマップを持つ図だけ UI スレッドで捨て（`ReleaseDiagrams`）、残りはワーカーで捨てる。ノードの Y 位置は `entries_` とは別の連続配列 `tops_` に持つ（SoA）ので、高さが変わったときの後続ノードの一括シフト（`ShiftTops`）がエントリ全体をなめない。CodeBlock の構文トークン（`syntax_tokens`、UTF-16 位置）は計測ワーカーが `text_layout` と一緒に作り、描画時のグリフランの色分けに使う。

遅延レイアウトで計測する dirty が残っているかは `LayoutCache` 自身が `HasDirtyNodes()` として持ち、`LayoutEngine` が計測のたびに決め直す。キャッシュと一緒に動くので、推定直後（true）のキャッシュを文書ごと差し替えても遅延レイアウトが拾う。

`TableLayoutData` は `unique_ptr` でテーブルノードのみ確保される。セルレイアウトはフラット配列 `[row * col_count + col]` で持ち、`natural_col_widths` / `cell_heights` / `cell_applied_widths` / `cell_natural_widths` などをまとめて管理する。セルのレイアウトは自然幅を測るために無限幅で作り、左揃えで列幅に収まるセルはそのまま使う（`ApplyCellWidth`。折り返さないので行組みは同じで、列幅を付けると組み直しになる）。セル単位の配列は `AllocateCells` でまとめて確保し、`cell_layouts` を最後に確保するので、途中で確保に失敗しても配列どうしのサイズは食い違わない。描画とヒットテストに要る幾何（`col_widths` / `row_heights` / `col_cum_x` / `row_cum_y`）が揃っているかは `HasGeometry` で判定する。幾何は計測完了時（`FinalizeTableLayout`）に揃い、`entry.height` だけを推定値で伸ばしたときなどは `InvalidateGeometry` で無効にして次の計測で作り直す。揃っていないテーブルは描かず、ヒットテストもしない（推定の行高さで描く線形のフォールバックは持たない）。表の総幅は `TableWidth`（`col_cum_x` の末尾）、列の刻み幅は `ColumnStep`、セル本文の原点は `TableCellTextOrigin` で求め、描画とヒットテストが同じ式を使う。インラインコードの背景は行ごとの配列 `row_code_bgs`（各行は列の昇順）で持ち、インラインコードを含むテーブルだけ `row_count` 分確保して行の計測で作り直す。描画は行の背景を列の昇順に読み進め、セルごとに行の全件を見ない。行単位 evict（`EvictInvisibleTableRows`）はセルレイアウトが生存しうる行範囲 `[live_row_begin, live_row_end)` のうち keep 範囲外の差分だけを捨て、背景は幅が変わらない限り有効なので残す。初回の計測（`MeasureTableCells`）は列幅を決めるために全セルを測るが、`kMaxKeptTableCells`（1024）を超える表はセルの自然幅と折り返さない高さ（`cell_nowrap_heights`）だけを残してレイアウトを捨て、全行を evict 済みにする（セルのレイアウトは 1 個 7KB ほどあり、5 万行 6 列の表で 2GB になる）。このとき列ごとに中身（テキストと装飾）が同じセルは最初の 1 つだけを測り、残りはその値を使う（`FindRepresentativeCells`。状態や種別のような同じ値が並ぶ表では計測が 4 割ほど縮む）。見出し行は書式が違うので対象にしない。evict 中の行の高さは、列幅に収まるセルは折り返さない高さそのもの、収まらないセルは折り返しの行数を見積もった値で、行が可視になって `RestoreEvictedTableRows` が作り直したときに実測へ揃える。

`DiagramEntry` の `width` / `height` は読み込んだ画像・描画した図の実寸で、再計測（幅変更・ズーム）でも表示高さの計算に使う。デバイスロスト時の `InvalidateAllDiagramBitmaps()` はビットマップを捨てても画像の寸法は残す。`png` はクリップボードコピー用の元 PNG データ（`PngBytes` = `shared_ptr<const pmr::vector<uint8_t>>`）。`error` は描画失敗時の表示用メッセージで、空でなければ再要求しない（リロード・幅変更・テーマ変更・デバイスロストで消えて再試行の契機になる）。`code_hash` は図のコード部分のハッシュで、結果を捨てても残し、要求し直すたびにテキストを読み直さない。`DiagramEntry` は画像/図のノードでしか使わないため、`diagrams_` は全ノード分の枠を持たず、ノード番号をキーにした `std::pmr::unordered_map<size_t, DiagramEntry>` として持つ。要素の位置はほかの要素の追加で動かないので、Mermaid の処理待ちが持つポインタも有効なまま。確保は結果を書き込む `EnsureDiagram(i)` だけが行い、読み取り・描画・破棄は未確保なら `nullptr` を返す `FindDiagram(i)` を使うので、表示していない画像/図ノードには確保しない。検索ヒットがあるノードでのみ `search_hl_cache` が確保される（フレーム間キャッシュ、`SearchState::generation_` と一致する間は HitTestTextRange を再計算しない）。検索・選択ハイライトは、本文 `text_layout` のうち可視範囲の上下に 1 画面ずつ足した行のテキスト範囲（`line_window`）を共有し、可視範囲がその窓に収まる間は引き直さない。

### 4.3 Document

```cpp
class Document {
public:
    constexpr Document() noexcept = default;
    Document(Document&&) noexcept = default;   // move-only
    Document& operator=(Document&&) noexcept = default;

    // text は LF 正規化済みの UTF-8 (FileLoader が UTF-8 のまま渡す)
    static Document FromMarkdown(std::pmr::string text, size_t byte_size,
                                 std::wstring_view path, std::stop_token stop_token = {});
    // byte_size の計算と LF 正規化を内蔵した版 (ヘルプの埋め込みリソース / テスト用)
    static Document FromMarkdown(std::pmr::string utf8, std::wstring_view path);

    constexpr const std::pmr::vector<Node>& GetNodes() const noexcept;  // 書き換え用のアクセサは持たない
    constexpr const std::pmr::wstring&      GetFilePath() const noexcept;
    bool                                    HasBackingFile() const noexcept;    // 空パスとヘルプの仮想パスは false
    constexpr const std::pmr::vector<TocEntry>& GetToc() const noexcept;   // 文書順の見出し
    constexpr bool                          IsEmpty() const noexcept;
    const RawText&                          GetRawText() const noexcept;        // リロード差分比較用 (UTF-8 ラッパ)
    constexpr size_t                        GetLoadedByteSize() const noexcept; // partial-write 検出用
    constexpr const std::pmr::wstring&      GetDirectory() const noexcept;      // キャッシュ済み参照

    // アンカー (ASCII 大文字小文字は無視) を付けた見出し、または脚注の定義 / 参照のノード番号。見つからなければ -1。
    int FindAnchorIndex(std::string_view anchor);
    constexpr const std::pmr::vector<size_t>& GetImageNodeIndices() const noexcept;
    constexpr const std::pmr::vector<size_t>& GetDiagramNodeIndices() const noexcept;
    constexpr const std::pmr::vector<size_t>& GetTableNodeIndices() const noexcept;

private:
    std::pmr::vector<Node> nodes_;
    std::unique_ptr<TextArena> text_arena_;                    // 加工したテキストと全ノードの runs・URL・画像の src
    std::pmr::vector<std::unique_ptr<NodeTableData>> tables_;  // Table ノードが指すテーブル本体
    RawText raw_text_;                                         // パース入力 (LF 正規化済み)
    // ... TOC・各種ノード番号の索引・脚注・アンカー索引など
};
```

`Document` は `FromMarkdown` で作ったら中身を変えない。別の内容を表示するときは新しい `Document` を作り、`App::ReplaceDocument` が推定高さ入りの `LayoutCache` と一緒に丸ごと差し替える。このときノード数とキャッシュのエントリ数が等しいことを assert で確かめ、以降の描画・ヒットテスト・スクロール計算はこの一致を前提にする。`Document` は move-only で、ノードは `raw_text_`・`text_arena_`・`tables_` の要素を直接指すが、どれも move で位置が変わらない（`RawText` は共有文字列、ほかは `unique_ptr`）ので、move してもポインタの付け替えは要らない。

アンカーは文書内リンクのクリック時にしか使わないため、パースでは作らない。`FindAnchorIndex` はクエリを ASCII 小文字に畳み、`HeadingAnchorIndex`（アンカー → ノード番号のハッシュ索引）を引く。無ければ TOC の続きから見出しのスラグを作って github-slugger と同じ規則で連番を付け、見つかるまでだけ索引を延ばす。索引はリンクを辿った文書にだけ作られ、内容を差し替えると捨てる。115MB・見出し 14 万件の文書では、パースが約 65ms 短くなる代わりに、存在しないアンカーを初めて引いたときだけ約 50ms・+19MB かかる。

脚注のアンカー (`user-content-fn-N` / `user-content-fnref-N[-M]`) は、パース時に作った `footnote_def_indices_`（脚注番号 → 定義ノード）と `footnote_refs_`（参照の位置）を引く。見出しの索引より先に照合するので、脚注のアンカーと同じスラグの見出しがあっても脚注が優先される。

`raw_text_` はパース入力の UTF-8 テキストを常時保持する。これはリロード時の差分比較（`AnalyzeReloadDiff`、差分位置からのスクロール量は `layout_computer.h` の `layout::CalcScrollYForDiff`）用で、ノードレベルでは検出できない編集（空行追加・末尾空白等）も UTF-8 byte オフセット精度で差分位置を求めるため。`RawText` は不変の共有文字列で持ち、非同期リロードのワーカーは `Share()` で旧テキストを受け取って、パースの前に差分を判定する。`raw_text_` は常に LF 正規化済み（`FileLoader` か 2 引数版の `FromMarkdown` が `NormalizeNewlines` を通す）なので、リロード diff の新テキストも `FileLoader` で同じ正規化を通してから比較する。これによりファイル全体の改行コードを変換して保存するエディタ（issue #273）でも差分位置が編集行を正しく指し、改行コード変換のみの保存は NoChange としてスクロール位置が維持される。

### 4.4 NodeType / AlertType

```cpp
enum class NodeType : uint8_t {
    Heading, Paragraph, CodeBlock, HorizontalRule,
    ListItem, BlockQuote, Table, TaskListItem, Image,
    Diagram  // Mermaid / LaTeX 等、ビットマップで描くコードブロック。code_language() で描画器を選ぶ
};

enum class AlertType : uint8_t {
    None = 0, Note = 1, Tip = 2, Important = 3, Warning = 4, Caution = 5
};
inline constexpr size_t ALERT_TYPE_COUNT = 5;

// テキストではなくビットマップを描くノード (Image / Diagram)
constexpr bool IsBitmapDrawNode(const Node& node) noexcept;
```

### 4.5 TextSelection

```cpp
struct TextSelection {
    int      start_node = -1;
    uint32_t start_pos  = 0;
    int      end_node   = -1;
    uint32_t end_pos    = 0;
    bool     active     = false;

    struct NodeRange { uint32_t start; uint32_t end; };
    // node_index に対応する選択範囲 [start, end)。end は text_size でクランプ済み
    constexpr NodeRange ClampedRange(int node_index, size_t text_size) const noexcept;

    constexpr void Clear() noexcept;
    static constexpr TextSelection MakeOrdered(
        int node_a, uint32_t pos_a,
        int node_b, uint32_t pos_b) noexcept;
};
```

---

## 5. 処理フロー

### 5.1 アプリケーション起動

```mermaid
flowchart TD
    START[wWinMain] --> DLL[SetDefaultDllDirectories<br>System32 限定 + d2d1 / dwrite を先に読む]
    DLL --> D3D[PrefetchD3DDevice<br>D3D11 デバイスを別スレッドで先行作成]
    D3D --> COM[CoInitializeEx<br>COINIT_APARTMENTTHREADED + COINIT_DISABLE_OLE1DDE]
    COM --> CFGLOAD[ConfigService::Load<br>settings.ini読み込み]
    CFGLOAD --> I18NINIT[i18n::Init<br>config.LoadWString General/Language]
    I18NINIT --> PLAN[PlanStartup → DecideStartupPlan<br>引数 __wargv / 前回ファイル / cwd]
    PLAN --> WIN[Win32Window window&#40;config&#41;]
    WIN --> PCHK{document_path<br>あり？}
    PCHK -->|Yes| PRELOAD[StartPreloadAsync<br>共有 scheduler で読み込み + パース + 推定]
    PCHK -->|No| PREP
    PRELOAD --> PREP[restore_scroll か restore_scroll_if_same_file で同一ファイルなら<br>RestoreScrollPosition + SetInitialDirectory]
    PREP --> CREATE[Win32Window::Create]

    CREATE --> REG[RegisterClassExW]
    REG --> WND[CreateWindowExW<br>WS_EX_ACCEPTFILES + WS_EX_NOREDIRECTIONBITMAP]
    WND --> INIT[App::Init]
    INIT --> CFG[ThemeService::LoadDarkMode<br>+ LoadZoomIndex]
    CFG --> D2D[Renderer::Init&#40;hwnd, theme&#41;<br>保存済みテーマで D2D / DWrite / WIC ファクトリ + Formats]
    D2D --> LSCHED[layout_scheduler_.Init + SetLayoutScheduler<br>WarmUp でワーカーとフォントを先に起こす]
    LSCHED --> SCHED[InitSharedScheduler<br>スレッド数の記録のみ、初回 Post で起動]
    SCHED --> FCACHE[MermaidFileCache::Init<br>索引は初回利用まで読まない]
    FCACHE --> MERM[MermaidRenderer::Init<br>WebView2 は初回描画要求で起動]
    MERM --> PANES[RestorePaneState<br>SessionService::LoadPaneState]
    PANES --> SHOWW[DWMWA_CLOAK で隠したまま<br>RestoreWindowPlacement / ShowWindow]
    SHOWW --> ATTACH{LoadInitialDocument<br>AttachPreload}

    ATTACH -->|AppliedSync| APPLY[OnParseComplete]
    ATTACH -->|AttachedAsync| WAIT[完了は PARSE_COMPLETE で合流<br>大きければスピナー]
    ATTACH -->|None| HELP[LoadHelpDocument]

    APPLY --> DEV[App::CreateRenderDevice<br>先行作成した D3D デバイスで D2D デバイス + Brushes]
    WAIT --> DEV
    HELP --> DEV
    DEV --> PEEK[PARSE_COMPLETE が届いていれば<br>OnParseComplete]
    PEEK --> SHOW[UpdateWindow → cloak 解除]
    SHOW --> LOOP[RunMessageLoop]
    LOOP --> DESTROY[Win32Window破棄<br>COMオブジェクト解放]
    DESTROY --> EXIT[CoUninitialize]
    EXIT --> END[終了]
```

`DecideStartupPlan` は、ファイル引数ならそのファイル、フォルダ引数・引数なしなら前回ファイルを `document_path` にし、実在しないパスの引数では前回ファイルを復元せずヘルプを出す。ファイルペインのフォルダは、ファイル引数ならその親、フォルダ引数ならそのフォルダ、それ以外は cwd（cwd がシステムフォルダや exe のフォルダなら前回ファイルのフォルダ）にする。ファイル引数が前回ファイルと同じ表記（大文字小文字は区別しない）ならスクロール位置を復元する（`restore_scroll`）。表記が違えば `restore_scroll_if_same_file` を立て、2 ファイルを開く `std::filesystem::equivalent` での確認はプリロードを始めた後に `wWinMain` が行う。描画デバイスの作成（`CreateRenderDevice`）は起動時の読み込みの取り込みより後に置き、デバイスを待つ間に終わった読み込みや画像デコードは最初のフレームに入れる。

### 5.2 ファイル読み込みから描画まで

```mermaid
flowchart TD
    TRIGGER[ファイルオープン<br>Ctrl+O / D&D / ファイルペイン / リンク] --> LMF[App::LoadMarkdownFile]
    LMF --> ANIM{IsAsyncLoadCandidate<br>大きいファイル？}
    ANIM -->|Yes| ASYNC[BeginAsyncLoad<br>さらに大きければローディングアニメーション]
    ANIM -->|No| SYNC[DoLoadMarkdownFile<br>UI スレッドで同期実行]
    ASYNC --> WORKER[AsyncLoadCoordinator のワーカー<br>FileLoader → Document::FromMarkdown<br>→ MakeEstimatedLayoutCache]
    WORKER --> POST[WM_APP+6 PARSE_COMPLETE ポスト]
    POST --> OPC[App::OnParseComplete<br>TakeAsync]
    SYNC --> EXEC[FileLoadService::ExecuteLoad<br>FileLoader → Document::FromMarkdown]
    EXEC --> EST[MakeEstimatedLayoutCache]

    OPC --> REPL[App::ReplaceDocument<br>Document と推定済み LayoutCache を差し替え]
    EST --> REPL
    REPL --> RESET[FinishLoadMarkdownFile<br>ResetViewForNewDocument]
    RESET --> FE[FileExplorer 更新<br>ペイン非表示中は列挙しない]
    FE --> RESTORE[RestoreScrollAfterLoadAction<br>復元先ノード or 先頭]
    RESTORE --> LAYOUT[App::RelayoutViewport ImagesAndDiagrams<br>LayoutService::ViewportLayout<br>Reestimate → ApplyScrollTarget → EnsureVisibleLayout]
    LAYOUT --> FINAL[SyncMaxScroll<br>+ LoadImages + RequestMermaidRenders]
    FINAL --> INVAL[Invalidate + SettleLayout + SyncTocActiveAndAutoScroll<br>計測待ちが残れば遅延レイアウト]
    INVAL --> PAINT[WM_PAINT → OnPaint → Renderer::Render]
```

### 5.3 描画フロー

```mermaid
sequenceDiagram
    participant ML as メッセージループ
    participant APP as App
    participant R as Renderer
    participant BE as D2DRenderBackend
    participant CG as CommandGenerator
    participant RT as ID2D1DeviceContext

    Note over ML: 前フレームの表示待ちの間は WM_PAINT を配らず待つ (期限 1 秒)
    ML->>APP: WM_PAINT → OnPaint
    Note right of APP: 最小化中は描かずに戻る
    APP->>APP: EnsureVisibleLayout (ロード中以外)
    APP->>R: Render(RenderParams)
    R->>R: PrepareChrome (タイトルバー / サイドペインのコマンド生成)
    R->>BE: AcquireFrame (ループで取れていなければ待つ)
    R->>RT: BeginDraw / Clear
    R->>RT: ExecuteDrawCommands(chrome_cmds_, rt, brushes)
    R->>CG: GenerateMdPane(nodes, cache, md_rect, scroll_y, selection, ...)
    CG-->>R: DrawCommandList
    R->>RT: ExecuteDrawCommands(cmds, rt, brushes)
    R->>R: DrawNavOverlay / DrawGestureTrail / DrawGestureOverlay
    R->>R: DrawToastOverlay / DrawSearchBar / DrawMdScrollbar
    Note right of R: ローディング中は DrawLoading がスピナーを描く。ContextMenu は別経路で描画
    R->>RT: EndDraw()
    R->>BE: Present (スワップチェーン)
```

スワップチェーンは `SetMaximumFrameLatency(1)` で、Frame Latency Waitable Object がシグナルするまで次のフレームを描かない。メッセージループはこのハンドルも `MsgWaitForMultipleObjects` で待ち、待つ間は `QS_PAINT` を外して `WM_PAINT` を取り出さず、入力などを先に配る（待つ間の入力を反映して描ける）。期限は `D2DRenderBackend::kFrameWaitTimeoutMs`（1000ms）で、GPU が止まっても抜けられる。モーダルループや `UpdateWindow` からの描画のようにループが待っていない経路では、`Renderer::BeginFrame` の `AcquireFrame` が同じ期限で待つ。

---

## 6. ビルドシステム

### 6.1 CMake 構成

```mermaid
graph TD
    ROOT[CMakeLists.txt] --> CORE[mendo_core<br>STATIC LIBRARY]
    ROOT --> EXE[mendo<br>WIN32 EXECUTABLE]
    ROOT --> TEST[mendo_tests<br>単一テストバイナリ]

    CORE --> MD4C[md4c<br>third_party]
    CORE -.->|MENDO_USE_TRACY=ON| TRACY[Tracy v0.14.1<br>FetchContent]
    EXE --> CORE
    EXE --> WV2[WebView2 SDK<br>v1.0.3967.48<br>WebView2LoaderStatic.lib]
    EXE --> RC[mendo.rc<br>リソース]
    TEST --> CORE
    TEST --> GTEST[Google Test v1.18.0<br>FetchContent]
    TEST --> SHLWAPI[shlwapi.lib]

    CORE --> D2D1[d2d1.lib]
    CORE --> DWRITE[dwrite.lib]
    CORE --> WIC_LIB[windowscodecs.lib]
    EXE --> COMCTL[comctl32.lib<br>遅延ロード]
    EXE --> DELAY[delayimp.lib<br>遅延ロード]
```

### 6.2 ビルドコマンド

**通常ビルド:**

```bash
cmake -B build
cmake --build build --config Release
```

**テストなしビルド:**

```bash
cmake -B build -DMENDO_BUILD_TESTS=OFF
cmake --build build --config Release
```

**Tracy プロファイラ有効化（既定 OFF）:**

```bash
cmake -B build -DMENDO_USE_TRACY=ON   # Tracy v0.14.1 を FetchContent で取得
```

**テスト実行:**

```bash
build/tests/Release/mendo_tests.exe --gtest_brief=1
```

### 6.3 ビルドターゲット

| ターゲット | 種別 | 説明 |
|:-----------|:-----|:-----|
| `md4c` | 静的ライブラリ | サードパーティ Markdown パーサ |
| `mendo_core` | 静的ライブラリ | テスト可能なコアロジック（WinMain・ウィンドウ・`App`・レンダラ実装を含まない） |
| `mendo` | 実行ファイル (WIN32) | メインアプリケーション |
| `mendo_tests` | テスト | 全テストを含む単一バイナリ |

`mendo_core` には **DirectWrite / WebView2 抽象化越し** のサービスが入る — `ITextMeasurer` / `IMermaidRenderer` を通して使い、`MermaidRenderer`（WebView2）と `Renderer` / `D2DRenderBackend`（Direct2D）、`App` / `Win32Window` は `mendo` 実行ファイル側に置く。`DWriteTextMeasurer` は `mendo_core` に含まれ、`dwrite.lib` への依存を持つ。

### 6.4 mermaid.min.js の埋め込み

`mermaid.min.js` は `res/mendo.rc` で `IDR_MERMAID_JS` の RCDATA として無圧縮のまま EXE に埋め込み、`WebResourceRequested` でリソースのバイト列をそのまま配信する。ビルド時の圧縮ステップや実行時の展開処理は持たず、`cabinet.lib` にも依存しない。配布は zip なので事前圧縮しても配布サイズはほぼ変わらず、展開（ワーカー起動のたびに 12〜18ms）の方が高くつくためである。

### 6.5 MSVC ビルド最適化

| オプション | 用途 |
|:----------|:-----|
| `/MP` | マルチプロセッサコンパイル |
| `/GL` | Whole Program Optimization (Release) |
| `/Gy` | Function-Level Linking (Release) |
| `/Gw` | グローバルデータの COMDAT 化。`/OPT:REF` で未使用分を除去させる (Release) |
| `/LTCG` | Link-Time Code Generation (Release) |
| `/OPT:REF` | 未参照関数/データの除去 (Release) |
| `/OPT:ICF` | 同一COMDATの統合 (Release) |
| `/MT` | CRT静的リンク（単一exe配布のため） |
| `/DELAYLOAD` | `comdlg32.dll`（ファイルダイアログでしか使わない）、`comctl32.dll`（検索の Edit とツールチップを初めて作るまで要らない）、`shell32.dll`（ドロップ・外部で開く・設定フォルダの取得の予備だけ）と、KnownDLLs に無い `d2d1` / `dwrite` / `d3d11` / `dwmapi` / `uxtheme` を遅延ロード。`/DEPENDENTLOADFLAG:0x800` と `SetDefaultDllDirectories` で System32 からだけ解決させる |

---

## 7. テスト仕様

### 7.1 テストフレームワーク

- **Google Test** v1.18.0（FetchContentで自動取得）
- 単一テストバイナリ `mendo_tests` に全テストをリンク（CTest への個別ケース登録は行っていない）

### 7.2 テスト対象範囲

```mermaid
pie title テストカバレッジ（ファイル数ベース）
    "テスト済みモジュール" : 48
    "UIコード（テスト対象外）" : 13
```

> **テスト対象外**: `main.cpp`, `window/window.cpp`, `app/app.cpp` / `app_file.cpp` / `app_mouse.cpp`（Win32/D2D ハンドラ実装・副作用の実行）, `render/renderer*.cpp`, `render/d2d_render_backend.cpp`, `mermaid/mermaid.cpp`, `ui/context_menu.cpp`（D2D 描画部分） は Win32 / Direct2D / WebView2 依存のためユニットテスト対象外。Mermaid 描画とテキスト計測はインターフェース（`IMermaidRenderer` / `ITextMeasurer`）越しに mendo_core 側でテストされ、描画コマンドは WIC ビットマップのレンダーターゲットで実行して確かめる。

### 7.3 テストソース一覧（抜粋）

| テストファイル | 対象モジュール |
|:--------------|:-------------|
| `test_parser.cpp` | Markdownパーサ |
| `test_layout.cpp` | レイアウトエンジン |
| `test_layout_cache.cpp` | LayoutCache |
| `test_text_measurer.cpp` | ITextMeasurer |
| `test_measurer_parity.cpp` | DWriteTextMeasurer 整合性 |
| `test_syntax.cpp` | シンタックスハイライト |
| `test_theme.cpp` | テーマ定義 |
| `test_theme_service.cpp` | ThemeService |
| `test_document.cpp` | Document |
| `test_document_service.cpp` | DocumentService |
| `test_document_utils.cpp` | テキスト操作 |
| `test_html_entity.cpp` | HTML 文字参照の解決 |
| `test_anchor.cpp` | アンカーID生成（`GenerateAnchorId` はテスト用ヘルパー `document_test_helpers.h` に置く）・見出しアンカー索引・脚注アンカー |
| `test_toc.cpp` | 目次生成・`FindActiveTocIndex` |
| `test_types.cpp` | 型・データ構造 |
| `test_file_loader.cpp` | FileLoader |
| `test_file_io.cpp` | ファイルI/O ユーティリティ |
| `test_file_load_service.cpp` | FileLoadService |
| `test_file_explorer.cpp` | FileExplorer |
| `test_session_service.cpp` | SessionService |
| `test_config_service.cpp` | ConfigService（設定永続化） |
| `test_ini_parser.cpp` | IniParser |
| `test_locale.cpp` | i18n ロケール |
| `test_string_convert.cpp` | UTF-8 ↔ Wide 変換 |
| `test_pane_layout.cpp` | ペインレイアウト計算 |
| `test_pane_controller.cpp` | PaneController |
| `test_viewport_manager.cpp` | ViewportManager |
| `test_scroll_restoration.cpp` | ScrollRestoration |
| `test_hit_test_service.cpp` | hit_test |
| `test_last_hover_pos.cpp` | LastHoverPos |
| `test_mouse_gesture.cpp` | MouseGesture・ジェスチャ / スワイプのオーバーレイ状態 |
| `test_swipe_detector.cpp` | SwipeDetector |
| `test_nav_history.cpp` | NavHistory |
| `test_navigation_service.cpp` | リンク解決・ナビゲーション |
| `test_nav_button_format.cpp` | ナビゲーションボタン定数 |
| `test_titlebar.cpp` | TitleBar |
| `test_context_menu.cpp` | ContextMenu |
| `test_search_state.cpp` | SearchState |
| `test_search_bar_controller.cpp` | SearchBarController |
| `test_toast_notifier.cpp` | ToastNotifier |
| `test_tooltip_target.cpp` | TooltipTarget |
| `test_ui_constants.cpp` | UIConstants |
| `test_copy_button.cpp` | コピーボタン矩形 |
| `test_image.cpp` | ImageLoader |
| `test_help.cpp` | ヘルプドキュメント |
| `test_mermaid_util.cpp` | Mermaidユーティリティ |
| `test_mermaid_file_cache.cpp` | MermaidFileCache |
| `test_app_controller.cpp` | app_controller |
| `test_reducer.cpp` | Reducer |
| `test_resource_manager.cpp` | ResourceManager |
| `test_command_executor.cpp` | CommandExecutor（`ExecuteDrawCommands`、構文色の画素一致） |
| `test_command_generator_frame.cpp` | CommandGenerator フレーム描画 |
| `test_command_generator_content.cpp` | CommandGenerator コンテンツ描画 (モック経路) |
| `test_command_generator_highlight.cpp` | CommandGenerator ハイライト描画 |
| `test_lru_cache.cpp` | LruCache |
| `test_memory_eviction.cpp` | メモリエビクション |
| `test_task_scheduler.cpp` | TaskScheduler・ワーカー数（`SharedWorkerCount` / `LayoutWorkerCount`） |
| `test_utility.cpp` | ユーティリティ関数（`utility.h`、FNV-1a ハッシュを含む） |
| `test_async_load_coordinator.cpp` | AsyncLoadCoordinator（起動時の先行読み込みと `Attach` を含む） |
| `test_parallel_measure.cpp` | 並列計測と `ProcessDirtyBatch` のダーティバッチ計測（件数上限・クリップ・計測失敗の再試行） |
| `test_clipboard_manager.cpp` | ClipboardManager |
| `test_block_h_scroll.cpp` | ブロック横スクロール |
| `test_utf8_codec.cpp` | UTF-8 コーデック |

### 7.4 主要テストケース

#### Parser テスト

- [x] 見出し（H1〜H6）
- [x] 段落・太字・斜体・取り消し線・インラインコード
- [x] リンクとURL抽出
- [x] コードブロック（言語指定あり/なし）
- [x] 順序付き/なしリスト（ネスト）
- [x] タスクリスト
- [x] テーブル（アライメント）
- [x] 引用ブロック / GitHub Alerts
- [x] 水平線
- [x] Mermaid / LaTeX ブロックの検出（`NodeType::Diagram`）
- [x] リンク URL・画像 src の文字参照の解決
- [x] 装飾 run 数の上限（4096 個、表はセルごと）

#### Reducer / Effect テスト

- [x] 各 `AppAction` に対する状態遷移
- [x] Effect 発行の妥当性（`HasEffect<T>`, `GetEffect<T>`）
- [x] スクロール／ズーム／ナビゲーションの結合動作

#### NavHistory テスト

- [x] Push / GoBack / GoForward
- [x] 履歴上限（MAX_HISTORY = 1024、超過時は最古を破棄）
- [x] 参照モデルとのランダム操作比較
- [x] ロード中（保留中）に次の移動が重なる連続操作
- [x] ファイル切替時の履歴保持

#### MermaidFileCache テスト

- [x] キャッシュの保存と読み込み（バイナリ形式 `MEMC` v1）
- [x] LRU エビクション（4096エントリ / 1GB上限）
- [x] DPR ごとのエントリ分離（DPR 不一致でも消去しない）
- [x] 索引の遅延読み込み（`Init` でディスクに触れない）と変更時のみの書き戻し

#### 検索テスト

- [x] 検索バーの表示/非表示
- [x] クエリによるテキスト検索と一致件数
- [x] 次/前の一致へのナビゲーション（ラップアラウンド）
- [x] 大文字小文字区別・ハイライト切替
- [x] テーブルセル内の検索
- [x] スクロール位置に基づく最近接一致の選択

#### ビューポートテスト

- [x] ScrollTarget による位置復元
- [x] ズームイン/アウト/リセット
- [x] 全選択/クリア
- [x] max_scroll の同期

---

## 8. DPI対応仕様

### 8.1 DPI認識レベル

**Per-Monitor DPI Awareness V2** を採用（`res/mendo.manifest` の `dpiAwareness` で指定）。

```mermaid
flowchart LR
    START[アプリ起動] --> SET[マニフェスト<br>dpiAwareness = PerMonitorV2]
    SET --> INIT[初期DPIでリソース作成]
    INIT --> MOVE{モニタ間移動？}
    MOVE -->|Yes| WM[WM_DPICHANGED<br>DpiChangedAction]
    WM --> RECREATE[DPI 反映<br>ID2D1DeviceContext::SetDpi<br>LayoutCache::NotifyDpiChanged]
    RECREATE --> RESIZE[推奨矩形へ移動・リサイズ<br>SetWindowPos]
    RESIZE --> RELAYOUT[WM_SIZE<br>ペインレイアウト再計算]
    RELAYOUT --> REPAINT[再描画]
    REPAINT --> MOVE
    MOVE -->|No| WAIT[メッセージ待ち]
    WAIT --> MOVE
```

### 8.2 DIP変換

すべての座標・サイズはDIPで管理する。

```
物理ピクセル = DIP × (DPI / 96.0)
```

`IDWriteTextLayout` も DIP 単位のため、DPI 変更そのものではブラシ・テキストフォーマットの作り直しや再計測をせず、ハイライト系のキャッシュだけを捨てる。

---

## 9. タイマーIDとカスタムメッセージ

### 9.1 タイマーID（`src/app/app_constants.h`）

```cpp
namespace app_timer {

// Win32 API に渡す際は std::to_underlying でキャスト。1 始まりのギャップなし連番。
enum class Id : UINT_PTR {
    LOADING_ANIM = 1,
    SWIPE_OVERLAY,         // 2
    TOAST,                 // 3
    TOOLTIP,               // 4
    MERMAID_BATCH,         // 5
    BITMAP_MANAGE,         // 6
    MERMAID_INIT_RETRY,    // 7
    FILE_RELOAD_DEBOUNCE,  // 8
    MERMAID_IDLE,          // 9
    DEVICE_RECREATE_RETRY, // 10
    END,  // 番兵。新規タイマーはこの直前に追加する。
};

inline constexpr UINT FRAME_INTERVAL_MS         = 16;   // ~60fps アニメーション用
inline constexpr UINT FILE_RELOAD_DEBOUNCE_MS   = 200;  // ファイル変更通知のデバウンス
inline constexpr UINT FILE_RELOAD_RETRY_MS      = 50;   // truncate→rewrite 検出後の短縮リトライ
inline constexpr UINT BITMAP_MANAGE_DELAY_MS    = 150;  // スクロール停止後のビットマップ解放/再構築
inline constexpr UINT TOAST_DURATION_MS         = 1300; // トースト表示時間
inline constexpr UINT DEVICE_RECREATE_RETRY_MS  = 500;  // 描画デバイスの作り直しに失敗した後の再試行間隔

// [kFirstTimer, kLastTimer] の範囲判定に使う
inline constexpr Id kFirstTimer = Id::LOADING_ANIM;
inline constexpr Id kLastTimer  = static_cast<Id>(std::to_underlying(Id::END) - 1);
}
```

遅延レイアウト・検索の続き・GPU 転送のように時間で区切って進める仕事はタイマーを使わない。WM_TIMER は約 15.6ms 刻みでしか発火せず 4〜8ms の仕事の合間が空くので、メッセージループが入力と描画を配り終えた空き時間に `App::RunIdleWork` で進める。

### 9.2 カスタムメッセージ（`src/app/app_constants.h`）

```cpp
namespace app_msg {
inline constexpr UINT IMAGE_LOADED   = WM_APP + 2;  // +1, +3 は欠番
inline constexpr UINT SEARCH_FOCUS   = WM_APP + 4;
inline constexpr UINT SEARCH_UNFOCUS = WM_APP + 5;
inline constexpr UINT PARSE_COMPLETE = WM_APP + 6;
inline constexpr UINT MERMAID_DISK_LOADED = WM_APP + 7;  // Mermaid PNG の worker デコード完了
inline constexpr UINT END            = WM_APP + 8;  // 上限（この値未満が有効範囲）
}

namespace app_param {
inline constexpr WPARAM SEARCH_FOCUS_SELECT_ALL    = 0;
inline constexpr WPARAM SEARCH_FOCUS_SET_CARET     = 1;  // lParam = caret (int)
inline constexpr WPARAM SEARCH_FOCUS_SET_SELECTION = 2;  // lParam = (anchor << 32) | caret
inline constexpr WPARAM SEARCH_UNFOCUS_CLOSE       = 0;
inline constexpr WPARAM SEARCH_UNFOCUS_FILE_SWITCH = 1;

LPARAM MakeSearchSelectionLParam(int anchor, int caret) noexcept;   // 32bit×2 パッキング
std::pair<int, int> UnpackSearchSelectionLParam(LPARAM lp) noexcept;
}
```

`SEARCH_FOCUS` の `WPARAM` は `SEARCH_FOCUS_SELECT_ALL` / `SEARCH_FOCUS_SET_CARET` / `SEARCH_FOCUS_SET_SELECTION` を区別する。`SEARCH_UNFOCUS` の `WPARAM` はファイル切替に伴うアンフォーカスかユーザー操作によるクローズかを区別する。非同期ロード切替閾値 `kAsyncLoadBytes`（64KB）とスピナー表示下限 `kLoadingAnimBytes`（16MB）は `src/io/file_load_service.cpp` に定義する。

---

## 10. 非機能要件

### 10.1 パフォーマンス目標

| 項目 | 目標値 |
|:-----|:------|
| 小規模ファイル（< 100行）の初回表示 | < 50ms |
| 大規模ファイル（> 10,000行）の初回表示 | < 500ms |
| スクロール時のフレームレート | 60fps |
| メモリ使用量（1万行ファイル表示時） | < 100MB |
| ファイル変更検出の応答時間 | < 500ms |

### 10.2 対応環境

| 項目 | 要件 |
|:-----|:-----|
| OS | Windows 10 1809 以降 / Windows 11（WebP 画像のデコーダ WebP Image Extension が 1809 以降のため） |
| アーキテクチャ | x64 / arm64 |
| ランタイム | WebView2 Runtime（Mermaid描画に必要） |
| コンパイラ | MSVC (C++23) |
| ビルドツール | CMake 4.2+（Visual Studio 2026 ジェネレータ） |

---

## 11. 付録

### 付録A: ファイル一覧

```
src/
├── main.cpp                       # エントリポイント (wWinMain)
├── pch.h                          # プリコンパイル済みヘッダ
├── app/                           # アプリケーション層
│   ├── app.h / app.cpp            # App 統括（Init / Dispatch / 副作用の実行 Execute / OnPaint / レイアウト / テーマ適用 / コンテキストメニュー）+ SystemCursors + AppHost (ResourceManagerHost / SearchBarHost の実装)
│   ├── app_file.cpp               # LoadMarkdownFile / BeginAsyncLoad / OnParseComplete
│   ├── app_mouse.cpp              # マウス入力ハンドラ (クリック / ホバー / ドラッグ)
│   ├── clipboard_manager.h / .cpp # ClipboardManager (コピー/PNG 保存/SVG コピー)
│   ├── app_state.h                # AppState (全状態集約) + reducer と App が導出するペイン/ブロック情報
│   ├── reload_flow.h              # 非同期ロード完了時の分岐判定 (純粋関数)
│   ├── startup_plan.h / .cpp      # 起動時の初期表示の決定
│   ├── app_events.h               # AppAction variant + app_controller の宣言
│   ├── app_constants.h            # タイマーID / WM_APP+N / WPARAM 定義
│   ├── app_controller.cpp         # app_controller (Event→Action)
│   ├── reducer.h / reducer.cpp    # Reducer (Action+State→Effects, in-place) + ドメイン別 Reduce*
│   ├── side_effect.h              # SideEffect 単一variant + PushEffect
│   └── resource_manager.h         # ResourceManager + ResourceManagerHost (ヘッダ内定義)
├── window/                        # ウィンドウ層
│   └── window.h / window.cpp      # Win32Window
├── render/                        # 描画層
│   ├── renderer.h / renderer.cpp  # Direct2D 描画統括 + タイトルバー / オーバーレイ描画
│   ├── renderer_resources.cpp     # ブラシ・フォーマット
│   ├── renderer_pane.cpp          # ペイン描画
│   ├── renderer_search.cpp        # 検索バー描画
│   ├── d2d_render_backend.h / .cpp # D2DRenderBackend (D3D11 デバイス / スワップチェーン / フレーム待ち)
│   ├── render_params.h            # レンダリングパラメータ + ResolveGestureOverlay
│   ├── draw_command.h             # DrawCommand variant + DrawCommandList (pmr::vector) + FixedBrushArray / ExecuteDrawCommands 宣言
│   ├── brush_id.h                 # ブラシ ID 定義
│   ├── text_layout_cache.h        # 繰り返し描く短い文字列のテキストレイアウト作り置き
│   ├── command_generator.h / .cpp # 描画コマンド生成 (テーブル / ハイライトを含む)
│   └── command_executor.cpp       # 描画コマンド実行 (固定ブラシ・可視グリフランの描画と構文色付け)
├── core/                          # コアドメイン
│   ├── document_types.h           # Node (64B), AlertType, NodeType, NodeTableData, TocEntry etc.
│   ├── text_types.h               # TextRun, TextSelection etc.
│   ├── text_arena.h               # TextArena (ノードのテキスト・TextRun・リンク URL を置くチャンク式バンプアロケータ)
│   ├── parser.h / parser.cpp      # Markdown パース (md4c UTF-8) + ResolveHtmlEntity 宣言
│   ├── parser_alerts.h / .cpp     # GitHub Alerts 検出
│   ├── anchor.h / anchor.cpp      # 見出しアンカー索引 + 脚注アンカーの生成・解決
│   ├── html_entities.cpp          # HTML エンティティ解決
│   ├── html_entity_table.h        # HTML5 名前付き文字参照表 (生成コード)
│   ├── unicode_lower_table.h      # 非 ASCII の単純小文字化表 (生成コード)
│   ├── document.h / document.cpp  # Document + RawText (パース入力 UTF-8 の保持)
│   ├── document_utils.h / .cpp    # テキスト操作 + リロード判定 (AnalyzeReloadDiff / DecideReload / FindNodeBySourceOffset)
│   ├── selection_html.h / .cpp    # 選択範囲の HTML 生成
│   ├── syntax.h / syntax.cpp      # シンタックスハイライト
│   └── syntax_keywords.h          # 言語別キーワード辞書
├── layout/                        # レイアウト層
│   ├── layout.h / layout.cpp      # LayoutEngine + LayoutService + ワーカー並列計測 (ViewportClip / MeasureIndicesParallel)
│   ├── layout_cache.h / .cpp      # LayoutCache (レイアウトデータ)
│   ├── layout_computer.h / .cpp   # ノード Y 位置・高さ再計算ユーティリティ (layout) + 文書位置と Y 座標の突き合わせ (FindActiveTocIndex / CalcScrollYForDiff)
│   ├── viewport_manager.h         # ViewportManager + ScrollTarget
│   ├── block_h_scroll.h           # ブロック横スクロール幾何計算 + 描画コンテキスト
│   ├── doc_dwrite_bridge.h / .cpp # Document⇔DirectWrite 計測ブリッジ
│   ├── text_measurer.h            # ITextMeasurer インターフェース (本番/モック差し替え用) + MeasureEntry
│   └── dwrite_measurer.h / .cpp   # DirectWrite 実装
├── theme/                         # テーマ層
│   ├── theme.h / theme.cpp        # Theme + theme_palette
│   ├── theme_palette.h            # 配色パレット定義
│   └── theme_service.h / .cpp     # テーマ管理サービス
├── nav/                           # ナビゲーション層
│   └── nav.h / nav.cpp            # NavHistory + LinkClickResult / HandleLinkClick
├── input/                         # 入力処理層
│   ├── hit_test_service.h / .cpp  # hit_test 名前空間の関数群 + MdPaneHitContext
│   ├── mouse_gesture.h            # MouseGesture (ヘッダオンリー)
│   └── swipe_detector.h           # SwipeDetector (ヘッダオンリー)
├── io/                            # I/O層
│   ├── file_loader.h / .cpp       # FileLoader (LoadFile)
│   ├── file_dialog_service.h / .cpp # OpenFileDialog 等
│   ├── file_watcher.h / .cpp      # ReadDirectoryChangesW 監視
│   ├── file_load_service.h / .cpp # ロード制御 + スピナーの状態 + DocumentService (FileLoader → Document)
│   ├── file_explorer.h / .cpp     # ディレクトリブラウザ
│   ├── image_loader.h / .cpp      # 非同期画像読み込み
│   ├── async_load_coordinator.h / .cpp # 非同期ロード調停 (世代カウンタ管理・起動時の先行読み込み)
│   ├── async_load_result.h        # AsyncLoadResult (Document + LayoutCache) / AsyncLoadOutcome
│   ├── ini_parser.h               # IniParser (ヘッダオンリー)
│   └── config_service.h / .cpp    # ConfigService + SessionService
├── mermaid/                       # Mermaid層
│   ├── mermaid.h / mermaid.cpp    # MermaidRenderer (WebView2)
│   ├── mermaid_renderer_interface.h # IMermaidRenderer
│   ├── mermaid_util.h / .cpp      # ヘルパー
│   └── mermaid_file_cache.h / .cpp # 永続キャッシュ (MEMC v1)
├── ui/                            # UI層 (Win32/D2D 依存ありの可視層)
│   ├── titlebar.h / titlebar.cpp  # TitleBar
│   ├── context_menu.h / .cpp      # ContextMenu (D2D描画)
│   ├── context_menu_impl.h        # 内部実装
│   ├── toast_notifier.h           # ToastNotifier (ヘッダオンリー)
│   ├── tooltip.h / tooltip.cpp    # Tooltip + TooltipTarget + ApplyDarkModeToWindow
│   ├── search_state.h / .cpp      # SearchState
│   ├── search_bar_controller.h    # SearchBarController + SearchBarHost (ヘッダ内定義)
│   ├── pane_controller.h / .cpp   # PaneController
│   └── last_hover_pos.h           # LastHoverPos (ヘッダオンリー)
└── util/                          # ユーティリティ層 (Win32 依存最小、テスト容易)
    ├── utility.h                  # 汎用ユーティリティ (ScopeGuard / FNV-1a / PngBytes / PmrFormat / overloaded)
    ├── win32_util.h               # Win32 小物 (IsKeyDown / LoadRcData / BindWindowInstance / LogHrFailure)
    ├── string_convert.h           # UTF-8 ↔ Wide 変換 (ヘッダオンリー)
    ├── utf8_codec.h               # UTF-8 低レベルコーデック
    ├── newline_util.h             # CRLF / CR → LF 正規化
    ├── file_io.h / .cpp           # ファイルI/Oヘルパー
    ├── wic_util.h                 # WIC ヘルパー
    ├── d2d_util.h                 # Direct2D ヘルパー
    ├── ascii_util.h               # ASCII 操作
    ├── lru_cache.h                # LruCache (汎用LRU)
    ├── worker_latch.h             # ワーカー完了同期ラッチ
    ├── parallel_for.h             # scheduler 上の chunk 分割並列実行
    ├── clipboard_util.h           # クリップボード補助
    ├── win_handle.h               # UniqueResource (RAII) + 定義済み traits
    ├── i18n.h                     # 国際化
    ├── resource.h                 # リソースID
    ├── ui_constants.h             # UI 定数 + DIP⇔ピクセル変換・ボタン矩形・SearchBarLayout
    ├── ui_types.h                 # DipPoint / DipRect / PaneRect / ScrollState / ScrollRestoration / OverlayButton / LeftDrag / NavDirection
    ├── pane_layout.h / .cpp       # PaneZone / PaneLayout / PaneScrollInfo / PaneLayoutCache
    ├── task_scheduler.h / .cpp    # TaskScheduler + ワーカー数 (SharedWorkerCount / LayoutWorkerCount)
    └── profiler.h                 # パフォーマンス計測
```

### 付録B: 依存ライブラリ

| ライブラリ | バージョン | 用途 | ライセンス |
|:-----------|:----------|:-----|:----------|
| md4c | 0.6.0 | Markdown パース | MIT |
| WebView2 SDK | 1.0.3967.48 | Mermaid描画用ブラウザコントロール | BSD |
| Google Test | 1.18.0 | ユニットテスト | BSD-3-Clause |
| mermaid.js | 11.15.0 | ダイアグラム描画 | MIT |
| Tracy | 0.14.1 | プロファイラ（`MENDO_USE_TRACY=ON` 時のみ） | BSD-3-Clause |

### 付録C: Markdown表示サンプル

本節は mendo の表示テストを兼ねている。

---

#### C.1 見出しレベルテスト

# 見出しレベル1
## 見出しレベル2
### 見出しレベル3
#### 見出しレベル4
##### 見出しレベル5
###### 見出しレベル6

---

#### C.2 テキスト装飾テスト

これは **太字** のテストです。
これは *斜体* のテストです。
これは ***太字かつ斜体*** のテストです。
これは ~~取り消し線~~ のテストです。
これは `インラインコード` のテストです。
これは **太字の中に *斜体* を含む**テストです。

---

#### C.3 リンクテスト

- 外部リンク: [GitHub](https://github.com)
- 内部アンカー: [はじめに](#1-はじめに)
- URLそのまま: https://example.com

---

#### C.4 リストテスト

##### 順序なしリスト

- 項目 A
  - サブ項目 A-1
  - サブ項目 A-2
    - サブサブ項目 A-2-a
    - サブサブ項目 A-2-b
  - サブ項目 A-3
- 項目 B
- 項目 C

##### 順序付きリスト

1. 第一ステップ
2. 第二ステップ
   1. サブステップ 2-1
   2. サブステップ 2-2
3. 第三ステップ

##### タスクリスト

- [x] Markdownパーサ統合 (md4c, UTF-8)
- [x] Direct2D / DirectWrite レンダリング
- [x] サービスアーキテクチャへのリファクタリング
- [x] Reducer / SideEffect による副作用の分離
- [x] AppState による状態集約
- [x] レンダーステート構築の値型化（render_params.h）
- [x] ナビゲーション機能（NavHistory + HandleLinkClick 自由関数）
- [x] マウスジェスチャ
- [x] カスタムタイトルバー
- [x] トースト通知
- [x] タッチパッドスワイプ検出
- [x] カスタムコンテキストメニュー
- [x] 非同期画像読み込み（WIC + TaskScheduler）
- [x] ヘルプドキュメント
- [x] 検索機能（インクリメンタル + ASCII の大文字小文字畳み込み）
- [x] 国際化 (i18n: ja/en)
- [x] Mermaid 永続ファイルキャッシュ（MEMC v1, DPR 量子化）
- [x] ツールチップ
- [x] Mermaidダイアグラム保存ボタン + SVG クリップボードコピー
- [x] ファイル監視（ReadDirectoryChangesW + 短縮リトライ）
- [x] SessionService によるセッション復元
- [x] SearchBarController による検索バー制御の分離
- [x] LRUキャッシュ汎用化
- [x] RAIIハンドルラッパー（UniqueResource + ClipboardSession）
- [x] パスインターン化による NavHistory メモリ削減
- [x] Node 拡張データの variant 統合（メモリ削減）
- [x] FileLoader メモリマップ + UTF-8→UTF-16 ワンパス変換
- [x] ライブリロード差分検出（AnalyzeReloadDiff）
- [ ] パフォーマンス最適化
- [ ] ドキュメント整備

---

#### C.5 引用テスト

> これは引用ブロックです。
> 複数行にわたる引用を表示できます。

> **ネストされた引用:**
>
> > 内側の引用です。
> > Direct2D は高速な2Dグラフィックスを提供します。

---

#### C.6 コードブロックテスト

##### C++ コード

```cpp
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

// Direct2D レンダーターゲットの初期化
HRESULT InitializeD2D(HWND hwnd, ID2D1HwndRenderTarget** ppRT) {
    ID2D1Factory* pFactory = nullptr;
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        &pFactory
    );

    if (SUCCEEDED(hr)) {
        RECT rc;
        GetClientRect(hwnd, &rc);

        D2D1_SIZE_U size = D2D1::SizeU(
            rc.right - rc.left,
            rc.bottom - rc.top
        );

        hr = pFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            ppRT
        );
    }

    if (pFactory) pFactory->Release();
    return hr;
}

int main() {
    std::vector<std::string> items = {"alpha", "beta", "gamma"};
    for (const auto& item : items) {
        std::cout << item << std::endl;
    }
    return 0;
}
```

##### Python コード

```python
import hashlib
from pathlib import Path
from typing import Optional

class MarkdownParser:
    """Markdownファイルのパーサクラス"""

    def __init__(self, path: str):
        self.path = Path(path)
        self._cache: dict[str, str] = {}

    def parse(self) -> list[dict]:
        """ファイルを読み込みパースする"""
        content = self.path.read_text(encoding="utf-8")
        lines = content.splitlines()

        nodes = []
        for i, line in enumerate(lines):
            if line.startswith("# "):
                nodes.append({
                    "type": "heading",
                    "level": 1,
                    "text": line[2:],
                    "line": i + 1
                })
            elif line.startswith("```"):
                pass

        return nodes

    def get_hash(self) -> Optional[str]:
        """ファイルのSHA-256ハッシュを返す"""
        if not self.path.exists():
            return None
        data = self.path.read_bytes()
        return hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    parser = MarkdownParser("README.md")
    for node in parser.parse():
        print(f"[L{node['line']}] {node['type']}: {node['text']}")
```

##### JavaScript コード

```javascript
/**
 * Mermaidダイアグラムのレンダリング
 * WebView2経由で呼び出される
 */
async function renderMermaid(code, config) {
    const { mermaid } = await import('./mermaid.esm.min.mjs');

    mermaid.initialize({
        startOnLoad: false,
        theme: config.darkMode ? 'dark' : 'default',
        securityLevel: 'strict',
        fontFamily: '"Yu Gothic UI", sans-serif',
    });

    try {
        const { svg } = await mermaid.render('diagram', code);

        const canvas = document.createElement('canvas');
        const ctx = canvas.getContext('2d');
        const img = new Image();

        return new Promise((resolve, reject) => {
            img.onload = () => {
                canvas.width = img.naturalWidth * 2;
                canvas.height = img.naturalHeight * 2;
                ctx.scale(2, 2);
                ctx.drawImage(img, 0, 0);
                resolve(canvas.toDataURL('image/png'));
            };
            img.onerror = reject;
            img.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
        });
    } catch (error) {
        console.error('Mermaid render failed:', error.message);
        return null;
    }
}
```

##### TypeScript コード

```typescript
import { readFileSync } from 'fs';
import * as path from 'path';

interface MarkdownNode {
    type: 'heading' | 'paragraph' | 'code_block';
    level?: number;
    text: string;
    language?: string;
}

type ParseResult = {
    nodes: MarkdownNode[];
    metadata: Record<string, unknown>;
};

class MarkdownLoader {
    private cache = new Map<string, ParseResult>();

    constructor(private readonly basePath: string) {}

    async load(filename: string): Promise<ParseResult> {
        const fullPath = path.join(this.basePath, filename);
        const cached = this.cache.get(fullPath);
        if (cached !== undefined) {
            return cached;
        }

        const content: string = readFileSync(fullPath, 'utf-8');
        const nodes: MarkdownNode[] = this.parse(content);
        const result: ParseResult = { nodes, metadata: {} };
        this.cache.set(fullPath, result);
        return result;
    }

    private parse(content: string): MarkdownNode[] {
        const lines = content.split('\n');
        const nodes: MarkdownNode[] = [];
        for (const line of lines) {
            if (line.startsWith('# ')) {
                nodes.push({ type: 'heading', level: 1, text: line.slice(2) });
            } else if (line.trim().length > 0) {
                nodes.push({ type: 'paragraph', text: line });
            }
        }
        return nodes;
    }
}

const loader = new MarkdownLoader('./docs');
loader.load('README.md').then((result) => {
    console.log(`ノード数: ${result.nodes.length}`);
});
```

##### Go コード

```go
package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

type MarkdownNode struct {
	Type  string
	Level int
	Text  string
}

func ParseMarkdown(content string) []MarkdownNode {
	var nodes []MarkdownNode
	scanner := bufio.NewScanner(strings.NewReader(content))

	for scanner.Scan() {
		line := scanner.Text()
		switch {
		case strings.HasPrefix(line, "# "):
			nodes = append(nodes, MarkdownNode{
				Type:  "heading",
				Level: 1,
				Text:  line[2:],
			})
		case strings.HasPrefix(line, "```"):
			lang := strings.TrimPrefix(line, "```")
			nodes = append(nodes, MarkdownNode{
				Type: "code_fence",
				Text: lang,
			})
		default:
			if len(strings.TrimSpace(line)) > 0 {
				nodes = append(nodes, MarkdownNode{
					Type: "paragraph",
					Text: line,
				})
			}
		}
	}
	return nodes
}

func main() {
	data, err := os.ReadFile("README.md")
	if err != nil {
		fmt.Fprintf(os.Stderr, "読み込みエラー: %v\n", err)
		os.Exit(1)
	}

	nodes := ParseMarkdown(string(data))
	for i, node := range nodes {
		fmt.Printf("[%d] %s: %s\n", i, node.Type, node.Text)
	}
}
```

##### Rust コード

```rust
use std::fs;
use std::io::{self};
use std::path::Path;
use std::collections::HashMap;

#[derive(Debug, Clone)]
enum NodeType {
    Heading(u8),
    Paragraph,
    CodeBlock(String),
}

#[derive(Debug, Clone)]
struct MarkdownNode {
    node_type: NodeType,
    text: String,
}

struct Parser {
    cache: HashMap<String, Vec<MarkdownNode>>,
}

impl Parser {
    fn new() -> Self {
        Parser { cache: HashMap::new() }
    }

    fn parse_file(&mut self, path: &Path) -> io::Result<&Vec<MarkdownNode>> {
        let key = path.to_string_lossy().to_string();
        if !self.cache.contains_key(&key) {
            let content = fs::read_to_string(path)?;
            let nodes = self.parse(&content);
            self.cache.insert(key.clone(), nodes);
        }
        Ok(self.cache.get(&key).unwrap())
    }

    fn parse(&self, content: &str) -> Vec<MarkdownNode> {
        let mut nodes = Vec::new();
        for line in content.lines() {
            if let Some(text) = line.strip_prefix("# ") {
                nodes.push(MarkdownNode {
                    node_type: NodeType::Heading(1),
                    text: text.to_string(),
                });
            } else if !line.trim().is_empty() {
                nodes.push(MarkdownNode {
                    node_type: NodeType::Paragraph,
                    text: line.to_string(),
                });
            }
        }
        nodes
    }
}

fn main() -> io::Result<()> {
    let mut parser = Parser::new();
    let path = Path::new("README.md");
    let nodes = parser.parse_file(path)?;

    for (i, node) in nodes.iter().enumerate() {
        println!("[{}] {:?}: {}", i, node.node_type, node.text);
    }
    Ok(())
}
```

##### Bash コード

```bash
#!/bin/bash
# Markdownファイルの見出しを抽出するスクリプト

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly DEFAULT_EXT="*.md"

usage() {
    echo "Usage: $(basename "$0") [-d directory] [-l level] [-h]"
    echo "  -d  検索ディレクトリ"
    echo "  -l  見出しレベル (1-6)"
    echo "  -h  ヘルプ"
    exit 0
}

extract_headings() {
    local file="$1"
    local level="${2:-}"

    if [[ ! -f "$file" ]]; then
        echo "エラー: ファイルが見つかりません: $file" >&2
        return 1
    fi

    local pattern="^#{1,6} "
    if [[ -n "$level" ]]; then
        pattern="^#{${level}} "
    fi

    grep -nE "$pattern" "$file" | while IFS=: read -r lineno content; do
        local depth="${content%%[! #]*}"
        depth="${#depth}"
        printf "  L%-4d [H%d] %s\n" "$lineno" "$depth" "${content#*# }"
    done
}

main() {
    local dir="."
    local level=""

    while getopts "d:l:h" opt; do
        case "$opt" in
            d) dir="$OPTARG" ;;
            l) level="$OPTARG" ;;
            h) usage ;;
            *) usage ;;
        esac
    done

    echo "=== Markdown見出し一覧 ==="
    find "$dir" -name "$DEFAULT_EXT" -type f | sort | while read -r file; do
        echo ""
        echo "📄 $file"
        extract_headings "$file" "$level"
    done
}

main "$@"
```

##### PowerShell コード

```powershell
#Requires -Version 5.1
<#
.SYNOPSIS
    Markdownファイルの解析ユーティリティ
.DESCRIPTION
    指定ディレクトリ内のMarkdownファイルから見出しとリンクを抽出する
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Path = ".",

    [ValidateRange(1, 6)]
    [int]$HeadingLevel = 0,

    [switch]$IncludeLinks
)

class MarkdownInfo {
    [string]$File
    [System.Collections.Generic.List[hashtable]]$Headings
    [System.Collections.Generic.List[hashtable]]$Links

    MarkdownInfo([string]$file) {
        $this.File = $file
        $this.Headings = [System.Collections.Generic.List[hashtable]]::new()
        $this.Links = [System.Collections.Generic.List[hashtable]]::new()
    }
}

function Get-MarkdownHeadings {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, ValueFromPipeline)]
        [string]$FilePath,

        [int]$Level = 0
    )

    process {
        $info = [MarkdownInfo]::new($FilePath)
        $lineNum = 0

        foreach ($line in Get-Content -Path $FilePath -Encoding UTF8) {
            $lineNum++

            if ($line -match '^(#{1,6})\s+(.+)$') {
                $depth = $Matches[1].Length
                if ($Level -eq 0 -or $depth -eq $Level) {
                    $info.Headings.Add(@{
                        Line  = $lineNum
                        Level = $depth
                        Text  = $Matches[2]
                    })
                }
            }

            if ($line -match '\[([^\]]+)\]\(([^)]+)\)') {
                $info.Links.Add(@{
                    Line = $lineNum
                    Text = $Matches[1]
                    Url  = $Matches[2]
                })
            }
        }

        Write-Output $info
    }
}

$mdFiles = Get-ChildItem -Path $Path -Filter "*.md" -Recurse -File
Write-Host "=== Markdown解析結果 ===" -ForegroundColor Cyan

foreach ($file in $mdFiles) {
    $result = $file.FullName | Get-MarkdownHeadings -Level $HeadingLevel
    Write-Host "`n$($result.File)" -ForegroundColor Green

    foreach ($h in $result.Headings) {
        $indent = "  " * $h.Level
        Write-Host "${indent}[H$($h.Level)] L$($h.Line): $($h.Text)"
    }

    if ($IncludeLinks -and $result.Links.Count -gt 0) {
        Write-Host "  --- リンク ---" -ForegroundColor Yellow
        foreach ($link in $result.Links) {
            Write-Host "  L$($link.Line): [$($link.Text)]($($link.Url))"
        }
    }
}
```

##### Cmd (バッチ) コード

```cmd
@echo off
rem Markdownファイルの見出しを簡易抽出するバッチスクリプト
setlocal enabledelayedexpansion

set "TARGET_DIR=%~1"
if "%TARGET_DIR%"=="" set "TARGET_DIR=."

echo === Markdown 見出し抽出ツール ===
echo 対象ディレクトリ: %TARGET_DIR%
echo.

set "FILE_COUNT=0"
set "HEADING_COUNT=0"

for /r "%TARGET_DIR%" %%f in (*.md) do (
    set /a FILE_COUNT+=1
    echo ファイル: %%f
    set "LINE_NUM=0"

    for /f "usebackq delims=" %%l in ("%%f") do (
        set /a LINE_NUM+=1
        set "LINE=%%l"

        if "!LINE:~0,2!"=="# " (
            echo   L!LINE_NUM!: [H1] !LINE:~2!
            set /a HEADING_COUNT+=1
        )
        if "!LINE:~0,3!"=="## " (
            echo   L!LINE_NUM!: [H2] !LINE:~3!
            set /a HEADING_COUNT+=1
        )
        if "!LINE:~0,4!"=="### " (
            echo   L!LINE_NUM!: [H3] !LINE:~4!
            set /a HEADING_COUNT+=1
        )
    )
    echo.
)

echo === 結果 ===
echo ファイル数: %FILE_COUNT%
echo 見出し数:   %HEADING_COUNT%

endlocal
exit /b 0
```

##### JSON コード

```json
{
  "name": "mendo",
  "version": "0.1.0",
  "description": "Direct2D / DirectWrite による高速 Markdown ビュアー",
  "platform": {
    "os": "Windows",
    "minVersion": "10.0.17763",
    "architecture": ["x64"]
  },
  "build": {
    "language": "C++23",
    "compiler": "MSVC",
    "system": "CMake",
    "minCmakeVersion": "3.20"
  },
  "features": {
    "rendering": true,
    "syntaxHighlight": true,
    "mermaid": true,
    "search": true,
    "darkMode": true,
    "i18n": ["ja", "en"],
    "experimental": null
  },
  "dependencies": [
    { "name": "md4c",        "version": "latest",       "license": "MIT" },
    { "name": "WebView2",    "version": "1.0.2903.40",  "license": "BSD" },
    { "name": "WIL",         "version": "1.0.240803.1", "license": "MIT" },
    { "name": "Google Test", "version": "1.18.0",       "license": "BSD-3-Clause" }
  ],
  "performance": {
    "smallFileMs": 50,
    "largeFileMs": 500,
    "targetFps": 60,
    "memoryBudgetMB": 100,
    "fileSizeLimitGB": 4
  },
  "zoomSteps": [
    0.25, 0.33, 0.50, 0.67, 0.75, 0.80, 0.90,
    1.00,
    1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00, 5.00
  ]
}
```

##### JSONC コード（コメント・末尾カンマ対応）

```jsonc
{
  // タイマーIDの一覧（src/app/app_constants.h と一致）
  "timers": {
    "LOADING_ANIM":          1,
    "SWIPE_OVERLAY":         2,
    "TOAST":                 3,
    "TOOLTIP":               4,
    "MERMAID_BATCH":         5,
    "BITMAP_MANAGE":         6,
    "MERMAID_INIT_RETRY":    7,
    "FILE_RELOAD_DEBOUNCE":  8,
    "MERMAID_IDLE":          9,
    "DEVICE_RECREATE_RETRY": 10,
  },
  /* カスタム Win32 メッセージ
     WM_APP+N の形式で識別子を採番している (+1, +3 は欠番) */
  "messages": {
    "IMAGE_LOADED":        "WM_APP+2",
    "SEARCH_FOCUS":        "WM_APP+4",
    "SEARCH_UNFOCUS":      "WM_APP+5",
    "PARSE_COMPLETE":      "WM_APP+6",
    "MERMAID_DISK_LOADED": "WM_APP+7",
  },
}
```

##### Java コード

```java
import java.util.List;

/** 行数を数える */
public final class LineCounter {
    private static final long LIMIT = 1_000_000L;

    @Override
    public String toString() {
        return """
            LineCounter(limit=%d)
            """.formatted(LIMIT);
    }

    public static int count(List<String> lines) {
        int n = 0;
        for (String line : lines) {
            if (!line.isBlank()) n++;
        }
        return n;
    }
}
```

##### C# コード

```csharp
#region Paths
namespace Mendo.Tools;

public record DocEntry(string Path, int Lines);

public static class DocScanner
{
    private const string Root = @"C:\Users\docs\";

    public static async Task<List<DocEntry>> ScanAsync(string pattern = "*.md")
    {
        var result = new List<DocEntry>();
        foreach (var file in Directory.EnumerateFiles(Root, pattern))
        {
            var lines = await File.ReadAllLinesAsync(file);
            result.Add(new DocEntry(file, lines.Length));
        }
        decimal ratio = 0.5m;
        Console.WriteLine($"found {result.Count} files ({ratio})");
        return result;
    }
}
#endregion
```

##### Kotlin コード

```kotlin
data class Heading(val level: Int, val text: String)

fun parseHeadings(lines: List<String>): List<Heading> =
    lines.filter { it.startsWith("#") }
        .map { line ->
            val level = line.takeWhile { it == '#' }.length
            Heading(level, line.drop(level).trim())
        }

val template = """
    # ${'$'}title
""".trimIndent()
```

##### Swift コード

```swift
import Foundation

struct Heading {
    let level: Int
    var text: String
}

#if DEBUG
let verbose = true
#endif

func parse(_ line: String) -> Heading? {
    guard line.hasPrefix("#") else { return nil }
    let level = line.prefix { $0 == "#" }.count
    return Heading(level: level, text: String(line.dropFirst(level)))
}
```

##### PHP コード

```php
<?php
declare(strict_types=1);

#[Route('/docs/{name}')]
function renderDoc(string $name): string
{
    // 拡張子を補う
    $path = __DIR__ . "/docs/{$name}.md"; # 相対パス
    if (!file_exists($path)) {
        throw new RuntimeException("not found: $name");
    }
    return htmlspecialchars(file_get_contents($path));
}
```

##### Ruby コード

```ruby
=begin
見出しを抽出するスクリプト
=end
require 'set'

class HeadingExtractor
  attr_reader :levels

  def initialize(levels = 1..3)
    @levels = levels
  end

  def extract(text)
    text.each_line.filter_map do |line|
      next unless line.start_with?('#') # 見出し行だけ
      level = line[/\A#+/].size
      [level, line.strip] if @levels.include?(level)
    end
  end
end

puts HeadingExtractor.new.extract(File.read("README.md")).inspect
```

##### SQL コード

```sql
-- 最近更新されたドキュメント
CREATE TABLE documents (
    id INTEGER PRIMARY KEY,
    path VARCHAR(260) NOT NULL UNIQUE,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

/* 直近 10 件 */
SELECT d.path, COUNT(h.id) AS headings
FROM documents d
LEFT JOIN headings h ON h.doc_id = d.id
WHERE d.updated_at > '2026-01-01' AND `d`.`path` LIKE '%.md'
GROUP BY d.path
ORDER BY headings DESC
LIMIT 10;
```

##### YAML コード

```yaml
# GitHub Actions のワークフロー
name: build
on:
  push:
    branches: [main, "release/*"]
env:
  BUILD_TYPE: Release
  RETRY: 3
  VERBOSE: false
defaults: &defaults
  shell: pwsh
jobs:
  build:
    runs-on: windows-latest
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@v4
      - name: Configure
        <<: *defaults
        run: |
          cmake -B build
          cmake --build build --config Release
      - name: Test
        run: build/tests/Release/mendo_tests.exe --gtest_brief=1 # 簡潔な出力
```

##### TOML コード

```toml
# Cargo.toml
[package]
name = "mendo-tools"
version = "0.1.0"
edition = "2021"
released = 2026-10-17T09:00:00+09:00
description = """
Markdown を扱う
補助ツール群
"""

[dependencies]
serde = { version = "1.0", features = ["derive"] }
regex = "1"

[[bin]]
name = 'mendo-scan'
path = 'src/main.rs'
test = false
```

##### INI コード

```ini
; mendo の設定例
[window]
width = 1280
height = 800
maximized = false

[font]
family = "Yu Gothic UI"
size: 14
```

##### HTML コード

```html
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <title>mendo &amp; Markdown</title>
  <style>
    body { margin: 0 auto; max-width: 960px; }
    .note:hover { color: #0969da !important; }
  </style>
</head>
<body>
  <!-- 本文 -->
  <p class="note" data-id=42>Hello&nbsp;world</p>
  <input type="checkbox" checked disabled>
  <script>
    const items = document.querySelectorAll(".note");
    for (const el of items) {
      el.addEventListener("click", () => console.log(el.dataset.id));
    }
  </script>
</body>
</html>
```

##### XML コード

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project Sdk="Microsoft.NET.Sdk">
  <!-- ビルド設定 -->
  <PropertyGroup>
    <TargetFramework>net9.0</TargetFramework>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <None Include="docs\**\*.md" CopyToOutputDirectory="PreserveNewest" />
  </ItemGroup>
  <Description><![CDATA[a < b && c > d]]></Description>
</Project>
```

##### CSS コード

```css
/* ライトテーマ */
@import url("base.css");

:root {
  --accent: #0969da;
}

.markdown-body h1,
.markdown-body h2::after {
  border-bottom: 1px solid rgba(0, 0, 0, 0.1);
  margin: 1.5em 0 16px;
}

@media (max-width: 600px) {
  .markdown-body {
    padding: 0 8px;
    background: url(data/bg.png) no-repeat;
    font-family: "Segoe UI", sans-serif !important;
  }
}
```

##### SCSS コード

```scss
$accent: #0969da;
$radius: 6px;

// ボタン
.button {
  border-radius: $radius;
  color: $accent;

  &:hover {
    color: darken($accent, 10%);
  }

  &--primary {
    background: $accent;
  }
}
```

##### diff コード

```diff
diff --git a/src/core/syntax.cpp b/src/core/syntax.cpp
index 1a2b3c4..5d6e7f8 100644
--- a/src/core/syntax.cpp
+++ b/src/core/syntax.cpp
@@ -496,6 +496,8 @@ SyntaxLanguage DetectLanguage(std::string_view info_string) noexcept
         { "json", SyntaxLanguage::Json },
-        { "mermaid", SyntaxLanguage::Mermaid },
+        { "mermaid", SyntaxLanguage::Mermaid },
+        { "math", SyntaxLanguage::LatexMath },
     };
\ No newline at end of file
```

##### Dockerfile コード

```dockerfile
# syntax=docker/dockerfile:1
FROM node:20-alpine AS build
WORKDIR /app
COPY package*.json ./
RUN npm ci && \
    npm run build

FROM nginx:1.27
ARG PORT=8080
ENV NGINX_PORT=${PORT}
COPY --from=build /app/dist /usr/share/nginx/html
EXPOSE 8080
CMD ["nginx", "-g", "daemon off;"]
```

##### Makefile コード

```makefile
# ビルド設定
CXX      := cl
CXXFLAGS ?= /O2 /std:c++latest
SRCS     := $(wildcard src/*.cpp)
OBJS      = $(SRCS:.cpp=.obj)

.PHONY: all clean

all: mendo.exe

mendo.exe: $(OBJS)
	$(CXX) $(CXXFLAGS) /Fe$@ $^

%.obj: %.cpp
	@echo "compiling $<"
	$(CXX) $(CXXFLAGS) /c $< /Fo$@

ifeq ($(OS),Windows_NT)
RM = del /q
else
RM = rm -f
endif

clean:
	$(RM) $(OBJS) mendo.exe
```

##### Zig コード

```zig
const std = @import("std");

/// 見出しの深さを数える
pub fn headingLevel(line: []const u8) ?u8 {
    var level: u8 = 0;
    for (line) |c| {
        if (c != '#') break;
        level += 1;
    }
    return if (level == 0) null else level;
}

pub fn main() !void {
    const help =
        \\usage: mendo-scan <file>
        \\  "-v" で詳細表示
    ;
    const n: u32 = @intCast(help.len);
    std.debug.print("{d} bytes, max = 0x{X}\n", .{ n, 1_000 });
}
```

##### Haskell コード

```haskell
{-# LANGUAGE OverloadedStrings #-}
module Main where

import qualified Data.Map as M

-- | 見出しを数える
data Heading = Heading { level :: Int, title :: String }
  deriving (Show, Eq)

countHeadings :: [String] -> M.Map Int Int
countHeadings = foldl' step M.empty
  where
    step acc' line = M.insertWith (+) (depth line) 1 acc'
    depth = length . takeWhile (== '#')

{- 入れ子の {- コメント -} も
   ひとまとまり -}
main :: IO ()
main = do
  let xs = ["# a", "## b", "## c"]
  print (countHeadings xs)
  print ('\n', "done")
```

##### Clojure コード

```clojure
(ns mendo.toc
  (:require [clojure.string :as str]))

(defn heading-level
  "見出しの深さを返す。見出しでなければ nil。"
  [line]
  (let [n (count (take-while #(= % \#) line))]
    (when (pos? n) n)))

#_(println "debug")
(def headings (filter heading-level ["# a" "text" "## b"]))
(println (str/join \newline headings) #"\d+" {:count 2})
```

##### Emacs Lisp コード

```elisp
;;; mendo.el --- mendo で開く  -*- lexical-binding: t -*-

(defcustom mendo-executable "mendo.exe"
  "mendo の実行ファイル。"
  :type 'string)

(defun mendo-open-current-file (&optional arg)
  "現在のファイルを mendo で開く。"
  (interactive "P")
  (let ((file (buffer-file-name)))
    (when (and file (not arg))
      (start-process "mendo" nil mendo-executable file))))

(define-key global-map (kbd "C-c m") #'mendo-open-current-file)
(insert ?a ?\n)
```

##### Common Lisp コード

```lisp
(defpackage :mendo-toc
  (:use :cl))
(in-package :mendo-toc)

#| 見出しの一覧を作る
   #| 入れ子のコメント |# |#
(defun heading-level (line)
  "LINE が見出しなら深さを返す。"
  (let ((n (or (position #\# line :test-not #'char=) (length line))))
    (if (plusp n) n nil)))

(defun collect-headings (lines &key (max-level 3))
  (loop for line in lines
        for level = (heading-level line)
        when (and level (<= level max-level))
          collect (cons level line)))

(format t "~a~%" (collect-headings '("# a" "## b") :max-level #x2))
```

##### Scheme / Racket コード

```racket
#lang racket
;; 見出しの深さを数える
(define (heading-level line)
  (let loop ([i 0])
    (cond
      [(and (< i (string-length line))
            (char=? (string-ref line i) #\#))
       (loop (add1 i))]
      [else i])))

#;(displayln "無効化された式")
(define headings
  (filter (λ (l) (> (heading-level l) 0)) '("# a" "text" "## b")))
(displayln (list headings #t 1/2 #:sep ", "))
```

##### Lua コード

```lua
--[[ Neovim の設定例
     mendo で Markdown を開く ]]
local M = {}

local function open_in_mendo(path)
  local cmd = { "mendo.exe", path or vim.fn.expand("%:p") }
  vim.fn.jobstart(cmd, { detach = true })
end

function M.setup(opts)
  opts = opts or {}
  for key, value in pairs(opts) do
    print(key, value) -- 設定の確認
  end
  vim.keymap.set("n", "<leader>m", open_in_mendo, { desc = [[mendo で開く]] })
  return #opts == 0 and nil or 0x10
end

return M
```

##### Vim script コード

```vim
" mendo 連携
set nocompatible
set number          " 行番号を表示
let g:mendo_path = 'C:\Tools\mendo.exe'

function! s:OpenInMendo(...) abort
  let l:file = a:0 > 0 ? a:1 : expand('%:p')
  call job_start([g:mendo_path, l:file])
endfunction

command! -nargs=? Mendo call s:OpenInMendo(<f-args>)
nnoremap <silent> <leader>m :Mendo<CR>

augroup mendo
  autocmd!
  autocmd FileType markdown setlocal spell | echo "spell on"
augroup END

if has('nvim')
  lua << EOF
  require('mendo').setup({ auto_open = false })
EOF
endif
```

##### Vim9 script コード

```vim
vim9script
# mendo 連携 (Vim9)
var count: number = 0

def OpenInMendo(file: string = expand('%:p')): void
  count += 1
  job_start(['mendo.exe', file])  # バックグラウンドで起動
enddef

nnoremap <silent> <leader>m <ScriptCmd>OpenInMendo()<CR>
```

---

#### C.7 テーブルテスト

##### 基本テーブル

| 列1 | 列2 | 列3 |
|-----|-----|-----|
| A1 | B1 | C1 |
| A2 | B2 | C2 |
| A3 | B3 | C3 |

##### アライメント指定テーブル

| 左寄せ | 中央寄せ | 右寄せ |
|:-------|:-------:|-------:|
| apple | banana | cherry |
| 123 | 456 | 789 |
| short | medium length text | extraordinarily long text |

##### テーブル内リンク

| 名前 | リンク | 備考 |
|:-----|:-------|:-----|
| GitHub | [github.com](https://github.com) | 外部リンク |
| はじめに | [1. はじめに](#1-はじめに) | 内部アンカーリンク |
| **太字リンク** | [**Bold Link**](https://example.com) | 太字+リンク |
| 複合 | 前文 [リンク](https://example.com) 後文 | テキスト中のリンク |

##### コンポーネントマトリクス

| # | コンポーネント | 主要ファイル | 配置 | テスト |
|:--|:--------------|:------------|:-----|:------|
| 1 | Win32Window | window.h/cpp | mendo | なし |
| 2 | App | app.h + app_*.cpp | mendo | 部分 |
| 3 | AppState | app_state.h | mendo_core | あり (各サブgrp) |
| 4 | app_controller | app_events.h + app_controller.cpp | mendo_core | あり |
| 5 | Reducer | reducer.h/cpp | mendo_core | あり |
| 6 | SideEffect + App::Execute | side_effect.h + app.cpp | mendo_core (型) / mendo (実行) | Reducer 経由 |
| 7 | Renderer | renderer*.cpp | mendo | なし |
| 8 | CommandGenerator | command_generator.h/cpp | mendo_core | あり |
| 9 | ExecuteDrawCommands | draw_command.h + command_executor.cpp | mendo_core | あり |
| 10 | Parser | parser.cpp + parser_alerts.cpp + anchor.cpp | mendo_core | あり |
| 11 | LayoutEngine + LayoutService | layout.h/cpp | mendo_core | あり |
| 12 | DWriteTextMeasurer | dwrite_measurer.h/cpp | mendo_core | parity |
| 13 | Document | document.h/cpp | mendo_core | あり |
| 14 | DocumentService | file_load_service.h/cpp | mendo_core | あり |
| 15 | Theme | theme.h/cpp | mendo_core | あり |
| 16 | ThemeService | theme_service.h/cpp | mendo_core | あり |
| 17 | Syntax | syntax.h/cpp | mendo_core | あり |
| 18 | FileLoader | file_loader.h/cpp | mendo_core | あり |
| 19 | FileWatcher | file_watcher.h/cpp | mendo_core | あり |
| 20 | FileLoadService | file_load_service.h/cpp | mendo_core | あり |
| 21 | AsyncLoadCoordinator | async_load_coordinator.h/cpp | mendo_core | あり |
| 22 | FileExplorer | file_explorer.h/cpp | mendo_core | あり |
| 23 | TOC + FindActiveTocIndex | document_types.h + layout_computer.h/cpp | mendo_core | あり |
| 24 | PaneController | pane_controller.h/cpp | mendo_core | あり |
| 25 | ViewportManager | viewport_manager.h | mendo_core | あり |
| 26 | NavHistory + HandleLinkClick | nav.h / nav.cpp | mendo_core | あり |
| 27 | MouseGesture | mouse_gesture.h | mendo_core | あり |
| 28 | SwipeDetector | swipe_detector.h | mendo_core | あり |
| 29 | hit_test | hit_test_service.h/cpp | mendo_core | あり |
| 30 | ConfigService + SessionService | config_service.h/cpp | mendo_core | あり |
| 31 | MermaidRenderer | mermaid.h/cpp | mendo | なし |
| 32 | MermaidFileCache | mermaid_file_cache.h/cpp | mendo_core | あり |
| 33 | TitleBar | titlebar.h/cpp | mendo_core | あり |
| 34 | ContextMenu | context_menu.h/cpp | mendo_core | あり |
| 35 | ToastNotifier | toast_notifier.h | mendo_core | あり |
| 36 | Tooltip + TooltipTarget | tooltip.h/cpp | mendo_core | TooltipTarget のみ |
| 37 | SearchState | search_state.h/cpp | mendo_core | あり |
| 38 | SearchBarController | search_bar_controller.h + app.h/cpp (AppHost) | mendo_core / mendo (AppHost) | あり |
| 39 | ImageLoader | image_loader.h/cpp | mendo_core | あり |
| 40 | ResourceManager | resource_manager.h + app.h/cpp (AppHost) | mendo_core / mendo (AppHost) | あり |
| 41 | i18n | i18n.h | mendo_core | あり |
| 42 | IniParser | ini_parser.h | mendo_core | あり |
| 43 | TaskScheduler | task_scheduler.h/cpp | mendo_core | あり |
| 44 | LruCache | lru_cache.h | mendo_core | あり |
| 45 | UniqueResource + Clipboard utils | win_handle.h + clipboard_util.h | mendo_core | なし |
| 46 | LastHoverPos | last_hover_pos.h | mendo_core | あり |
| 47 | ScrollRestoration | ui_types.h 内 | mendo_core | あり |
| 48 | SystemCursors | app.h 内 | mendo_core | なし |
| 49 | UIConstants | ui_constants.h | mendo_core | あり |
| 50 | StringConvert | string_convert.h | mendo_core | あり |
| 51 | Profiler | profiler.h | mendo_core | なし |

---

#### C.8 水平線テスト

上のテキスト

---

中間のテキスト

***

下のテキスト

___

最後のテキスト

---

#### C.9 Mermaid ダイアグラムテスト

##### フローチャート

```mermaid
flowchart TD
    A[開始] --> B{条件分岐}
    B -->|Yes| C[処理A]
    B -->|No| D[処理B]
    C --> E[結果出力]
    D --> E
    E --> F[終了]
```

##### シーケンス図

```mermaid
sequenceDiagram
    actor User as ユーザー
    participant App as mendo
    participant FS as ファイルシステム

    User->>App: ファイルをドロップ
    App->>FS: ファイル読み込み
    FS-->>App: UTF-8テキスト
    App->>App: Markdownパース
    App->>App: レイアウト計算
    App->>App: Direct2D描画
    App-->>User: 画面表示
```

##### クラス図

```mermaid
classDiagram
    class Win32Window {
        -HWND hwnd_
        -unique_ptr~App~ app_
        +Create() bool
        +RunMessageLoop() int
    }

    class App {
        -AppState state_
        -Renderer renderer_
        +Init(hwnd) bool
        +OnPaint()
        +Dispatch(action)
        -Execute(effects)
    }

    class Reducer {
        <<function>>
        +Reduce(state, action) SideEffectList
    }

    class SideEffect {
        <<variant>>
        InvalidateWindow
        SetTimer
        LoadFile
    }

    Win32Window --> App
    App --> Reducer
    Reducer ..> SideEffect
    App ..> SideEffect : visit で実行
```

##### 状態遷移図

```mermaid
stateDiagram-v2
    [*] --> 初期状態
    初期状態 --> ファイル表示中 : ファイルを開く
    ファイル表示中 --> ファイル表示中 : スクロール / ズーム
    ファイル表示中 --> テキスト選択中 : マウスドラッグ
    テキスト選択中 --> ファイル表示中 : マウスリリース
    ファイル表示中 --> ジェスチャ中 : 右ドラッグ開始
    ジェスチャ中 --> ファイル表示中 : ナビゲーション実行 / 中止
    ファイル表示中 --> ファイル表示中 : ファイル変更検出 → 再読み込み
    ファイル表示中 --> 初期状態 : ファイルを閉じる
    初期状態 --> [*]
```

##### ガントチャート

```mermaid
gantt
    title mendo 開発タイムライン
    dateFormat  YYYY-MM-DD
    section 基盤
    プロジェクトセットアップ      :done, a1, 2025-01-01, 7d
    Win32ウィンドウ基盤           :done, a2, after a1, 14d
    Direct2D/DirectWrite初期化    :done, a3, after a2, 7d

    section コア機能
    Markdownパーサ統合            :done, b1, after a3, 14d
    テキストレイアウトエンジン    :done, b2, after b1, 14d
    レンダリングエンジン          :done, b3, after b2, 21d

    section 拡張機能
    シンタックスハイライト        :done, c1, after b3, 7d
    Mermaidダイアグラム           :done, c2, after c1, 14d
    3ペインレイアウト             :done, c3, after c2, 7d
    テーマ・ズーム                :done, c4, after c3, 7d

    section リファクタリング
    サービスアーキテクチャ        :done, e1, after c4, 14d
    Command パターン              :done, e2, after e1, 7d
    Reducer / SideEffect 抽象化   :done, e3, after e2, 14d
    AppState 状態集約             :done, e4, after e3, 7d
    IWin32Host 境界分離           :done, e5, after e4, 7d
    レンダーステート構築の整理    :done, e6, after e5, 5d

    section UI拡張
    カスタムタイトルバー          :done, f1, after e6, 7d
    トースト通知                  :done, f2, after f1, 3d
    タッチパッドスワイプ          :done, f3, after f2, 3d
    カスタムコンテキストメニュー  :done, f4, after f3, 5d
    非同期画像読み込み            :done, f5, after f4, 5d
    検索機能                      :done, f6, after f5, 7d
    Mermaid永続キャッシュ         :done, f7, after f6, 5d
    NavHistory パスインターン     :done, f8, after f7, 3d

    section 品質
    ユニットテスト整備            :done, d1, after f8, 14d
    パフォーマンス最適化          :active, d2, after d1, 14d
    ドキュメント整備              :active, d3, after d1, 7d
```

---

#### C.10 複合テスト

以下は複数の書式が混在する段落のテストです。

**Direct2D** は Microsoft が提供する *ハードウェアアクセラレーション対応* の2Dグラフィックス API であり、`ID2D1RenderTarget` インターフェースを通じて図形やテキストの描画を行う。詳しくは [Microsoft Docs](https://docs.microsoft.com) を参照。~~古いGDI+ベースの描画~~ は本アプリケーションでは使用しない。

> **注意**: mendo は `IDWriteTextLayout` の ***カスタムレンダラ*** を使用して、リンクの下線やインラインコードの背景色などを実現している。通常の `DrawTextLayout` 呼び出しでは不十分なケースに対応するためである。

コードブロック内の `#include <d2d1.h>` のような記述と、本文中の `d2d1.h` というインラインコードの区別が正しく表示されることを確認する。

---

#### C.11 LaTeX 数式テスト

`$$...$$` のみで構成される段落を KaTeX (mermaid.js にバンドル済み) で描画する。他のテキストと混在する段落や blockquote 内、インライン `$...$` はテキスト表示にフォールバックする。

##### 基本的な数式

アインシュタインの有名な式:

$$E = mc^2$$

ピタゴラスの定理:

$$a^2 + b^2 = c^2$$

二次方程式の解の公式:

$$x = \frac{-b \pm \sqrt{b^2 - 4ac}}{2a}$$

##### ギリシャ文字・分数・累乗

$$\alpha + \beta = \gamma$$

$$\frac{\partial f}{\partial x} = \lim_{h \to 0} \frac{f(x+h) - f(x)}{h}$$

##### 総和・積分

$$\sum_{n=1}^{\infty} \frac{1}{n^2} = \frac{\pi^2}{6}$$

$$\int_{-\infty}^{\infty} e^{-x^2} \, dx = \sqrt{\pi}$$

##### 行列

$$\begin{pmatrix} a & b \\ c & d \end{pmatrix} \begin{pmatrix} x \\ y \end{pmatrix} = \begin{pmatrix} ax + by \\ cx + dy \end{pmatrix}$$

##### 複数行の数式

$$
\begin{aligned}
(a + b)^2 &= a^2 + 2ab + b^2 \\
(a - b)^2 &= a^2 - 2ab + b^2
\end{aligned}
$$

##### math コードブロック

GitHub / GitLab 互換の ` ```math ` コードブロックも `$$...$$` と同じく KaTeX で描画される。

```math
\begin{aligned}
\nabla \cdot \mathbf{E} &= \frac{\rho}{\varepsilon_0} \\
\nabla \times \mathbf{B} &= \mu_0 \mathbf{J} + \mu_0 \varepsilon_0 \frac{\partial \mathbf{E}}{\partial t}
\end{aligned}
```

##### フォールバック（混在段落）

段落内に数式以外の内容が混じるときは KaTeX 描画されず、`$$` 記号付きのテキストとして表示される。例: 文中に $$x^2 + y^2 = r^2$$ が含まれるこの段落は通常のテキストとして描画される。

##### インライン数式（非対応）

インライン `$...$` は LaTeX 描画対象外で、元の `$` 記号を含むテキストとして表示される。例: 変数 $x$ と $y$ の関係は $x = f(y)$ で表される。

##### 誤検出しないこと

通常のドル記号（金額表記など）は数式として扱われない。例: コーヒー1杯は $5、ランチは $10 以下で済む。

##### blockquote 内の数式（非対応）

blockquote 内の `$$...$$` は引用文脈を保つため KaTeX 描画されない。

> $$E = mc^2$$ — この引用ブロック内の数式はテキスト表示となる。

---

## GitHub Alerts

GitHub Alerts記法を用いたコールアウトの表示テスト。各Alert種別ごとに色分けされたバー・ラベル・背景が描画されることを確認する。

> [!NOTE]
> これは補足情報です。ユーザーが流し読みしていても気づけるよう、重要な補足事項をここに記載します。

> [!TIP]
> `Ctrl+O` でファイルを開くことができます。**ドラッグ＆ドロップ**にも対応しています。

> [!IMPORTANT]
> mendo は **Direct2D / DirectWrite** を前提としたレンダリングを行うため、Windows 10 1809 以降が必要です。

> [!WARNING]
> 非常に大きなMarkdownファイル（数十MB以上）を開くと、初回パースに時間がかかる場合があります。

> [!CAUTION]
> 設定ファイル（`%LOCALAPPDATA%\mendo\settings.ini`）を手動で編集する場合、不正なフォーマットでエントリを記述するとそのエントリは既定値で再生成されます。**編集前にバックアップを取ってください。**

### 複数段落のAlert

> [!NOTE]
> Alert内に複数段落を含めることができます。
>
> 2段落目のテキストもAlert内に表示されます。`インラインコード`や**太字**などの書式も使用可能です。

### 通常の引用ブロックとの共存

> これは通常の引用ブロックです。Alertではないため、従来どおり灰色のバーで表示されます。

> [!TIP]
> 通常の引用ブロックとAlertは同じ文書内で共存できます。

---

## 脚注

GitHub 互換の脚注記法の表示テスト。参照は `[1]` 形式のリンクになり、定義は文書末尾に参照順の番号付きリストとして表示される[^sample-basic]。参照をクリックすると定義へ、定義の `↩` をクリックすると参照へ移動する。

### 名前付きラベルと複数回の参照

ラベルには数字以外も使える[^sample-named]。同じ脚注を何度参照しても番号は同じで[^sample-basic]、定義側には参照の数だけ `↩` `↩2` … が付く。

### さまざまな場所からの参照

- リスト項目の中からの参照[^sample-list]
- **太字の中からの参照[^sample-named]**

> [!TIP]
> Alert の中からも参照できる[^sample-alert]。

| 場所 | 例 |
|:--|:--|
| テーブルセル | セル内の参照[^sample-table] |

### 定義の書式

定義の本文には **太字**・`コード`・リンクを書け、インデントした継続行も同じ脚注に含まれる[^sample-multiline]。

### 脚注として扱われない記法

定義の無い参照 [^sample-undefined] はテキストのまま表示される。どこからも参照されない定義は表示されない。

[^sample-basic]: 基本的な脚注。定義はどこに書いても文書末尾に集められる。
[^sample-named]: 名前付きラベルの脚注。番号はラベルではなく、最初に参照された順で振られる。
[^sample-list]: リスト項目から参照された脚注。
[^sample-alert]: Alert から参照された脚注。脚注の中から別の脚注も参照できる[^sample-basic]。
[^sample-table]: テーブルセルから参照された脚注。md4c の数え方に関わらず `↩` は 1 つだけ付く。
[^sample-multiline]: 1 行目の本文。**太字**、`code`、[md4c](https://github.com/mity/md4c) を含む。
    インデントした継続行も同じ脚注の本文になる。
[^sample-unused]: どこからも参照されないので表示されない。
