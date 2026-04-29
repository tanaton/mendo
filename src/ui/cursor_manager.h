#pragma once
#include <windows.h>

// システムカーソルハンドルのキャッシュ。
// LoadCursorW の呼び出しを初期化時に一度だけ行う。
class CursorManager {
public:
    void Init() noexcept
    {
        arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        hand_ = LoadCursorW(nullptr, IDC_HAND);
        ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
        sizewe_ = LoadCursorW(nullptr, IDC_SIZEWE);
    }

    constexpr HCURSOR Arrow() const noexcept { return arrow_; }
    constexpr HCURSOR Hand() const noexcept { return hand_; }
    constexpr HCURSOR IBeam() const noexcept { return ibeam_; }
    constexpr HCURSOR SizeWE() const noexcept { return sizewe_; }

private:
    HCURSOR arrow_ = nullptr;
    HCURSOR hand_ = nullptr;
    HCURSOR ibeam_ = nullptr;
    HCURSOR sizewe_ = nullptr;
};
