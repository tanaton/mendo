#pragma once
#include "side_effect.h"
#include <memory_resource>
#include <string>
#include <windows.h>
#include <string_view>

// SideEffectExecutor が Win32 API 境界を跨ぐための adapter interface。
// 本番では Win32Host（HWND を保持）を、
// テストでは mock 実装を注入して副作用を検証する。
class IWin32Host {
public:
    virtual ~IWin32Host() = default;

    virtual void Invalidate() = 0;

    virtual void SetTimer(app_timer::Id id, UINT ms) = 0;
    virtual void KillTimer(app_timer::Id id) = 0;

    virtual void SetCapture() = 0;
    virtual void ReleaseCapture() = 0;

    // 入力は全て UTF-8。CF_UNICODETEXT 用の UTF-16 変換は実装側で行う。
    virtual void WriteClipboardText(std::string_view text) = 0;
    virtual void WriteClipboardHtml(std::string_view html, std::string_view plain) = 0;

    // ShellExecuteW は null 終端文字列を要求するため、既に null 終端を保証している
    // pmr::wstring を直接受ける。wstring_view 経由の null 終端コピーを 1 回省略できる。
    virtual void ShellOpen(const std::pmr::wstring& url) = 0;
    virtual void SearchFocus(effect::SearchFocus action) = 0;
    virtual void SearchUnfocus(effect::SearchUnfocus action) = 0;
    virtual void SetWindowPosition(int x, int y, int cx, int cy) = 0;
};

// IWin32Host の本番 Win32 実装。HWND を保持し、実際の Win32 API 呼び出しに委譲する。
class Win32Host final : public IWin32Host {
public:
    void Init(HWND hwnd) noexcept;

    void Invalidate() override;
    void SetTimer(app_timer::Id id, UINT ms) override;
    void KillTimer(app_timer::Id id) override;
    void SetCapture() override;
    void ReleaseCapture() override;
    void WriteClipboardText(std::string_view text) override;
    void WriteClipboardHtml(std::string_view html, std::string_view plain) override;
    void ShellOpen(const std::pmr::wstring& url) override;
    void SearchFocus(effect::SearchFocus action) override;
    void SearchUnfocus(effect::SearchUnfocus action) override;
    void SetWindowPosition(int x, int y, int cx, int cy) override;

private:
    HWND hwnd_ = nullptr;
};
