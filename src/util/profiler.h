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
//   MENDO_TRACE(msg)                 : リテラルメッセージ（reload 系トレース）。
//   MENDO_TRACEF(fmt, ...)           : printf 形式トレース。
//   MENDO_STATF(fmt, ...)            : printf 形式の統計値ログ。
//   MENDO_LOGF(prefix, fmt, ...)     : 任意プレフィックスでログ出力。

#define MENDO_PROFILE_CONCAT2(a, b) a##b
#define MENDO_PROFILE_CONCAT(a, b) MENDO_PROFILE_CONCAT2(a, b)

#ifdef MENDO_USE_TRACY

#include <tracy/Tracy.hpp>
#include <cstdio>

// Tracy の ZoneScopedN は固定変数名 `___tracy_scoped_zone` を生成するため、
// 同一スコープに 2 個書くと再定義エラーになる。__LINE__ で固有名を作る
// ZoneNamedN を使って同一関数内で複数 MENDO_PROFILE を書ける形にする。
#define MENDO_PROFILE(label) \
    ZoneNamedN(MENDO_PROFILE_CONCAT(_mendo_zone_, __LINE__), label, true)
#define MENDO_FRAME_MARK() FrameMark
#define MENDO_PLOT(label, value) TracyPlot(label, value)

// printf 形式のメッセージは sprintf してから TracyMessage に渡す。
// 既存の MENDO_TRACE/STATF はすべて ASCII リテラル + 整数/小数フォーマットなので
// char バッファで足りる。256B を超えるメッセージは _TRUNCATE で切り詰められる。
#define MENDO_LOGF(prefix, fmt, ...)                                            \
    do {                                                                        \
        char _mendo_buf[256];                                                   \
        const int _mendo_n = _snprintf_s(_mendo_buf, sizeof(_mendo_buf),        \
                                         _TRUNCATE, prefix fmt, __VA_ARGS__);   \
        if (_mendo_n > 0) {                                                     \
            TracyMessage(_mendo_buf, static_cast<size_t>(_mendo_n));            \
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
#define MENDO_LOGF(prefix, fmt, ...) ((void)0)
#define MENDO_TRACE(msg) ((void)0)
#define MENDO_TRACEF(fmt, ...) ((void)0)
#define MENDO_STATF(fmt, ...) ((void)0)

#endif
