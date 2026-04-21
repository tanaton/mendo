#pragma once

// DIP 単位の矩形。D2D1_RECT_F と同じメンバ順・同型で POD なので、
// レンダラ側では <d2d1.h> をインクルードした後に ToD2DRect() で
// 同一レイアウトの D2D1_RECT_F に変換する。ヘッダから <d2d1.h> 依存を
// 剥がしたいコンポーネント（app_state.h 経由で広がる titlebar.h など）の
// 公開 API で使用する。
struct DipRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};
