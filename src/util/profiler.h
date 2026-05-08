#pragma once

// 計測ユーティリティ。バックエンドは Tracy のみ。
//
// 使い方:
//   {
//       MENDO_PROFILE("ParseMarkdown");
//       ... 計測対象の処理 ...
//   }
//
// CMake -DMENDO_USE_TRACY=ON で有効化される。OFF のときは全マクロが no-op に展開され、
// バイナリにも一切のコードが残らない。
//
// マクロ一覧:
//   MENDO_PROFILE(label)             : スコープ計測。label は文字列リテラル必須。
//   MENDO_FRAME_MARK()               : フレーム境界マーカー。OnPaint 末尾で 1 回呼ぶ。
//   MENDO_PLOT(label, value)         : 数値の時系列プロット。double / int 受け入れ可。
//   MENDO_COUNT_INC(counter)         : 累積カウンタの ++。Tracy OFF で no-op (DCE 担保)。
//   MENDO_COUNT_ADD(counter, n)      : 累積カウンタへの加算。Tracy OFF で no-op。
//   MENDO_COUNT_SET(counter, value)  : 累積でない直近値の代入。Tracy OFF で no-op。
//   MENDO_IF_TRACY(...)              : Tracy ON のときだけ展開する任意コード。
//   MENDO_TRACE(msg)                 : リテラルメッセージ（reload 系トレース）。
//   MENDO_TRACEF(fmt, ...)           : printf 形式トレース。
//   MENDO_STATF(fmt, ...)            : printf 形式の統計値ログ。
//   MENDO_LOGF(prefix, fmt, ...)     : 任意プレフィックスでログ出力。

// __LINE__ ベースで一意な識別子を生成するための内部マクロ。
// MENDO_PROFILE の展開で参照されるため #undef で隠せないが、`_MENDO_DETAIL_` prefix で
// 他 TU/サードパーティとの命名衝突を最小化する。
#define _MENDO_DETAIL_CONCAT2(a, b) a##b
#define _MENDO_DETAIL_CONCAT(a, b) _MENDO_DETAIL_CONCAT2(a, b)

#ifdef MENDO_USE_TRACY

#include <tracy/Tracy.hpp>
#include <cstdio>
#include <cstring>

// Tracy の ZoneScopedN は固定変数名 `___tracy_scoped_zone` を生成するため、
// 同一スコープに 2 個書くと再定義エラーになる。__LINE__ で固有名を作る
// ZoneNamedN を使って同一関数内で複数 MENDO_PROFILE を書ける形にする。
#define MENDO_PROFILE(label) \
    ZoneNamedN(_MENDO_DETAIL_CONCAT(_mendo_zone_, __LINE__), label, true)
#define MENDO_FRAME_MARK() FrameMark
#define MENDO_PLOT(label, value) TracyPlot(label, value)

// 累積カウンタ系。Tracy OFF では完全に no-op になるため、対象カウンタが
// 他から参照されない限り、変数定義ごと dead code として消える。
#define MENDO_COUNT_INC(counter) (++(counter))
#define MENDO_COUNT_ADD(counter, n) ((counter) += (n))
#define MENDO_COUNT_SET(counter, value) ((counter) = (value))
#define MENDO_IF_TRACY(...) __VA_ARGS__

// _snprintf_s は _TRUNCATE 指定時、収まれば書き込んだ文字数を返し、切り詰めが
// 起きると -1 を返す（バッファ自体は NUL 終端される）。-1 を 0 と同様に
// 捨てると長メッセージが silently drop されるため、切り詰め時は実長を取り直す。
#define MENDO_LOGF(prefix, fmt, ...)                                            \
    do {                                                                        \
        char _mendo_buf[256];                                                   \
        const int _mendo_n = _snprintf_s(_mendo_buf, sizeof(_mendo_buf),        \
                                         _TRUNCATE, prefix fmt, __VA_ARGS__);   \
        const size_t _mendo_len = (_mendo_n >= 0)                               \
            ? static_cast<size_t>(_mendo_n)                                     \
            : strnlen(_mendo_buf, sizeof(_mendo_buf) - 1);                      \
        if (_mendo_len > 0) {                                                   \
            TracyMessage(_mendo_buf, _mendo_len);                               \
        }                                                                       \
    } while (0)

// TracyMessageL はリテラル前提（NULL 終端の文字列をそのまま参照保持する）。
#define MENDO_TRACE(msg) TracyMessageL("[mendo-reload] " msg)
#define MENDO_TRACEF(fmt, ...) MENDO_LOGF("[mendo-reload] ", fmt, __VA_ARGS__)
#define MENDO_STATF(fmt, ...) MENDO_LOGF("[mendo-stat] ", fmt, __VA_ARGS__)

#else

// MENDO_USE_TRACY 未定義時は全マクロを完全に消す。
#define MENDO_PROFILE(label) ((void)0)
#define MENDO_FRAME_MARK() ((void)0)
#define MENDO_PLOT(label, value) ((void)0)
#define MENDO_COUNT_INC(counter) ((void)0)
#define MENDO_COUNT_ADD(counter, n) ((void)0)
#define MENDO_COUNT_SET(counter, value) ((void)0)
#define MENDO_IF_TRACY(...)
#define MENDO_LOGF(prefix, fmt, ...) ((void)0)
#define MENDO_TRACE(msg) ((void)0)
#define MENDO_TRACEF(fmt, ...) ((void)0)
#define MENDO_STATF(fmt, ...) ((void)0)

#endif
