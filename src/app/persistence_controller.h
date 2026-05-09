#pragma once
#include <windows.h>

class SessionService;
struct AppState;

// アプリ起動・終了時のセッション情報の永続化を担当する。
//   - 直前に開いていたファイルパス
//   - ペインの幅と表示状態
//   - スクロール位置 (ノード+オフセット)
// SessionService との橋渡しを集約し、AppState からの値取り出し / PaneController への
// 値書き戻しを 1 か所にまとめる。 終了時の Save 群は OnDestroy から、Load は Init から呼ぶ。
class PersistenceController {
public:
    PersistenceController(SessionService& session, AppState& state) noexcept
        : session_(session), state_(state)
    {}

    // ヘルプ仮想パスは保存しない。
    void SaveLastFilePath();
    void SavePaneState();
    // hwnd の client 幅を使って動的最大幅を計算する。hwnd_ が null なら既定値を使う。
    void LoadPaneState(HWND hwnd);
    // 先頭可視ノードが存在する場合のみ書き込む。
    void SaveScrollPosition();

private:
    SessionService& session_;
    AppState& state_;
};
