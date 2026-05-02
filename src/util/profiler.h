#pragma once
#include <windows.h>
#include <cstdio>

// 処理ブロックごとの時間計測ユーティリティ。
// OutputDebugStringW でタイミング情報を出力する（DebugView や VS 出力ウィンドウで確認可能）。
//
// 使い方:
//   {
//       MENDO_PROFILE("ParseMarkdown");
//       ... 計測対象の処理 ...
//   }
//
// MENDO_PROFILE_ENABLED を 0 にするとすべての計測コードが消える。
#ifndef MENDO_PROFILE_ENABLED
#define MENDO_PROFILE_ENABLED 1
#endif

#if MENDO_PROFILE_ENABLED

class ScopedProfileTimer {
public:
    explicit ScopedProfileTimer(const wchar_t* label) noexcept : label_(label)
    {
        QueryPerformanceCounter(&start_);
    }

    ~ScopedProfileTimer() noexcept
    {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);

        const double elapsed_us = static_cast<double>(end.QuadPart - start_.QuadPart) * 1'000'000.0 / static_cast<double>(Frequency().QuadPart);

        wchar_t buf[256];
        if (elapsed_us >= 1000.0) {
            _snwprintf_s(buf, _TRUNCATE, L"[mendo-profile] %s: %.2f ms\n", label_, elapsed_us / 1000.0);
        }
        else {
            _snwprintf_s(buf, _TRUNCATE, L"[mendo-profile] %s: %.1f us\n", label_, elapsed_us);
        }
        OutputDebugStringW(buf);
    }

    ScopedProfileTimer(const ScopedProfileTimer&) = delete;
    ScopedProfileTimer& operator=(const ScopedProfileTimer&) = delete;

private:
    static const LARGE_INTEGER& Frequency() noexcept
    {
        static const LARGE_INTEGER freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f;
        }();
        return freq;
    }

    const wchar_t* label_;
    LARGE_INTEGER start_;
};

#define MENDO_PROFILE_CONCAT2(a, b) a##b
#define MENDO_PROFILE_CONCAT(a, b) MENDO_PROFILE_CONCAT2(a, b)
#define MENDO_PROFILE(label) ScopedProfileTimer MENDO_PROFILE_CONCAT(_mendo_timer_, __LINE__)(L##label)

#define MENDO_LOGF(prefix, fmt, ...)                                            \
    do {                                                                        \
        wchar_t _mendo_buf[256];                                                \
        _snwprintf_s(_mendo_buf, _TRUNCATE, prefix L##fmt L"\n", __VA_ARGS__);  \
        OutputDebugStringW(_mendo_buf);                                         \
    } while (0)

#define MENDO_TRACE(msg) OutputDebugStringW(L"[mendo-reload] " L##msg L"\n")
#define MENDO_TRACEF(fmt, ...) MENDO_LOGF(L"[mendo-reload] ", fmt, __VA_ARGS__)
#define MENDO_STATF(fmt, ...) MENDO_LOGF(L"[mendo-stat] ", fmt, __VA_ARGS__)

#else

#define MENDO_PROFILE(label) ((void)0)
#define MENDO_TRACE(msg) ((void)0)
#define MENDO_TRACEF(fmt, ...) ((void)0)
#define MENDO_STATF(fmt, ...) ((void)0)

#endif
